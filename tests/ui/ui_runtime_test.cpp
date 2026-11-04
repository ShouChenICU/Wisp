#include "wisp/ui/ui_runtime.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <memory>
#include <mutex>
#include <set>
#include <string>

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

// lv_display_create() (called from
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

// Calling snapshot_rgb888() from inside a
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

// LVGL's own LV_COLOR_FORMAT_RGB888 draw
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

// Cards and dialogs live on the top layer; a snapshot shows what the panel
// shows, so they are composited over the screen (blended where
// translucent). The sys layer (the perf overlay) stays out.
TEST(UiRuntime, SnapshotIncludesTheTopLayer) {
    auto disp = std::make_shared<FakeDisplay>("display.main", 480, 480, 2);
    wisp::ui::UiRuntime ui(disp, nullptr);
    ASSERT_TRUE(ui.start());

    ui.post([] {
        lv_obj_t* scr = lv_screen_active();
        lv_obj_set_style_bg_color(scr, lv_color_hex(0xFF0000), 0);
        lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
        lv_obj_t* box = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(box);
        lv_obj_set_pos(box, 100, 100);
        lv_obj_set_size(box, 50, 50);
        lv_obj_set_style_bg_color(box, lv_color_hex(0x0000FF), 0);
        lv_obj_set_style_bg_opa(box, LV_OPA_COVER, 0);
        lv_obj_t* veil = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(veil);
        lv_obj_set_pos(veil, 300, 300);
        lv_obj_set_size(veil, 50, 50);
        lv_obj_set_style_bg_color(veil, lv_color_hex(0x000000), 0);
        lv_obj_set_style_bg_opa(veil, LV_OPA_50, 0);
    });
    ASSERT_TRUE(wait_until([&] { return disp->flush_count() > 0; }, 2000));

    auto shot = ui.snapshot_rgb888();
    ASSERT_TRUE(shot);
    const auto& bytes = shot.value();
    auto px = [&](int x, int y) { return &bytes[(static_cast<size_t>(y) * 480 + x) * 3]; };
    EXPECT_NEAR(px(10, 10)[0], 255, 5);  // screen: red
    EXPECT_NEAR(px(10, 10)[2], 0, 5);
    EXPECT_NEAR(px(120, 120)[0], 0, 5);  // opaque top-layer box: blue
    EXPECT_NEAR(px(120, 120)[2], 255, 5);
    EXPECT_NEAR(px(320, 320)[0], 128, 8);  // 50% black over red
    EXPECT_NEAR(px(320, 320)[1], 0, 5);

    ui.stop();
}

