#include <gtest/gtest.h>

#include <algorithm>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "support/fake_devices.hpp"
#include "support/temp_dir.hpp"
#include "support/wait.hpp"
#include "wisp/app/app.hpp"
#include "wisp/app/board.hpp"
#include "wisp/app/power_manager.hpp"
#include "wisp/app/system_notices.hpp"
#include "wisp/base/json.hpp"
#include "wisp/core/boot_report.hpp"
#include "wisp/core/button_service.hpp"
#include "wisp/core/device_registry.hpp"
#include "wisp/core/log.hpp"
#include "wisp/core/notification_service.hpp"
#include "wisp/core/storage_manager.hpp"
#include "wisp/core/wakelock.hpp"
#include "wisp/os/os.hpp"
#include "wisp/port/sim/settings_json_file.hpp"
#include "wisp/ui/ui_runtime.hpp"

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
    std::span<const uint8_t> font_image;
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
        platform.font_image = opts.font_image;
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

// vector-font spec 3.6: without a font image the UI still starts, on
// bitmap fonts, and the boot report is printed as usual.
TEST(App, BootsWithoutFontImage) {
    auto h = make_app({.with_display = true, .font_image = {}});
    // The log ring is shared by the whole process, so only lines logged by
    // this start() count.
    const auto before = wisp::log::ring_entries();
    const uint32_t last_seq = before.empty() ? 0 : before.back().seq;
    ASSERT_TRUE(h.app.start());
    ASSERT_NE(h.app.ui(), nullptr);
    ASSERT_FALSE(h.lines.empty());
    EXPECT_EQ(h.lines[0].rfind("WISP_BOOT_REPORT ", 0), 0u);
    const auto entries = wisp::log::ring_entries();
    EXPECT_TRUE(std::any_of(entries.begin(), entries.end(), [last_seq](const auto& e) {
        return e.seq > last_seq && e.tag == "ui" &&
               e.msg == "fonts: no font image; large text uses bitmap fonts";
    }));
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

// App::stop() while in standby must leave standby first: buttons/storage
// are members of App::Impl (not torn down by stop() itself), so without
// this, a subsequent start() on the same App instance would resume with
// the screen asleep and the poll period still widened to the standby
// value -- see app.cpp's doc comment on the exit_standby() call stop()
// makes.
TEST(AppBoot, RestartAfterStoppingWhileInStandbyResetsScreenAndButtons) {
    auto h = make_app({.with_display = true});
    ASSERT_TRUE(h.app.start());
    ASSERT_NE(h.app.ui(), nullptr);
    ASSERT_NE(h.app.power_manager(), nullptr);

    // enter_standby() calls Display::set_power() directly (not via
    // post()); spec 5.3 requires that come from the UI thread (debug-
    // asserted -- see wisp/hal/display.hpp), so it's driven through
    // ui()->post() here rather than called straight from the test
    // thread, the same way production callers always reach it.
    wisp::ui::UiRuntime* ui = h.app.ui();
    wisp::app::PowerManager* pm = h.app.power_manager();
    auto entered = std::make_shared<wisp::os::Semaphore>(1, 0);
    ASSERT_TRUE(ui->post([pm, entered] {
        pm->enter_standby();
        entered->give();
    }));
    entered->take(wisp::os::kForever);
    ASSERT_TRUE(wait_until([&] { return pm->standby(); }, 2000));
    ASSERT_TRUE(wait_until([&] { return h.app.buttons().poll_interval_ms() == 50u; }, 2000));

    h.app.stop();
    ASSERT_TRUE(h.app.start());

    ASSERT_NE(h.app.ui(), nullptr);
    EXPECT_FALSE(h.app.ui()->paused());
    EXPECT_EQ(h.app.buttons().poll_interval_ms(), 20u);

    h.app.stop();
}

// Regression test for the race described on App::stop()'s exit-standby
// post (app.cpp): power_timer's last tick may have already posted
// enter_standby_if_still_idle() onto the UI task (see
// PowerManager::tick()) by the time power_timer->stop() returns, without
// that job having run yet -- so a standby() read taken on the
// stop()-calling thread right then cannot be trusted.
//
// Reproducing this needs enter_standby() to still be queued, unexecuted,
// at the exact moment stop() runs its check -- a one-instruction-wide
// window in production. Two gates turn it into a structural guarantee
// instead of a real-time race:
//   - Gate 1 (`release1`) freezes the post queue *before* stop() is
//     called, so stop_self_check()'s own UI-thread teardown post (always
//     the first thing App::stop() does to the UI task) is guaranteed to
//     land behind it rather than run immediately.
//   - A short sleep then lets that teardown job actually get enqueued
//     (a plain queue push, done in microseconds -- the sleep is a
//     generous margin, not a tight race), after which enter_standby()
//     is posted behind a *second* gate (`release2`). Because the UI
//     task drains its queue strictly in order, enter_standby() now
//     cannot run before the teardown job -- the thing stop() is
//     actually waiting on -- regardless of real scheduling.
//   - Releasing gate 1 lets stop() past the teardown wait and into
//     power_timer->stop() + the standby() check (and, in the fixed
//     code, its own guarded post) -- a few non-blocking instructions
//     that need no further queue progress to complete. A second short
//     sleep lets that run to completion before gate 2 is released, so
//     enter_standby() -- still frozen behind it until then -- cannot
//     possibly run before the check does.
TEST(AppBoot, StopUndoesStandbyQueuedJustBeforeTheCheck) {
    auto h = make_app({.with_display = true});
    ASSERT_TRUE(h.app.start());
    wisp::ui::UiRuntime* ui = h.app.ui();
    wisp::app::PowerManager* pm = h.app.power_manager();
    ASSERT_NE(ui, nullptr);
    ASSERT_NE(pm, nullptr);

    auto release1 = std::make_shared<wisp::os::Semaphore>(1, 0);
    ASSERT_TRUE(ui->post([release1] { release1->take(wisp::os::kForever); }));

    auto stopped = std::make_shared<wisp::os::Semaphore>(1, 0);
    wisp::os::TaskConfig stopper_config;
    stopper_config.name = "stopper";
    auto task = wisp::os::Task::spawn(stopper_config, [&h, stopped] {
        h.app.stop();
        stopped->give();
    });

    wisp::os::sleep_ms(50);

    auto release2 = std::make_shared<wisp::os::Semaphore>(1, 0);
    ASSERT_TRUE(ui->post([release2] { release2->take(wisp::os::kForever); }));
    ASSERT_TRUE(ui->post([pm] { pm->enter_standby(); }));

    release1->give();
    wisp::os::sleep_ms(50);
    release2->give();

    stopped->take(wisp::os::kForever);
    task->join();

    EXPECT_TRUE(h.display->powered());
    EXPECT_EQ(h.app.buttons().poll_interval_ms(), 20u);
}

TEST(AppNotifications, BatteryLowPostsSystemNotice) {
    auto h = make_app({.with_display = false});
    ASSERT_TRUE(h.app.start());
    wisp::core::NotificationService* notes = h.app.notifications();
    ASSERT_NE(notes, nullptr);
    h.app.bus().publish(
        {"power.battery.low", "power.main", 0, {{"battery_percent", wisp::Value(int64_t{12})}}});
    ASSERT_TRUE(wait_until([&] { return notes->unread_count() == 1; }, 2000));
    auto n = notes->unread()[0];
    EXPECT_EQ(n.source, "system");
    EXPECT_EQ(n.code, "battery_low");
    EXPECT_EQ(n.arg, "12");
    EXPECT_EQ(n.action, "");
}

TEST(AppNotifications, BatteryLowKeepsOneNoticeAndChargingClearsIt) {
    auto h = make_app({.with_display = false});
    ASSERT_TRUE(h.app.start());
    wisp::core::NotificationService* notes = h.app.notifications();
    ASSERT_NE(notes, nullptr);
    auto low = [&](int64_t pct) {
        h.app.bus().publish(
            {"power.battery.low", "power.main", 0, {{"battery_percent", wisp::Value(pct)}}});
        ASSERT_TRUE(h.app.bus().wait_idle(2000));
    };
    low(12);
    low(9);
    ASSERT_EQ(notes->unread_count(), 1u);
    EXPECT_EQ(notes->unread()[0].arg, "9");

    h.app.bus().publish({"power.charging.changed",
                         "power.main",
                         0,
                         {{"charging", wisp::Value(false)}, {"usb_present", wisp::Value(false)}}});
    ASSERT_TRUE(h.app.bus().wait_idle(2000));
    EXPECT_EQ(notes->unread_count(), 1u);
    h.app.bus().publish({"power.charging.changed",
                         "power.main",
                         0,
                         {{"charging", wisp::Value(true)}, {"usb_present", wisp::Value(true)}}});
    EXPECT_TRUE(wait_until([&] { return notes->unread_count() == 0; }, 2000));
}

TEST(AppNotifications, WakeLockReminderPosts) {
    using wisp::core::WakeLockKind;
    auto h = make_app({.with_display = false});
    ASSERT_TRUE(h.app.start());
    wisp::core::NotificationService* notes = h.app.notifications();
    ASSERT_NE(notes, nullptr);
    wisp::core::WakeLockRegistry& locks = h.app.wakelocks();
    const uint32_t scene_lock = locks.acquire("scene.a", WakeLockKind::Cpu, "timer");
    locks.acquire("system.net", WakeLockKind::Network, "sync");

    // Not yet due.
    wisp::app::post_wakelock_reminders(locks, *notes, wisp::os::now_ms());
    EXPECT_EQ(notes->unread_count(), 0u);

    wisp::app::post_wakelock_reminders(
        locks, *notes, wisp::os::now_ms() + wisp::core::WakeLockRegistry::kReminderMs);
    auto scene = notes->find("system", "wakelock_long:scene.a");
    ASSERT_TRUE(scene);
    EXPECT_EQ(scene->code, "wakelock_long");
    EXPECT_EQ(scene->arg, "scene.a");
    EXPECT_EQ(scene->action, "release_lock");
    EXPECT_EQ(scene->action_arg, std::to_string(scene_lock));
    auto system = notes->find("system", "wakelock_long:system.net");
    ASSERT_TRUE(system);
    EXPECT_EQ(system->arg, "system.net");
    EXPECT_EQ(system->action, "");
    EXPECT_EQ(system->action_arg, "");
}

TEST(AppNotifications, DeletedSceneLosesItsNotices) {
    auto h = make_app({.with_display = false});
    ASSERT_TRUE(h.app.start());
    wisp::core::NotificationService* notes = h.app.notifications();
    ASSERT_NE(notes, nullptr);
    wisp::core::Notification n;
    n.source = "pomodoro";
    n.title = "t";
    ASSERT_TRUE(notes->post(n, false));
    ASSERT_TRUE(notes->post(n, false));
    h.app.bus().publish({"scene.state",
                         "scene",
                         0,
                         {{"id", wisp::Value("pomodoro")}, {"state", wisp::Value("closed")}}});
    ASSERT_TRUE(h.app.bus().wait_idle(2000));
    EXPECT_EQ(notes->unread_count(), 2u);
    h.app.bus().publish({"scene.state",
                         "scene",
                         0,
                         {{"id", wisp::Value("pomodoro")}, {"state", wisp::Value("deleted")}}});
    EXPECT_TRUE(wait_until([&] { return notes->unread_count() == 0; }, 2000));
}

TEST(AppNotifications, StopSavesPendingNotices) {
    auto h = make_app({.with_display = false});
    ASSERT_TRUE(h.app.start());
    auto root = h.app.storage().write_root(wisp::core::Area::System);
    ASSERT_TRUE(root);
    h.app.notifications()->post_system(wisp::core::notice::kRestoreFailed, "weather");
    h.app.stop();
    std::ifstream f(root.value() + "/notifications.json");
    std::string text((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    EXPECT_NE(text.find("restore_failed"), std::string::npos) << text;
}
