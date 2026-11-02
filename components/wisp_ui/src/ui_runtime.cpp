#include "wisp/ui/ui_runtime.hpp"

// Sanity check: this translation unit must be compiled against wisp_ui's
// own components/wisp_ui/config/lv_conf.h (see AGENTS.md's "LVGL 配置"
// trap and cmake/wisp_host_deps.cmake's LV_BUILD_CONF_PATH wiring), not
// some other lv_conf.h LVGL happened to find via its __has_include probe.
#ifndef WISP_LV_CONF
#error "wisp lv_conf.h not in use"
#endif

static_assert(LV_COLOR_DEPTH == 16, "wisp_ui requires LV_COLOR_DEPTH == 16 (RGB565)");

#include <algorithm>
#include <atomic>
#include <cstring>
#include <deque>
#include <mutex>
#include <utility>

#include "wisp/base/error.hpp"
#include "wisp/core/log.hpp"
#include "wisp/os/os.hpp"
#include "wisp/os/queue.hpp"
#include "wisp/ui/align.hpp"

// lv_font_cjk_16.c is a plain C translation unit (ported from MyOS; see
// components/wisp_ui/fonts/lv_font_cjk_16.c), so its one exported symbol
// has C linkage.
extern "C" {
extern const lv_font_t lv_font_cjk_16;
}

namespace wisp::ui {

namespace {

constexpr size_t kPostQueueCapacity = 32;
constexpr uint64_t kPerfWindowMs = 1000;

int cap_int(const wisp::Properties& caps, const char* key, int fallback) {
    auto it = caps.find(key);
    if (it == caps.end()) {
        return fallback;
    }
    return static_cast<int>(it->second.as_int());
}

void prune_frame_times(std::deque<uint64_t>& samples, uint64_t now) {
    uint64_t cutoff = now > kPerfWindowMs ? now - kPerfWindowMs : 0;
    while (!samples.empty() && samples.front() < cutoff) {
        samples.pop_front();
    }
}

void prune_duration_samples(std::deque<std::pair<uint64_t, uint32_t>>& samples, uint64_t now) {
    uint64_t cutoff = now > kPerfWindowMs ? now - kPerfWindowMs : 0;
    while (!samples.empty() && samples.front().first < cutoff) {
        samples.pop_front();
    }
}

}  // namespace

const lv_font_t* default_font() {
    // lv_font_montserrat_16 is `const`, so it can't be modified in place;
    // this is a mutable copy with `fallback` pointed at the CJK font.
    // Function-local static: lazily constructed exactly once (C++11 magic
    // statics are thread-safe), the first time anyone (UiRuntime::start()
    // or an outside caller, e.g. Task 12) asks for it.
    static lv_font_t s_ui_font = [] {
        lv_font_t font = lv_font_montserrat_16;
        font.fallback = &lv_font_cjk_16;
        return font;
    }();
    return &s_ui_font;
}

struct UiRuntime::Impl {
    std::shared_ptr<hal::Display> display;
    std::shared_ptr<hal::Touch> touch;
    UiConfig config;

    os::Queue<std::function<void()>> post_queue{kPostQueueCapacity};
    std::unique_ptr<os::Task> task;
    std::atomic<bool> running{false};

    // on_ui_thread() reads this instead of `task` directly: `task` itself
    // (the unique_ptr) is only ever written by whichever thread calls
    // start()/stop(), with no synchronization against a concurrent reader
    // -- a real data race if on_ui_thread() dereferenced it directly. This
    // atomic mirror is written once, right after `task` is assigned in
    // start(), and cleared in stop() before `task` is reset, so every read
    // through it is race-free regardless of which thread is asking.
    std::atomic<os::Task*> active_task{nullptr};

    int width = 0;
    int height = 0;
    int area_align = 1;

    void* buf1 = nullptr;
    void* buf2 = nullptr;

    lv_display_t* lv_disp = nullptr;
    lv_indev_t* lv_indev = nullptr;

    // Mirrors whatever the sysmon overlay's hidden flag actually is,
    // written from inside the very lambda set_perf_overlay() posts (so it
    // reflects reality, not just "a call was requested") -- see
    // perf_overlay_enabled() below. Readable from any thread.
    std::atomic<bool> perf_overlay_enabled{false};

