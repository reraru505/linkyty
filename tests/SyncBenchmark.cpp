#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/singleton.h"
#include "common/subsystems.h"
#include "common/threads.h"
#include "kernel/memory.h"
#include "kernel/pthread.h"
#include "kernel/syncOnAddress.h"
#include "libs/errno.h"
#include "loader/runtimeLinker.h"

#include <atomic>
#include <chrono>
#include <cinttypes>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <string>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

struct ContextSwitches {
	uint64_t voluntary    = 0;
	uint64_t nonvoluntary = 0;
};

static ContextSwitches ReadContextSwitches() {
	ContextSwitches cs {};
	std::ifstream   file("/proc/self/status");
	std::string     line;
	while (std::getline(file, line)) {
		if (line.starts_with("voluntary_ctxt_switches:")) {
			std::sscanf(line.c_str(), "voluntary_ctxt_switches: %" SCNu64, &cs.voluntary);
		} else if (line.starts_with("nonvoluntary_ctxt_switches:")) {
			std::sscanf(line.c_str(), "nonvoluntary_ctxt_switches: %" SCNu64, &cs.nonvoluntary);
		}
	}
	return cs;
}

// -----------------------------------------------------------------------------
// Benchmark 1: Uncontended Mutex Lock/Unlock
// -----------------------------------------------------------------------------
void BenchmarkUncontendedMutex(uint64_t iterations) {
	std::printf("\n========================================================\n");
	std::printf("Benchmark 1: Uncontended PthreadMutex Latency (%lu ops)\n", iterations);
	std::printf("========================================================\n");

	Libs::LibKernel::PthreadMutex mutex {};
	int err = Libs::LibKernel::PthreadMutexInit(&mutex, nullptr, "uncontended_bench");
	if (err != 0) {
		std::fprintf(stderr, "Failed to init mutex: %d\n", err);
		return;
	}

	const auto cs_before = ReadContextSwitches();
	const auto start     = Clock::now();

	for (uint64_t i = 0; i < iterations; i++) {
		Libs::LibKernel::PthreadMutexLock(&mutex);
		Libs::LibKernel::PthreadMutexUnlock(&mutex);
	}

	const auto end       = Clock::now();
	const auto cs_after  = ReadContextSwitches();

	const double total_ms =
	    std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(end - start).count();
	const double ns_per_op = (total_ms * 1e6) / static_cast<double>(iterations);
	const double mops_sec  = static_cast<double>(iterations) / (total_ms * 1e3);

	std::printf("  Total Time:          %.3f ms\n", total_ms);
	std::printf("  Latency per Lock/Unlock: %.1f ns\n", ns_per_op);
	std::printf("  Throughput:          %.2f M ops/sec\n", mops_sec);
	std::printf("  Voluntary Context Switches:    %" PRIu64 "\n",
	            cs_after.voluntary - cs_before.voluntary);
	std::printf("  Non-Voluntary Context Switches: %" PRIu64 "\n",
	            cs_after.nonvoluntary - cs_before.nonvoluntary);

	Libs::LibKernel::PthreadMutexDestroy(&mutex);
}

// -----------------------------------------------------------------------------
// Benchmark 2: Contended Mutex Scaling (Worker Threads)
// -----------------------------------------------------------------------------
struct ContendedWorkerArg {
	Libs::LibKernel::PthreadMutex* mutexes;
	size_t                         num_mutexes;
	uint64_t                       ops_per_thread;
	std::atomic<bool>*             start_flag;
	uint64_t                       shared_counter;
};

static KYTY_SYSV_ABI void* ContendedWorkerFunc(void* param) {
	auto* arg = static_cast<ContendedWorkerArg*>(param);

	while (!arg->start_flag->load(std::memory_order_acquire)) {
		std::this_thread::yield();
	}

	for (uint64_t i = 0; i < arg->ops_per_thread; i++) {
		size_t m_idx = i % arg->num_mutexes;
		Libs::LibKernel::PthreadMutexLock(&arg->mutexes[m_idx]);
		arg->shared_counter++;
		Libs::LibKernel::PthreadMutexUnlock(&arg->mutexes[m_idx]);
	}

	return nullptr;
}