// Spec 6.7: while paused, the UI task must not call
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
    //
    // This job deliberately does NOT
    // invalidate anything (an earlier version did, with
    // lv_obj_invalidate(lv_screen_active())) -- so the flush this test
    // observes after resuming below can only be attributed to
    // set_paused(false)'s own forced redraw, not to this job having
    // invalidated something itself (which would have produced a flush
    // on resume even if set_paused(false) forgot to force one of its
    // own, defeating the point of the check).
    std::atomic<bool> ran{false};
    ASSERT_TRUE(ui.post([&] { ran = true; }));
    EXPECT_TRUE(wait_until([&] { return ran.load(); }, 2000));
    size_t flushes_at_pause = disp->flush_count();

    // Give the (paused) UI task a little real time to prove it does NOT
    // flush on its own while paused.
    wisp::os::sleep_ms(200);
    EXPECT_EQ(disp->flush_count(), flushes_at_pause);

    size_t flushes_before_resume = disp->flush_count();
    ui.set_paused(false);
    EXPECT_TRUE(wait_until([&] { return !ui.paused(); }, 2000));

    // The resume redraw must cover the *whole* screen (lv_obj_invalidate
    // (lv_screen_active()) -- see UiRuntime::set_paused()'s doc comment
    // -- not just whatever had been invalidated before pausing), proving
    // it really is set_paused(false)'s own forced full redraw. LVGL
    // splits a full-screen redraw across multiple flush() calls (one per
    // buffer-height row stripe), so a single flush_count() increment is
    // not enough to know the whole frame has landed: wait for the union
    // of every flush area since resuming to cover the full screen before
    // reading it for the exact assertion below, rather than reading
    // areas() as soon as the first stripe arrives.
    constexpr int kDisplayWidth = 480;
    constexpr int kDisplayHeight = 480;
    int min_x1 = kDisplayWidth;
    int min_y1 = kDisplayHeight;
    int max_x2 = -1;
    int max_y2 = -1;
    auto union_covers_screen = [&] {
        auto areas = disp->areas();
        min_x1 = kDisplayWidth;
        min_y1 = kDisplayHeight;
        max_x2 = -1;
        max_y2 = -1;
        for (size_t i = flushes_before_resume; i < areas.size(); ++i) {
            min_x1 = std::min(min_x1, static_cast<int>(areas[i].x1));
            min_y1 = std::min(min_y1, static_cast<int>(areas[i].y1));
            max_x2 = std::max(max_x2, static_cast<int>(areas[i].x2));
            max_y2 = std::max(max_y2, static_cast<int>(areas[i].y2));
        }
        return min_x1 == 0 && min_y1 == 0 && max_x2 == kDisplayWidth - 1 &&
               max_y2 == kDisplayHeight - 1;
    };
    ASSERT_TRUE(wait_until(union_covers_screen, 2000));
    EXPECT_EQ(min_x1, 0);
    EXPECT_EQ(min_y1, 0);
    EXPECT_EQ(max_x2, kDisplayWidth - 1);
    EXPECT_EQ(max_y2, kDisplayHeight - 1);

    ui.stop();
}

// A post()'d job that invalidates something
// and then pauses (set_paused(true)) -- e.g. PowerManager::enter_standby
// () runs entirely inside one post()'d job, and could easily run right
// after some other posted job dirtied a widget -- must not have that
// dirty area flushed against a display ui_main()'s own loop already
// considers paused. Before this fix, drain_post_queue() (where the job
// below runs) was unconditionally followed by one more
// lv_timer_handler() call on the very same loop iteration, flushing
// whatever the job just invalidated even though paused was already true
// by the time that call ran -- the paused check at the top of the loop
// only takes effect starting the *next* iteration.
TEST(UiRuntime, PausingInsideAPostedJobSkipsThatIterationsFlush) {
    auto disp = std::make_shared<FakeDisplay>("display.main", 480, 480, 2);
    wisp::ui::UiRuntime ui(disp, nullptr);
    ASSERT_TRUE(ui.start());
    ASSERT_TRUE(wait_until([&] { return disp->flush_count() > 0; }, 2000));

    // Sampled on the UI task, inside the job: the initial frame may still
    // be flushing in chunks when the wait above returns, so a baseline
    // taken on the test thread could miss a legitimate earlier flush.
    std::atomic<size_t> flushes_before{0};
    std::atomic<bool> ran{false};
    ASSERT_TRUE(ui.post([&] {
        flushes_before = disp->flush_count();
        lv_obj_invalidate(lv_screen_active());
        // LVGL's own refresh timer (LV_DEF_REFR_PERIOD, 16ms) only
        // actually redraws a dirty area once it's due -- sleeping here,
        // on the UI task, past that period before set_paused(true)
        // guarantees it is overdue by the time ui_main() would call
        // lv_timer_handler() again right after this job returns (the
        // call this test exists to prove does NOT happen), rather than
        // leaving that race up to however this test host happens to
        // schedule the surrounding 10ms-ish loop iterations.
        wisp::os::sleep_ms(50);
        ui.set_paused(true);
        ran = true;
    }));
    ASSERT_TRUE(wait_until([&] { return ran.load(); }, 2000));

    // Barrier: a second job can only run once ui_main() has looped back
    // around and reached the paused branch -- which, in turn, can only
    // happen once this iteration's lv_timer_handler() call (the one this
    // test is checking did NOT happen) has already returned, if it ran
    // at all. So by the time this barrier job runs, any such flush from
    // this iteration is guaranteed to have already happened.
    std::atomic<bool> barrier_ran{false};
    ASSERT_TRUE(ui.post([&] { barrier_ran = true; }));
    ASSERT_TRUE(wait_until([&] { return barrier_ran.load(); }, 2000));

    EXPECT_EQ(disp->flush_count(), flushes_before.load());

    ui.stop();
}

