// Benchmarks for the emulator's own synchronization primitives.
//
// Every guest thread that blocks - on a guest mutex, condvar, semaphore, or while waiting for a
// signal - funnels through Common::Mutex / Common::CondVar. The per-operation cost is small, but
// it is paid on every handoff, so a fixed penalty here multiplies across a 13-thread job system.
//
// Suspects:
//   condvar round trip    wait/signal ping-pong latency (plain and with the poll hook installed)
//   poll notice latency   with KernelDispatchPendingSignalForCurrentThread installed as the poll
//                         hook, every Wait() becomes a 10 ms pthread_cond_timedwait loop, so any
//                         state change that is noticed only by the timeout costs up to 10 ms
//   idle poll rate        what a blocked-but-idle thread costs with that hook installed
//   mutex                 Common::Mutex is PTHREAD_MUTEX_RECURSIVE, uncontended and contended
//   sleep                 Common::Thread::SleepMicro spins under SPIN_LIMIT_NS (50 us), then sleeps
//
// Recorded on a Ryzen 5 3600 (6c/12t). Run before and after a change and compare.

#include "common/threads.h"
#include "common/timer.h"

#include <atomic>
#include <chrono>
#include <cstdint>
#include <cstdio>
#include <pthread.h>
#include <thread>
#include <vector>

namespace {

using Clock = std::chrono::steady_clock;

double Micros(Clock::duration d) {
	return std::chrono::duration<double, std::micro>(d).count();
}

void Report(const char* name, double value, const char* unit) {
	std::printf("  %-24s %12.3f %s\n", name, value, unit);
	std::printf("RESULT %s %s=%.3f\n", name, unit, value);
}

Common::Mutex   g_mutex;
Common::CondVar g_to_waiter;
Common::CondVar g_to_main;
bool            g_ping = false;
bool            g_pong = false;

// One full main -> waiter -> main round trip per iteration; a hop is half of that.
double MeasureRoundTrip(int iterations) {
	g_ping = false;
	g_pong = false;

	std::thread waiter([&] {
		for (int i = 0; i < iterations; i++) {
			g_mutex.Lock();
			while (!g_ping) {
				g_to_waiter.Wait(&g_mutex);
			}
			g_ping = false;
			g_pong = true;
			g_to_main.Signal();
			g_mutex.Unlock();
		}
	});

	std::this_thread::sleep_for(std::chrono::milliseconds(20));
	const auto begin = Clock::now();
	for (int i = 0; i < iterations; i++) {
		g_mutex.Lock();
		g_ping = true;
		g_to_waiter.Signal();
		while (!g_pong) {
			g_to_main.Wait(&g_mutex);
		}
		g_pong = false;
		g_mutex.Unlock();
	}
	const auto elapsed = Clock::now() - begin;
	waiter.join();
	return Micros(elapsed) / static_cast<double>(iterations);
}

// How long a state change can go unnoticed by a blocked waiter when nobody signals it: with the
// poll hook installed the only chance the waiter gets is its 10 ms timeout expiring. This is the
// granularity that guest signal delivery inherits.
std::atomic<uint64_t> g_poll_calls {0};

void PollingCallback() {
	g_poll_calls.fetch_add(1, std::memory_order_relaxed);
}

double MeasurePollInterval(int iterations) {
	Common::CondVar::SetWaitPollCallback(PollingCallback);

	Common::Mutex    mutex;
	Common::CondVar  cond;
	std::atomic_bool stop {false};

	std::thread waiter([&] {
		mutex.Lock();
		while (!stop.load(std::memory_order_relaxed)) {
			cond.Wait(&mutex);
		}
		mutex.Unlock();
	});

	std::this_thread::sleep_for(std::chrono::milliseconds(50));
	const auto before = g_poll_calls.load(std::memory_order_relaxed);
	const auto begin  = Clock::now();
	while (g_poll_calls.load(std::memory_order_relaxed) < before + static_cast<uint64_t>(iterations)) {
		std::this_thread::sleep_for(std::chrono::microseconds(100));
	}
	const auto per = Micros(Clock::now() - begin) / static_cast<double>(iterations);

	mutex.Lock();
	stop.store(true, std::memory_order_relaxed);
	cond.SignalAll();
	mutex.Unlock();
	waiter.join();
	Common::CondVar::SetWaitPollCallback(nullptr);
	return per;
}

// Callbacks per second fired by blocked-but-idle threads while the hook is installed.
double MeasureIdlePollFireRate(uint32_t threads, double seconds) {
	Common::CondVar::SetWaitPollCallback(PollingCallback);
	g_poll_calls.store(0, std::memory_order_relaxed);

	Common::Mutex    mutex;
	Common::CondVar  cond;
	std::atomic_bool stop {false};
	std::vector<std::thread> waiters;

	for (uint32_t t = 0; t < threads; t++) {
		waiters.emplace_back([&] {
			mutex.Lock();
			while (!stop.load(std::memory_order_relaxed)) {
				cond.Wait(&mutex);
			}
			mutex.Unlock();
		});
	}

	std::this_thread::sleep_for(
	    std::chrono::milliseconds(static_cast<int>(seconds * 1000.0)));

	mutex.Lock();
	stop.store(true, std::memory_order_relaxed);
	cond.SignalAll();
	mutex.Unlock();
	for (auto& w: waiters) {
		w.join();
	}
	Common::CondVar::SetWaitPollCallback(nullptr);
	return static_cast<double>(g_poll_calls.load(std::memory_order_relaxed)) / seconds;
}

double MeasureMutexNanos(int iterations, uint32_t threads) {
	Common::Mutex    shared;
	std::atomic_bool go {false};
	std::vector<std::thread> workers;

	for (uint32_t t = 0; t < threads; t++) {
		workers.emplace_back([&, t] {
			Common::Mutex own;
			auto&        target = threads == 1 ? own : shared;
			while (!go.load(std::memory_order_acquire)) {
				std::this_thread::yield();
			}
			for (int i = 0; i < iterations / static_cast<int>(threads); i++) {
				target.Lock();
				target.Unlock();
			}
		});
	}
	const auto begin = Clock::now();
	go.store(true, std::memory_order_release);
	for (auto& w: workers) {
		w.join();
	}
	return static_cast<double>(Micros(Clock::now() - begin) * 1000.0) /
	       static_cast<double>(iterations);
}

// Plain (non-recursive) pthread mutex, so Common::Mutex's PTHREAD_MUTEX_RECURSIVE choice can be
// priced against the alternative rather than argued about.
double MeasurePlainMutexNanos(int iterations, uint32_t threads) {
	pthread_mutex_t  shared;
	pthread_mutex_init(&shared, nullptr);
	std::atomic_bool go {false};
	std::vector<std::thread> workers;

	for (uint32_t t = 0; t < threads; t++) {
		workers.emplace_back([&, t] {
			pthread_mutex_t own;
			pthread_mutex_init(&own, nullptr);
			auto* target = threads == 1 ? &own : &shared;
			while (!go.load(std::memory_order_acquire)) {
				std::this_thread::yield();
			}
			for (int i = 0; i < iterations / static_cast<int>(threads); i++) {
				pthread_mutex_lock(target);
				pthread_mutex_unlock(target);
			}
			if (threads == 1) {
				pthread_mutex_destroy(&own);
			}
		});
	}
	const auto begin = Clock::now();
	go.store(true, std::memory_order_release);
	for (auto& w: workers) {
		w.join();
	}
	pthread_mutex_destroy(&shared);
	return static_cast<double>(Micros(Clock::now() - begin) * 1000.0) /
	       static_cast<double>(iterations);
}

void MeasureSleep(uint32_t micros, int iterations) {
	const auto begin = Clock::now();
	for (int i = 0; i < iterations; i++) {
		Common::Thread::SleepMicro(micros);
	}
	const auto per_call = Micros(Clock::now() - begin) / static_cast<double>(iterations);
	char name[64];
	std::snprintf(name, sizeof(name), "sleep-%u-us", micros);
	Report(name, per_call, "us/call");
}

} // namespace