    // Task 18b/spec 6.7: set by set_paused(), read at the top of every
    // ui_main() loop iteration. Just an atomic flag -- see
    // UiRuntime::set_paused()'s doc comment for why that's enough (no
    // extra synchronization needed against stop(), post(), or the
    // normal lv_timer_handler() path).
    std::atomic<bool> paused{false};

    // Guards touch_activity_cb against a set_touch_activity_cb() call
    // racing indev_read_cb() reading/invoking it (both can happen from
    // any thread/the UI task respectively). Taken only briefly on each
    // side -- see indev_read_cb(), which copies the std::function out
    // under the lock and invokes the copy after releasing it, so a
    // reentrant call from inside the callback itself (e.g. back into
    // this UiRuntime) can't deadlock on it.
    os::Mutex touch_activity_mutex;
    std::function<void()> touch_activity_cb;

    // --- perf (protected by perf_mutex) ---
    mutable os::Mutex perf_mutex;
    uint32_t frames_total = 0;
    std::deque<uint64_t> frame_times;                         // ms timestamps
    std::deque<std::pair<uint64_t, uint32_t>> flush_samples;  // (ts_ms, duration_ms)
    std::deque<std::pair<uint64_t, uint32_t>> busy_samples;   // (ts_ms, duration_ms)

    void ui_main();
    void drain_post_queue();
    void record_busy(uint32_t duration_ms);
    void record_flush(uint32_t duration_ms);
    void record_frame();
    Result<std::vector<uint8_t>> take_snapshot();

    static void flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map);
    static void invalidate_area_cb(lv_event_t* e);
    static void indev_read_cb(lv_indev_t* indev, lv_indev_data_t* data);
};

void UiRuntime::Impl::drain_post_queue() {
    std::function<void()> fn;
    while (post_queue.pop(fn, 0)) {
        fn();
        fn = nullptr;  // release captured state before the next pop
    }
}

void UiRuntime::Impl::ui_main() {
    // Tracks whether the *previous* iteration was in the paused branch
    // below, so the exact loop iteration where paused flips true->false
    // is detected here (on the UI task, the only place lv_* may be
    // called) and gets exactly one lv_obj_invalidate() -- see
    // UiRuntime::set_paused()'s doc comment.
    bool was_paused = false;

    while (running.load(std::memory_order_acquire)) {
        if (paused.load(std::memory_order_acquire)) {
            was_paused = true;
            // Blocks indefinitely waiting for post()'d work -- no LVGL
            // timer fires and no flush happens while paused, and (M1,
            // fix round 1) no periodic wake happens either: spec 6.7
            // requires standby to have "没有周期性的 UI 定时器，也没有
            // 高频轮询", so this must not poll on any fixed interval of
            // its own. A concurrent post() wakes this immediately
            // (Queue::push() signals the same semaphore pop() waits on);
            // stop() additionally pushes a no-op job after flipping
            // `running` to false specifically to wake a UI task that
            // might currently be blocked right here (see
            // UiRuntime::stop()) -- without that, a paused UI task would
            // never notice `running` went false until the next
            // post()'d job happened to arrive, which might be never.
            std::function<void()> fn;
            if (post_queue.pop(fn, os::kForever)) {
                fn();
                fn = nullptr;
            }
            continue;
        }
        if (was_paused) {
            // Just resumed: whatever was invalidated while paused was
            // never actually drawn (lv_timer_handler() never ran), so
            // force one full redraw now, before the normal loop below
            // resumes calling it.
            lv_obj_invalidate(lv_screen_active());
            was_paused = false;
        }

        uint64_t loop_start = os::now_ms();
        drain_post_queue();
        uint32_t next_ms = lv_timer_handler();
        uint64_t loop_end = os::now_ms();
        record_busy(static_cast<uint32_t>(loop_end - loop_start));

        uint32_t sleep_ms = std::min<uint32_t>(next_ms, 10);
        os::sleep_ms(sleep_ms);
    }
    // stop() may have post()'d a final piece of work (or one may already
    // have been queued) right before flipping `running` to false; drain it
    // now so it still runs before the caller tears LVGL down.
    drain_post_queue();
}

void UiRuntime::Impl::record_busy(uint32_t duration_ms) {
    uint64_t now = os::now_ms();
    std::lock_guard<os::Mutex> lock(perf_mutex);
    busy_samples.emplace_back(now, duration_ms);
    prune_duration_samples(busy_samples, now);
}

