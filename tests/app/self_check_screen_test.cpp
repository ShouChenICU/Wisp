// Smoke/regression tests for the real self-check screen (Task 12), on top
// of the same App/Board test harness app_boot_test.cpp uses. Boots an App
// with every fake device wired up, waits for the UI to actually render,
// and checks on *behavior* (does it render at all, does it render
// differently under an IMU error, does a persisted setting survive a
// restart, does WISP_PERF get emitted) rather than on exact pixel
// content, which would make these tests brittle against any layout
// tweak.

#include <gtest/gtest.h>

#include <array>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "support/fake_devices.hpp"
#include "support/temp_dir.hpp"
#include "support/wait.hpp"
#include "wisp/app/app.hpp"
#include "wisp/app/board.hpp"
#include "wisp/app/power_manager.hpp"
#include "wisp/base/error.hpp"
#include "wisp/base/value.hpp"
#include "wisp/core/device_registry.hpp"
#include "wisp/core/settings.hpp"
#include "wisp/hal/display_geometry.hpp"
#include "wisp/os/os.hpp"
#include "wisp/port/sim/settings_json_file.hpp"
#include "wisp/ui/ui_runtime.hpp"

using wisp::test::FakeButton;
using wisp::test::FakeClock;
using wisp::test::FakeDisplay;
using wisp::test::FakeImu;
using wisp::test::FakePower;
using wisp::test::FakeStorage;
using wisp::test::wait_until;

namespace {

// A Board that registers one of every fake device the self-check screen
// knows how to look up (display and imu are supplied by the caller, so
// tests can keep a handle to inject errors/timing into them; touch is
// intentionally left out -- FakeDisplay/FakeImu/etc. don't include a
// FakeTouch, and the screen must tolerate that the same way it tolerates
// a missing power/clock/SD device), so building the screen exercises
// every section of spec 8's layout rather than just the "device missing"
// fallback text.
class FullFakeBoard : public wisp::app::Board {
public:
    FullFakeBoard(std::shared_ptr<FakeDisplay> display, std::shared_ptr<FakeImu> imu,
                  uint32_t init_sleep_ms = 0)
        : display_(std::move(display)), imu_(std::move(imu)), init_sleep_ms_(init_sleep_ms) {}

    std::string name() const override { return "fake-full"; }

    void init(wisp::core::DeviceRegistry& registry, wisp::core::BootReport& /*report*/) override {
        if (init_sleep_ms_ > 0) {
            // Gives boot_ms (used by both WISP_BOOT_REPORT and the
            // self-check header) a deterministic, non-flaky non-zero
            // floor to assert against -- see BootTimeIsSetBeforeBuild
            // below (fix round 1, issue 3).
            wisp::os::sleep_ms(init_sleep_ms_);
        }
        registry.add(display_);
        registry.add(std::make_shared<FakeStorage>("storage.int", wisp::test::temp_dir().string(),
                                                   /*removable=*/false));
        registry.add(imu_);
        registry.add(std::make_shared<FakeButton>("button.power"));
        registry.add(std::make_shared<FakePower>("power.main"));
        registry.add(std::make_shared<FakeClock>("clock.main"));
    }

private:
    std::shared_ptr<FakeDisplay> display_;
    std::shared_ptr<FakeImu> imu_;
    uint32_t init_sleep_ms_;
};

// Bundles everything a test needs to build one App over FullFakeBoard: the
// externally-held fakes it can poke, the captured emit_line() lines, and
// the settings file path (shared across Apps in the persistence test).
struct Harness {
    std::shared_ptr<FakeDisplay> display;
    std::shared_ptr<FakeImu> imu;
    std::string settings_path;
    std::mutex lines_mutex;
    std::vector<std::string> lines;
    wisp::app::App app;

    explicit Harness(std::string settings_path_in, uint32_t init_sleep_ms = 0,
                     std::string display_shape = "rounded_rect")
        : display(std::make_shared<FakeDisplay>("display.main", 480, 480, /*area_align=*/1,
                                                std::move(display_shape))),
          imu(std::make_shared<FakeImu>("imu.main")),
          settings_path(std::move(settings_path_in)),
          app(build_platform(init_sleep_ms)) {}

