#include <SDL3/SDL.h>
#include <SDL3/SDL_main.h>

#include "common/emulatorConfig.h"
#include "common/logging/log.h"
#include "common/singleton.h"
#include "common/subsystems.h"
#include "kernel/eventQueue.h"
#include "kernel/memory.h"
#include "kernel/pthread.h"
#include "kernel/semaphore.h"
#include "kernel/syncOnAddress.h"
#include "libs/errno.h"
#include "loader/runtimeLinker.h"

#include <atomic>
#include <malloc.h>
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

// -----------------------------------------------------------------------------
// Benchmark 4: KernelSema Uncontended Signal/Wait & Poll Latency
// -----------------------------------------------------------------------------
void BenchmarkUncontendedSemaphore(uint64_t iterations) {
	std::printf("\n========================================================\n");
	std::printf("Benchmark 4: Uncontended KernelSema Latency (%lu ops)\n", iterations);
	std::printf("========================================================\n");

	Libs::LibKernel::Semaphore::KernelSema sem = nullptr;
	int err = Libs::LibKernel::Semaphore::KernelCreateSema(&sem, "uncontended_sema", 0, 0, 10000000,
	                                                       nullptr);
	if (err != 0 || sem == nullptr) {
		std::fprintf(stderr, "Failed to create semaphore: %d\n", err);
		return;
	}

	// Part A: Signal + Wait
	const auto cs_before = ReadContextSwitches();
	const auto start     = Clock::now();

	for (uint64_t i = 0; i < iterations; i++) {
		Libs::LibKernel::Semaphore::KernelSignalSema(sem, 1);
		Libs::LibKernel::Semaphore::KernelWaitSema(sem, 1, nullptr);
	}

	const auto end       = Clock::now();
	const auto cs_after  = ReadContextSwitches();

	const double total_ms =
	    std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(end - start).count();
	const double ns_per_op = (total_ms * 1e6) / static_cast<double>(iterations);
	const double mops_sec  = static_cast<double>(iterations) / (total_ms * 1e3);

	std::printf("  [Signal + Wait]\n");
	std::printf("  Total Time:          %.3f ms\n", total_ms);
	std::printf("  Latency per Pair:    %.1f ns\n", ns_per_op);
	std::printf("  Throughput:          %.2f M ops/sec\n", mops_sec);
	std::printf("  Voluntary Context Switches:    %" PRIu64 "\n",
	            cs_after.voluntary - cs_before.voluntary);
	std::printf("  Non-Voluntary Context Switches: %" PRIu64 "\n",
	            cs_after.nonvoluntary - cs_before.nonvoluntary);

	// Part B: Signal + Poll
	const auto poll_cs_before = ReadContextSwitches();
	const auto poll_start     = Clock::now();

	for (uint64_t i = 0; i < iterations; i++) {
		Libs::LibKernel::Semaphore::KernelSignalSema(sem, 1);
		Libs::LibKernel::Semaphore::KernelPollSema(sem, 1);
	}

	const auto poll_end      = Clock::now();
	const auto poll_cs_after = ReadContextSwitches();

	const double poll_total_ms =
	    std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(poll_end - poll_start)
	        .count();
	const double poll_ns_per_op = (poll_total_ms * 1e6) / static_cast<double>(iterations);
	const double poll_mops_sec  = static_cast<double>(iterations) / (poll_total_ms * 1e3);

	std::printf("  [Signal + Poll]\n");
	std::printf("  Total Time:          %.3f ms\n", poll_total_ms);
	std::printf("  Latency per Pair:    %.1f ns\n", poll_ns_per_op);
	std::printf("  Throughput:          %.2f M ops/sec\n", poll_mops_sec);
	std::printf("  Voluntary Context Switches:    %" PRIu64 "\n",
	            poll_cs_after.voluntary - poll_cs_before.voluntary);
	std::printf("  Non-Voluntary Context Switches: %" PRIu64 "\n",
	            poll_cs_after.nonvoluntary - poll_cs_before.nonvoluntary);

	Libs::LibKernel::Semaphore::KernelDeleteSema(sem);
}

