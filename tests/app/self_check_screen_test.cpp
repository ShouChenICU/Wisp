// Smoke/regression tests for the real self-check screen (Task 12), on top
// of the same App/Board test harness app_boot_test.cpp uses. Boots an App
// with every fake device wired up, waits for the UI to actually render,
// and checks on *behavior* (does it render at all, does it render
// differently under an IMU error, does a persisted setting survive a
// restart, does WISP_PERF get emitted) rather than on exact pixel
// content, which would make these tests brittle against any layout
// tweak.

#include <gtest/gtest.h>

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
#include "wisp/base/error.hpp"
#include "wisp/base/value.hpp"
#include "wisp/core/device_registry.hpp"
#include "wisp/core/settings.hpp"
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

    explicit Harness(std::string settings_path_in, uint32_t init_sleep_ms = 0)
        : display(std::make_shared<FakeDisplay>("display.main", 480, 480, /*area_align=*/1)),
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

// Fix round 1, issue 3: build_header() used to read BootReport::to_value()
// before App::start() had called set_boot_ms() on it (that happened in a
// later step), so the self-check header's "启动 .. ms" was always "启动 0
// ms" regardless of how long boot actually took. set_boot_ms() is now
// called before the screen is built; this asserts the *report* --  which
// the header reads from -- already reflects a real, non-zero duration by
// the time start() returns (FullFakeBoard's deliberate sleep makes this
// non-flaky: without it, a sub-millisecond boot could legitimately still
// round to 0ms on a fast machine).
TEST(SelfCheckScreen, BootTimeIsSetBeforeScreenIsBuilt) {
    Harness h(wisp::test::temp_dir().string() + "/settings.json", /*init_sleep_ms=*/20);
    ASSERT_TRUE(h.app.start());

    auto rep = h.app.report().to_value(h.app.platform().board->name());
    const wisp::Value* boot_ms = rep.find("boot_ms");
    ASSERT_NE(boot_ms, nullptr);
    EXPECT_GT(boot_ms->as_int(), 0);

    h.app.stop();
}

// Spec 8: a runtime IMU read failure must show "读取失败" for that row,
// not crash. Toggling FakeImu mid-run (rather than comparing two separate
// App instances, whose headers would differ anyway from run to run just
// because of boot_ms/timing) isolates the IMU row specifically: within
// one App, only injecting the error should change anything rendered.
TEST(SelfCheckScreen, ImuReadFailureRendersDifferentlyAndDoesNotCrash) {
    Harness h(wisp::test::temp_dir().string() + "/settings.json");
    ASSERT_TRUE(h.app.start());
    ASSERT_TRUE(h.wait_for_flushes(5));

    auto healthy = h.app.ui()->snapshot_rgb888();
    ASSERT_TRUE(healthy);

    h.imu->set_next_error(wisp::ErrorCode::IoError, "boom");
    size_t flush_before_error = h.display->flush_count();
    ASSERT_TRUE(h.wait_for_flushes(flush_before_error + 5));

    auto errored = h.app.ui()->snapshot_rgb888();
    ASSERT_TRUE(errored);
    EXPECT_NE(healthy.value(), errored.value());

    // Still alive and rendering -- the "不崩溃" half of spec 8's
    // requirement.
    size_t flush_before_more = h.display->flush_count();
    EXPECT_TRUE(h.wait_for_flushes(flush_before_more + 5));

    h.app.stop();
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