    bool wait_for_flushes(size_t more_than, uint32_t timeout_ms = 3000) {
        return wait_until([&] { return display->flush_count() > more_than; }, timeout_ms);
    }

private:
    wisp::app::Platform build_platform(uint32_t init_sleep_ms) {
        wisp::app::Platform platform;
        platform.board = std::make_unique<FullFakeBoard>(display, imu, init_sleep_ms);
        platform.settings =
            std::make_unique<wisp::port::sim::JsonFileSettingsBackend>(settings_path);
        platform.emit_line = [this](const std::string& line) {
            std::lock_guard<std::mutex> lock(lines_mutex);
            lines.push_back(line);
        };
        return platform;
    }
};

// Recursively searches `obj`'s subtree for the first lv_label whose
// current text contains `needle`, returning a copy of its text (empty if
// none found). Used below to check specific rows' rendered text (N2, Task
// 12b review round) without needing any test-only accessor into
// SelfCheck's internals -- it's entirely opaque to this test file (see
// self_check.hpp's forward declaration) -- or a snapshot pixel diff of
// the whole screen.
std::string find_label_text_containing(lv_obj_t* obj, const char* needle) {
    if (lv_obj_check_type(obj, &lv_label_class)) {
        const char* text = lv_label_get_text(obj);
        if (text != nullptr && std::string(text).find(needle) != std::string::npos) {
            return text;
        }
    }
    uint32_t count = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < count; ++i) {
        std::string found = find_label_text_containing(lv_obj_get_child(obj, i), needle);
        if (!found.empty()) {
            return found;
        }
    }
    return {};
}

// Runs `probe` (which must itself only call lv_* -- it's invoked on the UI
// task) and blocks until it has returned, regardless of which thread
// calls this -- the same pattern self_check_screen.cpp's own
// run_on_ui_thread_blocking() uses internally, reimplemented here since
// that helper isn't reachable from outside self_check_screen.cpp.
std::string run_on_ui_thread(wisp::ui::UiRuntime* ui, const std::function<std::string()>& probe) {
    std::string result;
    wisp::os::Semaphore done(1, 0);
    if (!ui->post([&] {
            result = probe();
            done.give();
        })) {
        return {};
    }
    done.take(wisp::os::kForever);
    return result;
}

// Like run_on_ui_thread() above but for a void `fn` -- used to call
// PowerManager::enter_standby()/exit_standby() directly from a test:
// both call display->set_power() straight through (not via post()), and
// spec 5.3 requires every Display call to come from the UI thread
// (debug-asserted -- see display.hpp). Production code only ever
// reaches them already on the UI task (PowerManager posts first when it
// has a UiRuntime), so tests driving them directly need to reproduce
// that same calling convention.
void run_on_ui_blocking(wisp::ui::UiRuntime* ui, const std::function<void()>& fn) {
    wisp::os::Semaphore done(1, 0);
    if (!ui->post([&] {
            fn();
            done.give();
        })) {
        return;
    }
    done.take(wisp::os::kForever);
}

}  // namespace

TEST(SelfCheckScreen, RendersNonUniformFrame) {
    Harness h(wisp::test::temp_dir().string() + "/settings.json");
    ASSERT_TRUE(h.app.start());
    ASSERT_NE(h.app.ui(), nullptr);

    // Give the sampling task and the 100ms UI refresh timer a few ticks,
    // then wait for several real flushes so the screen has actually been
    // drawn (not just the first, possibly-still-empty frame).
    ASSERT_TRUE(h.wait_for_flushes(5));

    auto snapshot = h.app.ui()->snapshot_rgb888();
    ASSERT_TRUE(snapshot);
    const auto& pixels = snapshot.value();
    ASSERT_EQ(pixels.size(), 480u * 480u * 3u);

    // A uniform frame (every pixel byte identical) would mean the screen
    // is effectively blank. The self-check screen has text, colored
    // device-state labels and the IMU tilt ball frame, so the real thing
    // must vary.
    bool all_same = true;
    for (size_t i = 1; i < pixels.size(); ++i) {
        if (pixels[i] != pixels[0]) {
            all_same = false;
            break;
        }
    }
    EXPECT_FALSE(all_same);

    h.app.stop();
}

