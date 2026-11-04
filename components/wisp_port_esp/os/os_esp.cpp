// ESP-IDF implementation of wisp::os (components/wisp_os/include/wisp/os/os.hpp),
// built directly on FreeRTOS primitives (xSemaphoreCreate*/xTaskCreatePinnedToCore)
// and esp_timer/heap_caps. components/wisp_port_sim/os/os_sim.cpp is the semantic
// reference this file matches (Timer stop-from-callback rules, Task join,
// Semaphore saturation, alloc/dealloc routing); see each section below for
// how the same contract is realized on top of FreeRTOS instead of
// <thread>/<mutex>/<condition_variable>.
//
// Every Timer in the whole process shares one real
// FreeRTOS task to run its cb() on (ESP_TIMER_TASK dispatch -- see the
// Timer section below), sized by CONFIG_ESP_TIMER_TASK_STACK_SIZE (raised
// to 6144 in sdkconfig.defaults). Keep every Timer cb() short and
// non-blocking: a slow or blocking cb() delays every *other* Timer in the
// process, not just its own next period.

#include <atomic>
#include <cstdio>
#include <cstdlib>

#include "esp_heap_caps.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "freertos/idf_additions.h"
#include "freertos/semphr.h"
#include "freertos/task.h"
#include "wisp/os/os.hpp"

namespace wisp::os {

namespace {

// Exceptions are disabled (-fno-exceptions), so a FreeRTOS/esp_timer
// resource-creation failure that a constructor/destructor has no return
// value to report (Mutex, Semaphore, Timer -- unlike Task::spawn(), which
// can and does report its own failure by returning nullptr) is handled by
// aborting with a clear message rather than silently continuing with a
// null handle that every later call would dereference. Plain abort() (not
// assert()/configASSERT(), which NDEBUG / CONFIG_FREERTOS_ASSERT_* can
// compile out or soften to a no-crash log-and-continue) so this can never
// silently become "do nothing" in a release config.
[[noreturn]] void fatal(const char* what) {
    std::printf("wisp::os (ESP): fatal: %s\n", what);
    std::fflush(stdout);
    std::abort();
}

// Rounds `ms` milliseconds up to the nearest whole FreeRTOS tick, never
// down. pdMS_TO_TICKS()/portTICK_PERIOD_MS-style truncating division
// means that with the default CONFIG_FREERTOS_HZ=100 (10 ms/tick),
// ms in 1..9 would truncate to 0 ticks -- sleep_ms(1..9) would return
// immediately instead of actually sleeping, and Semaphore::take() with a
// timeout_ms in 1..9 would poll (an effective timeout of 0) instead of
// blocking for any time at all. That breaks os.hpp's "blocks for at
// least `ms`"/"waits for up to `timeout_ms`" contracts and turns every
// short-timeout retry loop (EventBus, the self-check screen, UiRuntime)
// into a tight busy-loop -- a watchdog-trip risk, not just a latency
// bug. Any ms > 0 therefore always rounds up to at least 1 tick; ms == 0
// stays 0 ticks (an explicit non-blocking poll, matching
// Semaphore::take()'s documented timeout_ms == 0 behavior, not "wait
// zero time" by accident). kForever is handled by callers before this.
TickType_t ms_to_ticks(uint32_t ms) {
    if (ms == 0) {
        return 0;
    }
    // 64-bit product: ms (up to UINT32_MAX) * configTICK_RATE_HZ can
    // overflow 32 bits once the tick rate is raised (e.g. to 1000 Hz --
    // see sdkconfig.defaults), so this is computed in 64 bits even
    // though the result is narrowed back to a TickType_t (32 bits on
    // this target) afterward.
    uint64_t ticks = (static_cast<uint64_t>(ms) * configTICK_RATE_HZ + 999) / 1000;
    // Not reachable for ms > 0 with any configTICK_RATE_HZ >= 1 (the
    // +999 numerator alone is already >= 1000), but kept as an explicit,
    // self-documenting floor rather than relying on that arithmetic fact.
    return ticks == 0 ? 1 : static_cast<TickType_t>(ticks);
}

}  // namespace

uint64_t now_ms() { return static_cast<uint64_t>(esp_timer_get_time() / 1000); }

uint64_t now_us() { return static_cast<uint64_t>(esp_timer_get_time()); }

void sleep_ms(uint32_t ms) { vTaskDelay(ms_to_ticks(ms)); }

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

Mutex::Mutex() : impl_(std::make_unique<Impl>()) {
    impl_->handle = xSemaphoreCreateMutex();
    if (impl_->handle == nullptr) {
        fatal("xSemaphoreCreateMutex failed (Mutex)");
    }
}

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
    if (impl_->handle == nullptr) {
        fatal("xSemaphoreCreateCounting failed (Semaphore)");
    }
}

