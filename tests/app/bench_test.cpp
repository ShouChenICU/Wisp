// The `bench` console command (wisp/app/bench.hpp): picking the object to
// scroll, the scroll bench on the self-check screen of an App with a
// FakeDisplay, its result line, and the cases where it refuses to run.

#include "wisp/app/bench.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <string>
#include <vector>

#include "support/lvgl_probe.hpp"
#include "support/wait.hpp"
#include "ui_net_harness.hpp"
#include "wisp/app/power_manager.hpp"
#include "wisp/base/json.hpp"
#include "wisp/ui/ui_runtime.hpp"

using wisp::app::bench_command;
using wisp::app::main_scrollable;
using wisp::test::run_on_ui_blocking;
using wisp::test::UiNetHarness;
using wisp::test::UiNetOptions;
using wisp::test::wait_until;

namespace {

std::string bench(UiNetHarness& h, const std::vector<std::string>& args) {
    return bench_command().run(*h.app, args);
}

UiNetOptions no_wifi() {
    UiNetOptions opts;
    opts.with_wifi = false;
    return opts;
}

// What the target saw during a run, recorded by its own event callback.
struct ScrollLog {
    std::atomic<int> begins{0};
    std::atomic<int> ends{0};
    std::atomic<int> scrolls{0};
    std::atomic<int32_t> max_y{0};
};

void log_scroll(lv_event_t* e) {
    auto* log = static_cast<ScrollLog*>(lv_event_get_user_data(e));
    auto* obj = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    switch (lv_event_get_code(e)) {
        case LV_EVENT_SCROLL_BEGIN:
            ++log->begins;
            break;
        case LV_EVENT_SCROLL_END:
            ++log->ends;
            break;
        case LV_EVENT_SCROLL:
            ++log->scrolls;
            if (lv_obj_get_scroll_y(obj) > log->max_y) log->max_y = lv_obj_get_scroll_y(obj);
            break;
        default:
            break;
    }
}

}  // namespace

TEST(Bench, MainScrollableIsTheLargestShownOneThatScrolls) {
    UiNetHarness h(no_wifi());
    ASSERT_TRUE(h.start());
    bool picked_largest = false;
    bool none_without_overflow = false;
    run_on_ui_blocking(h.app->ui(), [&] {
        lv_obj_t* screen = lv_obj_create(nullptr);  // not loaded
        auto box = [&](int size, bool overflow) {
            lv_obj_t* obj = lv_obj_create(screen);
            lv_obj_set_size(obj, size, size);
            lv_obj_t* child = lv_obj_create(obj);
            lv_obj_set_size(child, 10, overflow ? size * 2 : 10);
            return obj;
        };
        box(100, true);
        lv_obj_t* largest = box(300, true);
        box(400, false);                                      // nothing beyond its box
        lv_obj_add_flag(box(450, true), LV_OBJ_FLAG_HIDDEN);  // not shown
        lv_obj_remove_flag(box(460, true), LV_OBJ_FLAG_SCROLLABLE);
        picked_largest = main_scrollable(screen) == largest;
        lv_obj_delete(screen);

        lv_obj_t* flat = lv_obj_create(nullptr);
        none_without_overflow = main_scrollable(flat) == nullptr;
        lv_obj_delete(flat);
    });
    EXPECT_TRUE(picked_largest);
    EXPECT_TRUE(none_without_overflow);
}

