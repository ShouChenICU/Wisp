#include "wisp/ui/ui_runtime.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <memory>

#include "lvgl_private.h"
#include "support/fake_devices.hpp"
#include "support/wait.hpp"
#include "wisp/os/os.hpp"

using wisp::test::FakeDisplay;
using wisp::test::wait_until;

TEST(UiRuntime, RendersPostedContentWithAlignedFlushes) {
    auto disp = std::make_shared<FakeDisplay>("display.main", 480, 480, /*area_align=*/2);
    wisp::ui::UiRuntime ui(disp, nullptr);
    ASSERT_TRUE(ui.start());

    std::atomic<bool> ran{false};
    std::atomic<bool> on_ui{false};
    std::atomic<size_t> flushes_before{0};
    ui.post([&] {
        on_ui = ui.on_ui_thread();
        lv_obj_t* l = lv_label_create(lv_screen_active());
        lv_label_set_text(l, "Wisp 你好");
        // The initial screen render may already have flushed before this
        // runs; only flushes after this point show the label.
        flushes_before = disp->flush_count();
        ran = true;
    });

    ASSERT_TRUE(wait_until([&] { return ran && disp->flush_count() > flushes_before; }, 5000));
    EXPECT_TRUE(on_ui);
    EXPECT_FALSE(ui.on_ui_thread());

    for (auto& a : disp->areas()) {
        EXPECT_EQ(a.x1 % 2, 0);
        EXPECT_EQ(a.y1 % 2, 0);
        EXPECT_EQ((a.x2 + 1) % 2, 0);
    }

    auto shot = ui.snapshot_rgb888(5000);
    ASSERT_TRUE(shot);
    EXPECT_EQ(shot.value().size(), 480u * 480u * 3u);

    ui.stop();
}

TEST(UiRuntime, PerfCountsFramesUnderAnimation) {
    auto disp = std::make_shared<FakeDisplay>("display.main", 480, 480, 2);
    wisp::ui::UiRuntime ui(disp, nullptr);
    ASSERT_TRUE(ui.start());

    ui.post([] {
        lv_obj_t* o = lv_obj_create(lv_screen_active());
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, o);
        lv_anim_set_values(&a, 0, 300);
        lv_anim_set_duration(&a, 1000);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_exec_cb(&a,
                            [](void* v, int32_t x) { lv_obj_set_x(static_cast<lv_obj_t*>(v), x); });
        lv_anim_start(&a);
    });

    wisp::os::sleep_ms(1500);
    auto p = ui.perf();
    EXPECT_GT(p.fps, 5.0f);
    EXPECT_GT(p.frames, 5u);
    EXPECT_LE(p.cpu_percent, 100);

    ui.stop();
}

// Fix round 1, review finding #1: lv_display_create() (called from
// start()) itself calls lv_sysmon_show_performance() when
// LV_USE_PERF_MONITOR is on, so without the lv_sysmon_hide_performance()
// fix in start(), the overlay would default to visible and
// set_perf_overlay(false) would be the only way to hide it once -- a
// regression test for both the default state and both toggle directions.
// lv_obj_has_flag() et al are only safe to call on the UI task, so every
// check below runs via ui.post() and reports back through an atomic.
TEST(UiRuntime, PerfOverlayDefaultsHiddenAndToggles) {
    auto disp = std::make_shared<FakeDisplay>("display.main", 480, 480, 2);
    wisp::ui::UiRuntime ui(disp, nullptr);
    ASSERT_TRUE(ui.start());

    auto overlay_hidden = [&]() -> bool {
        std::atomic<bool> done{false};
        std::atomic<bool> hidden{false};
        ui.post([&] {
            lv_display_t* d = lv_display_get_default();
            hidden = lv_obj_has_flag(d->perf_label, LV_OBJ_FLAG_HIDDEN);
            done = true;
        });
        EXPECT_TRUE(wait_until([&] { return done.load(); }, 2000));
        return hidden.load();
    };

    EXPECT_TRUE(overlay_hidden());

    ui.set_perf_overlay(true);
    EXPECT_TRUE(wait_until([&] { return !overlay_hidden(); }, 2000));

    ui.set_perf_overlay(false);
    EXPECT_TRUE(wait_until([&] { return overlay_hidden(); }, 2000));

    ui.stop();
}

// Fix round 1, review finding #2: calling snapshot_rgb888() from inside a
// post()'d function (i.e. on the UI task) used to post-and-block, which
// deadlocks (the posted work can only run once the UI task's own loop
// drains its queue, which can't happen while that same loop is stuck
// waiting on this call) -- it would time out instead of hanging forever,
// but still never succeed. Verifies the on_ui_thread() short-circuit in
// snapshot_rgb888() instead takes the snapshot directly.
TEST(UiRuntime, SnapshotFromUiThreadDoesNotDeadlock) {
    auto disp = std::make_shared<FakeDisplay>("display.main", 480, 480, 2);
    wisp::ui::UiRuntime ui(disp, nullptr);
    ASSERT_TRUE(ui.start());

    std::atomic<bool> done{false};
    auto result = std::make_shared<wisp::Result<std::vector<uint8_t>>>(
        wisp::Result<std::vector<uint8_t>>::err(wisp::ErrorCode::Timeout,
                                                "snapshot callback never ran"));
    ui.post([&, result] {
        EXPECT_TRUE(ui.on_ui_thread());
        *result = ui.snapshot_rgb888();
        done = true;
    });

    ASSERT_TRUE(wait_until([&] { return done.load(); }, 2000));
    ASSERT_TRUE(*result);
    EXPECT_EQ(result->value().size(), 480u * 480u * 3u);

    ui.stop();
}