int main() {
	const auto threads = std::thread::hardware_concurrency();
	std::printf("SyncPrimitiveTests: %u host threads\n", threads);

	const auto roundtrip = MeasureRoundTrip(20000);
	Report("condvar-hop", roundtrip / 2.0, "us");

	Common::CondVar::SetWaitPollCallback(PollingCallback);
	const auto roundtrip_hook = MeasureRoundTrip(20000);
	Common::CondVar::SetWaitPollCallback(nullptr);
	Report("condvar-hop-pollhook", roundtrip_hook / 2.0, "us");

	Report("poll-interval", MeasurePollInterval(20), "us");

	const auto idle_threads = threads > 12 ? 12u : threads;
	Report("idle-poll-fires-12t", MeasureIdlePollFireRate(idle_threads, 2.0), "/s");

	Report("mutex-recursive-1t", MeasureMutexNanos(500000, 1), "ns");
	Report("mutex-plain-1t", MeasurePlainMutexNanos(500000, 1), "ns");
	Report("mutex-recursive-4t", MeasureMutexNanos(500000, 4), "ns");
	Report("mutex-plain-4t", MeasurePlainMutexNanos(500000, 4), "ns");

	MeasureSleep(1, 20000);
	MeasureSleep(20, 20000);
	MeasureSleep(200, 2000);
	MeasureSleep(2000, 200);

	std::printf("SyncPrimitiveTests: done\n");
	return 0;
}
