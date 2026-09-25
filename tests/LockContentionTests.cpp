// Contention suite for the emulator's own synchronization primitives.
//
// Demon's Souls runs 13 job workers that hammer the region-tracking spin locks and the bitmaps
// next to them. Reproducing that from the game costs a six-minute boot into gameplay; these
// scenarios isolate the same access patterns and run in about a second, so a change to lock
// layout or spin behaviour can be measured before it is ever put in front of the game.
//
// Scenarios:
//   own-lock          every thread owns a lock (ideal case, should scale with cores)
//   shared-lock       all threads contend on one lock (worst case, true sharing)
//   region-like       owner locks while other threads read adjacent data (false sharing, the
//                     RegionManager layout: lock followed by bitmaps read without the lock)
//   own-counter       per-thread 8-byte counters in one array (cache-line granularity baseline)
//   job-counters      13 threads doing fetch-add on 16 counters per line (the guest job system's
//                     own pattern, kept as a reference point for how bad sharing can get)
//
// Every scenario reports ns/op plus throughput. Run it before and after a change and compare.
//
// Recorded on a Ryzen 5 3600 (6c/12t), 12 threads, 250 ms per scenario:
//
//   scenario        lock 8 B, fence-spin     lock alignas(64), pause+yield
//   own-lock                       9.2 ns/op                  1.0 ns/op
//   region-like                   18.7 ns/op                  6.0 ns/op
//   shared-lock                  238.2 ns/op                228.5 ns/op
//   own-counter (control)          3.9 ns/op                  4.3 ns/op
//   job-counters (control)         7.5 ns/op                  7.9 ns/op
//
// The two control scenarios are unchanged, so the differences in the first two are the change and
// not machine noise. An unconditional pause was measured too and cost shared-lock 23% (292 ns/op);
// hence the adaptive backoff rather than a pause on every retry.

#include "graphics/host_gpu/regionManager.h"

#include <array>
#include <atomic>
#include <barrier>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <thread>
#include <vector>

