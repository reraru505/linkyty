#include "common/profiler.h"

#include "common/emulatorConfig.h"
#include "common/logging/log.h"

#include <algorithm>
#include <atomic>
#include <common/TracyProtocol.hpp>
#include <common/TracyVersion.hpp>
#include <cstdio>
#include <cstdlib>
#include <cstdlib>
#include <cstring>
#include <dirent.h>
#include <fmt/format.h>
#include <dirent.h>
#include <fcntl.h>
#include <signal.h>
#include <sys/syscall.h>
#include <thread>
#include <tracy/Tracy.hpp>
#include <ucontext.h>
#include <unistd.h>
#include <vector>

namespace {

thread_local std::vector<Profiler::ScopedBlock*> g_block_stack;

void RemoveBlock(Profiler::ScopedBlock* block) {
	auto block_it = std::find(g_block_stack.rbegin(), g_block_stack.rend(), block);
	if (block_it != g_block_stack.rend()) {
		g_block_stack.erase(std::next(block_it).base());
	}
}

// ---------------------------------------------------------------------------
// PC sampler
// ---------------------------------------------------------------------------
//
// ptrace is commonly restricted (yama ptrace_scope), which rules out an external profiler, so the
// sampling lives in-process. A helper thread polls /proc/self/task for threads in state R and
// pokes one of them with SIGPROF; the handler stores the interrupted instruction pointer together
// with its thread id into a per-thread block and writes full blocks straight to the output file,
// all of which is async-signal-safe. Raw addresses are aggregated and symbolized offline.

constexpr uint32_t PcBlockSamples = 512;

int                   g_pc_fd = -1;
std::atomic_bool      g_pc_stop {false};
std::thread           g_pc_thread;

struct PcBlock {
	uint64_t pc[PcBlockSamples];
	// Top of the interrupted stack: for leaf functions (memset, the unwinder, allocators) this is
	// the return address, which is what makes a hot leaf attributable to a caller.
	uint64_t caller[PcBlockSamples];
	uint64_t tid[PcBlockSamples];
	uint32_t count = 0;
};

// Constant-initialized POD so the handler can touch it without running a dynamic initializer.
thread_local PcBlock t_pc_block;

void PcFlush(PcBlock& block) {
	if (block.count == 0 || g_pc_fd < 0) {
		block.count = 0;
		return;
	}
	const auto bytes = static_cast<size_t>(block.count);
	(void)::write(g_pc_fd, block.pc, bytes * sizeof(uint64_t));
	(void)::write(g_pc_fd, block.caller, bytes * sizeof(uint64_t));
	(void)::write(g_pc_fd, block.tid, bytes * sizeof(uint64_t));
	block.count = 0;
}

void PcSampleHandler(int, siginfo_t*, void* context) {
	auto*       uc = static_cast<ucontext_t*>(context);
	const auto* m  = &uc->uc_mcontext;
	const auto  pc = static_cast<uint64_t>(m->gregs[REG_RIP]);
	const auto  tid = static_cast<uint64_t>(::syscall(SYS_gettid));
	auto&       block = t_pc_block;
	if (block.count == PcBlockSamples) {
		PcFlush(block);
	}
	// Find the return address on the top of the interrupted stack. Reading only [rsp] is not
	// enough: helper functions such as memset push registers first, so their [rsp] is a saved
	// register. Scan the first few slots for a value that can only be a return address (guest or
	// emulator text); the window stays inside the thread's own live frame.
	uint64_t caller = 0;
	const auto stack = static_cast<uint64_t>(m->gregs[REG_RSP]);
	if ((stack & 0x7u) == 0 && stack > 0x10000u) {
		const auto* slots = reinterpret_cast<const uint64_t*>(stack);
		for (int i = 0; i < 16; i++) {
			const auto candidate = slots[i];
			const bool guest     = candidate >= 0x900000000ull && candidate < 0x903850000ull;
			const bool emulator  = candidate >= 0x117e000ull && candidate < 0x170c000ull;
			if (guest || emulator) {
				caller = candidate;
				break;
			}
		}
	}
	block.pc[block.count]     = pc;
	block.caller[block.count] = caller;
	block.tid[block.count]    = tid;
	block.count++;
}

std::vector<pid_t> RunningThreads() {
	std::vector<pid_t> result;
	DIR*               dir = ::opendir("/proc/self/task");
	if (dir == nullptr) {
		return result;
	}
	while (auto* entry = ::readdir(dir)) {
		if (entry->d_name[0] < '0' || entry->d_name[0] > '9') {
			continue;
		}
		char path[64] {};
		std::snprintf(path, sizeof(path), "/proc/self/task/%s/stat", entry->d_name);
		int fd = ::open(path, O_RDONLY | O_CLOEXEC);
		if (fd < 0) {
			continue;
		}
		char    buffer[512] {};
		const auto size = ::read(fd, buffer, sizeof(buffer) - 1);
		::close(fd);
		if (size <= 0) {
			continue;
		}
		// "pid (comm) state ...": skip past the comm field, which may contain spaces.
		const auto* close_paren = std::strrchr(buffer, ')');
		if (close_paren == nullptr || close_paren[1] != ' ' || close_paren[2] != 'R') {
			continue;
		}
		result.push_back(static_cast<pid_t>(std::strtol(entry->d_name, nullptr, 10)));
	}
	::closedir(dir);
	return result;
}

// The guest image and its data live in mappings that belong to this process, so they can be copied
// out directly (no ptrace) for offline disassembly. Anything in the guest VA range is dumped, plus
// every other executable mapping of decent size.
void DumpExecutableRegions(const char* path) {
	auto* maps = std::fopen("/proc/self/maps", "r");
	if (maps == nullptr) {
		return;
	}
	auto* out = std::fopen(path, "wb");
	if (out == nullptr) {
		std::fclose(maps);
		return;
	}
	char line[512] {};
	while (std::fgets(line, sizeof(line), maps) != nullptr) {
		unsigned long long lo = 0;
		unsigned long long hi = 0;
		char               perms[8] {};
		char               rest[256] {};
		if (std::sscanf(line, "%llx-%llx %7s %255[^\n]", &lo, &hi, perms, rest) < 3) {
			continue;
		}
		const auto size     = hi - lo;
		const bool guest    = lo >= 0x900000000ull && lo < 0x1000000000ull;
		const bool anon     = std::strstr(rest, "/") == nullptr;
		const bool exec     = perms[2] == 'x';
		const bool readable = perms[0] == 'r';
		// Only dump mappings this thread may read: the guest trampoline pages are exec-only, and
		// reading one past their end faults the emulator (it is the emulator's own memory).
		const bool wanted = readable && ((guest && size <= (512u << 20u)) ||
		                                 (exec && anon && size >= (8u << 20u)));
		if (!wanted || size == 0) {
			if (guest && size != 0) {
				LOGF("PC sampler: skipping guest region 0x%llx-0x%llx (%s, not readable)\n", lo, hi,
				     perms);
			}
			continue;
		}
		const auto header = fmt::format("# region 0x{:x}-0x{:x} {} {}\n", lo, hi, perms, rest);
		std::fwrite(header.data(), 1, header.size(), out);
		std::fwrite(reinterpret_cast<const void*>(lo), 1, size, out);
		LOGF("PC sampler: dumped 0x%llx-0x%llx %s %s (%llu KB)\n", lo, hi, perms, rest, size >> 10u);
	}
	std::fclose(out);
	std::fclose(maps);
}

// Samples are keyed by tid; without this map there is no way to tell the render thread from a job
// worker in an offline dump (the process is usually gone by then).
void DumpThreadNames(const char* path) {
	auto* out = std::fopen(path, "w");
	if (out == nullptr) {
		return;
	}
	auto* dir = opendir("/proc/self/task");
	if (dir != nullptr) {
		while (auto* entry = readdir(dir)) {
			const auto tid = std::strtoul(entry->d_name, nullptr, 10);
			if (tid == 0) {
				continue;
			}
			const auto comm_path = fmt::format("/proc/self/task/{}/comm", tid);
			auto*      comm      = std::fopen(comm_path.c_str(), "r");
			if (comm == nullptr) {
				continue;
			}
			char name[64] {};
			if (std::fgets(name, sizeof(name), comm) != nullptr) {
				std::fprintf(out, "%lu %s", tid, name);
			}
			std::fclose(comm);
		}
		closedir(dir);
	}
	std::fclose(out);
}

void PcSamplerMain() {
	Profiler::SetThreadName("PcSampler");
	if (const char* dump = std::getenv("LINKYTY_DUMP_RWX"); dump != nullptr) {
		std::this_thread::sleep_for(std::chrono::seconds(25));
		DumpExecutableRegions(dump);
	}
	while (!g_pc_stop.load(std::memory_order_relaxed)) {
		const auto running = RunningThreads();
		for (const auto tid: running) {
			(void)::syscall(SYS_tgkill, ::getpid(), tid, SIGPROF);
		}
		std::this_thread::sleep_for(std::chrono::microseconds(500));
	}
}

} // namespace

