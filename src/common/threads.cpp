#include "common/threads.h"

#include "common/assert.h"

#include <algorithm>
#include <atomic>
#include <cerrno>
#include <chrono>             // IWYU pragma: keep
#include <condition_variable> // IWYU pragma: keep
#include <mutex>

#define KYTY_POSIX_HIGH_RES_SLEEP
#include <ctime>
#include <sstream>
#include <thread>

#ifdef KYTY_POSIX_HIGH_RES_SLEEP
// Spin for very short waits; use an absolute deadline for longer waits.
static void SleepHighResolutionNanos(uint64_t nanos) {
	if (nanos == 0) {
		return;
	}

	constexpr uint64_t NANOS_PER_SEC = 1000000000;
	constexpr uint64_t SPIN_LIMIT_NS = 50000; // below this a context switch dominates

	timespec deadline {};
	if (clock_gettime(CLOCK_MONOTONIC, &deadline) != 0) {
		std::this_thread::sleep_for(std::chrono::nanoseconds(nanos));
		return;
	}

	auto target_nsec = static_cast<uint64_t>(deadline.tv_nsec) + nanos;
	deadline.tv_sec += static_cast<time_t>(target_nsec / NANOS_PER_SEC);
	deadline.tv_nsec = static_cast<long>(target_nsec % NANOS_PER_SEC);

	if (nanos <= SPIN_LIMIT_NS) {
		timespec now {};
		do {
			if (clock_gettime(CLOCK_MONOTONIC, &now) != 0) {
				return;
			}
		} while (now.tv_sec < deadline.tv_sec ||
		         (now.tv_sec == deadline.tv_sec && now.tv_nsec < deadline.tv_nsec));
		return;
	}

	while (clock_nanosleep(CLOCK_MONOTONIC, TIMER_ABSTIME, &deadline, nullptr) == EINTR) {
	}
}
#endif

namespace Common {

using thread_id_t = std::thread::id;

static wait_poll_func_t g_cond_wait_poll_callback = nullptr;

struct ThreadPrivate {
	ThreadPrivate(thread_func_t f, void* a): func(f), arg(a), m_thread(&Run, this) {}

	static void Run(ThreadPrivate* t) {
		t->unique_id = Thread::GetThreadIdUnique();
		t->started   = true;
		t->func(t->arg);
	}