// UiRuntime::set_touch_activity_cb() had
// no direct test -- only PowerManager's own tests exercised it
// indirectly. This drives it directly: a touch point appearing makes
// indev_read_cb() (run from inside lv_timer_handler(), on the UI task)
// invoke the installed callback.
TEST(UiRuntime, TouchActivityCallbackFiresWhenATouchPointAppears) {
    auto disp = std::make_shared<FakeDisplay>("display.main", 480, 480, 2);
    auto touch = std::make_shared<wisp::test::FakeTouch>("touch.main");
    wisp::ui::UiRuntime ui(disp, touch);
    ASSERT_TRUE(ui.start());

    std::atomic<int> calls{0};
    ui.set_touch_activity_cb([&] { ++calls; });

    // No callback yet: nothing has touched the (empty) FakeTouch.
    wisp::os::sleep_ms(50);
    EXPECT_EQ(calls.load(), 0);

    touch->set_points({{100, 100, 0}});
    EXPECT_TRUE(wait_until([&] { return calls.load() > 0; }, 2000));

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

namespace {

// A display whose transfers run in the background, like a DMA-fed panel:
// flush_async() only takes note of the buffer and wait_flush() finishes
// it. It checks the protocol UiRuntime must follow: never two transfers at
// once, and nothing writes into a buffer while it is on the wire (a
// checksum taken at flush_async() must still match at wait_flush()).
class AsyncFakeDisplay : public wisp::hal::Display {
public:
    AsyncFakeDisplay(int width, int height, bool rgb565_be) {
        info_.id = "display.main";
        info_.cls = kClass;
        info_.model = "FAKE";
        info_.caps = wisp::Properties{
            {"width", width},         {"height", height},
            {"area_align", 2},        {"shape", wisp::Value("rect")},
            {"rgb565_be", rgb565_be},
        };
    }

    const wisp::hal::DeviceInfo& info() const override { return info_; }

    wisp::Result<void> flush(const wisp::hal::Area& area, const uint16_t* px) override {
        auto r = flush_async(area, px);
        if (!r) return r;
        return wait_flush();
    }

    wisp::Result<void> flush_async(const wisp::hal::Area& area, const uint16_t* px) override {
        assert_owner_thread();
        std::lock_guard<std::mutex> lock(mutex_);
        if (fail_next_ > 0) {
            // Like a panel transfer that could not be queued: nothing
            // is in flight afterwards.
            --fail_next_;
            failed_.push_back(area);
            return wisp::Result<void>::err(wisp::ErrorCode::DeviceFailure, "fake flush failure");
        }
        if (in_flight_ != nullptr) ++violations_;
        sent_.push_back(area);
        in_flight_ = px;
        in_flight_bytes_ = static_cast<size_t>(area.x2 - area.x1 + 1) * (area.y2 - area.y1 + 1) * 2;
        in_flight_sum_ = checksum(px, in_flight_bytes_);
        buffers_.insert(px);
        if (first_bytes_.empty()) {
            const auto* b = reinterpret_cast<const uint8_t*>(px);
            first_bytes_.assign(b, b + 2);
        }
        ++async_flushes_;
        return wisp::Result<void>::ok();
    }

    wisp::Result<void> wait_flush() override {
        if (wait_ms_ > 0) wisp::os::sleep_ms(wait_ms_);
        std::lock_guard<std::mutex> lock(mutex_);
        if (in_flight_ != nullptr && checksum(in_flight_, in_flight_bytes_) != in_flight_sum_) {
            ++violations_;
        }
        in_flight_ = nullptr;
        return wisp::Result<void>::ok();
    }

    wisp::Result<void> set_brightness(uint8_t) override { return wisp::Result<void>::ok(); }
    wisp::Result<void> set_power(bool) override { return wisp::Result<void>::ok(); }
    bool powered() const override { return true; }

    int violations() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return violations_;
    }
    size_t async_flushes() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return async_flushes_;
    }
    size_t distinct_buffers() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return buffers_.size();
    }
    bool in_flight() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return in_flight_ != nullptr;
    }
    // The next `n` flush_async() calls fail and send nothing.
    void fail_next_flushes(int n) {
        std::lock_guard<std::mutex> lock(mutex_);
        fail_next_ = n;
    }
    std::vector<wisp::hal::Area> failed() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return failed_;
    }
    std::vector<wisp::hal::Area> sent() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return sent_;
    }
    // Every wait_flush() takes this long, as if each stripe were still on
    // the wire. Set before UiRuntime::start().
    void set_wait_ms(uint32_t ms) { wait_ms_ = ms; }
    void reset_first_bytes() {
        std::lock_guard<std::mutex> lock(mutex_);
        first_bytes_.clear();
    }
    std::vector<uint8_t> first_bytes() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return first_bytes_;
    }