namespace {

using Libs::Graphics::TrackingSpinLock;

constexpr size_t   kRegionLikeBitmaps = 16; // 128 B, mirrors RegionBits
constexpr uint64_t kRunMicros         = 250'000;

uint32_t ThreadCount() {
	const auto cpus = std::thread::hardware_concurrency();
	return cpus == 0 ? 4u : (cpus > 16u ? 16u : cpus);
}

// Runs `body(thread_index)` on every thread while the clock is running, returns total operations.
template <typename Body>
uint64_t RunThreads(uint32_t threads, Body&& body) {
	std::barrier            start(static_cast<ptrdiff_t>(threads));
	std::atomic_bool        stop {false};
	std::vector<uint64_t>   counts(threads, 0);
	std::vector<std::thread> workers;
	workers.reserve(threads);

	for (uint32_t t = 0; t < threads; t++) {
		workers.emplace_back([&, t] {
			uint64_t local = 0;
			start.arrive_and_wait();
			while (!stop.load(std::memory_order_relaxed)) {
				body(t);
				local++;
			}
			counts[t] = local;
		});
	}

	const auto deadline =
	    std::chrono::steady_clock::now() + std::chrono::microseconds(kRunMicros);
	while (std::chrono::steady_clock::now() < deadline) {
		std::this_thread::yield();
	}
	stop.store(true, std::memory_order_relaxed);
	for (auto& w: workers) {
		w.join();
	}
	uint64_t total = 0;
	for (auto c: counts) {
		total += c;
	}
	return total;
}

void Report(const char* name, uint64_t ops, uint32_t threads) {
	const auto ns_per_op = static_cast<double>(kRunMicros) * 1000.0 / static_cast<double>(ops);
	const auto mops      = static_cast<double>(ops) / static_cast<double>(kRunMicros);
	std::printf("  %-14s threads=%2u ops=%10llu  %8.1f ns/op  %7.2f Mops/s\n", name, threads,
	            static_cast<unsigned long long>(ops), ns_per_op, mops);
	std::printf("RESULT %s threads=%u ns_per_op=%.1f\n", name, threads, ns_per_op);
}

// The lock is meant to be used once per region. If it is narrower than a cache line, independent
// locks end up sharing one, and every acquire invalidates the line for the other owners.
void TestOwnLock() {
	const auto threads = ThreadCount();
	std::vector<TrackingSpinLock> locks(threads);
	const auto ops = RunThreads(threads, [&](uint32_t t) {
		auto& lock = locks[t];
		lock.lock();
		lock.unlock();
	});
	Report("own-lock", ops, threads);
}

void TestSharedLock() {
	const auto    threads = ThreadCount();
	TrackingSpinLock lock;
	std::atomic_uint64_t sink {0};
	const auto ops = RunThreads(threads, [&](uint32_t) {
		lock.lock();
		sink.fetch_add(1, std::memory_order_relaxed);
		lock.unlock();
	});
	Report("shared-lock", ops, threads);
}

// Mirrors one RegionManager: a lock, then the dirty/readable bitmaps that other threads read
// without holding it. Whatever width the lock has decides whether locking invalidates the
// readers' lines - which is exactly what the alignment fix is meant to change.
struct RegionLike {
	TrackingSpinLock                          lock;
	std::array<std::atomic_uint64_t, kRegionLikeBitmaps> bits {};
};

void TestRegionLike() {
	const auto threads = ThreadCount();
	std::vector<RegionLike> regions(threads);
	// Half of the threads lock their own region and set bits; the rest only read bitmaps.
	const auto readers = threads / 2;

	const auto ops = RunThreads(threads, [&](uint32_t t) {
		if (t < readers) {
			volatile uint64_t sum = 0;
			for (const auto& region: regions) {
				for (const auto& word: region.bits) {
					sum += word.load(std::memory_order_relaxed);
				}
			}
			return;
		}
		auto& region = regions[t];
		region.lock.lock();
		region.bits[t % kRegionLikeBitmaps].fetch_or(1, std::memory_order_relaxed);
		region.lock.unlock();
	});
	Report("region-like", ops, threads);
}

void TestOwnCounter() {
	const auto                threads = ThreadCount();
	std::vector<std::atomic_uint64_t> counters(threads);
	const auto ops = RunThreads(threads, [&](uint32_t t) {
		counters[t].fetch_add(1, std::memory_order_relaxed);
	});
	Report("own-counter", ops, threads);
}

// The guest's own job-system pattern: counters packed 16 per cache line, every worker
// fetch-adding them. Reference point only - this is guest code and cannot be changed here.
void TestJobCounters() {
	const auto                   threads = ThreadCount();
	std::array<std::atomic_uint32_t, 256> counters {};
	const auto ops = RunThreads(threads, [&](uint32_t t) {
		counters[t % counters.size()].fetch_add(1, std::memory_order_relaxed);
	});
	Report("job-counters", ops, threads);
}

void TestLockSemantics() {
	// The tracking lock EXITs on recursion, which the benchmark above must not be hiding.
	TrackingSpinLock lock;
	lock.lock();
	lock.unlock();
	lock.lock();
	lock.unlock();

	// Independent locks must not interfere, and the counter scenarios must not lose updates.
	std::atomic_uint64_t counter {0};
	std::thread          a([&] {
        for (int i = 0; i < 1000; i++) { counter.fetch_add(1, std::memory_order_relaxed); }
    });
	std::thread b([&] {
		for (int i = 0; i < 1000; i++) {
			counter.fetch_add(1, std::memory_order_relaxed);
		}
	});
	a.join();
	b.join();
	if (counter.load() != 2000) {
		std::printf("LockContentionTests: counter mismatch\n");
		std::abort();
	}
}

} // namespace

int main() {
	const auto threads = ThreadCount();
	std::printf("LockContentionTests: %u threads\n", threads);
	std::printf("  sizeof(TrackingSpinLock)=%zu alignof=%zu  (cache line 64 B)\n",
	            sizeof(TrackingSpinLock), alignof(TrackingSpinLock));
	std::printf("  region-like layout: lock at +0, bitmaps at +%zu, %zu B per instance\n",
	            offsetof(RegionLike, bits), sizeof(RegionLike));
	std::printf("  lock occupies its own cache line: %s\n",
	            sizeof(TrackingSpinLock) >= 64 ? "yes" : "NO (shares with neighbours)");

	TestLockSemantics();
	TestOwnLock();
	TestSharedLock();
	TestRegionLike();
	TestOwnCounter();
	TestJobCounters();

	std::printf("LockContentionTests: done\n");
	return 0;
}