namespace Profiler {

ScopedBlock::ScopedBlock(const tracy::SourceLocationData* source_location) {
	if (tracy::ProfilerAvailable()) {
		m_zone.emplace(source_location, TRACY_CALLSTACK, true);
		g_block_stack.push_back(this);
	}
}

ScopedBlock::~ScopedBlock() {
	End();
}

void ScopedBlock::End() {
	if (m_zone.has_value()) {
		m_zone.reset();
		RemoveBlock(this);
	}
}

void EndBlock() {
	if (!g_block_stack.empty()) {
		g_block_stack.back()->End();
	}
}

void SetThreadName(const char* name) {
	if (tracy::ProfilerAvailable() && name != nullptr) {
		tracy::SetThreadName(name);
	}
}

void Initialize() {
	StartPcSampler();
	switch (Config::GetProfilerDirection()) {
		case Config::ProfilerDirection::Network:
			if (!tracy::ProfilerAvailable()) {
				tracy::StartupProfiler();
				TracySetProgramName("KytyPS5");
				::printf("Tracy profiler enabled: client %d.%d.%d, protocol %u, "
				         "broadcast %u, connect to 127.0.0.1:8086\n",
				         tracy::Version::Major, tracy::Version::Minor, tracy::Version::Patch,
				         tracy::ProtocolVersion, tracy::BroadcastVersion);
			}
			break;
		case Config::ProfilerDirection::None:
		default: break;
	}
}

void Shutdown() {
	PcFlush(t_pc_block);
	if (tracy::ProfilerAvailable()) {
		tracy::ShutdownProfiler();
	}
}

void StartPcSampler() {
	const char* path = std::getenv("LINKYTY_PC_PROFILE");
	if (path == nullptr || path[0] == '\0') {
		return;
	}
	g_pc_fd = ::open(path, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0644);
	if (g_pc_fd < 0) {
		::printf("PC sampler: cannot open %s\n", path);
		return;
	}
	struct sigaction action {};
	action.sa_sigaction = PcSampleHandler;
	action.sa_flags     = SA_SIGINFO | SA_RESTART;
	sigfillset(&action.sa_mask);
	if (::sigaction(SIGPROF, &action, nullptr) != 0) {
		::printf("PC sampler: sigaction failed\n");
		return;
	}
	g_pc_thread = std::thread(PcSamplerMain);
	const auto names = fmt::format("{}.names", path);
	::printf("PC sampler: writing raw samples to %s (thread names to %s)\n", path, names.c_str());
	std::thread([names] {
		for (;;) {
			DumpThreadNames(names.c_str());
			if (g_pc_stop.load()) {
				break;
			}
			std::this_thread::sleep_for(std::chrono::seconds(10));
		}
	}).detach();
}

} // namespace Profiler