// Task 12b review round, issue N1: boot_ms is now recorded right after
// Step 4 (UI bring-up) finishes -- i.e. after the self-check screen is
// already built, per spec 6.1's boot sequence, which only *ends* once the
// screen is up -- rather than right before, as fix round 1 (issue 3) had
// it. The header renders once during build() (with boot_ms still at its
// default, 0) and is then explicitly refreshed (refresh_self_check_header()
// in self_check.hpp) once the real value is known; this checks both the
// *report* (used for WISP_BOOT_REPORT) and the header label itself end up
// showing that same real value by the time start() returns (FullFakeBoard's
// deliberate sleep makes this non-flaky: without it, a sub-millisecond boot
// could legitimately still round to 0ms on a fast machine, and any 0 would
// then be indistinguishable from the stale pre-refresh header text).
TEST(SelfCheckScreen, BootTimeIsSetAndHeaderRefreshedAfterScreenIsBuilt) {
    Harness h(wisp::test::temp_dir().string() + "/settings.json", /*init_sleep_ms=*/20);
    ASSERT_TRUE(h.app.start());
    ASSERT_TRUE(h.wait_for_flushes(5));

    auto rep = h.app.report().to_value(h.app.platform().board->name());
    const wisp::Value* boot_ms = rep.find("boot_ms");
    ASSERT_NE(boot_ms, nullptr);
    EXPECT_GT(boot_ms->as_int(), 0);

    std::string header_text = run_on_ui_thread(
        h.app.ui(), [] { return find_label_text_containing(lv_screen_active(), "启动"); });
    EXPECT_NE(header_text.find(std::to_string(boot_ms->as_int())), std::string::npos)
        << "header: " << header_text << " boot_ms: " << boot_ms->as_int();

    h.app.stop();
}

// Spec 8: a runtime IMU read failure must show "读取失败" for that row,
// not crash. Task 12b review round, issue N2: this now reads the IMU
// row's own label text (on the UI thread) rather than diffing the whole
// screen's snapshot_rgb888() -- a full-frame diff only proved *something*
// changed (it could just as easily have been boot_ms jitter or an
// unrelated timer tick), not specifically that the IMU row renders the
// error text.
//
// Task 18b review: waiting for "N more flushes" to infer that the 100ms
// refresh_timer_ has ticked since the error was injected is only a
// coincidental proxy -- any other source of a same-sized flush burst
// (e.g. a full-screen redraw triggered by unrelated background work,
// such as another os::Timer thread starting up around the same time)
// satisfies the flush-count threshold just as well without the IMU row
// having actually been refreshed yet, making the test flaky. Polling the
// label text itself with a bounded timeout (as done below) checks the
// thing the test actually cares about, directly, instead of inferring it
// from an unrelated side channel.
TEST(SelfCheckScreen, ImuReadFailureRendersDifferentlyAndDoesNotCrash) {
    Harness h(wisp::test::temp_dir().string() + "/settings.json");
    ASSERT_TRUE(h.app.start());
    ASSERT_TRUE(h.wait_for_flushes(5));

    auto imu_row_text = [&] {
        return run_on_ui_thread(
            h.app.ui(), [] { return find_label_text_containing(lv_screen_active(), "IMU:"); });
    };

    EXPECT_NE(imu_row_text(), "IMU: 读取失败");

    h.imu->set_next_error(wisp::ErrorCode::IoError, "boom");
    ASSERT_TRUE(wait_until([&] { return imu_row_text() == "IMU: 读取失败"; }, 3000));

    // Still alive and rendering -- the "不崩溃" half of spec 8's
    // requirement.
    size_t flush_before_more = h.display->flush_count();
    EXPECT_TRUE(h.wait_for_flushes(flush_before_more + 5));

    h.app.stop();
}

