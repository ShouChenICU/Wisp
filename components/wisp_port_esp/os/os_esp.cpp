// ESP-IDF implementation of wisp::os (components/wisp_os/include/wisp/os/os.hpp),
// built directly on FreeRTOS primitives (xSemaphoreCreate*/xTaskCreatePinnedToCore)
// and esp_timer/heap_caps. components/wisp_port_sim/os/os_sim.cpp is the semantic
// reference this file matches (Timer stop-from-callback rules, Task join,
// Semaphore saturation, alloc/dealloc routing); see each section below for
// how the same contract is realized on top of FreeRTOS instead of
// <thread>/<mutex>/<condition_variable>.

#include <atomic>

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "wisp/os/os.hpp"

namespace wisp::os {

uint64_t now_ms() { return static_cast<uint64_t>(esp_timer_get_time() / 1000); }

void sleep_ms(uint32_t ms) { vTaskDelay(pdMS_TO_TICKS(ms)); }

void* alloc(size_t bytes, Mem kind) {
    switch (kind) {
        case Mem::Large:
            return heap_caps_malloc(bytes, MALLOC_CAP_SPIRAM);
        case Mem::Fast:
            return heap_caps_malloc(bytes, MALLOC_CAP_INTERNAL | MALLOC_CAP_8BIT);
        case Mem::Dma:
            return heap_caps_malloc(bytes, MALLOC_CAP_DMA | MALLOC_CAP_INTERNAL);
    }
    return nullptr;
}

void dealloc(void* p) { heap_caps_free(p); }

// ---------------------------------------------------------------------------
// Mutex
// ---------------------------------------------------------------------------

struct Mutex::Impl {
    SemaphoreHandle_t handle;
};

Mutex::Mutex() : impl_(std::make_unique<Impl>()) { impl_->handle = xSemaphoreCreateMutex(); }

// unique_ptr<Impl>'s own destructor only frees the Impl struct itself; the
// FreeRTOS handle it holds needs its own explicit delete call first (unlike
// the sim port, where Impl just wraps a std::mutex that cleans itself up).
Mutex::~Mutex() {
    if (impl_) {
        vSemaphoreDelete(impl_->handle);
    }
}

void Mutex::lock() { xSemaphoreTake(impl_->handle, portMAX_DELAY); }
void Mutex::unlock() { xSemaphoreGive(impl_->handle); }
bool Mutex::try_lock() { return xSemaphoreTake(impl_->handle, 0) == pdTRUE; }

// ---------------------------------------------------------------------------
// Semaphore
// ---------------------------------------------------------------------------

struct Semaphore::Impl {
    SemaphoreHandle_t handle;
};

Semaphore::Semaphore(uint32_t max_count, uint32_t initial) : impl_(std::make_unique<Impl>()) {
    impl_->handle = xSemaphoreCreateCounting(max_count, initial);
}

Semaphore::~Semaphore() {
    if (impl_) {
        vSemaphoreDelete(impl_->handle);
    }
}

bool Semaphore::take(uint32_t timeout_ms) {
    TickType_t ticks = timeout_ms == kForever ? portMAX_DELAY : pdMS_TO_TICKS(timeout_ms);
    return xSemaphoreTake(impl_->handle, ticks) == pdTRUE;
}

void Semaphore::give() {
    // xSemaphoreGive() on a counting semaphore already refuses to exceed
    // max_count (returns pdFALSE in that case) -- exactly the "saturating"
    // behavior os.hpp documents -- so the return value is simply ignored,
    // same as the sim port.
    xSemaphoreGive(impl_->handle);
}

// ---------------------------------------------------------------------------
// Task
// ---------------------------------------------------------------------------

struct Task::Impl {
    TaskHandle_t handle = nullptr;
    std::function<void()> fn;
    // Given by trampoline() right before it calls vTaskDelete(NULL);
    // join() waits on it. A binary semaphore (max count 1): fn runs at
    // most once, so it is given at most once.
    SemaphoreHandle_t done = nullptr;
    std::atomic<bool> joined{false};

