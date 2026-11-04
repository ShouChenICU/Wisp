#pragma once

// The LVGL 9.5 runtime over wisp::hal::Display/Touch: a dedicated UI task
// that owns every lv_* call, a post() queue other threads use to schedule
// work onto it, aligned partial flushes (see align.hpp), an off-thread
// snapshot_rgb888(), and a rolling 1s perf window. See AGENTS.md's "UI
// 单线程" rule: nothing outside UiRuntime's own UI task may call lv_*
// directly.
//
// This header (like ui_runtime.cpp) is built against the exact same
// components/wisp_ui/config/lv_conf.h on both the PC simulator and the
// firmware build -- it must stay free of esp_*/freertos*/SDL*
// headers (see wisp/os/os.hpp for the same portability rule). Including
// "lvgl.h" itself is fine: LVGL itself is portable, and wisp_ui's whole
// purpose is to be the portable LVGL wrapper.

#include <cstdint>
#include <functional>
#include <memory>
#include <vector>

#include "lvgl.h"
#include "wisp/base/result.hpp"
#include "wisp/hal/display.hpp"
#include "wisp/hal/touch.hpp"

namespace wisp::ui {

// The draw buffers' size and placement are the display's to declare, in
// its caps (wisp/hal/draw_buf_policy.hpp), not the runtime's.
struct UiConfig {
    uint32_t task_stack = 8192;
    int task_priority = 4;
};

// A thread-safe snapshot of the UI task's activity. `fps`, `cpu_percent`
// and `avg_flush_ms` are all computed over a rolling ~1s window (see
// UiRuntime::perf()); `frames` is the odd one out -- a lifetime total
// (not windowed): lv_display_flush_is_last() became true this many times
// since start(). `avg_flush_ms` is how long the UI task spends on the
// display per stripe: starting the transfer (Display::flush_async()) plus
// waiting for it (Display::wait_flush()) when LVGL needs the buffer back.
struct UiPerf {
    float fps = 0;
    uint8_t cpu_percent = 0;
    float avg_flush_ms = 0;
    uint32_t frames = 0;
};

// The default UI font: lv_font_montserrat_16, with the built-in
// lv_font_cjk_16 (GB2312 plus Japanese kana and level-1 kanji, 4bpp; see
// tools/gen_cjk_font.py) as its fallback so Chinese and Japanese text
// falls back to it glyph-by-glyph. Both are drawn from a cache of glyphs
// expanded to 8 bits per pixel (src/a8_font.hpp), with the same pixels.
// Lazily constructed on first call (thread-safe, C++11 magic statics);
// safe to call from any thread, before or after any UiRuntime exists.
// UiRuntime::start() installs this as the default theme font; exposed at
// namespace scope since it doesn't depend on any UiRuntime instance (e.g.
// for wisp_app's self-check UI).
const lv_font_t* default_font();

// Runs a single-threaded LVGL 9.5 instance over a wisp::hal::Display (and,
// optionally, a wisp::hal::Touch -- pass nullptr to run display-only).
class UiRuntime {
public:
    UiRuntime(std::shared_ptr<hal::Display> display, std::shared_ptr<hal::Touch> touch,
              UiConfig config = {});

    // Calls stop() if start() succeeded and stop() wasn't already called.
    ~UiRuntime();

    UiRuntime(const UiRuntime&) = delete;
    UiRuntime& operator=(const UiRuntime&) = delete;

    // Calls lv_init(), creates the LVGL display (resolution taken from
    // `display`'s caps) with two partial draw buffers sized and placed
    // by hal::draw_buf_policy() from the same caps, in the byte order of caps
    // "rgb565_be",
    // installs default_font() as the theme font, registers the
    // LV_EVENT_INVALIDATE_AREA handler that aligns flush areas to the
    // display's caps "area_align" (default 1, i.e. unaligned), wires up
    // the pointer input device if `touch` is non-null, and starts the UI
    // task. Fails with Busy if another UiRuntime in this process is
    // already started (LVGL 9.5 has only one global instance under
    // LV_USE_OS == LV_OS_NONE); fails with InvalidArgument if `display`'s
    // caps don't carry a usable width/height; fails with NoMemory if the
    // draw buffers, the LVGL display, or (when `touch` is non-null) the
    // LVGL input device can't be allocated.
    Result<void> start();