void UiRuntime::Impl::record_flush(uint32_t duration_ms) {
    uint64_t now = os::now_ms();
    std::lock_guard<os::Mutex> lock(perf_mutex);
    flush_samples.emplace_back(now, duration_ms);
    prune_duration_samples(flush_samples, now);
}

void UiRuntime::Impl::record_frame() {
    uint64_t now = os::now_ms();
    std::lock_guard<os::Mutex> lock(perf_mutex);
    ++frames_total;
    frame_times.push_back(now);
    prune_frame_times(frame_times, now);
}

Result<std::vector<uint8_t>> UiRuntime::Impl::take_snapshot() {
    lv_draw_buf_t* buf = lv_snapshot_take(lv_screen_active(), LV_COLOR_FORMAT_RGB888);
    if (buf == nullptr) {
        return Result<std::vector<uint8_t>>::err(ErrorCode::DeviceFailure,
                                                 "lv_snapshot_take failed");
    }

    size_t row_bytes = static_cast<size_t>(width) * 3;
    std::vector<uint8_t> out(row_bytes * static_cast<size_t>(height));
    for (int y = 0; y < height; ++y) {
        const uint8_t* src = buf->data + static_cast<size_t>(y) * buf->header.stride;
        uint8_t* dst = out.data() + static_cast<size_t>(y) * row_bytes;
        for (int x = 0; x < width; ++x) {
            // LVGL's LV_COLOR_FORMAT_RGB888 draw buffers store each pixel
            // as (blue, green, red) -- see lv_color_t in lv_color.h and
            // the fill loop in lv_draw_sw_blend_to_rgb888.c -- but this
            // function is documented (and tested) to return (R, G, B).
            dst[x * 3 + 0] = src[x * 3 + 2];  // R
            dst[x * 3 + 1] = src[x * 3 + 1];  // G
            dst[x * 3 + 2] = src[x * 3 + 0];  // B
        }
    }

    lv_draw_buf_destroy(buf);
    return Result<std::vector<uint8_t>>::ok(std::move(out));
}

void UiRuntime::Impl::flush_cb(lv_display_t* disp, const lv_area_t* area, uint8_t* px_map) {
    auto* impl = static_cast<Impl*>(lv_display_get_user_data(disp));
    hal::Area hal_area{area->x1, area->y1, area->x2, area->y2};

    uint64_t t0 = os::now_ms();
    impl->display->flush(hal_area, reinterpret_cast<const uint16_t*>(px_map));
    uint64_t t1 = os::now_ms();
    impl->record_flush(static_cast<uint32_t>(t1 - t0));

    lv_display_flush_ready(disp);

    if (lv_display_flush_is_last(disp)) {
        impl->record_frame();
    }
}

void UiRuntime::Impl::invalidate_area_cb(lv_event_t* e) {
    auto* impl = static_cast<Impl*>(lv_event_get_user_data(e));
    lv_area_t* area = lv_event_get_invalidated_area(e);

    hal::Area in{area->x1, area->y1, area->x2, area->y2};
    hal::Area aligned = align_area(in, impl->area_align, impl->width, impl->height);

    area->x1 = aligned.x1;
    area->y1 = aligned.y1;
    area->x2 = aligned.x2;
    area->y2 = aligned.y2;
}