// -----------------------------------------------------------------------------
// Benchmark 5: Contended Semaphore Worker Pool (Demon's Souls job scheduler pattern)
// -----------------------------------------------------------------------------
struct SemaWorkerArg {
	Libs::LibKernel::Semaphore::KernelSema sem             = nullptr;
	std::atomic<bool>*                     stop_flag       = nullptr;
	std::atomic<uint64_t>*                 tasks_completed = nullptr;
};

static KYTY_SYSV_ABI void* SemaWorkerFunc(void* param) {
	auto* arg = static_cast<SemaWorkerArg*>(param);

	while (true) {
		int res = Libs::LibKernel::Semaphore::KernelWaitSema(arg->sem, 1, nullptr);
		if (res != 0) {
			break;
		}
		if (arg->stop_flag->load(std::memory_order_relaxed)) {
			break;
		}
		arg->tasks_completed->fetch_add(1, std::memory_order_relaxed);
	}

	return nullptr;
}

void BenchmarkContendedSemaphorePool(int num_workers, uint64_t total_tasks) {
	std::printf("\n========================================================\n");
	std::printf("Benchmark 5: Contended KernelSema Pool (%d Workers, %lu tasks)\n", num_workers,
	            total_tasks);
	std::printf("========================================================\n");

	Libs::LibKernel::Semaphore::KernelSema sem = nullptr;
	Libs::LibKernel::Semaphore::KernelCreateSema(&sem, "pool_sem", 0, 0, 10000000, nullptr);

	std::atomic<bool>     stop_flag{false};
	std::atomic<uint64_t> tasks_completed{0};

	std::vector<SemaWorkerArg>            args(num_workers);
	std::vector<Libs::LibKernel::Pthread> workers(num_workers);

	for (int i = 0; i < num_workers; i++) {
		args[i].sem             = sem;
		args[i].stop_flag       = &stop_flag;
		args[i].tasks_completed = &tasks_completed;
		Libs::LibKernel::PthreadCreate(&workers[i], nullptr, SemaWorkerFunc, &args[i],
		                               "sema_worker");
	}

	// Warmup delay
	std::this_thread::sleep_for(std::chrono::milliseconds(20));
	const auto cs_before = ReadContextSwitches();
	const auto start     = Clock::now();

	for (uint64_t i = 0; i < total_tasks; i++) {
		Libs::LibKernel::Semaphore::KernelSignalSema(sem, 1);
	}

	while (tasks_completed.load(std::memory_order_acquire) < total_tasks) {
		std::this_thread::yield();
	}

	const auto end      = Clock::now();
	const auto cs_after = ReadContextSwitches();

	stop_flag.store(true, std::memory_order_release);
	Libs::LibKernel::Semaphore::KernelSignalSema(sem, num_workers);

	for (int i = 0; i < num_workers; i++) {
		Libs::LibKernel::PthreadJoin(workers[i], nullptr);
	}

	const double total_ms =
	    std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(end - start).count();
	const double ns_per_task   = (total_ms * 1e6) / static_cast<double>(total_tasks);
	const double tasks_per_sec = static_cast<double>(total_tasks) / (total_ms * 1e-3);

	std::printf("  Total Time:          %.3f ms\n", total_ms);
	std::printf("  Latency per Task:    %.1f ns\n", ns_per_task);
	std::printf("  Throughput:          %.2f M tasks/sec\n", tasks_per_sec / 1e6);
	std::printf("  Voluntary Context Switches:    %" PRIu64 "\n",
	            cs_after.voluntary - cs_before.voluntary);
	std::printf("  Non-Voluntary Context Switches: %" PRIu64 "\n",
	            cs_after.nonvoluntary - cs_before.nonvoluntary);

	Libs::LibKernel::Semaphore::KernelDeleteSema(sem);
}

// -----------------------------------------------------------------------------
// Benchmark 6: KernelEqueue User Event Round-Trip Ping-Pong
// -----------------------------------------------------------------------------
struct EqPingPongData {
	Libs::LibKernel::EventQueue::KernelEqueue eq_a;
	Libs::LibKernel::EventQueue::KernelEqueue eq_b;
	uint64_t                                  iterations = 0;
	std::atomic<bool>                         ready      = false;
};