private:
    static uint64_t checksum(const uint16_t* px, size_t bytes) {
        const auto* b = reinterpret_cast<const uint8_t*>(px);
        uint64_t h = 1469598103934665603ull;
        for (size_t i = 0; i < bytes; ++i) h = (h ^ b[i]) * 1099511628211ull;
        return h;
    }

    wisp::hal::DeviceInfo info_;
    mutable std::mutex mutex_;
    const uint16_t* in_flight_ = nullptr;
    int fail_next_ = 0;
    std::vector<wisp::hal::Area> failed_;
    std::vector<wisp::hal::Area> sent_;
    size_t in_flight_bytes_ = 0;
    uint64_t in_flight_sum_ = 0;
    std::set<const uint16_t*> buffers_;
    std::vector<uint8_t> first_bytes_;
    int violations_ = 0;
    size_t async_flushes_ = 0;
    uint32_t wait_ms_ = 0;
};

}  // namespace

// Two partial buffers: LVGL renders into one while the other is on the
// wire. A moving, text-filled screen split into many stripes exercises
// the hand-over between them.
TEST(UiRuntime, RendersIntoOneBufferWhileTheOtherIsOnTheWire) {
    auto disp = std::make_shared<AsyncFakeDisplay>(480, 480, false);
    wisp::ui::UiConfig config;
    config.buffer_lines = 16;
    wisp::ui::UiRuntime ui(disp, nullptr, config);
    ASSERT_TRUE(ui.start());

    wisp::os::Semaphore done(1, 0);
    bool has_second = false;
    uint32_t first_bytes = 0;
    uint32_t second_bytes = 0;
    ASSERT_TRUE(ui.post([&] {
        lv_display_t* d = lv_display_get_default();
        has_second = d->buf_2 != nullptr;
        first_bytes = d->buf_1 != nullptr ? d->buf_1->data_size : 0;
        second_bytes = d->buf_2 != nullptr ? d->buf_2->data_size : 0;
        lv_obj_t* col = lv_obj_create(lv_screen_active());
        lv_obj_set_size(col, 480, 480);
        lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
        for (int i = 0; i < 40; ++i) {
            lv_label_set_text(lv_label_create(col), "Wisp 你好 0123456789 abcdefghijklmnop");
        }
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, col);
        lv_anim_set_values(&a, 0, 400);
        lv_anim_set_duration(&a, 300);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_exec_cb(&a, [](void* v, int32_t y) {
            lv_obj_scroll_to_y(static_cast<lv_obj_t*>(v), y, LV_ANIM_OFF);
        });
        lv_anim_start(&a);
        done.give();
    }));
    ASSERT_TRUE(done.take(5000));
    EXPECT_TRUE(has_second);
    EXPECT_EQ(first_bytes, 480u * config.buffer_lines * 2u);
    EXPECT_EQ(second_bytes, 480u * config.buffer_lines * 2u);

    ASSERT_TRUE(wait_until([&] { return disp->async_flushes() > 200; }, 10000));
    EXPECT_EQ(disp->distinct_buffers(), 2u);
    EXPECT_EQ(disp->violations(), 0);

    ui.stop();
    // stop() waits for the last transfer before freeing the buffers.
    EXPECT_FALSE(disp->in_flight());
}