void UiRuntime::Impl::indev_read_cb(lv_indev_t* indev, lv_indev_data_t* data) {
    auto* impl = static_cast<Impl*>(lv_indev_get_user_data(indev));
    auto points = impl->touch->read();
    if (points && !points.value().empty()) {
        const hal::TouchPoint& pt = points.value().front();
        data->point.x = pt.x;
        data->point.y = pt.y;
        data->state = LV_INDEV_STATE_PRESSED;

        std::function<void()> cb;
        {
            std::lock_guard<os::Mutex> lock(impl->touch_activity_mutex);
            cb = impl->touch_activity_cb;
        }
        if (cb) {
            cb();
        }
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

UiRuntime::UiRuntime(std::shared_ptr<hal::Display> display, std::shared_ptr<hal::Touch> touch,
                     UiConfig config)
    : impl_(std::make_unique<Impl>()) {
    impl_->display = std::move(display);
    impl_->touch = std::move(touch);
    impl_->config = config;
}

UiRuntime::~UiRuntime() { stop(); }

Result<void> UiRuntime::start() {
    if (impl_->running.load()) {
        return Result<void>::err(ErrorCode::AlreadyExists, "UiRuntime already started");
    }
    if (!impl_->display) {
        return Result<void>::err(ErrorCode::InvalidArgument, "display must not be null");
    }
    if (lv_is_initialized()) {
        WLOG_W("ui", "lv_init() already active; a previous UiRuntime was not stopped");
        return Result<void>::err(
            ErrorCode::Busy, "LVGL is already initialized by another UiRuntime in this process");
    }

    const wisp::Properties& caps = impl_->display->info().caps;
    impl_->width = cap_int(caps, "width", 0);
    impl_->height = cap_int(caps, "height", 0);
    impl_->area_align = cap_int(caps, "area_align", 1);
    if (impl_->width <= 0 || impl_->height <= 0) {
        return Result<void>::err(ErrorCode::InvalidArgument,
                                 "display caps missing a usable width/height");
    }

    size_t buf_bytes = static_cast<size_t>(impl_->width) * impl_->config.buffer_lines * 2;
    impl_->buf1 = os::alloc(buf_bytes, os::Mem::Dma);
    impl_->buf2 = os::alloc(buf_bytes, os::Mem::Dma);
    if (impl_->buf1 == nullptr || impl_->buf2 == nullptr) {
        if (impl_->buf1 != nullptr) {
            os::dealloc(impl_->buf1);
            impl_->buf1 = nullptr;
        }
        if (impl_->buf2 != nullptr) {
            os::dealloc(impl_->buf2);
            impl_->buf2 = nullptr;
        }
        return Result<void>::err(ErrorCode::NoMemory, "failed to allocate LVGL draw buffers");
    }

    lv_init();
    lv_tick_set_cb([]() -> uint32_t { return static_cast<uint32_t>(os::now_ms()); });
    lv_log_register_print_cb([](lv_log_level_t level, const char* buf) {
        (void)level;
        // LVGL's print callback hands us a line that already ends in
        // '\n' (sometimes "\r\n"); trim it so WLOG_I doesn't produce a
        // blank line underneath every LVGL log message.
        size_t len = std::strlen(buf);
        while (len > 0 && (buf[len - 1] == '\n' || buf[len - 1] == '\r')) {
            --len;
        }
        WLOG_I("lvgl", "%.*s", static_cast<int>(len), buf);
    });

    // Tears down everything lv_init() above (and whatever ran before this
    // point in start()) set up, for a failure partway through the rest of
    // setup below.
    auto fail_after_lv_init = [this](ErrorCode code, const char* message) {
        lv_deinit();
        impl_->lv_disp = nullptr;
        impl_->lv_indev = nullptr;
        os::dealloc(impl_->buf1);
        impl_->buf1 = nullptr;
        os::dealloc(impl_->buf2);
        impl_->buf2 = nullptr;
        return Result<void>::err(code, message);
    };

    impl_->lv_disp = lv_display_create(impl_->width, impl_->height);
    if (impl_->lv_disp == nullptr) {
        return fail_after_lv_init(ErrorCode::NoMemory, "lv_display_create failed");
    }
    lv_display_set_user_data(impl_->lv_disp, impl_.get());
    lv_display_set_buffers(impl_->lv_disp, impl_->buf1, impl_->buf2,
                           static_cast<uint32_t>(buf_bytes), LV_DISPLAY_RENDER_MODE_PARTIAL);
    lv_display_set_flush_cb(impl_->lv_disp, &Impl::flush_cb);
    lv_display_add_event_cb(impl_->lv_disp, &Impl::invalidate_area_cb, LV_EVENT_INVALIDATE_AREA,
                            impl_.get());
    lv_display_set_default(impl_->lv_disp);

    // lv_display_create() itself calls lv_sysmon_show_performance() when
    // LV_USE_PERF_MONITOR is on (see src/display/lv_display.c), so the
    // overlay is visible the moment the display exists -- undo that here
    // so it starts hidden, as lv_conf.h's comment promises; the label it
    // just created stays around, so later show/hide calls (from
    // set_perf_overlay()) can use it unconditionally.
    lv_sysmon_hide_performance(impl_->lv_disp);

    lv_theme_t* theme =
        lv_theme_default_init(impl_->lv_disp, lv_palette_main(LV_PALETTE_BLUE),
                              lv_palette_main(LV_PALETTE_RED), false, default_font());
    lv_display_set_theme(impl_->lv_disp, theme);

    if (impl_->touch) {
        impl_->lv_indev = lv_indev_create();
        if (impl_->lv_indev == nullptr) {
            return fail_after_lv_init(ErrorCode::NoMemory, "lv_indev_create failed");
        }
        lv_indev_set_type(impl_->lv_indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_display(impl_->lv_indev, impl_->lv_disp);
        lv_indev_set_user_data(impl_->lv_indev, impl_.get());
        lv_indev_set_read_cb(impl_->lv_indev, &Impl::indev_read_cb);
    }

    impl_->running.store(true, std::memory_order_release);
    os::TaskConfig task_config;
    task_config.name = "wisp_ui";
    task_config.stack_bytes = impl_->config.task_stack;
    task_config.priority = impl_->config.task_priority;
    Impl* impl = impl_.get();
    impl_->task = os::Task::spawn(task_config, [impl] { impl->ui_main(); });
    impl_->active_task.store(impl_->task.get(), std::memory_order_release);

    return Result<void>::ok();
}

void UiRuntime::stop() {
    if (on_ui_thread()) {
        // task->join() below would be a self-join (deadlock): the UI task
        // can't wait for itself to finish. There's no safe way to honor
        // this call from here, so refuse it instead of hanging.
        WLOG_E("ui", "UiRuntime::stop() called from the UI task itself; ignoring");
        return;
    }
    if (!impl_->running.exchange(false, std::memory_order_acq_rel)) {
        return;
    }
    // M1 (fix round 1): ui_main()'s paused branch now blocks forever
    // (os::kForever) waiting for post()'d work, rather than polling
    // every kPausedPollMs -- so if the UI task happens to be paused and
    // sitting in that wait right now, it will never notice `running`
    // just went false on its own. Push a no-op to wake it; if the queue
    // is already full, there is pending work that will wake it on its
    // own (either way, join() below will see it exit promptly).
    impl_->post_queue.push([] {}, 0);
    if (impl_->task) {
        impl_->task->join();
    }
    impl_->active_task.store(nullptr, std::memory_order_release);
    impl_->task.reset();

    // The UI task has fully exited: no further lv_* call is in flight, so
    // tearing LVGL down now is safe.
    lv_deinit();
    impl_->lv_disp = nullptr;
    impl_->lv_indev = nullptr;

    if (impl_->buf1 != nullptr) {
        os::dealloc(impl_->buf1);
        impl_->buf1 = nullptr;
    }
    if (impl_->buf2 != nullptr) {
        os::dealloc(impl_->buf2);
        impl_->buf2 = nullptr;
    }
}

bool UiRuntime::post(std::function<void()> fn) { return impl_->post_queue.push(std::move(fn), 0); }

bool UiRuntime::on_ui_thread() const {
    os::Task* task = impl_->active_task.load(std::memory_order_acquire);
    return task != nullptr && task->is_current();
}

void UiRuntime::set_paused(bool paused) {
    bool was_paused = impl_->paused.exchange(paused, std::memory_order_acq_rel);
    // M1 (fix round 1): ui_main()'s paused branch blocks forever
    // (os::kForever) waiting for post()'d work, rather than polling --
    // so unpausing (paused: true -> false) must itself wake a UI task
    // that might be sitting in that wait right now, or it would never
    // notice `paused` went false until some *other* post()'d job
    // happened to arrive on its own. Only needed on an actual true->
    // false transition (not a redundant set_paused(false) while already
    // unpaused, nor set_paused(true), which doesn't need waking
    // anything up). If the queue is already full, there is pending work
    // that will wake it on its own either way.
    if (was_paused && !paused) {
        impl_->post_queue.push([] {}, 0);
    }
}

bool UiRuntime::paused() const { return impl_->paused.load(std::memory_order_acquire); }

void UiRuntime::set_touch_activity_cb(std::function<void()> cb) {
    std::lock_guard<os::Mutex> lock(impl_->touch_activity_mutex);
    impl_->touch_activity_cb = std::move(cb);
}

Result<std::vector<uint8_t>> UiRuntime::snapshot_rgb888(uint32_t timeout_ms) {
    if (on_ui_thread()) {
        // Posting-and-blocking here would deadlock: the posted lambda can
        // only run once this same UI task reaches its next
        // drain_post_queue() call, which can't happen while this call is
        // still blocked waiting for it. Just take the snapshot directly.
        return impl_->take_snapshot();
    }

    struct SnapshotState {
        os::Semaphore done{1, 0};
        Result<std::vector<uint8_t>> result =
            Result<std::vector<uint8_t>>::err(ErrorCode::Timeout, "snapshot did not complete");
    };
    auto state = std::make_shared<SnapshotState>();
    Impl* impl = impl_.get();

    bool posted = post([impl, state] {
        state->result = impl->take_snapshot();
        state->done.give();
    });
    if (!posted) {
        return Result<std::vector<uint8_t>>::err(ErrorCode::Busy, "UI post queue full");
    }
    if (!state->done.take(timeout_ms)) {
        return Result<std::vector<uint8_t>>::err(ErrorCode::Timeout, "snapshot timed out");
    }
    return state->result;
}

UiPerf UiRuntime::perf() const {
    uint64_t now = os::now_ms();
    std::lock_guard<os::Mutex> lock(impl_->perf_mutex);

    prune_frame_times(impl_->frame_times, now);
    prune_duration_samples(impl_->flush_samples, now);
    prune_duration_samples(impl_->busy_samples, now);

    UiPerf p;
    p.frames = impl_->frames_total;

    // Task 18b/spec 6.7: while paused, ui_main() never runs
    // lv_timer_handler()/flush_cb(), so busy_samples/flush_samples
    // simply decay to empty (and frame_times stop growing) as the 1s
    // window slides past the last real activity -- fps/cpu_percent/
    // avg_flush_ms would eventually read 0 on their own, but only after
    // up to kPerfWindowMs of a stale, decaying reading in the meantime.
    // Reporting the window as empty immediately instead avoids ever
    // showing a bogus non-zero FPS/CPU% for a UI that is not currently
    // rendering at all. `frames` is a lifetime total (see UiPerf's doc
    // comment), so it is reported as-is either way.
    if (impl_->paused.load(std::memory_order_acquire)) {
        return p;
    }
    // The window is exactly kPerfWindowMs (1s) by construction, so the
    // frame count already *is* the rate (frames per second).
    p.fps = static_cast<float>(impl_->frame_times.size());

    uint32_t busy_total_ms = 0;
    for (const auto& sample : impl_->busy_samples) {
        busy_total_ms += sample.second;
    }
    float cpu = static_cast<float>(busy_total_ms) / static_cast<float>(kPerfWindowMs) * 100.0f;
    cpu = std::max(0.0f, std::min(100.0f, cpu));
    p.cpu_percent = static_cast<uint8_t>(cpu + 0.5f);

    if (!impl_->flush_samples.empty()) {
        uint32_t flush_total_ms = 0;
        for (const auto& sample : impl_->flush_samples) {
            flush_total_ms += sample.second;
        }
        p.avg_flush_ms =
            static_cast<float>(flush_total_ms) / static_cast<float>(impl_->flush_samples.size());
    }

    return p;
}

void UiRuntime::set_perf_overlay(bool enabled) {
    // The sysmon label already exists by the time start() returns (see
    // the lv_sysmon_hide_performance() call there), so both of these are
    // always safe to call unconditionally.
    Impl* impl = impl_.get();
    post([impl, enabled] {
        if (enabled) {
            lv_sysmon_show_performance(impl->lv_disp);
        } else {
            lv_sysmon_hide_performance(impl->lv_disp);
        }
        // Written here (inside the posted lambda, after the LVGL call
        // above has actually run), not synchronously in set_perf_overlay()
        // itself -- a caller of perf_overlay_enabled() right after
        // set_perf_overlay() returns is asking "has it taken effect yet",
        // not "was it requested".
        impl->perf_overlay_enabled.store(enabled, std::memory_order_release);
    });
}

bool UiRuntime::perf_overlay_enabled() const {
    return impl_->perf_overlay_enabled.load(std::memory_order_acquire);
}

}  // namespace wisp::ui