static KYTY_SYSV_ABI void* EqPingPongWorker(void* param) {
	auto* data = static_cast<EqPingPongData*>(param);
	data->ready.store(true, std::memory_order_release);

	Libs::LibKernel::EventQueue::KernelEvent ev {};
	int                                      out = 0;

	for (uint64_t i = 0; i < data->iterations; i++) {
		Libs::LibKernel::EventQueue::KernelWaitEqueue(data->eq_a, &ev, 1, &out, nullptr);
		Libs::LibKernel::EventQueue::KernelTriggerUserEvent(data->eq_b, 1, nullptr);
	}

	return nullptr;
}

void BenchmarkEventQueuePingPong(uint64_t roundtrips) {
	std::printf("\n========================================================\n");
	std::printf("Benchmark 6: KernelEqueue Ping-Pong Round-Trip (%lu hops)\n", roundtrips);
	std::printf("========================================================\n");

	Libs::LibKernel::EventQueue::KernelEqueue eq_a = 0;
	Libs::LibKernel::EventQueue::KernelEqueue eq_b = 0;

	Libs::LibKernel::EventQueue::KernelCreateEqueue(&eq_a, "bench_eq_a");
	Libs::LibKernel::EventQueue::KernelCreateEqueue(&eq_b, "bench_eq_b");

	Libs::LibKernel::EventQueue::KernelAddUserEvent(eq_a, 1);
	Libs::LibKernel::EventQueue::KernelAddUserEvent(eq_b, 1);

	EqPingPongData data {};
	data.eq_a       = eq_a;
	data.eq_b       = eq_b;
	data.iterations = roundtrips;

	Libs::LibKernel::Pthread worker {};
	Libs::LibKernel::PthreadCreate(&worker, nullptr, EqPingPongWorker, &data, "eq_worker");

	while (!data.ready.load(std::memory_order_acquire)) {
		std::this_thread::yield();
	}

	const auto cs_before = ReadContextSwitches();
	const auto start     = Clock::now();

	Libs::LibKernel::EventQueue::KernelEvent ev {};
	int                                      out = 0;

	for (uint64_t i = 0; i < roundtrips; i++) {
		Libs::LibKernel::EventQueue::KernelTriggerUserEvent(eq_a, 1, nullptr);
		Libs::LibKernel::EventQueue::KernelWaitEqueue(eq_b, &ev, 1, &out, nullptr);
	}

	const auto end      = Clock::now();
	const auto cs_after = ReadContextSwitches();

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

	Libs::LibKernel::EventQueue::KernelDeleteEqueue(eq_a);
	Libs::LibKernel::EventQueue::KernelDeleteEqueue(eq_b);
}

} // namespace