Semaphore::~Semaphore() {
    if (impl_) {
        vSemaphoreDelete(impl_->handle);
    }
}

bool Semaphore::take(uint32_t timeout_ms) {
    TickType_t ticks = timeout_ms == kForever ? portMAX_DELAY : ms_to_ticks(timeout_ms);
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
    // std::atomic, not a plain TaskHandle_t: join()
    // nulls this out once the task has actually finished and its TCB is
    // gone, so a *later* is_current() call can't spuriously match a
    // brand-new, unrelated task whose TCB FreeRTOS happened to recycle
    // at the same address. Atomic because join() (typically called from
    // a different task than the one `handle` identifies) writes it while
    // is_current() may read it concurrently from this handle's own task,
    // right up until that task actually exits.
    std::atomic<TaskHandle_t> handle{nullptr};
    std::function<void()> fn;
    // Given by trampoline() right before it calls vTaskDelete(NULL);
    // join() waits on it. A binary semaphore (max count 1): fn runs at
    // most once, so it is given at most once.
    SemaphoreHandle_t done = nullptr;
    std::atomic<bool> joined{false};
    // Created by xTaskCreatePinnedToCoreWithCaps() (an external stack).
    // Such a task must be deleted with vTaskDeleteWithCaps(), and not by
    // itself: self-deletion spawns a helper task and aborts when that
    // cannot be allocated. So it suspends itself after giving `done`, and
    // join() deletes it.
    bool with_caps = false;

    // FreeRTOS's TaskFunction_t trampoline. Impl is a private nested type
    // of Task (see wisp/os/os.hpp), so a free function outside Task
    // cannot name it even from within this same .cpp file -- only Impl's
    // own members (and Task's other members, like spawn() below) have
    // access to it. A static member function has the same void(void*)
    // signature/calling convention as a free function (no implicit
    // `this`), so it's fully ABI-compatible with TaskFunction_t.
    static void trampoline(void* arg) {
        auto* impl = static_cast<Impl*>(arg);
        const bool with_caps = impl->with_caps;
        impl->fn();
        // `impl` may be freed by join() as soon as `done` is given.
        xSemaphoreGive(impl->done);
        if (with_caps) {
            for (;;) vTaskSuspend(nullptr);
        }
        vTaskDelete(nullptr);
    }
};

Task::Task() = default;

Task::~Task() { join(); }

