#pragma once

// Portable OS abstraction. This header declares types and functions only;
// it must never include platform headers (esp_*, freertos/*, driver/*, IDF
// hal/, soc/*, sdkconfig.h, SDL*) or expose platform types (e.g. no
// std::thread in this header). Every class below uses PIMPL (an opaque
// `Impl` struct behind a std::unique_ptr) so that different ports (the PC
// simulator, and later ESP-IDF/FreeRTOS) can each define `Impl` however they
// like without changing this header.
//
// Implementations are provided at link time by exactly one port component
// (e.g. wisp_port_sim_os for the host simulator).

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>

namespace wisp::os {

// Milliseconds since an arbitrary, monotonic epoch (not wall-clock time).
// Only differences between calls are meaningful.
uint64_t now_ms();

// Blocks the calling thread/task for at least `ms` milliseconds.
void sleep_ms(uint32_t ms);

// Passed as a timeout to mean "wait indefinitely".
constexpr uint32_t kForever = UINT32_MAX;

// Memory allocation hints. The simulator ignores them; the ESP-IDF port
// will route Fast/Dma to internal/DMA-capable RAM and Large to PSRAM.
enum class Mem { Fast, Large, Dma };

void* alloc(size_t bytes, Mem kind);
void free(void* p);

// A mutual-exclusion lock satisfying BasicLockable (lock/unlock/try_lock),
// so it can be used directly with std::lock_guard / std::unique_lock.
class Mutex {
public:
    Mutex();
    ~Mutex();

    Mutex(const Mutex&) = delete;
    Mutex& operator=(const Mutex&) = delete;

    void lock();
    void unlock();
    bool try_lock();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// A counting semaphore with an explicit maximum count.
class Semaphore {
public:
    // `initial` must not exceed `max_count`.
    Semaphore(uint32_t max_count, uint32_t initial);
    ~Semaphore();

    Semaphore(const Semaphore&) = delete;
    Semaphore& operator=(const Semaphore&) = delete;

    // Waits for up to `timeout_ms` (or kForever) for the count to become
    // positive, then decrements it and returns true. Returns false if the
    // timeout elapsed first. `timeout_ms == 0` polls without blocking.
    bool take(uint32_t timeout_ms);

    // Increments the count (saturating at max_count) and wakes one waiter.
    void give();

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

struct TaskConfig {
    const char* name = "wisp";
    size_t stack_bytes = 4096;
    int priority = 5;
    int core = -1;
};

// A unit of concurrent execution (an RTOS task on the ESP port, a thread on
// the simulator).
class Task {
public:
    // Starts `fn` running concurrently and returns a handle to it.
    static std::unique_ptr<Task> spawn(const TaskConfig& config, std::function<void()> fn);

    // The task must have been joined (or fn must have already returned)
    // before the handle is destroyed.
    ~Task();

    Task(const Task&) = delete;
    Task& operator=(const Task&) = delete;

    // Waits for fn to return. Safe to call even after fn has already
    // finished; safe to call at most meaningfully once (further calls are
    // a cheap no-op).
    void join();

    // True if called from within fn's own task/thread.
    bool is_current() const;

private:
    Task();

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

// A one-shot or periodic timer that invokes `cb` on a dedicated
// task/thread.
class Timer {
public:
    Timer(const char* name, uint32_t period_ms, bool periodic, std::function<void()> cb);

    // Stops the timer (see stop()) if it is still active.
    ~Timer();

    Timer(const Timer&) = delete;
    Timer& operator=(const Timer&) = delete;

    // Idempotent: starting an already-active timer is a no-op.
    void start();

    // Stops the timer and blocks until any in-flight invocation of cb has
    // returned, so cb is guaranteed not to be running once stop() returns.
    // Idempotent. Must not be called from within cb itself.
    void stop();

    bool active() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace wisp::os