    // FreeRTOS's TaskFunction_t trampoline. Impl is a private nested type
    // of Task (see wisp/os/os.hpp), so a free function outside Task
    // cannot name it even from within this same .cpp file -- only Impl's
    // own members (and Task's other members, like spawn() below) have
    // access to it. A static member function has the same void(void*)
    // signature/calling convention as a free function (no implicit
    // `this`), so it's fully ABI-compatible with TaskFunction_t.
    static void trampoline(void* arg) {
        auto* impl = static_cast<Impl*>(arg);
        impl->fn();
        xSemaphoreGive(impl->done);
        vTaskDelete(nullptr);
    }
};

Task::Task() = default;

Task::~Task() { join(); }

std::unique_ptr<Task> Task::spawn(const TaskConfig& config, std::function<void()> fn) {
    std::unique_ptr<Task> task(new Task());
    task->impl_ = std::make_unique<Impl>();
    task->impl_->fn = std::move(fn);
    task->impl_->done = xSemaphoreCreateBinary();

    BaseType_t core_id = config.core == -1 ? tskNO_AFFINITY : static_cast<BaseType_t>(config.core);
    BaseType_t ok = xTaskCreatePinnedToCore(
        &Impl::trampoline, config.name, static_cast<uint32_t>(config.stack_bytes),
        task->impl_.get(), static_cast<UBaseType_t>(config.priority), &task->impl_->handle,
        core_id);
    if (ok != pdPASS) {
        // xTaskCreatePinnedToCore() failed (e.g. out of memory for the
        // task's stack/TCB): per os.hpp, spawn() returns nullptr rather
        // than a Task handle for a task that was never actually created.
        // Mark this Impl as already "joined" first so that ~Task() (run
        // when `task` goes out of scope below) takes its join() fast
        // path instead of calling xSemaphoreTake() on the handle this
        // deletes next.
        task->impl_->joined.store(true);
        vSemaphoreDelete(task->impl_->done);
        task->impl_->done = nullptr;
        return nullptr;
    }
    return task;
}

void Task::join() {
    if (impl_ && !impl_->joined.exchange(true)) {
        xSemaphoreTake(impl_->done, portMAX_DELAY);
        vSemaphoreDelete(impl_->done);
    }
}

bool Task::is_current() const {
    return impl_ && impl_->handle != nullptr && impl_->handle == xTaskGetCurrentTaskHandle();
}

// ---------------------------------------------------------------------------
// Timer
// ---------------------------------------------------------------------------

// Built on esp_timer with ESP_TIMER_TASK dispatch (every esp_timer in the
// whole process is invoked, one at a time, from the single shared
// "esp_timer" task -- see esp_timer.c's timer_process_alarm()/timer_task()).
// That single-task, non-reentrant dispatch is what os.hpp's stop()/start()
// contract is built around:
//
//   - esp_timer_stop() itself never blocks (it just unlinks the timer from
//     esp_timer's internal alarm list under a short critical section --
//     see esp_timer.c), so matching Timer::stop()'s "blocks until any
//     in-flight invocation of cb has returned" (when called from a
//     different thread) needs this file's own bookkeeping: running_mutex_
//     is held by trampoline() for exactly the duration of cb(), so
//     taking-then-releasing it from stop() is precisely "wait for any
//     in-flight cb to finish, then proceed" (and resolves immediately if
//     none is in flight).
//   - cb_running_task_ records which task is currently inside *this*
//     timer's own trampoline(), which is how is_on_timer_thread() tells a
//     call made synchronously from within cb() itself (don't block --
//     self-join would deadlock) apart from a call made from some other
//     task while a callback merely happens to be in flight (block
//     normally).
//   - esp_timer_stop()/esp_timer_delete() are themselves safe to call from
//     within a timer's own callback: timer_process_alarm() releases the
//     list lock before invoking the callback and only re-acquires it
//     after, so there is no self-deadlock on that lock either.

struct Timer::Impl {
    esp_timer_handle_t handle = nullptr;
    uint32_t period_ms = 0;
    bool periodic = false;
    std::function<void()> cb;

    std::atomic<bool> active{false};
    std::atomic<TaskHandle_t> cb_running_task{nullptr};

    // Guards "a cb() invocation of this timer is currently in flight" --
    // held by trampoline() only across the cb() call itself.
    SemaphoreHandle_t running_mutex = nullptr;
    // Serializes start()/stop()/the destructor against each other when
    // called from different tasks (mirrors the sim port's control_mtx).
    SemaphoreHandle_t control_mutex = nullptr;