// Fix round 1, review finding #3: LVGL's own LV_COLOR_FORMAT_RGB888 draw
// buffers store each pixel as (blue, green, red), not (red, green, blue);
// snapshot_rgb888() must swap bytes 0 and 2 per pixel so its documented
// (R, G, B) byte order is actually correct. Paints the active screen pure
// red and checks the first pixel comes back ~(255, 0, 0), not ~(0, 0,
// 255).
TEST(UiRuntime, SnapshotBytesAreRgbNotBgr) {
    auto disp = std::make_shared<FakeDisplay>("display.main", 480, 480, 2);
    wisp::ui::UiRuntime ui(disp, nullptr);
    ASSERT_TRUE(ui.start());

    ui.post([] {
        lv_obj_t* scr = lv_screen_active();
        lv_obj_set_style_bg_color(scr, lv_color_hex(0xFF0000), 0);
        lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    });
    ASSERT_TRUE(wait_until([&] { return disp->flush_count() > 0; }, 2000));

    auto shot = ui.snapshot_rgb888();
    ASSERT_TRUE(shot);
    const auto& bytes = shot.value();
    ASSERT_GE(bytes.size(), 3u);
    EXPECT_NEAR(bytes[0], 255, 5);  // R
    EXPECT_NEAR(bytes[1], 0, 5);    // G
    EXPECT_NEAR(bytes[2], 0, 5);    // B

    ui.stop();
}

// Task 18b, spec 6.7: while paused, the UI task must not call
// lv_timer_handler() (so no flush happens, even if something posted
// invalidates the screen), but post()'d work must still run -- and
// promptly, not just eventually. Unpausing must produce a flush shortly
// after (the forced lv_obj_invalidate() -- see UiRuntime::set_paused()'s
// doc comment).
TEST(UiRuntime, SetPausedStopsRenderingButKeepsRunningPostedWork) {
    auto disp = std::make_shared<FakeDisplay>("display.main", 480, 480, 2);
    wisp::ui::UiRuntime ui(disp, nullptr);
    ASSERT_TRUE(ui.start());

    // Let the initial screen render settle before pausing.
    ASSERT_TRUE(wait_until([&] { return disp->flush_count() > 0; }, 2000));

    ui.set_paused(true);
    EXPECT_TRUE(ui.paused());

    // Post a job and wait for it to actually run, rather than racing the
    // UI task's own control flow by sampling flush_count() right after
    // set_paused(true) returns: set_paused() only flips a flag the UI
    // task reads at the top of its *next* loop iteration, so a flush
    // already in flight from an iteration that started just before this
    // call (reading paused() as still false back then) could otherwise
    // still land shortly after. Once this post()'d job has actually run,
    // same-thread sequencing on the UI task guarantees any such in-flight
    // lv_timer_handler() call (and whatever flush it triggered) has
    // already finished -- the paused branch below can only pick up and
    // run this job once that same thread is done with whatever it was
    // doing before. Capturing flush_count() only *after* that point is
    // what makes the following check deterministic.
    std::atomic<bool> ran{false};
    ASSERT_TRUE(ui.post([&] {
        // Invalidating while paused must not produce a flush: the paused
        // UI task never calls lv_timer_handler() to act on it.
        lv_obj_invalidate(lv_screen_active());
        ran = true;
    }));
    EXPECT_TRUE(wait_until([&] { return ran.load(); }, 2000));
    size_t flushes_at_pause = disp->flush_count();

    // Give the (paused) UI task a little real time to prove it does NOT
    // flush on its own while paused.
    wisp::os::sleep_ms(200);
    EXPECT_EQ(disp->flush_count(), flushes_at_pause);

    size_t flushes_before_resume = disp->flush_count();
    ui.set_paused(false);
    EXPECT_TRUE(wait_until([&] { return !ui.paused(); }, 2000));
    EXPECT_TRUE(wait_until([&] { return disp->flush_count() > flushes_before_resume; }, 2000));

    ui.stop();
}

// perf() must not report a bogus/stale FPS or busy% while paused -- see
// UiRuntime::perf()'s doc comment.
TEST(UiRuntime, PerfIsZeroedWhilePaused) {
    auto disp = std::make_shared<FakeDisplay>("display.main", 480, 480, 2);
    wisp::ui::UiRuntime ui(disp, nullptr);
    ASSERT_TRUE(ui.start());

    ui.post([] {
        lv_obj_t* o = lv_obj_create(lv_screen_active());
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, o);
        lv_anim_set_values(&a, 0, 300);
        lv_anim_set_duration(&a, 1000);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_exec_cb(&a,
                            [](void* v, int32_t x) { lv_obj_set_x(static_cast<lv_obj_t*>(v), x); });
        lv_anim_start(&a);
    });
    wisp::os::sleep_ms(500);
    ASSERT_GT(ui.perf().fps, 0.0f);

    ui.set_paused(true);
    ASSERT_TRUE(wait_until([&] { return ui.paused(); }, 2000));
    wisp::os::sleep_ms(50);
    wisp::ui::UiPerf p = ui.perf();
    EXPECT_EQ(p.fps, 0.0f);
    EXPECT_EQ(p.cpu_percent, 0);
    EXPECT_EQ(p.avg_flush_ms, 0.0f);

    ui.stop();
}