TEST(Bench, ScrollMovesTheSelfCheckListAndPutsItBack) {
    UiNetHarness h(no_wifi());
    ASSERT_TRUE(h.start());
    // The home screen does not scroll; the diagnostics' list does.
    ASSERT_TRUE(h.open_diagnostics());
    wisp::ui::UiRuntime* ui = h.app->ui();
    ASSERT_NE(ui, nullptr);
    lv_obj_t* target = nullptr;
    ASSERT_TRUE(wait_until(
        [&] {
            run_on_ui_blocking(ui, [&] { target = main_scrollable(lv_screen_active()); });
            return target != nullptr;
        },
        3000));

    ScrollLog log;
    int32_t y_before = -1;
    run_on_ui_blocking(ui, [&] {
        y_before = lv_obj_get_scroll_y(target);
        lv_obj_add_event_cb(target, &log_scroll, LV_EVENT_ALL, &log);
    });

    const std::string out = bench(h, {"bench", "scroll", "1"});

    int32_t y_after = -1;
    run_on_ui_blocking(ui, [&] {
        y_after = lv_obj_get_scroll_y(target);
        lv_obj_remove_event_cb_with_user_data(target, &log_scroll, &log);
    });

    const std::string prefix = "WISP_BENCH ";
    ASSERT_EQ(out.rfind(prefix, 0), 0u) << out;
    EXPECT_EQ(out.find('\n'), std::string::npos) << out;
    auto parsed = wisp::from_json(out.substr(prefix.size()));
    ASSERT_TRUE(parsed) << out;
    const wisp::Value& v = parsed.value();
    ASSERT_NE(v.find("bench"), nullptr);
    EXPECT_EQ(v.find("bench")->as_string(), "scroll");
    for (const char* key : {"fps", "frames", "ms", "cpu", "px"}) {
        EXPECT_NE(v.find(key), nullptr) << key;
    }
    if (v.find("frames") == nullptr || v.find("ms") == nullptr) return;
    EXPECT_GE(v.find("frames")->as_int(), 2);
    EXPECT_GT(v.find("fps")->as_double(), 0.0);
    EXPECT_GE(v.find("ms")->as_int(), 500);
    EXPECT_LE(v.find("ms")->as_int(), 1500);
    EXPECT_EQ(v.find("px")->as_int(), wisp::app::kScrollBenchStepPx);
    EXPECT_GE(v.find("cpu")->as_int(), 0);
    EXPECT_LE(v.find("cpu")->as_int(), 100);

    // One drag's worth of events, a scroll per frame, and back at the start
    // (going back is a jump of its own, with another BEGIN/END pair).
    EXPECT_EQ(log.begins.load(), 2);
    EXPECT_EQ(log.ends.load(), 2);
    EXPECT_GE(log.scrolls.load(), v.find("frames")->as_int());
    EXPECT_GT(log.max_y.load(), y_before);
    EXPECT_EQ(y_after, y_before);
}

TEST(Bench, RejectsBadArguments) {
    UiNetHarness h(no_wifi());
    ASSERT_TRUE(h.start());
    for (const std::vector<std::string>& args :
         std::vector<std::vector<std::string>>{{"bench"},
                                               {"bench", "spin"},
                                               {"bench", "scroll", "0"},
                                               {"bench", "scroll", "121"},
                                               {"bench", "scroll", "2x"},
                                               {"bench", "scroll", "-1"},
                                               {"bench", "scroll", "1", "2"}}) {
        const std::string out = bench(h, args);
        EXPECT_EQ(out.rfind("error: ", 0), 0u) << out;
    }
}

TEST(Bench, NeedsADisplay) {
    UiNetOptions opts = no_wifi();
    opts.with_display = false;
    UiNetHarness h(opts);
    ASSERT_TRUE(h.start());
    const std::string out = bench(h, {"bench", "scroll", "1"});
    EXPECT_EQ(out.rfind("error: ", 0), 0u) << out;
}

TEST(Bench, RefusesWhileTheScreenIsOff) {
    UiNetHarness h(no_wifi());
    ASSERT_TRUE(h.start());
    wisp::app::PowerManager* pm = h.app->power_manager();
    ASSERT_NE(pm, nullptr);
    run_on_ui_blocking(h.app->ui(), [&] { pm->enter_standby(); });
    ASSERT_TRUE(pm->standby());
    const std::string out = bench(h, {"bench", "scroll", "1"});
    EXPECT_EQ(out.rfind("error: ", 0), 0u) << out;
    run_on_ui_blocking(h.app->ui(), [&] { pm->exit_standby(); });
}
