// Smoke test for the real self-check screen (Task 12), on top of the same
// App/Board test harness app_boot_test.cpp uses. Boots an App with every
// fake device wired up, waits for the UI to actually render, and checks
// that what it rendered isn't a blank/uniform frame -- i.e. that building
// the screen produced *something* (labels, the IMU tilt ball, etc.),
// without asserting on exact pixel content (which would make this test
// brittle against any layout tweak).

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <utility>

#include "support/fake_devices.hpp"
#include "support/temp_dir.hpp"
#include "support/wait.hpp"
#include "wisp/app/app.hpp"
#include "wisp/app/board.hpp"
#include "wisp/core/device_registry.hpp"
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
// knows how to look up (display, touch is intentionally left out --
// FakeDisplay/FakeImu/etc. don't include a FakeTouch, and the screen must
// tolerate that the same way it tolerates a missing power/clock/SD
// device), so building the screen exercises every section of spec 8's
// layout rather than just the "device missing" fallback text.
class FullFakeBoard : public wisp::app::Board {
public:
    explicit FullFakeBoard(std::shared_ptr<FakeDisplay> display) : display_(std::move(display)) {}

    std::string name() const override { return "fake-full"; }

    void init(wisp::core::DeviceRegistry& registry, wisp::core::BootReport& /*report*/) override {
        registry.add(display_);
        registry.add(std::make_shared<FakeStorage>("storage.int", wisp::test::temp_dir().string(),
                                                   /*removable=*/false));
        registry.add(std::make_shared<FakeImu>("imu.main"));
        registry.add(std::make_shared<FakeButton>("button.power"));
        registry.add(std::make_shared<FakePower>("power.main"));
        registry.add(std::make_shared<FakeClock>("clock.main"));
    }

private:
    std::shared_ptr<FakeDisplay> display_;
};

}  // namespace

TEST(SelfCheckScreen, RendersNonUniformFrame) {
    auto display = std::make_shared<FakeDisplay>("display.main", 480, 480, /*area_align=*/1);
    std::string settings_dir = wisp::test::temp_dir().string();

    wisp::app::Platform platform;
    platform.board = std::make_unique<FullFakeBoard>(display);
    platform.settings =
        std::make_unique<wisp::port::sim::JsonFileSettingsBackend>(settings_dir + "/settings.json");
    platform.emit_line = [](const std::string&) {};

    wisp::app::App app(std::move(platform));
    ASSERT_TRUE(app.start());
    ASSERT_NE(app.ui(), nullptr);

    // Give the sampling task and the 100ms UI refresh timer a few ticks,
    // then wait for several real flushes so the screen has actually been
    // drawn (not just the first, possibly-still-empty frame).
    ASSERT_TRUE(wait_until([&] { return display->flush_count() > 5; }, 3000));

    auto snapshot = app.ui()->snapshot_rgb888();
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

    app.stop();
}