// ---------------------------------------------------------------------------
// Guest memory bookkeeping
// ---------------------------------------------------------------------------
// Every mmap/munmap/VirtualQuery the guest performs mutates sorted-range vectors (insert/erase in
// the middle) and builds temporary range vectors. These benchmarks price that path directly.
// Values below are guest ABI constants, same as the VM allocation suite.
namespace {

constexpr int      SceKernelProtCpuRead = 0x01;
constexpr int      SceKernelProtCpuRw   = 0x02;
constexpr int      SceKernelMapFixed    = 0x10;
constexpr int      SceKernelMtypeC      = 11;
constexpr uint64_t SceKernelPageSize  = 0x4000;

size_t RetainedHeapBytes() {
	struct mallinfo2 info = ::mallinfo2();
	return info.uordblks;
}

void PrintMemoryFootprint(size_t heap_before, size_t heap_after, uint64_t failures) {
	const auto delta_kib =
	    (static_cast<double>(heap_after) - static_cast<double>(heap_before)) / 1024.0;
	std::printf("  Retained heap delta: %+.1f KiB\n", delta_kib);
	if (failures != 0) {
		std::printf("  Failures:            %" PRIu64 "\n", failures);
	}
}

// reserve -> query -> munmap, the shape of a single guest allocation.
void BenchmarkGuestRangeChurn(uint64_t iterations) {
	std::printf("\n========================================================\n");
	std::printf("Benchmark 7: Guest Range Churn (reserve+query+munmap, %lu ops)\n", iterations);
	std::printf("========================================================\n");

	uint64_t ops      = 0;
	uint64_t failures = 0;
	const auto heap_before = RetainedHeapBytes();
	const auto start       = Clock::now();

	for (uint64_t i = 0; i < iterations; i++) {
		void* addr = nullptr;
		if (Libs::LibKernel::Memory::KernelReserveVirtualRange(
		        &addr, SceKernelPageSize, 0, SceKernelPageSize) != 0 ||
		    addr == nullptr) {
			failures++;
			continue;
		}
		Libs::LibKernel::Memory::VirtualQueryInfo info {};
		if (Libs::LibKernel::Memory::KernelVirtualQuery(addr, 0, &info, sizeof(info)) != 0) {
			failures++;
		}
		if (Libs::LibKernel::Memory::KernelMunmap(reinterpret_cast<uint64_t>(addr),
		                                          SceKernelPageSize) != 0) {
			failures++;
		}
		ops++;
	}

	const auto total_ms = std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(
	                          Clock::now() - start)
	                          .count();
	if (ops == 0) {
		std::printf("  no successful operations\n");
		return;
	}
	std::printf("  Total Time:          %.3f ms\n", total_ms);
	std::printf("  Latency per Pair:    %.1f ns\n", total_ms * 1e6 / static_cast<double>(ops));
	std::printf("  Throughput:          %.2f K ops/sec\n",
	            static_cast<double>(ops) / total_ms * 1000.0);
	PrintMemoryFootprint(heap_before, RetainedHeapBytes(), failures);
}

// Allocate many ranges, free every other one, then refill: forces overlap splitting and merging.
void BenchmarkGuestFragmentation(uint32_t count) {
	constexpr uint64_t kSize = 64 * 1024;

	std::printf("\n========================================================\n");
	std::printf("Benchmark 8: Guest Fragmentation (%u x %" PRIu64 " KiB ranges)\n", count,
	            kSize / 1024);
	std::printf("========================================================\n");

	std::vector<uint64_t> kept;
	kept.reserve(count);
	uint64_t allocated = 0;
	uint64_t freed     = 0;
	uint64_t refilled  = 0;
	uint64_t failures  = 0;

	const auto heap_before = RetainedHeapBytes();
	const auto start       = Clock::now();

	for (uint32_t i = 0; i < count; i++) {
		void* addr = nullptr;
		if (Libs::LibKernel::Memory::KernelReserveVirtualRange(&addr, kSize, 0, kSize) != 0 ||
		    addr == nullptr) {
			failures++;
			continue;
		}
		kept.push_back(reinterpret_cast<uint64_t>(addr));
		allocated++;
	}

	for (size_t i = 0; i + 1 < kept.size(); i += 2) {
		if (Libs::LibKernel::Memory::KernelMunmap(kept[i], kSize) == 0) {
			freed++;
		} else {
			failures++;
		}
		kept[i] = 0;
	}

	for (uint32_t i = 0; i < count; i++) {
		void* addr = nullptr;
		if (Libs::LibKernel::Memory::KernelReserveVirtualRange(&addr, kSize, 0, kSize) != 0 ||
		    addr == nullptr) {
			failures++;
			continue;
		}
		refilled++;
		Libs::LibKernel::Memory::KernelMunmap(reinterpret_cast<uint64_t>(addr), kSize);
	}

	for (auto addr: kept) {
		if (addr != 0) {
			Libs::LibKernel::Memory::KernelMunmap(addr, kSize);
		}
	}

	const auto total_ms = std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(
	                          Clock::now() - start)
	                          .count();
	const auto ops = allocated + freed + refilled;
	std::printf("  Total Time:          %.3f ms\n", total_ms);
	std::printf("  Operations:          %" PRIu64 " (alloc %" PRIu64 ", free %" PRIu64
	            ", refill %" PRIu64 ")\n",
	            ops, allocated, freed, refilled);
	if (ops != 0) {
		std::printf("  Latency per Op:      %.1f ns\n", total_ms * 1e6 / static_cast<double>(ops));
	}
	PrintMemoryFootprint(heap_before, RetainedHeapBytes(), failures);
}

// VirtualQuery over an existing mapping: reads the sorted range vector and builds a result.
void BenchmarkGuestQueryStorm(uint64_t iterations) {
	constexpr uint64_t kSize = 16 * 1024 * 1024;

	std::printf("\n========================================================\n");
	std::printf("Benchmark 9: Guest VirtualQuery Storm (%lu queries)\n", iterations);
	std::printf("========================================================\n");

	void* base_raw = nullptr;
	if (Libs::LibKernel::Memory::KernelReserveVirtualRange(&base_raw, kSize, 0, 1u << 20u) != 0 ||
	    base_raw == nullptr) {
		std::printf("  reservation failed\n");
		return;
	}
	const auto base = reinterpret_cast<uint64_t>(base_raw);

	uint64_t failures      = 0;
	const auto heap_before = RetainedHeapBytes();
	const auto start       = Clock::now();

	for (uint64_t i = 0; i < iterations; i++) {
		Libs::LibKernel::Memory::VirtualQueryInfo info {};
		const auto address = reinterpret_cast<void*>(base + (i % 64u) * SceKernelPageSize);
		if (Libs::LibKernel::Memory::KernelVirtualQuery(address, 0, &info, sizeof(info)) != 0) {
			failures++;
		}
	}

	const auto total_ms = std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(
	                          Clock::now() - start)
	                          .count();
	std::printf("  Total Time:          %.3f ms\n", total_ms);
	std::printf("  Latency per Query:   %.1f ns\n",
	            total_ms * 1e6 / static_cast<double>(iterations));
	std::printf("  Throughput:          %.2f K queries/sec\n",
	            static_cast<double>(iterations) / total_ms * 1000.0);
	PrintMemoryFootprint(heap_before, RetainedHeapBytes(), failures);

	Libs::LibKernel::Memory::KernelMunmap(base, kSize);
}

// Physical direct memory: allocate/release pairs drive the pool's free map and block list.
void BenchmarkDirectMemoryChurn(uint64_t iterations) {
	constexpr uint64_t kChunk = 64 * 1024;

	std::printf("\n========================================================\n");
	std::printf("Benchmark 10: Guest Direct Memory Churn (allocate+release, %lu ops)\n",
	            iterations);
	std::printf("========================================================\n");

	uint64_t ops           = 0;
	uint64_t failures      = 0;
	const auto heap_before = RetainedHeapBytes();
	const auto start       = Clock::now();

	for (uint64_t i = 0; i < iterations; i++) {
		int64_t physical = -1;
		if (Libs::LibKernel::Memory::KernelAllocateDirectMemory(
		        0, static_cast<int64_t>(Libs::LibKernel::Memory::KernelGetDirectMemorySize()),
		        kChunk, kChunk, SceKernelMtypeC, &physical) != 0) {
			failures++;
			continue;
		}
		if (Libs::LibKernel::Memory::KernelReleaseDirectMemory(physical, kChunk) != 0) {
			failures++;
		}
		ops++;
	}

	const auto total_ms = std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(
	                          Clock::now() - start)
	                          .count();
	if (ops == 0) {
		std::printf("  no successful operations\n");
		return;
	}
	std::printf("  Total Time:          %.3f ms\n", total_ms);
	std::printf("  Latency per Pair:    %.1f ns\n", total_ms * 1e6 / static_cast<double>(ops));
	std::printf("  Throughput:          %.2f K ops/sec\n",
	            static_cast<double>(ops) / total_ms * 1000.0);
	PrintMemoryFootprint(heap_before, RetainedHeapBytes(), failures);
}


// Protection changes walk the mapping list and rebuild it. This measures that cost as the number
// of live mappings grows, which is what a streaming guest produces.
void BenchmarkMappingProtectionScaling(uint64_t iterations) {
	constexpr uint64_t kChunk = 64 * 1024;

	std::printf("\n========================================================\n");
	std::printf("Benchmark 11: Mapping Protection Scaling (%lu protects per size)\n", iterations);
	std::printf("========================================================\n");

	for (uint32_t mappings: {64u, 256u, 1024u}) {
		const uint64_t arena_size = kChunk * mappings;
		void*          arena_raw  = nullptr;
		if (Libs::LibKernel::Memory::KernelReserveVirtualRange(&arena_raw, arena_size, 0, 1u << 20u) !=
		        0 ||
		    arena_raw == nullptr) {
			std::printf("  mappings=%-5u reservation failed\n", mappings);
			continue;
		}
		const auto arena = reinterpret_cast<uint64_t>(arena_raw);

		std::vector<int64_t> physical;
		physical.reserve(mappings);
		uint32_t mapped   = 0;
		uint32_t failures = 0;

		for (uint32_t i = 0; i < mappings; i++) {
			int64_t phys = -1;
			if (Libs::LibKernel::Memory::KernelAllocateDirectMemory(
			        0, static_cast<int64_t>(Libs::LibKernel::Memory::KernelGetDirectMemorySize()),
			        kChunk, kChunk, SceKernelMtypeC, &phys) != 0) {
				failures++;
				continue;
			}
			void* at = reinterpret_cast<void*>(arena + i * kChunk);
			if (Libs::LibKernel::Memory::KernelMapDirectMemory(&at, kChunk, SceKernelProtCpuRw,
			                                                   SceKernelMapFixed, phys,
			                                                   kChunk) != 0) {
				Libs::LibKernel::Memory::KernelReleaseDirectMemory(phys, kChunk);
				failures++;
				continue;
			}
			physical.push_back(phys);
			mapped++;
		}

		if (mapped == 0) {
			std::printf("  mappings=%-5u no mappings established\n", mappings);
			Libs::LibKernel::Memory::KernelMunmap(arena, arena_size);
			continue;
		}

		const auto heap_before = RetainedHeapBytes();
		const auto start       = Clock::now();
		for (uint64_t i = 0; i < iterations; i++) {
			const auto index = static_cast<uint32_t>(i % mapped);
			const auto prot  = (i & 1u) != 0 ? SceKernelProtCpuRead : SceKernelProtCpuRw;
			if (Libs::LibKernel::Memory::KernelMprotect(
			        reinterpret_cast<const void*>(arena + index * kChunk), kChunk, prot) != 0) {
				failures++;
			}
		}
		const auto total_ms =
		    std::chrono::duration_cast<std::chrono::duration<double, std::milli>>(Clock::now() -
		                                                                          start)
		        .count();

		std::printf("  mappings=%-5u protects=%-8lu  %8.1f ns/protect   %6.3f ms total\n", mapped,
		            static_cast<unsigned long>(iterations),
		            total_ms * 1e6 / static_cast<double>(iterations), total_ms);
		PrintMemoryFootprint(heap_before, RetainedHeapBytes(), failures);

		for (uint32_t i = 0; i < mapped; i++) {
			Libs::LibKernel::Memory::KernelMunmap(arena + i * kChunk, kChunk);
		}
		Libs::LibKernel::Memory::KernelMunmap(arena, arena_size);
		for (auto phys: physical) {
			Libs::LibKernel::Memory::KernelReleaseDirectMemory(phys, kChunk);
		}
	}
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

	// 4. KernelSema uncontended Signal/Wait & Poll latency
	BenchmarkUncontendedSemaphore(2000000);

	// 5. Contended KernelSema worker pool (job system pattern)
	BenchmarkContendedSemaphorePool(4, 200000);
	BenchmarkContendedSemaphorePool(8, 200000);

	// 6. KernelEqueue user event round-trip ping-pong
	BenchmarkEventQueuePingPong(50000);

	// 7-10. Guest memory bookkeeping path
	BenchmarkGuestRangeChurn(20000);
	BenchmarkGuestFragmentation(256);
	BenchmarkGuestQueryStorm(200000);
	BenchmarkDirectMemoryChurn(20000);
	BenchmarkMappingProtectionScaling(20000);

	std::printf("\n========================================================\n");
	std::printf("Synchronization Benchmark Complete.\n");
	std::printf("========================================================\n");

	return 0;
}
