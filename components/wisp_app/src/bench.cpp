#include "wisp/app/bench.hpp"

#include <algorithm>
#include <cstdlib>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "lvgl_private.h"  // lv_obj_scroll_by_raw()
#include "wisp/app/power_manager.hpp"
#include "wisp/base/json.hpp"
#include "wisp/base/value.hpp"
#include "wisp/os/os.hpp"
#include "wisp/ui/ui_runtime.hpp"

namespace wisp::app {

namespace {

constexpr uint32_t kSampleMs = 1000;
// How often the screen's idle clock is reset during a run, well inside the
// shortest screen-off timeout.
constexpr uint32_t kActivityMs = 500;

// One run's state, owned by run_scroll_bench() and touched only on the UI
// thread while the callbacks below are installed.
struct ScrollRun {
    lv_display_t* disp = nullptr;
    lv_obj_t* target = nullptr;  // nullptr once deleted
    int32_t start_y = 0;
    // Toward the bottom while true.
    bool down = true;
    uint32_t frames = 0;
    uint64_t first_us = 0;
    uint64_t last_us = 0;
};

// One step per frame, before LVGL lays out and renders it, so the frame
// shows it. The step invalidates the list, which keeps frames coming.
void refr_start_cb(lv_event_t* e) {
    auto* run = static_cast<ScrollRun*>(lv_event_get_user_data(e));
    lv_obj_t* obj = run->target;
    if (obj == nullptr) return;
    int32_t room = run->down ? lv_obj_get_scroll_bottom(obj) : lv_obj_get_scroll_top(obj);
    if (room <= 0) {
        run->down = !run->down;
        room = run->down ? lv_obj_get_scroll_bottom(obj) : lv_obj_get_scroll_top(obj);
    }
    const int32_t step = room < kScrollBenchStepPx ? room : kScrollBenchStepPx;
    if (step <= 0) return;
    // Negative dy moves the content up, toward the bottom of the list.
    lv_obj_scroll_by_raw(obj, 0, run->down ? -step : step);
}

void refr_ready_cb(lv_event_t* e) {
    auto* run = static_cast<ScrollRun*>(lv_event_get_user_data(e));
    const uint64_t now = os::now_us();
    if (run->frames == 0) run->first_us = now;
    run->last_us = now;
    ++run->frames;
}

void target_deleted_cb(lv_event_t* e) {
    static_cast<ScrollRun*>(lv_event_get_user_data(e))->target = nullptr;
}

int32_t scroll_range_y(lv_obj_t* obj) {
    return lv_obj_get_scroll_top(obj) + lv_obj_get_scroll_bottom(obj);
}

void find_largest(lv_obj_t* obj, lv_obj_t*& best, int64_t& best_area) {
    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) return;
    if (lv_obj_has_flag(obj, LV_OBJ_FLAG_SCROLLABLE) && scroll_range_y(obj) > 0) {
        const int64_t area = static_cast<int64_t>(lv_obj_get_width(obj)) * lv_obj_get_height(obj);
        if (area > best_area) {
            best = obj;
            best_area = area;
        }
    }
    const uint32_t n = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < n; ++i) find_largest(lv_obj_get_child(obj, i), best, best_area);
}

std::string usage_error() {
    return "error: usage: bench scroll [seconds 1-" + std::to_string(kScrollBenchMaxSeconds) + "]";
}

}  // namespace

lv_obj_t* main_scrollable(lv_obj_t* screen) {
    if (screen == nullptr) return nullptr;
    lv_obj_update_layout(screen);
    lv_obj_t* best = nullptr;
    int64_t best_area = 0;
    find_largest(screen, best, best_area);
    return best;
}

