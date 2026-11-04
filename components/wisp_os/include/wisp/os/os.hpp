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

// Milliseconds since boot (since process start in the simulator);
// monotonic, not wall-clock time. Log timestamps and the admin API's
// `uptime_ms` share this base, so a browser can compare them (spec 6.2).
uint64_t now_ms();

// The same clock as now_ms(), in microseconds, for timing work that takes
// less than a millisecond (e.g. one display transfer).
uint64_t now_us();

// Blocks the calling thread/task for at least `ms` milliseconds.
void sleep_ms(uint32_t ms);

// Passed as a timeout to mean "wait indefinitely".
constexpr uint32_t kForever = UINT32_MAX;

// Memory allocation hints. The simulator ignores them; the ESP-IDF port
// will route Fast/Dma to internal/DMA-capable RAM and Large to PSRAM.
enum class Mem { Fast, Large, Dma };

void* alloc(size_t bytes, Mem kind);
void dealloc(void* p);

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
    // Higher runs preferentially over lower, relative to other wisp
    // tasks; the valid range and its meaning (idle/highest priority, how
    // many levels exist) is platform-defined. A port clamps an
    // out-of-range value into its own valid range rather than passing it
    // through uninterpreted -- see e.g. components/wisp_port_esp/os/os_esp.cpp's
    // Task::spawn(), which clamps to FreeRTOS's [0, configMAX_PRIORITIES - 1].
    int priority = 5;
    int core = -1;
    // Whether the task may read, write, erase or map internal flash,
    // directly or through any service it calls: Settings (NVS),
    // StorageManager files on internal storage (LittleFS), firmware
    // upload (OTA) and raw partitions. Callbacks the task runs on others'
    // behalf count too. Reading data already mapped into the address
    // space (constants, the linked-in web asset pack) does not. Declare
    // false only when that holds for every path the task can take.
    //
    // A declaration, not a placement: the port picks where the stack
    // goes. On the ESP port a task declaring false may get its stack in
    // external RAM (PSRAM), where a flash operation would assert -- a
    // silent abort, on hardware only (the simulator ignores this field).
    // A task declaring true always gets an internal stack, as does any
    // task when the build or PSRAM can't hold its stack. AGENTS.md
    // section 5.
    bool flash_access = true;
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
// task/thread. Keep `cb` short and non-blocking: some ports (e.g.
// ESP-IDF, via esp_timer's ESP_TIMER_TASK dispatch) run every Timer in
// the whole process on one shared dispatch task, so a slow `cb` delays
// every other Timer too, not just this one's own next period.
class Timer {
public:
    Timer(const char* name, uint32_t period_ms, bool periodic, std::function<void()> cb);

    // Stops the timer (see stop()) if it is still active. Must not be
    // called from within cb() itself (destroying a Timer from inside its
    // own callback is not supported).
    ~Timer();

    Timer(const Timer&) = delete;
    Timer& operator=(const Timer&) = delete;

    // Idempotent: starting an already-active timer is a no-op. This
    // includes a call made from within cb() itself (e.g. an attempt to
    // re-arm a one-shot timer so it fires again) — the timer is still
    // "active" for the duration of that cb() invocation, so start() just
    // returns without doing anything. To have a timer fire repeatedly,
    // construct it with periodic=true; re-arming a one-shot timer from
    // inside its own callback is not supported.
    void start();

    // Stops the timer. If called from a thread other than the timer's own
    // callback thread, blocks until any in-flight invocation of cb has
    // returned, so cb is guaranteed not to be running once stop() returns.
    // If called from within cb() itself, returns immediately without
    // blocking — joining the calling thread with itself would deadlock —
    // but cb is still guaranteed not to be invoked again after this
    // particular invocation returns. Idempotent either way.
    //
    // Do not call stop() (or destroy the Timer) while holding a lock that
    // any Timer's cb() might itself try to take. On ports that dispatch
    // every Timer's cb() from one shared task (see the class comment
    // above), the "blocks until any in-flight invocation has returned"
    // wait above may need to wait for that shared dispatch task to finish
    // whatever cb() (of this Timer, or — on such ports — any other due
    // Timer ahead of it in the dispatch order) is currently running; if
    // that cb() is itself blocked trying to take a lock the caller of
    // stop() is still holding, this becomes a deadlock, not just a delay.
    void stop();

    bool active() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace wisp::os