// Re-review 1 (N3): standby must stop this screen's own "wisp.sample"
// IMU polling task, not just UiRuntime's rendering -- spec 6.7's "待机
// 时... 没有高频轮询" applies to it too (it was previously still
// reading the IMU at ~50Hz throughout standby). Drives
// PowerManager::enter_standby()/exit_standby() directly (the same calls
// a power-key press makes) rather than simulating a button press, since
// that wiring is already covered by power_manager_test.cpp's own tests.
TEST(SelfCheckScreen, ImuSamplingStopsDuringStandbyAndResumesOnWake) {
    Harness h(wisp::test::temp_dir().string() + "/settings.json");
    ASSERT_TRUE(h.app.start());
    ASSERT_TRUE(h.wait_for_flushes(5));

    wisp::app::PowerManager* pm = h.app.power_manager();
    ASSERT_NE(pm, nullptr);

    // Let a few IMU reads happen first, so the check below proves
    // sampling actually *stopped*, not merely that it "hasn't started
    // yet".
    ASSERT_TRUE(wait_until([&] { return h.imu->read_count() > 0; }, 2000));

    run_on_ui_blocking(h.app.ui(), [&] { pm->enter_standby(); });
    ASSERT_TRUE(wait_until([&] { return pm->standby(); }, 2000));
    // pm->standby() flips synchronously inside enter_standby(), but
    // SelfCheck's own standby_ flag (consulted by sample_loop(), which
    // runs on its own task) only catches up once the EventBus dispatcher
    // has actually delivered the "power.state" event to on_power_event()
    // -- asynchronously, on the bus's own dispatcher task. Without
    // waiting for that dispatch to finish first, there's a race where
    // sample_loop() still takes one more real IMU read() between the
    // wait_until() above and the snapshot below, making this assertion
    // flaky under scheduler pressure.
    ASSERT_TRUE(h.app.bus().wait_idle(2000));

    size_t reads_at_standby = h.imu->read_count();
    // Real time for the (now-paused) sampling task to prove it does NOT
    // keep reading the IMU while in standby.
    wisp::os::sleep_ms(300);
    EXPECT_EQ(h.imu->read_count(), reads_at_standby);

    run_on_ui_blocking(h.app.ui(), [&] { pm->exit_standby(); });
    ASSERT_TRUE(wait_until([&] { return !pm->standby(); }, 2000));
    EXPECT_TRUE(wait_until([&] { return h.imu->read_count() > reads_at_standby; }, 2000));

    h.app.stop();
}

// Task 12b (spec 5.6): on a non-rectangular screen, nothing the self-check
// screen draws may land outside its safe_rect() -- the root column itself
// is now sized/positioned to exactly that rect (see self_check_screen.cpp's
// build()), so every pixel outside it should be untouched screen
// background. Checked against a pixel known to always be outside any
// safe_rect (the very corner, (0, 0)) rather than a hardcoded guess at the
// theme's background color.
TEST(SelfCheckScreen, NoContentOutsideSafeRect) {
    for (const std::string& shape : {std::string("rounded_rect"), std::string("circle")}) {
        Harness h(wisp::test::temp_dir().string() + "/settings.json", /*init_sleep_ms=*/0, shape);
        ASSERT_TRUE(h.app.start());
        ASSERT_TRUE(h.wait_for_flushes(5));

        auto snapshot = h.app.ui()->snapshot_rgb888();
        ASSERT_TRUE(snapshot);
        const auto& pixels = snapshot.value();
        constexpr int32_t kSize = 480;
        ASSERT_EQ(pixels.size(), static_cast<size_t>(kSize) * kSize * 3u);

        auto shape_result = wisp::hal::screen_shape_from_caps(h.display->info().caps);
        ASSERT_TRUE(shape_result) << shape;
        wisp::hal::Area safe = wisp::hal::safe_rect(shape_result.value());

        auto pixel_at = [&](int32_t x, int32_t y) {
            size_t idx = (static_cast<size_t>(y) * kSize + x) * 3;
            return std::array<uint8_t, 3>{pixels[idx], pixels[idx + 1], pixels[idx + 2]};
        };
        // (0, 0) is outside safe_rect() for both shapes under test (margin
        // 12 for rounded_rect, 71 for circle), so it's always the screen's
        // own background -- see self_check_screen.cpp's build(): nothing
        // is placed outside the root column.
        auto background = pixel_at(0, 0);

        for (int32_t y = 0; y < kSize; ++y) {
            for (int32_t x = 0; x < kSize; ++x) {
                if (x >= safe.x1 && x <= safe.x2 && y >= safe.y1 && y <= safe.y2) {
                    continue;
                }
                ASSERT_EQ(pixel_at(x, y), background) << shape << " " << x << "," << y;
            }
        }

        h.app.stop();
    }
}