    // Stops the UI task -- after it finishes draining any in-flight
    // post()'d work and the current lv_timer_handler() call -- then waits
    // for the display's last transfer, calls lv_deinit() and frees the
    // draw buffers. Idempotent; safe to call even if start() was never
    // called or failed. Calling it from the UI
    // task itself (e.g. from inside a post()'d function) would deadlock on
    // self-join, so that case is detected instead: it logs via WLOG_E and
    // returns immediately without stopping anything.
    void stop();

    // Queues `fn` to run on the UI task on its next loop iteration.
    // Returns false (without running it) if the post queue (fixed capacity
    // 32) is full. Safe to call from any thread, including the UI task
    // itself (e.g. to chain a follow-up action).
    bool post(std::function<void()> fn);

    // True only when called from within the UI task (e.g. from inside a
    // post()'d function, or an LVGL event/timer callback it triggered).
    bool on_ui_thread() const;

    // Pauses/resumes rendering (spec 6.7's standby). Safe to call from
    // any thread, including the UI task itself. While paused, the UI
    // task never calls lv_timer_handler() (so no LVGL timer fires and no
    // flush happens) -- it only blocks waiting for, and running,
    // whatever is post()'d to it, exactly like the unpaused loop already
    // does on top of its own lv_timer_handler() call. This is purely an
    // atomic flag: pausing never blocks, and never fights stop() for the
    // UI task (ui_main() keeps checking `running` -- see
    // ui_runtime.cpp -- even while the paused branch is blocked waiting
    // for post()'d work, so stop() still completes promptly).
    //
    // Unpausing (set_paused(false)) is picked up by the UI task itself
    // on its next loop iteration, which first calls
    // lv_obj_invalidate(lv_screen_active()) (forcing a full redraw --
    // whatever was invalidated while paused was never actually drawn)
    // before resuming the normal lv_timer_handler() loop.
    void set_paused(bool paused);

    // Thread-safe. Reflects what set_paused() last requested -- see its
    // doc comment for why this is *not* the same as "ui_main() has
    // actually reached its paused branch yet" (there can be up to one
    // loop iteration of lag, same as on_ui_thread()'s active_task mirror).
    bool paused() const;

    // spec 6.7: installs `cb`, called (on the UI task, from
    // inside the pointer input device's read callback) every time a
    // touch read returns at least one point. Used by
    // wisp::app::PowerManager to reset its idle clock on touch activity
    // without adding a second reader of `touch` (PowerManager still owns
    // Touch::set_power(), but never calls Touch::read() itself). A no-op
    // if this UiRuntime has no touch indev (constructed with
    // `touch == nullptr`). Thread-safe; pass an empty std::function to
    // remove a previously installed callback.
    void set_touch_activity_cb(std::function<void()> cb);

    // Takes an lv_snapshot_take() of the active screen with the top layer
    // (cards, dialogs) blended over it -- what the panel shows, minus the
    // sys layer (see perf_overlay_enabled()). Safe to call from
    // any thread: off the UI task, it posts the snapshot work and blocks
    // the calling thread for up to `timeout_ms` for it to finish; called
    // from the UI task itself (e.g. from inside a post()'d function), it
    // runs immediately instead of posting -- posting-and-blocking there
    // would freeze the UI task waiting on its own queue, which can only
    // drain by... the UI task running it, i.e. a deadlock. On success,
    // returns width*height*3 bytes: row-major, 3 bytes/pixel (R, G, B, in
    // that order -- LVGL's own RGB888 draw buffers store B,G,R, which this
    // swaps back), no padding between rows.
    Result<std::vector<uint8_t>> snapshot_rgb888(uint32_t timeout_ms = 2000);

    // A thread-safe snapshot of the last ~1s of UI task activity; see
    // UiPerf.
    UiPerf perf() const;

    // Shows/hides LVGL's built-in sysmon performance overlay (FPS + CPU%,
    // bottom-mid; see LV_USE_PERF_MONITOR in lv_conf.h). Posted to the UI
    // task; returns immediately without waiting for it to run.
    void set_perf_overlay(bool enabled);

    // Whether the sysmon overlay set_perf_overlay() last requested has
    // actually taken effect yet. Thread-safe. Note this is *not*
    // reflected in snapshot_rgb888(): the overlay lives in LVGL's
    // always-on-top "sys layer", a sibling of the active screen, and
    // snapshot_rgb888() composites the top layer but never the sys layer,
    // so no pixel comparison of its output can ever detect the overlay. This accessor is the only
    // way to observe it.
    bool perf_overlay_enabled() const;

private:
    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace wisp::ui