Result<ScrollBenchResult> run_scroll_bench(App& app, uint32_t seconds) {
    using R = Result<ScrollBenchResult>;
    if (seconds == 0 || seconds > kScrollBenchMaxSeconds) {
        return R::err(ErrorCode::InvalidArgument, "seconds out of range");
    }
    ui::UiRuntime* ui = app.ui();
    if (ui == nullptr) return R::err(ErrorCode::Unsupported, "no display");
    if (ui->on_ui_thread()) return R::err(ErrorCode::Busy, "called on the UI thread");
    PowerManager* power = app.power_manager();
    if (power != nullptr && power->standby()) return R::err(ErrorCode::Busy, "screen is off");
    if (power != nullptr) power->note_activity();

    auto run = std::make_unique<ScrollRun>();
    ScrollRun* r = run.get();
    bool started = false;
    ui->run_blocking([r, &started] {
        r->disp = lv_display_get_default();
        r->target = main_scrollable(lv_screen_active());
        if (r->disp == nullptr || r->target == nullptr) return;
        r->start_y = lv_obj_get_scroll_y(r->target);
        lv_obj_add_event_cb(r->target, &target_deleted_cb, LV_EVENT_DELETE, r);
        lv_obj_send_event(r->target, LV_EVENT_SCROLL_BEGIN, nullptr);
        lv_display_add_event_cb(r->disp, &refr_start_cb, LV_EVENT_REFR_START, r);
        lv_display_add_event_cb(r->disp, &refr_ready_cb, LV_EVENT_REFR_READY, r);
        // Nothing may be dirty yet; the first frame needs a reason to come.
        lv_obj_invalidate(r->target);
        started = true;
    });
    if (!started) return R::err(ErrorCode::NotFound, "nothing on the screen scrolls");

    const uint64_t start_ms = os::now_ms();
    const uint64_t end_ms = start_ms + static_cast<uint64_t>(seconds) * 1000;
    uint64_t next_sample = start_ms + 2 * kSampleMs;
    uint32_t cpu_sum = 0;
    uint32_t cpu_samples = 0;
    for (uint64_t now = start_ms; now < end_ms; now = os::now_ms()) {
        if (power != nullptr) power->note_activity();
        if (now >= next_sample) {
            cpu_sum += ui->perf().cpu_percent;
            ++cpu_samples;
            next_sample += kSampleMs;
        }
        const uint64_t wake = std::min<uint64_t>({end_ms, next_sample, now + kActivityMs});
        os::sleep_ms(static_cast<uint32_t>(wake - now));
    }
    // A run too short for the steady-state samples above takes one at the end.
    if (cpu_samples == 0) {
        cpu_sum = ui->perf().cpu_percent;
        cpu_samples = 1;
    }

    ui->run_blocking([r] {
        lv_display_remove_event_cb_with_user_data(r->disp, &refr_start_cb, r);
        lv_display_remove_event_cb_with_user_data(r->disp, &refr_ready_cb, r);
        if (r->target == nullptr) return;
        lv_obj_remove_event_cb_with_user_data(r->target, &target_deleted_cb, r);
        lv_obj_send_event(r->target, LV_EVENT_SCROLL_END, nullptr);
        lv_obj_scroll_to_y(r->target, r->start_y, LV_ANIM_OFF);
    });

    ScrollBenchResult result;
    result.frames = r->frames;
    result.px = kScrollBenchStepPx;
    result.cpu = static_cast<uint8_t>((cpu_sum + cpu_samples / 2) / cpu_samples);
    const uint64_t span_us = r->last_us - r->first_us;
    result.ms = static_cast<uint32_t>(span_us / 1000);
    if (r->frames >= 2 && span_us > 0) {
        result.fps = static_cast<double>(r->frames - 1) * 1e6 / static_cast<double>(span_us);
    }
    return R::ok(result);
}

std::string scroll_bench_line(const ScrollBenchResult& result) {
    // Two decimals: run-to-run noise on the device is about 0.05 fps.
    const double fps = static_cast<double>(static_cast<int64_t>(result.fps * 100 + 0.5)) / 100;
    Map payload{
        {"bench", Value("scroll")},
        {"fps", fps},
        {"frames", static_cast<int64_t>(result.frames)},
        {"ms", static_cast<int64_t>(result.ms)},
        {"cpu", static_cast<int64_t>(result.cpu)},
        {"px", static_cast<int64_t>(result.px)},
    };
    return "WISP_BENCH " + to_json(Value(payload));
}

ConsoleCommand bench_command() {
    return {"bench",
            "bench scroll [seconds]       scroll the screen's list 6 px a frame, report fps",
            [](App& app, const std::vector<std::string>& args) -> std::string {
                if (args.size() < 2 || args.size() > 3 || args[1] != "scroll") {
                    return usage_error();
                }
                uint32_t seconds = kScrollBenchDefaultSeconds;
                if (args.size() == 3) {
                    const std::string& s = args[2];
                    if (s.empty() || s.size() > 3 ||
                        s.find_first_not_of("0123456789") != std::string::npos) {
                        return usage_error();
                    }
                    seconds = static_cast<uint32_t>(std::strtoul(s.c_str(), nullptr, 10));
                    if (seconds == 0 || seconds > kScrollBenchMaxSeconds) return usage_error();
                }
                auto result = run_scroll_bench(app, seconds);
                if (!result) return "error: " + result.error().message;
                return scroll_bench_line(result.value());
            }};
}

}  // namespace wisp::app