    bool is_on_timer_thread() const {
        return cb_running_task.load() == xTaskGetCurrentTaskHandle();
    }

    // esp_timer_cb_t trampoline. Same private-nested-type access reason as
    // Task::Impl::trampoline above: must be a static member of Impl, not a
    // free function, since Impl is private to Timer.
    static void trampoline(void* arg) {
        auto* impl = static_cast<Impl*>(arg);
        xSemaphoreTake(impl->running_mutex, portMAX_DELAY);
        impl->cb_running_task.store(xTaskGetCurrentTaskHandle());
        impl->cb();
        impl->cb_running_task.store(nullptr);
        if (!impl->periodic) {
            // esp_timer already removed a one-shot timer from its
            // schedule before invoking the callback; mark it inactive now
            // that the callback itself has also finished, matching the
            // sim port's Timer::Impl::run() returning (and `running`
            // going false) right after a one-shot cb() call.
            impl->active.store(false);
        }
        xSemaphoreGive(impl->running_mutex);
    }
};

Timer::Timer(const char* name, uint32_t period_ms, bool periodic, std::function<void()> cb)
    : impl_(std::make_unique<Impl>()) {
    impl_->period_ms = period_ms;
    impl_->periodic = periodic;
    impl_->cb = std::move(cb);
    impl_->running_mutex = xSemaphoreCreateMutex();
    impl_->control_mutex = xSemaphoreCreateMutex();

    esp_timer_create_args_t args = {};
    args.callback = &Impl::trampoline;
    args.arg = impl_.get();
    args.dispatch_method = ESP_TIMER_TASK;
    args.name = name;
    args.skip_unhandled_events = false;
    esp_timer_create(&args, &impl_->handle);
}

Timer::~Timer() {
    stop();
    esp_timer_delete(impl_->handle);
    vSemaphoreDelete(impl_->running_mutex);
    vSemaphoreDelete(impl_->control_mutex);
}

void Timer::start() {
    // On the timer's own callback thread, start() is always a no-op --
    // see os.hpp's documented re-arm-from-within-cb restriction. Checked
    // before touching control_mutex_ for the same reason as the sim
    // port: control_mutex_ may be held by a concurrent stop()/destructor
    // call (from some other task) that is itself blocked inside
    // running_mutex_, waiting for this very callback invocation to
    // finish.
    if (impl_->is_on_timer_thread()) {
        return;
    }

    xSemaphoreTake(impl_->control_mutex, portMAX_DELAY);
    if (impl_->active.load()) {
        xSemaphoreGive(impl_->control_mutex);
        return;
    }
    uint64_t period_us = static_cast<uint64_t>(impl_->period_ms) * 1000;
    esp_err_t err = impl_->periodic ? esp_timer_start_periodic(impl_->handle, period_us)
                                    : esp_timer_start_once(impl_->handle, period_us);
    if (err == ESP_OK) {
        impl_->active.store(true);
    }
    xSemaphoreGive(impl_->control_mutex);
}

void Timer::stop() {
    // Joining a thread with itself is undefined; detect "called from
    // within cb() itself" the same way as start() above, before touching
    // control_mutex_. esp_timer_stop() itself never blocks (see this
    // section's file-header comment), so it's safe to call here even
    // though we are that very callback, still on the call stack.
    if (impl_->is_on_timer_thread()) {
        esp_timer_stop(impl_->handle);  // no-op (ESP_ERR_INVALID_STATE) if already unarmed
        impl_->active.store(false);
        return;
    }

    xSemaphoreTake(impl_->control_mutex, portMAX_DELAY);
    esp_timer_stop(impl_->handle);  // no-op (ESP_ERR_INVALID_STATE) if already unarmed
    // Block until any cb() invocation already in flight (started before
    // esp_timer_stop() above could prevent it) has returned.
    xSemaphoreTake(impl_->running_mutex, portMAX_DELAY);
    xSemaphoreGive(impl_->running_mutex);
    impl_->active.store(false);
    xSemaphoreGive(impl_->control_mutex);
}

bool Timer::active() const { return impl_->active.load(); }

}  // namespace wisp::os