void BenchmarkContendedMutex(int num_threads, uint64_t total_ops) {
	std::printf("\n========================================================\n");
	std::printf("Benchmark 2: Contended PthreadMutex (%d Threads, %lu ops)\n", num_threads,
	            total_ops);
	std::printf("========================================================\n");

	constexpr size_t NUM_MUTEXES = 4;
	Libs::LibKernel::PthreadMutex mutexes[NUM_MUTEXES] {};
	for (size_t i = 0; i < NUM_MUTEXES; i++) {
		Libs::LibKernel::PthreadMutexInit(&mutexes[i], nullptr, "contended_m");
	}

	const uint64_t ops_per_thread = total_ops / static_cast<uint64_t>(num_threads);
	std::atomic<bool> start_flag{false};

	std::vector<ContendedWorkerArg> args(num_threads);
	std::vector<Libs::LibKernel::Pthread> threads(num_threads);

	for (int i = 0; i < num_threads; i++) {
		args[i].mutexes        = mutexes;
		args[i].num_mutexes    = NUM_MUTEXES;
		args[i].ops_per_thread = ops_per_thread;
		args[i].start_flag     = &start_flag;
		args[i].shared_counter = 0;

		Libs::LibKernel::PthreadCreate(&threads[i], nullptr, ContendedWorkerFunc, &args[i],
		                               "bench_worker");
	}

	// Warmup delay then start
	std::this_thread::sleep_for(std::chrono::milliseconds(20));
	const auto cs_before = ReadContextSwitches();
	const auto start     = Clock::now();

	start_flag.store(true, std::memory_order_release);

	for (int i = 0; i < num_threads; i++) {
		Libs::LibKernel::PthreadJoin(threads[i], nullptr);
	}

	const auto end       = Clock::now();
	const auto cs_after  = ReadContextSwitches();

	const double total_ms =
	    std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(end - start).count();
	const double ns_per_op = (total_ms * 1e6) / static_cast<double>(total_ops);
	const double mops_sec  = static_cast<double>(total_ops) / (total_ms * 1e3);

	std::printf("  Total Time:          %.3f ms\n", total_ms);
	std::printf("  Latency per Contended Op:  %.1f ns\n", ns_per_op);
	std::printf("  Throughput:          %.2f M ops/sec\n", mops_sec);
	std::printf("  Voluntary Context Switches:    %" PRIu64 "\n",
	            cs_after.voluntary - cs_before.voluntary);
	std::printf("  Non-Voluntary Context Switches: %" PRIu64 "\n",
	            cs_after.nonvoluntary - cs_before.nonvoluntary);

	for (size_t i = 0; i < NUM_MUTEXES; i++) {
		Libs::LibKernel::PthreadMutexDestroy(&mutexes[i]);
	}
}

// -----------------------------------------------------------------------------
// Benchmark 3: SyncOnAddress (Futex) Ping-Pong Latency
// -----------------------------------------------------------------------------
struct PingPongData {
	uint32_t           flag_a      = 0;
	uint32_t           flag_b      = 0;
	uint64_t           iterations  = 0;
	std::atomic<bool>  ready       = false;
};

static KYTY_SYSV_ABI void* PingPongWorker(void* param) {
	auto* data = static_cast<PingPongData*>(param);
	data->ready.store(true, std::memory_order_release);

	for (uint64_t i = 0; i < data->iterations; i++) {
		// Wait for thread A to set flag_a = 1
		uint32_t timeout = 500000;
		while (__atomic_load_n(&data->flag_a, __ATOMIC_ACQUIRE) == 0) {
			Libs::LibKernel::SyncOnAddress::Wait32(&data->flag_a, 0, &timeout);
		}
		__atomic_store_n(&data->flag_a, 0, __ATOMIC_RELEASE);

		// Signal thread B by setting flag_b = 1 and waking
		__atomic_store_n(&data->flag_b, 1, __ATOMIC_RELEASE);
		Libs::LibKernel::SyncOnAddress::Wake(&data->flag_b, 1);
	}

	return nullptr;
}

