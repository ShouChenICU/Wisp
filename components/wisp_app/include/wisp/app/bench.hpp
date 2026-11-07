#pragma once

// Rendering benchmarks for the serial console, so frame-rate numbers come
// from a fixed, repeatable workload instead of a finger on the screen.
//
//   bench scroll [seconds]   WISP_BENCH {"bench":"scroll","cpu":..,"fps":..,
//                                        "frames":..,"ms":..,"px":6}
//   bench drag [seconds]     WISP_BENCH {"bench":"drag","cpu":..,"fps":..,
//                                        "frames":..,"ms":..,"navs":..,"px":16}
//
// The scroll bench is the method the reference board's rendering baseline
// was measured with (docs/hardware/waveshare_esp32_s3_touch_amoled_2_16.md,
// "性能基准", the "raw" column): on the UI thread, the active screen's main
// scrollable object moves 6 px per rendered frame with
// lv_obj_scroll_by_raw(), between one SCROLL_BEGIN and one SCROLL_END as in
// a finger drag, turning around at either end. Afterwards it goes back to
// where it was, so back-to-back runs start from the same place.
//
// fps counts LVGL's REFR_READY events, the frames UiRuntime::perf() counts
// too: (frames - 1) over the time from the first to the last. cpu is the
// mean of UiRuntime::perf()'s cpu_percent, sampled once a second after the
// first second (the wall time spent in lv_timer_handler(), as in WISP_PERF).
// The drag bench (mvp spec 6.3) moves whole pages instead: it brings the
// home screen to the front and, through an input device of its own that
// LVGL reads like the touch panel (so the shell's real drag path runs),
// drags a finger right from 1/8 to 7/8 of the screen's width kDragBenchStepPx
// a read, which opens Settings, lets go, rests while the page slides in
// (kSlideMs and kDragBenchRestMs), and drags back the other way, and so on.
// The finger's row is just under the status capsule, Settings' title.
// fps and cpu are as in the scroll bench, frames counted from the start
// (the rests included); navs is how many pages it reached. At the end it
// goes home again.
//
// The commands block the console for the whole run and keep the screen
// from going off meanwhile; they refuse to start while the screen is off.

#include <cstdint>
#include <string>

#include "lvgl.h"
#include "wisp/app/app.hpp"
#include "wisp/base/result.hpp"

namespace wisp::app {

struct ScrollBenchResult {
    double fps = 0;
    uint32_t frames = 0;
    // From the first frame to the last.
    uint32_t ms = 0;
    uint8_t cpu = 0;
    int px = 0;
};

// Pixels per frame, as in the baseline.
inline constexpr int kScrollBenchStepPx = 6;
inline constexpr uint32_t kScrollBenchDefaultSeconds = 10;
inline constexpr uint32_t kScrollBenchMaxSeconds = 120;

// The object a scroll bench moves on `screen`: of `screen` and everything
// under it that is shown, scrollable and has content beyond its box
// vertically, the one with the largest area; nullptr if none. UI thread
// only; updates the screen's layout first.
lv_obj_t* main_scrollable(lv_obj_t* screen);

// Runs the scroll bench for `seconds` on `app`'s active screen and blocks
// until it is done. Fails with Unsupported without a display, Busy while
// the screen is off, NotFound when nothing on the screen scrolls, and
// InvalidArgument for 0 or more than kScrollBenchMaxSeconds seconds. Not
// on the UI thread (it waits for frames the UI thread renders).
Result<ScrollBenchResult> run_scroll_bench(App& app, uint32_t seconds);

// "WISP_BENCH " followed by the result as one line of JSON.
std::string scroll_bench_line(const ScrollBenchResult& result);

struct DragBenchResult {
    double fps = 0;
    uint32_t frames = 0;
    uint32_t ms = 0;
    uint8_t cpu = 0;
    int px = 0;
    // Pages reached (Settings and Home each count).
    uint32_t navs = 0;
};

// The finger's step per touch read, and its rest after each release.
inline constexpr int kDragBenchStepPx = 16;
inline constexpr uint32_t kDragBenchRestMs = 80;

// Runs the drag bench for `seconds` and blocks until it is done. Fails as
// run_scroll_bench() does, with Unsupported without a shell and Busy when
// the home screen cannot be brought in front.
Result<DragBenchResult> run_drag_bench(App& app, uint32_t seconds);

std::string drag_bench_line(const DragBenchResult& result);

// The `bench` console command, for Platform::console_commands.
ConsoleCommand bench_command();

}  // namespace wisp::app