// avg_flush_ms is per stripe, not per frame: 30 stripes of 16 lines, each
// waited on for 2 ms, average about 2 ms however many make up a frame.
TEST(UiRuntime, AverageFlushTimeIsPerStripe) {
    auto disp = std::make_shared<AsyncFakeDisplay>(480, 480, false);
    disp->set_wait_ms(2);
    wisp::ui::UiConfig config;
    config.buffer_lines = 16;
    wisp::ui::UiRuntime ui(disp, nullptr, config);
    ASSERT_TRUE(ui.start());

    ASSERT_TRUE(ui.post([] {
        lv_obj_t* scr = lv_screen_active();
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, scr);
        lv_anim_set_values(&a, 0, 255);
        lv_anim_set_duration(&a, 300);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_exec_cb(&a, [](void* v, int32_t c) {
            lv_obj_set_style_bg_color(static_cast<lv_obj_t*>(v),
                                      lv_color_make(static_cast<uint8_t>(c), 0, 0), 0);
        });
        lv_anim_start(&a);
    }));
    ASSERT_TRUE(wait_until([&] { return ui.perf().fps >= 2.0f; }, 5000));
    wisp::ui::UiPerf p = ui.perf();
    EXPECT_GE(p.avg_flush_ms, 1.5f);
    EXPECT_LT(p.avg_flush_ms, 10.0f);

    ui.stop();
}

// caps "rgb565_be": LVGL renders straight into the panel's byte order, so
// pure red (0xF800) reaches flush() as F8 00 instead of the native 00 F8.
TEST(UiRuntime, RendersInTheDisplaysByteOrder) {
    for (bool be : {false, true}) {
        auto disp = std::make_shared<AsyncFakeDisplay>(480, 480, be);
        wisp::ui::UiRuntime ui(disp, nullptr);
        ASSERT_TRUE(ui.start());

        wisp::os::Semaphore done(1, 0);
        ASSERT_TRUE(ui.post([&] {
            lv_obj_t* scr = lv_screen_active();
            lv_obj_set_style_bg_color(scr, lv_color_hex(0xFF0000), 0);
            lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
            // Flushes before this point showed the theme's background.
            disp->reset_first_bytes();
            done.give();
        }));
        ASSERT_TRUE(done.take(5000));
        ASSERT_TRUE(wait_until([&] { return disp->first_bytes().size() == 2; }, 5000));
        std::vector<uint8_t> expected =
            be ? std::vector<uint8_t>{0xF8, 0x00} : std::vector<uint8_t>{0x00, 0xF8};
        EXPECT_EQ(disp->first_bytes(), expected) << "rgb565_be=" << be;

        ui.stop();
    }
}

// A stripe the display failed to send is not left stale on the panel: it
// is invalidated again and sent on a later refresh.
TEST(UiRuntime, AStripeThatFailedToSendIsDrawnAgain) {
    auto disp = std::make_shared<AsyncFakeDisplay>(480, 480, false);
    wisp::ui::UiRuntime ui(disp, nullptr);
    ASSERT_TRUE(ui.start());

    wisp::os::Semaphore done(1, 0);
    ASSERT_TRUE(ui.post([&] {
        lv_refr_now(nullptr);  // whatever start() left dirty
        disp->fail_next_flushes(1);
        lv_obj_set_style_bg_color(lv_screen_active(), lv_color_hex(0x00FF00), 0);
        done.give();
    }));
    ASSERT_TRUE(done.take(5000));
    ASSERT_TRUE(wait_until([&] { return !disp->failed().empty(); }, 5000));
    const wisp::hal::Area lost = disp->failed().front();
    const size_t sent_before = disp->sent().size();
    EXPECT_TRUE(wait_until(
        [&] {
            const auto sent = disp->sent();
            for (size_t i = sent_before; i < sent.size(); ++i) {
                const auto& a = sent[i];
                if (a.x1 <= lost.x1 && a.y1 <= lost.y1 && a.x2 >= lost.x2 && a.y2 >= lost.y2) {
                    return true;
                }
            }
            return false;
        },
        2000));
    ui.stop();
}
