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

namespace wisp::os {

uint64_t now_ms() {
    using namespace std::chrono;
    return static_cast<uint64_t>(
        duration_cast<milliseconds>(steady_clock::now().time_since_epoch()).count());
}

void sleep_ms(uint32_t ms) { std::this_thread::sleep_for(std::chrono::milliseconds(ms)); }

void* alloc(size_t bytes, Mem /*kind*/) {
    // The simulator has no Fast/Dma/PSRAM distinction; every kind maps to
    // the regular heap.
    return std::malloc(bytes);
}

void free(void* p) { std::free(p); }

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

    std::thread thread;
    std::mutex mtx;
    std::condition_variable cv;
    bool stop_requested = false;
    std::atomic<bool> running{false};

    void run() {
        while (true) {
            {
                std::unique_lock<std::mutex> lock(mtx);
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
    std::lock_guard<std::mutex> lock(impl_->mtx);
    if (impl_->running) {
        return;
    }
    if (impl_->thread.joinable()) {
        // A previous (non-periodic) run finished on its own; clean it up
        // before starting a new one.
        impl_->thread.join();
    }
    impl_->stop_requested = false;
    impl_->running = true;
    impl_->thread = std::thread([this] { impl_->run(); });
}

void Timer::stop() {
    {
        std::lock_guard<std::mutex> lock(impl_->mtx);
        impl_->stop_requested = true;
    }
    impl_->cv.notify_all();
    if (impl_->thread.joinable()) {
        impl_->thread.join();
    }
}

bool Timer::active() const { return impl_->running.load(); }

}  // namespace wisp::os