// TaskConfig::priority (os.hpp) is a portable int with no
// documented range -- clamped here (the ESP-IDF-specific mapping) to
// FreeRTOS's valid [0, configMAX_PRIORITIES - 1] range rather than
// passing an out-of-range value straight to xTaskCreatePinnedToCore(),
// which would silently misbehave (values are masked/UB-ish, not
// rejected). 0 is the idle-task priority (lowest); configMAX_PRIORITIES
// - 1 is the highest application priority.
std::unique_ptr<Task> Task::spawn(const TaskConfig& config, std::function<void()> fn) {
    std::unique_ptr<Task> task(new Task());
    task->impl_ = std::make_unique<Impl>();
    task->impl_->fn = std::move(fn);
    task->impl_->done = xSemaphoreCreateBinary();
    if (task->impl_->done == nullptr) {
        // Unlike Mutex/Semaphore/Timer (which have no
        // failure channel and abort via fatal()), spawn() already has a
        // documented "returns nullptr" failure path (for
        // xTaskCreatePinnedToCore() below) -- reuse it here instead of
        // aborting, since a task that was never created is exactly what
        // nullptr already means.
        //
        // `task` (a unique_ptr<Task>) goes out of scope
        // on this `return` and runs ~Task(), which calls join(). join()'s
        // fast path only applies once `joined` is already true -- without
        // marking it here, join() would instead fall into its normal path
        // and call xSemaphoreTake() on `impl_->done`, which is nullptr
        // (the very failure being handled), crashing. Mark joined before
        // returning, same as the xTaskCreatePinnedToCore() failure path
        // below.
        task->impl_->joined.store(true);
        return nullptr;
    }

    TaskHandle_t raw_handle = nullptr;
    int clamped_priority = config.priority;
    if (clamped_priority < 0) {
        clamped_priority = 0;
    } else if (clamped_priority > static_cast<int>(configMAX_PRIORITIES) - 1) {
        clamped_priority = static_cast<int>(configMAX_PRIORITIES) - 1;
    }
    BaseType_t core_id = config.core == -1 ? tskNO_AFFINITY : static_cast<BaseType_t>(config.core);
    BaseType_t ok = pdFAIL;
    if (config.external_stack &&
        heap_caps_get_largest_free_block(MALLOC_CAP_SPIRAM) >= config.stack_bytes) {
        // Set before the task can run: the trampoline reads it.
        task->impl_->with_caps = true;
        ok = xTaskCreatePinnedToCoreWithCaps(
            &Impl::trampoline, config.name, static_cast<uint32_t>(config.stack_bytes),
            task->impl_.get(), static_cast<UBaseType_t>(clamped_priority), &raw_handle, core_id,
            MALLOC_CAP_SPIRAM | MALLOC_CAP_8BIT);
        if (ok != pdPASS) task->impl_->with_caps = false;
    }
    if (ok != pdPASS) {
        ok = xTaskCreatePinnedToCore(
            &Impl::trampoline, config.name, static_cast<uint32_t>(config.stack_bytes),
            task->impl_.get(), static_cast<UBaseType_t>(clamped_priority), &raw_handle, core_id);
    }
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
    // Stored into the atomic after xTaskCreatePinnedToCore() returns,
    // rather than pointing it straight at &impl_->handle as the out-param
    // (which isn't a valid TaskHandle_t* in the first place), so there is
    // a narrow window, right after creation, where the new task is
    // already runnable (possibly on another core) but is_current() would
    // still read nullptr for it. This is accepted: nothing can validly
    // call is_current() on this Task before spawn() has returned the
    // unique_ptr<Task> that makes it reachable in the first place, and
    // this store happens immediately on return, before any caller can
    // observe the new Task at all.
    task->impl_->handle.store(raw_handle);
    return task;
}

void Task::join() {
    if (!impl_) {
        return;
    }
    // Joining a task from within that very task would
    // deadlock forever (it can never give `done` -- it's parked here
    // waiting for itself). configASSERT() (not a plain comment) so a
    // debug/assert-enabled build catches the bug immediately instead of
    // silently hanging; see os.hpp's ~Task()/join() doc comment for the
    // same restriction. Per ESP-IDF's FreeRTOSConfig.h, configASSERT(a)
    // expands to assert(a), so (like any assert()) this is compiled out
    // under NDEBUG -- not set by sdkconfig.defaults here, and ESP-IDF's
    // default build optimization level doesn't define it either, but a
    // project that does set NDEBUG/a "Release" optimization level loses
    // this check and falls back to the documented deadlock-on-self-join
    // behavior instead of a clear abort.
    configASSERT(!is_current());
    if (!impl_->joined.exchange(true)) {
        xSemaphoreTake(impl_->done, portMAX_DELAY);
        vSemaphoreDelete(impl_->done);
        if (impl_->with_caps) {
            // Waits until the task is off every core, then frees its
            // stack and TCB (see Impl::with_caps).
            vTaskDeleteWithCaps(impl_->handle.load());
        }
        // See Impl::handle's comment above.
        impl_->handle.store(nullptr);
    }
}