// M6, fix round 1: the perf-overlay toggle persists to Settings
// (ns="ui", key="perf_overlay") and is restored on the next boot. This
// drives that restore path directly (writing the setting the same way a
// prior session's long-press would have left it) rather than simulating
// the long-press gesture itself, which would need a touch indev this
// test harness doesn't have.
//
// Asserts via UiRuntime::perf_overlay_enabled(), not a pixel diff of
// snapshot_rgb888(): the sysmon overlay lives in LVGL's sys layer, a
// sibling of the active screen that lv_snapshot_take() structurally
// never renders (it only walks the one object -- here, the screen --
// it's given), so no snapshot comparison could ever actually detect it.
// An earlier version of this test diffed snapshots and was flaky (it
// "passed" only when something unrelated, like boot_ms jitter in the
// header, happened to differ between the two App instances).
TEST(SelfCheckScreen, PerfOverlaySettingPersistsAcrossRestarts) {
    std::string settings_path = wisp::test::temp_dir().string() + "/settings.json";

    {
        Harness h(settings_path);
        ASSERT_TRUE(h.app.start());
        ASSERT_NE(h.app.ui(), nullptr);
        EXPECT_FALSE(h.app.ui()->perf_overlay_enabled());
        h.app.stop();
    }

    // Simulate a previous session having toggled the overlay on, by
    // writing the setting directly through the same backend/file a real
    // long-press would have used (core::Settings(ns="ui")).
    {
        wisp::port::sim::JsonFileSettingsBackend backend(settings_path);
        wisp::core::Settings ui_settings(backend, "ui");
        ASSERT_TRUE(ui_settings.set("perf_overlay", true));
    }

    {
        Harness h(settings_path);
        ASSERT_TRUE(h.app.start());
        ASSERT_NE(h.app.ui(), nullptr);
        // start() blocks until the self-check screen's build() -- which
        // reads "perf_overlay" and, if true, calls
        // ui_->set_perf_overlay(true) -- has run, but set_perf_overlay()
        // itself only *posts* the actual lv_sysmon_show_performance()
        // call rather than waiting for it (see its doc comment); give
        // the UI task a moment to drain that second, chained post().
        EXPECT_TRUE(
            wisp::test::wait_until([&] { return h.app.ui()->perf_overlay_enabled(); }, 1000));
        h.app.stop();
    }
}

// M6, fix round 1: WISP_PERF is emitted every 5s via Platform::emit_line.
// Not "cheap" in wall-clock terms, but bounded and worth the ~5s to cover
// end to end (format, device lookups, heap_info being empty) rather than
// only unit-testing pieces of it.
TEST(SelfCheckScreen, EmitsWispPerfLine) {
    Harness h(wisp::test::temp_dir().string() + "/settings.json");
    ASSERT_TRUE(h.app.start());

    bool saw_perf_line = wait_until(
        [&] {
            std::lock_guard<std::mutex> lock(h.lines_mutex);
            for (const auto& line : h.lines) {
                if (line.rfind("WISP_PERF ", 0) == 0) {
                    return true;
                }
            }
            return false;
        },
        6000);
    EXPECT_TRUE(saw_perf_line);

    h.app.stop();
}