void BenchmarkSyncOnAddressPingPong(uint64_t roundtrips) {
	std::printf("\n========================================================\n");
	std::printf("Benchmark 3: SyncOnAddress Futex Round-Trip (%lu hops)\n", roundtrips);
	std::printf("========================================================\n");

	PingPongData data {};
	data.iterations = roundtrips;

	Libs::LibKernel::Pthread worker {};
	Libs::LibKernel::PthreadCreate(&worker, nullptr, PingPongWorker, &data, "pingpong_worker");

	while (!data.ready.load(std::memory_order_acquire)) {
		std::this_thread::yield();
	}

	const auto cs_before = ReadContextSwitches();
	const auto start     = Clock::now();

	for (uint64_t i = 0; i < roundtrips; i++) {
		// Signal worker by setting flag_a = 1 and waking
		__atomic_store_n(&data.flag_a, 1, __ATOMIC_RELEASE);
		Libs::LibKernel::SyncOnAddress::Wake(&data.flag_a, 1);

		// Wait for worker to set flag_b = 1
		uint32_t timeout = 500000;
		while (__atomic_load_n(&data.flag_b, __ATOMIC_ACQUIRE) == 0) {
			Libs::LibKernel::SyncOnAddress::Wait32(&data.flag_b, 0, &timeout);
		}
		__atomic_store_n(&data.flag_b, 0, __ATOMIC_RELEASE);
	}

	const auto end       = Clock::now();
	const auto cs_after  = ReadContextSwitches();

	Libs::LibKernel::PthreadJoin(worker, nullptr);

	const double total_ms =
	    std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(end - start).count();
	const double ns_per_roundtrip = (total_ms * 1e6) / static_cast<double>(roundtrips);
	const double roundtrips_per_sec = static_cast<double>(roundtrips) / (total_ms * 1e-3);

	std::printf("  Total Time:          %.3f ms\n", total_ms);
	std::printf("  Round-Trip Latency:  %.1f ns (%.2f us)\n", ns_per_roundtrip,
	            ns_per_roundtrip / 1000.0);
	std::printf("  Throughput:          %.0f round-trips/sec\n", roundtrips_per_sec);
	std::printf("  Voluntary Context Switches:    %" PRIu64 "\n",
	            cs_after.voluntary - cs_before.voluntary);
	std::printf("  Non-Voluntary Context Switches: %" PRIu64 "\n",
	            cs_after.nonvoluntary - cs_before.nonvoluntary);
}

} // namespace

int main([[maybe_unused]] int argc, [[maybe_unused]] char* argv[]) {
	Common::InitializeThreads();
	std::printf("Initializing Kyty Subsystems for Synchronization Benchmark...\n");

	Common::Subsystems subsystems(true);
	subsystems.Initialize<Config::Lifecycle>();
	Config::ConfigOptions options {};
	options.user_name = "BenchmarkUser";
	options.user_id   = 0x10000000;
	Config::Load(options);
	subsystems.Initialize<Log::Lifecycle>();
	subsystems.Initialize<Libs::LibKernel::Memory::Lifecycle>();
	subsystems.Initialize<Libs::LibKernel::PthreadLifecycle>();

	Libs::LibKernel::PthreadInitSelfForMainThread();
	(void)Common::Singleton<Loader::RuntimeLinker>::Instance();

	// 1. Uncontended mutex lock/unlock latency
	BenchmarkUncontendedMutex(2000000);

	// 2. Contended mutex scaling: 2, 4, and 8 threads
	BenchmarkContendedMutex(2, 500000);
	BenchmarkContendedMutex(4, 500000);
	BenchmarkContendedMutex(8, 500000);

	// 3. SyncOnAddress (Futex wait/wake) round-trip ping-pong
	BenchmarkSyncOnAddressPingPong(50000);

	std::printf("\n========================================================\n");
	std::printf("Synchronization Benchmark Complete.\n");
	std::printf("========================================================\n");

	return 0;
}