bool Task::is_current() const {
    return impl_ && impl_->handle.load() != nullptr &&
           impl_->handle.load() == xTaskGetCurrentTaskHandle();
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
//
// The running_mutex_ dance
// above has a real race against that same lock-release-before-invoking
// behavior. esp_timer's dispatcher pops a due timer off its internal
// list (releasing the list lock) and only *then* calls trampoline(),
// which is the first point at which running_mutex_ is actually taken.
// A stop() on another task landing in that exact window -- after the
// timer has been popped (so esp_timer_stop() is a no-op, nothing to
// unlink) but before trampoline() has reached its xSemaphoreTake(
// running_mutex_, ...) -- would see "not armed" and "running_mutex_
// free" and wrongly conclude no callback is in flight, return, and (if
// this was the destructor) proceed to free Impl while trampoline() is
// about to dereference it: a use-after-free. Because every esp_timer
// callback (ESP_TIMER_TASK dispatch) still runs strictly one-at-a-time
// on the single shared esp_timer task, a trivial "fence" fixes this: a
// second, dedicated one-shot esp_timer (fence_handle_/fence_sem_ below)
// is (re)armed for 0 delay and waited on. Since the esp_timer task
// processes alarms strictly in order and one at a time, if this timer's
// callback was already popped off the list (whether or not it has
// started running yet) when the fence is armed, that invocation is
// unconditionally complete by the time the esp_timer task gets around to
// (and finishes) invoking the fence's own callback -- there is no way
// for the fence to run "in between" a pop and its trampoline() call, nor
// concurrently with it, since it is the same single task doing both,
// strictly sequentially. This closes the window running_mutex_ alone
// cannot. Skipped (as before) when already on the timer's own thread --
// is_on_timer_thread() already returns early there, and fencing against
// oneself would deadlock (the fence could never run while we block
// waiting for it, since we *are* the esp_timer task).
//
// is_on_timer_thread() above
// is scoped to *this* Timer -- it only answers "is this Timer's own cb()
// currently on the call stack of the task we're running on". That misses
// a real case: Timer Y's cb() (running on the esp_timer task) calls
// X.stop() (or destroys X) for some *other* Timer X. From X's point of
// view, is_on_timer_thread() is false (X's own cb_running_task_ is
// nullptr -- X isn't the one running), so stop() took the "different
// thread" branch above: it would arm X's fence and block on fence_sem_,
// waiting for the esp_timer task to run the fence callback. But the
// esp_timer task is *itself* blocked right there, inside Y's cb(), on
// this very call stack -- it cannot service X's fence (or anything else)
// until Y's cb() returns, which it never will. Deadlock.
//
// Fixed by recognizing "currently running on the esp_timer task at all"
// (is_on_esp_timer_task() below), not just "currently running *this*
// Timer's own cb()". When stop()/the destructor is reached from the
// esp_timer task but not via this Timer's own cb() (i.e.
// is_on_timer_thread() is false but is_on_esp_timer_task() is true), both
// the running_mutex_ wait and the fence are skipped. This is safe, not
// just deadlock-avoiding: esp_timer dispatch is strictly one callback at
// a time on that one task (the same fact the fence relies on above), so
// if *this* call stack is on the esp_timer task running some other
// timer's (Y's) cb(), then X's own trampoline() cannot *also* be
// executing right now -- there is only one call stack on that task, and
// it is this one. X's running_mutex_ is therefore unconditionally free
// already (nothing to wait for), and esp_timer_stop(X) just above this
// already removed any future scheduled firing of X from the list before
// the dispatcher could ever pop it (dispatch hasn't returned to the list
// yet -- it's parked here inside Y's cb()), so there is nothing left for
// a fence to synchronize against either.

namespace {

// True if the calling task is esp_timer's own shared
// dispatch task, regardless of which Timer (if any) owns the cb()
// currently on its call stack. Looked up by name (esp_timer registers
// its dispatch task under this exact name -- see esp_timer.c's
// esp_timer_init()/timer_task()) rather than cached once in a static,
// so this doesn't depend on ordering between esp_timer's own init and
// the construction of the first wisp::os::Timer; xTaskGetHandle() is a
// cheap linear scan of the task list, fine to call on every stop().
bool is_on_esp_timer_task() {
    TaskHandle_t esp_timer_task = xTaskGetHandle("esp_timer");
    return esp_timer_task != nullptr && xTaskGetCurrentTaskHandle() == esp_timer_task;
}

}  // namespace

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

    // The "fence" used by stop() to synchronize with
    // the esp_timer task itself -- see the file-header comment above for
    // why running_mutex_ alone isn't enough. A dedicated one-shot timer
    // (not e.g. reusing `handle` below) so arming it never disturbs this
    // Timer's own schedule/active state, and a dedicated semaphore (not
    // reusing running_mutex_/control_mutex_) so the fence's own
    // bookkeeping can never be confused with this timer's real callback
    // state.
    esp_timer_handle_t fence_handle = nullptr;
    SemaphoreHandle_t fence_sem = nullptr;

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

    // fence_handle's callback: just signals fence_sem. See the
    // file-header comment on why reaching this point at all, from
    // stop(), is sufficient proof that any previously in-flight
    // trampoline() call (of the *real* timer, above) has returned.
    static void fence_trampoline(void* arg) { xSemaphoreGive(static_cast<SemaphoreHandle_t>(arg)); }
};