	thread_func_t    func;
	void*            arg;
	std::atomic_bool finished    = false;
	std::atomic_bool auto_delete = false;
	std::atomic_bool started     = false;
	int              unique_id   = 0;
	std::thread      m_thread;
};

static thread_id_t      g_main_thread;
static int              g_main_thread_int;
static std::atomic<int> g_thread_counter = 0;

void InitializeThreads() {
	g_main_thread     = std::this_thread::get_id();
	g_main_thread_int = Thread::GetThreadIdUnique();
}

Thread::Thread(thread_func_t func, void* arg)
    : m_thread(std::make_unique<ThreadPrivate>(func, arg)) {
	while (!m_thread->started) {
		Common::Thread::SleepMicro(1000);
	}
}

Thread::~Thread() {
	EXIT_IF(!m_thread->finished && !m_thread->auto_delete);

	m_thread.reset();
}

void Thread::Join() {
	EXIT_IF(m_thread->finished || m_thread->auto_delete);

	m_thread->m_thread.join();

	m_thread->finished = true;
}

void Thread::Detach() {
	EXIT_IF(m_thread->finished || m_thread->auto_delete);

	m_thread->auto_delete = true;
	m_thread->m_thread.detach();
}

void Thread::SleepMicro(uint32_t micros) {
#ifdef KYTY_WIN_CS
	SleepHighResolution100ns(static_cast<uint64_t>(micros) * 10);
#elif defined(KYTY_POSIX_HIGH_RES_SLEEP)
	SleepHighResolutionNanos(static_cast<uint64_t>(micros) * 1000);
#else
	std::this_thread::sleep_for(std::chrono::microseconds(micros));
#endif
}

void Thread::SleepNano(uint64_t nanos) {
#ifdef KYTY_WIN_CS
	SleepHighResolution100ns((nanos + 99) / 100);
#elif defined(KYTY_POSIX_HIGH_RES_SLEEP)
	SleepHighResolutionNanos(nanos);
#else
	std::this_thread::sleep_for(std::chrono::nanoseconds(nanos));
#endif
}

bool Thread::IsMainThread() {
	return g_main_thread == std::this_thread::get_id();
}

std::string Thread::GetId() const {
	std::stringstream ss;
	ss << m_thread->m_thread.get_id();
	return ss.str();
}

int Thread::GetUniqueId() const {
	return m_thread->unique_id;
}

std::string Thread::GetThreadId() {
	std::stringstream ss;
	ss << std::this_thread::get_id();
	return ss.str();
}

Mutex::Mutex() {
	pthread_mutexattr_t attr;
	pthread_mutexattr_init(&attr);
	pthread_mutexattr_settype(&attr, PTHREAD_MUTEX_RECURSIVE);
	pthread_mutex_init(&m_mutex, &attr);
	pthread_mutexattr_destroy(&attr);
}

Mutex::~Mutex() {
	pthread_mutex_destroy(&m_mutex);
}

void Mutex::Lock() {
	pthread_mutex_lock(&m_mutex);
}

void Mutex::Unlock() {
	pthread_mutex_unlock(&m_mutex);
}

bool Mutex::TryLock() {
	return pthread_mutex_trylock(&m_mutex) == 0;
}

CondVar::CondVar() {
	pthread_condattr_t attr;
	pthread_condattr_init(&attr);
	pthread_condattr_setclock(&attr, CLOCK_MONOTONIC);
	pthread_cond_init(&m_cond, &attr);
	pthread_condattr_destroy(&attr);
}

CondVar::~CondVar() {
	pthread_cond_destroy(&m_cond);
}

void CondVar::Wait(Mutex* mutex) {
	if (g_cond_wait_poll_callback == nullptr) {
		pthread_cond_wait(&m_cond, &mutex->m_mutex);
	} else {
		timespec ts {};
		clock_gettime(CLOCK_MONOTONIC, &ts);
		ts.tv_nsec += 10000000; // 10ms
		if (ts.tv_nsec >= 1000000000) {
			ts.tv_sec += 1;
			ts.tv_nsec -= 1000000000;
		}
		if (pthread_cond_timedwait(&m_cond, &mutex->m_mutex, &ts) == ETIMEDOUT) {
			if (g_cond_wait_poll_callback != nullptr) {
				pthread_mutex_unlock(&mutex->m_mutex);
				g_cond_wait_poll_callback();
				pthread_mutex_lock(&mutex->m_mutex);
			}
		}
	}
}

void CondVar::SetWaitPollCallback(wait_poll_func_t callback) {
	g_cond_wait_poll_callback = callback;
}

bool CondVar::WaitFor(Mutex* mutex, uint32_t micros) {
	timespec ts {};
	clock_gettime(CLOCK_MONOTONIC, &ts);
	const uint64_t nsec = static_cast<uint64_t>(ts.tv_nsec) + static_cast<uint64_t>(micros) * 1000ull;
	ts.tv_sec += static_cast<time_t>(nsec / 1000000000ull);
	ts.tv_nsec = static_cast<long>(nsec % 1000000000ull);

	return pthread_cond_timedwait(&m_cond, &mutex->m_mutex, &ts) == 0;
}

void CondVar::Signal() {
	pthread_cond_signal(&m_cond);
}

void CondVar::SignalAll() {
	pthread_cond_broadcast(&m_cond);
}

int Thread::GetThreadIdUnique() {
	static thread_local int tid = ++g_thread_counter;
	return tid;
}

} // namespace Common
