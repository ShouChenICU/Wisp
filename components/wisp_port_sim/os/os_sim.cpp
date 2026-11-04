// PC simulator implementation of wisp::os (components/wisp_os/include/wisp/os/os.hpp),
// built on <thread>/<mutex>/<condition_variable>/<chrono>. This file links
// against the host's pthreads via CMake's Threads::Threads; the public
// header it implements stays free of these platform types.

#include <atomic>
#include <chrono>
#include <condition_variable>
#include <cstdlib>
#include <mutex>
#include <thread>

#include "wisp/os/os.hpp"
#include "wisp/port/sim/sim_os.hpp"

namespace wisp::os {

namespace {

// set_alloc_failure() flags, indexed by Mem; DmaLarge is the last kind.
std::atomic<bool> g_fail[static_cast<size_t>(Mem::DmaLarge) + 1] = {};

bool failing(Mem kind) { return g_fail[static_cast<int>(kind)].load(std::memory_order_relaxed); }

// The simulator's "boot" is process start. A function-local static is
// safe to reach from other translation units' static initializers; the
// namespace-scope read below pins it to static-init time even if nothing
// calls now_ms() until much later.
std::chrono::steady_clock::time_point boot_epoch() {
    static const std::chrono::steady_clock::time_point t0 = std::chrono::steady_clock::now();
    return t0;
}

[[maybe_unused]] const std::chrono::steady_clock::time_point g_boot_epoch_at_startup = boot_epoch();

}  // namespace

uint64_t now_ms() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<milliseconds>(steady_clock::now() - boot_epoch()).count());
}

uint64_t now_us() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<microseconds>(steady_clock::now() - boot_epoch()).count());
}

void sleep_ms(uint32_t ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

void* alloc(size_t bytes, Mem kind) {
    // The simulator has no Fast/Dma/PSRAM distinction; every kind maps to
    // the regular heap.
    return failing(kind) ? nullptr : std::malloc(bytes);
}

void dealloc(void* p) { std::free(p); }

void* realloc(void* p, size_t bytes, Mem kind) {
    // std::realloc(p, 0) is implementation-defined (and deprecated in
    // C23); spell out the contract from os.hpp instead.
    if (bytes == 0) {
        std::free(p);
        return nullptr;
    }
    return failing(kind) ? nullptr : std::realloc(p, bytes);
}

}  // namespace wisp::os

namespace wisp::port::sim {

void set_alloc_failure(os::Mem kind, bool fail) {
    os::g_fail[static_cast<int>(kind)].store(fail, std::memory_order_relaxed);
}

}  // namespace wisp::port::sim

namespace wisp::os {

// ---------------------------------------------------------------------------
// Mutex
// ---------------------------------------------------------------------------

struct Mutex::Impl {
    std::mutex m;
};

Mutex::Mutex() : impl_(std::make_unique<Impl>()) {}
Mutex::~Mutex() = default;

void Mutex::lock() { impl_->m.lock(); }
void Mutex::unlock() { impl_->m.unlock(); }
bool Mutex::try_lock() { return impl_->m.try_lock(); }

// ---------------------------------------------------------------------------
// Semaphore
// ---------------------------------------------------------------------------

struct Semaphore::Impl {
    std::mutex mtx;
    std::condition_variable cv;
    uint32_t count;
    uint32_t max_count;
};

Semaphore::Semaphore(uint32_t max_count, uint32_t initial) : impl_(std::make_unique<Impl>()) {
    impl_->max_count = max_count;
    impl_->count = initial > max_count ? max_count : initial;
}

Semaphore::~Semaphore() = default;

bool Semaphore::take(uint32_t timeout_ms) {
    std::unique_lock<std::mutex> lock(impl_->mtx);
    auto has_count = [this] { return impl_->count > 0; };
    if (timeout_ms == kForever) {
        impl_->cv.wait(lock, has_count);
    } else if (!impl_->cv.wait_for(lock, std::chrono::milliseconds(timeout_ms), has_count)) {
        return false;
    }
    --impl_->count;
    return true;
}

void Semaphore::give() {
    std::lock_guard<std::mutex> lock(impl_->mtx);
    if (impl_->count < impl_->max_count) {
        ++impl_->count;
    }
    impl_->cv.notify_one();
}

// ---------------------------------------------------------------------------
// Task
// ---------------------------------------------------------------------------

struct Task::Impl {
    std::thread thread;
    std::atomic<bool> joined{false};
};

Task::Task() = default;

Task::~Task() { join(); }

std::unique_ptr<Task> Task::spawn(const TaskConfig& config, std::function<void()> fn) {
    // The host simulator has no stack-size/priority/core-affinity controls
    // comparable to an RTOS task; the config is accepted for API parity
    // with the ESP-IDF port but otherwise ignored here.
    (void)config;

    std::unique_ptr<Task> task(new Task());
    task->impl_ = std::make_unique<Impl>();
    task->impl_->thread = std::thread(std::move(fn));
    return task;
}

void Task::join() {
    if (impl_ && !impl_->joined.exchange(true)) {
        if (impl_->thread.joinable()) {
            impl_->thread.join();
        }
    }
}

bool Task::is_current() const {
    return impl_ && impl_->thread.get_id() == std::this_thread::get_id();
}

// ---------------------------------------------------------------------------
// Timer
// ---------------------------------------------------------------------------

struct Timer::Impl {
    uint32_t period_ms;
    bool periodic;
    std::function<void()> cb;