Timer::Timer(const char* name, uint32_t period_ms, bool periodic, std::function<void()> cb)
    : impl_(std::make_unique<Impl>()) {
    impl_->period_ms = period_ms;
    impl_->periodic = periodic;
    impl_->cb = std::move(cb);
    impl_->running_mutex = xSemaphoreCreateMutex();
    impl_->control_mutex = xSemaphoreCreateMutex();
    impl_->fence_sem = xSemaphoreCreateBinary();
    // Timer is constructed directly (no factory/nullptr
    // -return convention the way Task::spawn() has), so a resource
    // failure here has no graceful way to be reported -- abort via
    // fatal() rather than continuing with a null handle every start()/
    // stop() call would then dereference.
    if (impl_->running_mutex == nullptr || impl_->control_mutex == nullptr ||
        impl_->fence_sem == nullptr) {
        fatal("xSemaphoreCreateMutex/CreateBinary failed (Timer)");
    }

    esp_timer_create_args_t args = {};
    args.callback = &Impl::trampoline;
    args.arg = impl_.get();
    args.dispatch_method = ESP_TIMER_TASK;
    args.name = name;
    args.skip_unhandled_events = false;
    if (esp_timer_create(&args, &impl_->handle) != ESP_OK) {
        fatal("esp_timer_create failed (Timer)");
    }

    esp_timer_create_args_t fence_args = {};
    fence_args.callback = &Impl::fence_trampoline;
    fence_args.arg = impl_->fence_sem;
    fence_args.dispatch_method = ESP_TIMER_TASK;
    fence_args.name = "wisp_timer_fence";
    // skip_unhandled_events only affects periodic timers across light
    // sleep (see esp_timer.h); left at its zero-initialized default
    // (false) since fence_handle_ is always one-shot.
    if (esp_timer_create(&fence_args, &impl_->fence_handle) != ESP_OK) {
        fatal("esp_timer_create failed (Timer fence)");
    }
}

Timer::~Timer() {
    stop();
    esp_timer_delete(impl_->handle);
    esp_timer_delete(impl_->fence_handle);
    vSemaphoreDelete(impl_->running_mutex);
    vSemaphoreDelete(impl_->control_mutex);
    vSemaphoreDelete(impl_->fence_sem);
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

    // is_on_timer_thread() above already ruled out
    // "this Timer's own cb() is calling its own stop()". This check
    // catches the other case that can reach here while still on the
    // esp_timer task: some *other* Timer's cb() (synchronously, still on
    // the esp_timer task's call stack) calling this Timer's stop() or
    // destructor. Waiting on running_mutex_/the fence in that case would
    // deadlock (see this section's file-header comment: the esp_timer
    // task cannot service the fence it's blocked waiting on, since
    // servicing it requires returning to the dispatch loop this call
    // stack is itself parked inside of) -- and is provably unnecessary:
    // esp_timer dispatch is one callback at a time on one task, so if
    // *this* call stack is already on that task, this Timer's own
    // trampoline() cannot be concurrently running, and esp_timer_stop()
    // just above already prevented it from being dispatched later from
    // the list (dispatch can't have returned to the list yet -- we're
    // still inside the other callback).
    if (!is_on_esp_timer_task()) {
        // Block until any cb() invocation already in flight (started
        // before esp_timer_stop() above could prevent it) has returned.
        xSemaphoreTake(impl_->running_mutex, portMAX_DELAY);
        xSemaphoreGive(impl_->running_mutex);
        // The running_mutex_ wait above is not by
        // itself enough to rule out the race described in this
        // section's file-header comment (a callback already popped off
        // esp_timer's list, but not yet far enough into trampoline() to
        // have taken running_mutex_, at the moment esp_timer_stop() and
        // the running_mutex_ check above both ran). Fence against the
        // esp_timer task itself: arming fence_handle_ for 0 delay and
        // waiting for it to fire cannot return until the esp_timer task
        // has processed everything that was already ahead of it in the
        // dispatch order, which -- because dispatch is strictly one
        // callback at a time on one task -- includes fully finishing
        // any trampoline() call that was already in progress or already
        // popped off the list when this stop() call reached this point.
        esp_err_t fence_err = esp_timer_start_once(impl_->fence_handle, 0);
        if (fence_err == ESP_OK) {
            xSemaphoreTake(impl_->fence_sem, portMAX_DELAY);
        }
        // (If starting the fence itself failed -- e.g. transient
        // ESP_ERR_INVALID_STATE if it somehow raced its own still-pending
        // instance -- there is nothing more stop() can do to wait
        // further; this is the same best-effort posture as
        // esp_timer_stop()'s return value, already ignored above.)
    }
    impl_->active.store(false);
    xSemaphoreGive(impl_->control_mutex);
}

bool Timer::active() const { return impl_->active.load(); }

}  // namespace wisp::os
