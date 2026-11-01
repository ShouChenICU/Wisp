#include <gtest/gtest.h>

#include <map>
#include <memory>
#include <string>
#include <utility>
#include <vector>

#include "support/fake_devices.hpp"
#include "support/temp_dir.hpp"
#include "support/wait.hpp"
#include "wisp/app/app.hpp"
#include "wisp/app/board.hpp"
#include "wisp/base/json.hpp"
#include "wisp/core/boot_report.hpp"
#include "wisp/core/device_registry.hpp"
#include "wisp/port/sim/settings_json_file.hpp"

using wisp::test::FakeButton;
using wisp::test::FakeDisplay;
using wisp::test::FakeImu;
using wisp::test::FakeStorage;
using wisp::test::wait_until;

namespace {

// Options for make_app() below: which fake devices a FakeBoard registers,
// and which ones it instead records as DeviceState::Failed (keyed by
// device id, value is the recorded reason).
struct MakeAppOptions {
    bool with_display = true;
    bool with_internal_storage = true;
    std::map<std::string, std::string> fail;
};

// A wisp::app::Board that registers a fixed, test-controlled set of fake
// devices: "display.main" (if with_display), "storage.int" (if
// with_internal_storage), "imu.main" and "button.power" -- except that
// any id listed in `opts.fail` is instead recorded into the BootReport as
// DeviceState::Failed (with the configured reason) and never added to the
// registry, exercising App::start()'s "boot continues past a failed
// device" path.
class FakeBoard : public wisp::app::Board {
public:
    FakeBoard(MakeAppOptions opts, std::shared_ptr<FakeDisplay> display)
        : opts_(std::move(opts)), display_(std::move(display)) {}

    std::string name() const override { return "fake"; }

    void init(wisp::core::DeviceRegistry& registry, wisp::core::BootReport& report) override {
        if (opts_.with_display) {
            maybe_register(registry, report, display_);
        }
        if (opts_.with_internal_storage) {
            maybe_register(
                registry, report,
                std::make_shared<FakeStorage>("storage.int", wisp::test::temp_dir().string(),
                                              /*removable=*/false));
        }
        maybe_register(registry, report, std::make_shared<FakeImu>("imu.main"));
        maybe_register(registry, report, std::make_shared<FakeButton>("button.power"));
    }

private:
    void maybe_register(wisp::core::DeviceRegistry& registry, wisp::core::BootReport& report,
                        std::shared_ptr<wisp::hal::Device> device) {
        auto it = opts_.fail.find(device->info().id);
        if (it != opts_.fail.end()) {
            report.record({device->info().id, device->info().cls, wisp::core::DeviceState::Failed,
                           it->second});
            return;
        }
        registry.add(device);
    }

    MakeAppOptions opts_;
    std::shared_ptr<FakeDisplay> display_;
};

// Bundles a freshly constructed App together with the test-observable
// state its Platform feeds into (the emitted diagnostics lines, and the
// FakeDisplay if one was registered). See make_app() below.
struct Harness {
    std::vector<std::string> lines;
    std::shared_ptr<FakeDisplay> display;
    std::string settings_dir;
    wisp::app::App app;

    explicit Harness(const MakeAppOptions& opts)
        : settings_dir(wisp::test::temp_dir().string()), app(build_platform(opts)) {}

private:
    wisp::app::Platform build_platform(const MakeAppOptions& opts) {
        if (opts.with_display) {
            display = std::make_shared<FakeDisplay>("display.main", 480, 480, /*area_align=*/1);
        }

        wisp::app::Platform platform;
        platform.board = std::make_unique<FakeBoard>(opts, display);
        platform.settings = std::make_unique<wisp::port::sim::JsonFileSettingsBackend>(
            settings_dir + "/settings.json");
        platform.emit_line = [this](const std::string& line) { lines.push_back(line); };
        return platform;
    }
};

Harness make_app(MakeAppOptions opts) { return Harness(opts); }

}  // namespace

TEST(AppBoot, EmitsReportAndManifest) {
    auto h = make_app({.with_display = false});
    ASSERT_TRUE(h.app.start());
    ASSERT_EQ(h.lines.size(), 2u);
    EXPECT_EQ(h.lines[0].rfind("WISP_BOOT_REPORT ", 0), 0u);
    auto rep = wisp::from_json(h.lines[0].substr(17)).value();
    EXPECT_EQ(rep.find("notes")->find("mode")->as_string(), "headless");
    EXPECT_EQ(h.lines[1].rfind("WISP_MANIFEST ", 0), 0u);
    EXPECT_EQ(h.app.ui(), nullptr);
}

TEST(AppBoot, FailedDeviceIsReportedAndBootContinues) {
    auto h = make_app({.with_display = false, .fail = {{"imu.main", "disabled"}}});
    ASSERT_TRUE(h.app.start());
    EXPECT_EQ(h.app.report().find("imu.main")->state, wisp::core::DeviceState::Failed);
    EXPECT_EQ(h.app.registry().find("imu.main"), nullptr);
    EXPECT_EQ(h.app.report().find("storage.int")->state, wisp::core::DeviceState::Ok);
}

TEST(AppBoot, StartsUiWhenDisplayPresent) {
    auto h = make_app({.with_display = true});
    ASSERT_TRUE(h.app.start());
    ASSERT_NE(h.app.ui(), nullptr);
    EXPECT_TRUE(wait_until([&] { return h.display->flush_count() > 0; }, 2000));
    EXPECT_EQ(h.display->brightness(), 200);
}

TEST(AppBoot, MissingInternalStorageIsReadonlyNotFatal) {
    auto h = make_app({.with_display = false, .with_internal_storage = false});
    ASSERT_TRUE(h.app.start());
    auto rep = wisp::from_json(h.lines[0].substr(17)).value();
    EXPECT_EQ(rep.find("notes")->find("storage")->as_string(), "readonly");
}

// Not in the brief's Step 1 list, but the brief explicitly calls for
// start() to have *some* documented behavior on a second call while
// already started: App::start() returns ErrorCode::Busy without
// re-running the boot sequence (so e.g. no extra WISP_BOOT_REPORT/
// WISP_MANIFEST lines are emitted) -- see app.hpp's start() doc comment.
TEST(AppBoot, StartingTwiceIsBusy) {
    auto h = make_app({.with_display = false});
    ASSERT_TRUE(h.app.start());
    ASSERT_EQ(h.lines.size(), 2u);

    auto second = h.app.start();
    EXPECT_FALSE(second);
    EXPECT_EQ(second.error().code, wisp::ErrorCode::Busy);
    EXPECT_EQ(h.lines.size(), 2u);
}

// stop() must be safe (and a no-op) even if start() never succeeded --
// the destructor relies on exactly this when a test's App never started,
// or started and was already stop()'d manually.
TEST(AppBoot, StopBeforeStartAndStopTwiceAreSafe) {
    auto h = make_app({.with_display = false});
    h.app.stop();
    ASSERT_TRUE(h.app.start());
    h.app.stop();
    h.app.stop();
}