    // `thread` and the start/stop/join choreography are guarded by
    // control_mtx, which is held for the whole duration of start(), stop()
    // and the destructor (including the join() call itself) so the three
    // can never race on `thread`. `wait_mtx`/`cv`/`stop_requested` are a
    // separate pair used only by run()'s own wait loop; `running` and
    // `runner_id` are atomics so run() can update them without ever
    // needing control_mtx (control_mtx may be held by a concurrent
    // stop()/destructor that is blocked inside join() waiting for this
    // very thread to finish — if run() also needed control_mtx to finish,
    // that would deadlock).
    std::thread thread;
    std::mutex control_mtx;

    std::mutex wait_mtx;
    std::condition_variable cv;
    bool stop_requested = false;

    std::atomic<bool> running{false};
    std::atomic<std::thread::id> runner_id{};

    // True if the calling thread is this timer's own callback thread
    // (i.e. this call happened synchronously from within cb() itself).
    // Reads only atomics, so it is safe to call before ever touching
    // control_mtx — which matters: control_mtx can be held by some
    // *other* thread that is blocked inside join() waiting for this very
    // timer thread to exit (a concurrent stop()/start()/destructor call
    // from elsewhere), and acquiring it unconditionally would deadlock
    // against that join() the moment cb() itself tries to call
    // start()/stop() on the same Timer.
    bool is_on_timer_thread() const {
        return running.load() && runner_id.load() == std::this_thread::get_id();
    }

    void run() {
        // Published first so stop()-from-within-cb() can recognize "this
        // is the timer's own thread" as soon as the thread exists.
        runner_id = std::this_thread::get_id();
        while (true) {
            {
                std::unique_lock<std::mutex> lock(wait_mtx);
                bool stopped = cv.wait_for(lock, std::chrono::milliseconds(period_ms),
                                           [this] { return stop_requested; });
                if (stopped) {
                    break;
                }
            }
            cb();
            if (!periodic) {
                break;
            }
        }
        running = false;
    }
};

Timer::Timer(const char* /*name*/, uint32_t period_ms, bool periodic, std::function<void()> cb)
    : impl_(std::make_unique<Impl>()) {
    impl_->period_ms = period_ms;
    impl_->periodic = periodic;
    impl_->cb = std::move(cb);
}

Timer::~Timer() { stop(); }

void Timer::start() {
    // On the timer's own thread, start() is always a no-op (the timer is
    // necessarily already running) — return before ever touching
    // control_mtx, for the deadlock reason explained on
    // Impl::is_on_timer_thread() above.
    if (impl_->is_on_timer_thread()) {
        return;
    }

    std::lock_guard<std::mutex> control_lock(impl_->control_mtx);
    if (impl_->running) {
        return;
    }
    if (impl_->thread.joinable()) {
        // A previous run finished (naturally, or via a stop() called from
        // within its own cb()) but was never joined; clean it up before
        // starting a new one.
        impl_->thread.join();
    }
    {
        std::lock_guard<std::mutex> lock(impl_->wait_mtx);
        impl_->stop_requested = false;
    }
    // Invalidate runner_id before the new thread exists (and before
    // `running` becomes visible as true), so there is no window where a
    // concurrent caller could read running==true together with a stale
    // runner_id left over from a previous, already-exited incarnation of
    // this timer's thread — e.g. if the OS has since reused that exact
    // thread id for the very thread calling start()/stop() right now,
    // which would otherwise be misidentified as "the timer thread".
    impl_->runner_id = std::thread::id();
    impl_->running = true;
    impl_->thread = std::thread([this] { impl_->run(); });
}

void Timer::stop() {
    // Joining a thread with itself is undefined behavior (std::terminate
    // under -fno-exceptions); detect that before touching control_mtx, for
    // the deadlock reason explained on Impl::is_on_timer_thread() above.
    // Only signal the thread to stop and return without blocking; the
    // actual join happens later, from start() or the destructor, once
    // called from a different thread.
    if (impl_->is_on_timer_thread()) {
        {
            std::lock_guard<std::mutex> lock(impl_->wait_mtx);
            impl_->stop_requested = true;
        }
        impl_->cv.notify_all();
        return;
    }

    std::lock_guard<std::mutex> control_lock(impl_->control_mtx);
    {
        std::lock_guard<std::mutex> lock(impl_->wait_mtx);
        impl_->stop_requested = true;
    }
    impl_->cv.notify_all();

    if (impl_->thread.joinable()) {
        impl_->thread.join();
    }
}

bool Timer::active() const { return impl_->running.load(); }

}  // namespace wisp::os
