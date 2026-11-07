#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "app/ui_net_harness.hpp"
#include "firmware_confirm.hpp"
#include "storage_io.hpp"
#include "support/fake_devices.hpp"
#include "support/lvgl_probe.hpp"
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
#include "wisp/core/model_profiles.hpp"
#include "wisp/core/notification_service.hpp"
#include "wisp/core/settings.hpp"
#include "wisp/core/storage_manager.hpp"
#include "wisp/core/storage_worker.hpp"
#include "wisp/core/wakelock.hpp"
#include "wisp/os/flash_guard.hpp"
#include "wisp/os/os.hpp"
#include "wisp/port/sim/settings_json_file.hpp"
#include "wisp/scene/scene_runtime.hpp"
#include "wisp/shell/shell.hpp"
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
    // Registered as "storage.sd" when set.
    std::shared_ptr<FakeStorage> sd;
    // Every settings write_batch() first sleeps this long.
    uint32_t settings_write_delay_ms = 0;
};

// The simulator's JSON file backend, with writes slowed down so a test can
// tell whether someone waited for them.
class SlowSettings : public wisp::core::SettingsBackend {
public:
    SlowSettings(const std::string& path, uint32_t delay_ms) : inner_(path), delay_ms_(delay_ms) {}
    wisp::Result<wisp::Value> get(const std::string& ns, const std::string& key) override {
        return inner_.get(ns, key);
    }
    wisp::Result<void> set(const std::string& ns, const std::string& key,
                           const wisp::Value& value) override {
        return inner_.set(ns, key, value);
    }
    wisp::Result<void> erase(const std::string& ns, const std::string& key) override {
        return inner_.erase(ns, key);
    }
    wisp::Result<void> erase_all() override { return inner_.erase_all(); }
    wisp::Result<std::vector<wisp::core::SettingsEntry>> load_all() override {
        return inner_.load_all();
    }
    wisp::Result<void> write_batch(std::span<const wisp::core::SettingsChange> changes) override {
        std::this_thread::sleep_for(std::chrono::milliseconds(delay_ms_));
        return inner_.write_batch(changes);
    }

private:
    wisp::port::sim::JsonFileSettingsBackend inner_;
    uint32_t delay_ms_;
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
        if (opts_.sd) maybe_register(registry, report, opts_.sd);
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
    // Platform::reboot calls it.
    std::function<void()> on_reboot;
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
        if (opts.settings_write_delay_ms != 0) {
            platform.settings = std::make_unique<SlowSettings>(settings_dir + "/settings.json",
                                                               opts.settings_write_delay_ms);
        } else {
            platform.settings = std::make_unique<wisp::port::sim::JsonFileSettingsBackend>(
                settings_dir + "/settings.json");
        }
        platform.emit_line = [this](const std::string& line) { lines.push_back(line); };
        platform.reboot = [this] {
            if (on_reboot) on_reboot();
        };
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
    // A scene's screen lock only exists while that scene is in the
    // foreground, so the user can already see it: no reminder for it.
    locks.acquire("scene.clock", WakeLockKind::Screen, "keep_screen_on");

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
    EXPECT_FALSE(notes->find("system", "wakelock_long:scene.clock").has_value());
    EXPECT_EQ(notes->unread_count(), 2u);
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

TEST(App, StorageRefreshRunsOnTheWorker) {
    MakeAppOptions opts;
    opts.with_display = false;
    opts.sd = std::make_shared<FakeStorage>("storage.sd", wisp::test::temp_dir().string(),
                                            /*removable=*/true);
    std::atomic<int> polls{0};
    std::atomic<int> off_thread{0};
    std::atomic<bool> hold{false};
    wisp::os::Semaphore entered(1, 0);
    wisp::os::Semaphore release(1, 0);
    opts.sd->on_online = [&] {
        polls.fetch_add(1);
        if (!wisp::os::on_flash_thread()) off_thread.fetch_add(1);
        if (hold.exchange(false)) {
            entered.give();
            release.take(wisp::os::kForever);
        }
    };
    Harness h(opts);
    ASSERT_TRUE(h.app.start());
    ASSERT_NE(h.app.storage_worker(), nullptr);

    // The timer's own refresh may be in flight; ours goes in once it is not.
    hold.store(true);
    ASSERT_TRUE(wait_until([&] { return wisp::app::queue_storage_refresh(h.app); }, 3000));
    ASSERT_TRUE(entered.take(3000));
    // Still running: the next one (the timer's included) is skipped.
    EXPECT_FALSE(wisp::app::queue_storage_refresh(h.app));
    const int before = polls.load();
    release.give();
    ASSERT_TRUE(wait_until([&] { return wisp::app::queue_storage_refresh(h.app); }, 3000));
    ASSERT_TRUE(wait_until([&] { return polls.load() > before; }, 3000));

    // The 2 s timer queues it as well.
    const int timer_before = polls.load();
    ASSERT_TRUE(wait_until([&] { return polls.load() > timer_before; }, 5000));
    h.app.stop();
    EXPECT_EQ(off_thread.load(), 0);
}

TEST(App, RestartFlushesSettingsFirst) {
    MakeAppOptions opts;
    opts.with_display = false;
    opts.settings_write_delay_ms = 300;
    Harness h(opts);
    std::atomic<int64_t> on_disk{-2};
    // What a fresh boot would read, read on the flash thread: the worker
    // is still running when the restart hook fires.
    h.on_reboot = [&] {
        auto read = [&] {
            wisp::port::sim::JsonFileSettingsBackend fresh(h.settings_dir + "/settings.json");
            on_disk.store(wisp::core::Settings(fresh, "test").get_int("k", -1));
        };
        EXPECT_TRUE(h.app.storage_worker()->run_sync(read, wisp::os::kForever));
    };
    ASSERT_TRUE(h.app.start());

    ASSERT_TRUE(wisp::core::Settings(*h.app.platform().settings, "test").set("k", 7));
    wisp::app::restart_for_firmware(h.app);
    EXPECT_EQ(on_disk.load(), 7);
}

TEST(AppNotifications, FailedServiceSavesPostTheSettingsNotice) {
    auto h = make_app({.with_display = false});
    ASSERT_TRUE(h.app.start());
    wisp::core::NotificationService* notes = h.app.notifications();
    ASSERT_NE(notes, nullptr);
    // A non-empty directory where models.json goes makes every write fail.
    const std::string system = h.app.storage().write_root(wisp::core::Area::System).value();
    std::filesystem::create_directories(system + "/secrets/models.json/blocker");

    for (const char* name : {"a", "b", "c"}) {
        ASSERT_TRUE(h.app.models().create(
            {.kind = wisp::core::ModelKind::Llm, .name = name, .preset = "openai", .model = "m"}));
        // One write each, not one for all three.
        ASSERT_TRUE(h.app.storage_worker()->drain(2000));
    }
    ASSERT_TRUE(wait_until([&] { return notes->unread_count() == 1; }, 2000));
    EXPECT_EQ(notes->unread()[0].code, wisp::core::notice::kSettingsNotSaved);
}

// mvp spec 6.6: the device boots into the home screen and the hardware
// diagnostics, the old self-check screen, are not built at all until asked
// for. The tests below boot the networked harness with a touch panel (the
// back gesture) and an IMU (the diagnostics read it while they show).
namespace {

using wisp::test::run_on_ui_blocking;
using wisp::test::UiNetHarness;
using wisp::test::UiNetOptions;

UiNetOptions diag_opts() {
    UiNetOptions opts;
    opts.with_touch = true;
    opts.imu = std::make_shared<FakeImu>("imu.main");
    return opts;
}

std::vector<std::string> imu_holders(UiNetHarness& h) {
    std::vector<std::string> out;
    for (const auto& e : h.app->occupancy().list()) out.push_back(e.holder);
    return out;
}

lv_obj_t* active_screen(UiNetHarness& h) {
    lv_obj_t* scr = nullptr;
    run_on_ui_blocking(h.app->ui(), [&] { scr = lv_screen_active(); });
    return scr;
}

bool diagnostics_open(UiNetHarness& h) {
    bool open = false;
    run_on_ui_blocking(h.app->ui(), [&] { open = h.app->shell()->diagnostics_open(); });
    return open;
}

lv_obj_t* open_diagnostics(UiNetHarness& h) {
    run_on_ui_blocking(h.app->ui(), [&] { h.app->shell()->open_diagnostics(); });
    return active_screen(h);
}

// A back swipe from the left edge, as the touch panel reports it.
void back_swipe(UiNetHarness& h) {
    lv_indev_t* indev = nullptr;
    run_on_ui_blocking(h.app->ui(), [&] { indev = lv_indev_get_next(nullptr); });
    ASSERT_NE(indev, nullptr);
    auto feed = [&](std::vector<wisp::hal::TouchPoint> points) {
        h.touch->set_points(std::move(points));
        run_on_ui_blocking(h.app->ui(), [&] { lv_indev_read(indev); });
    };
    for (int16_t x : {4, 4, 60, 120, 180, 240}) feed({{x, 240, 0}});
    feed({});
}

const std::vector<std::string> kSelfCheck = {"system.selfcheck"};

}  // namespace

TEST(AppBoot, BootsIntoHome) {
    UiNetHarness h(diag_opts());
    ASSERT_TRUE(h.start());
    ASSERT_NE(h.app->shell(), nullptr);
    lv_obj_t* home = nullptr;
    run_on_ui_blocking(h.app->ui(), [&] { home = h.app->shell()->home_screen(); });
    ASSERT_NE(home, nullptr);
    EXPECT_EQ(active_screen(h), home);
    EXPECT_EQ(h.app->shell()->page(), wisp::shell::Page::Home);
    EXPECT_FALSE(diagnostics_open(h));
    // No self-check: nothing reads the IMU.
    wisp::os::sleep_ms(300);
    EXPECT_TRUE(imu_holders(h).empty());
}

TEST(AppBoot, DiagnosticsOpensAndBackReturns) {
    UiNetHarness h(diag_opts());
    ASSERT_TRUE(h.start());
    lv_obj_t* home = active_screen(h);

    lv_obj_t* diag = open_diagnostics(h);
    EXPECT_NE(diag, home);
    EXPECT_TRUE(diagnostics_open(h));
    EXPECT_FALSE(wisp::test::run_on_ui_thread(h.app->ui(), [&] {
                     return wisp::test::find_label_text_containing(diag, "Devices");
                 }).empty());
    ASSERT_TRUE(wait_until([&] { return imu_holders(h) == kSelfCheck; }, 3000));

    back_swipe(h);
    ASSERT_TRUE(wait_until([&] { return active_screen(h) == home; }, 3000));
    EXPECT_FALSE(diagnostics_open(h));
    // Torn down: its sampler let the IMU go.
    EXPECT_TRUE(wait_until([&] { return imu_holders(h).empty(); }, 3000));

    // And it opens again.
    EXPECT_NE(open_diagnostics(h), home);
    EXPECT_TRUE(diagnostics_open(h));
}

// The 3b-1 rule (spec 6.6): a scene over the diagnostics covers them and
// they let the IMU go; once the scene goes they take it again, and the back
// gesture is theirs once more.
TEST(AppBoot, DiagnosticsImuRuleUnchanged) {
    UiNetHarness h(diag_opts());
    const std::filesystem::path dir = std::filesystem::path(h.root) / "scenes" / "plain";
    std::filesystem::create_directories(dir);
    std::ofstream(dir / "scene.json")
        << R"({"id":"plain","name":"Plain","version":"1.0.0","schema_version":1})";
    std::ofstream(dir / "main.lua") << R"(return ui.screen { ui.label { text = "plain" } })";
    ASSERT_TRUE(h.start());
    lv_obj_t* home = active_screen(h);
    lv_obj_t* diag = open_diagnostics(h);
    ASSERT_TRUE(wait_until([&] { return imu_holders(h) == kSelfCheck; }, 3000));

    wisp::scene::SceneRuntime* rt = h.app->scenes();
    ASSERT_NE(rt, nullptr);
    ASSERT_TRUE(rt->open("plain"));
    ASSERT_TRUE(
        wait_until([&] { return rt->foreground() == std::optional<std::string>("plain"); }, 3000));
    ASSERT_TRUE(wait_until([&] { return active_screen(h) != diag; }, 3000));
    EXPECT_TRUE(wait_until([&] { return imu_holders(h).empty(); }, 3000));

    // The back gesture closes the scene, back to the diagnostics.
    back_swipe(h);
    ASSERT_TRUE(wait_until([&] { return !rt->foreground().has_value(); }, 3000));
    ASSERT_TRUE(wait_until([&] { return active_screen(h) == diag; }, 3000));
    EXPECT_TRUE(wait_until([&] { return imu_holders(h) == kSelfCheck; }, 3000));

    // ... and from there to the home screen.
    back_swipe(h);
    ASSERT_TRUE(wait_until([&] { return active_screen(h) == home; }, 3000));
    EXPECT_FALSE(diagnostics_open(h));
}

namespace {

// A scene "plain" in the harness's scene directory, opened in front.
wisp::scene::SceneRuntime* open_plain_scene(UiNetHarness& h) {
    wisp::scene::SceneRuntime* rt = h.app->scenes();
    if (rt == nullptr || !rt->open("plain")) return nullptr;
    if (!wait_until([&] { return rt->foreground() == std::optional<std::string>("plain"); }, 3000))
        return nullptr;
    return rt;
}

void write_plain_scene(UiNetHarness& h) {
    const std::filesystem::path dir = std::filesystem::path(h.root) / "scenes" / "plain";
    std::filesystem::create_directories(dir);
    std::ofstream(dir / "scene.json")
        << R"({"id":"plain","name":"Plain","version":"1.0.0","schema_version":1})";
    std::ofstream(dir / "main.lua") << R"(return ui.screen { ui.label { text = "plain" } })";
}

}  // namespace

// mvp spec 6.3, 6.6: going home (the agent_done notice's tap, say) with a
// scene in front sends the scene back and shows the home screen; the
// scene's view is not left running off-screen.
TEST(AppBoot, HomeSendsTheSceneInFrontBack) {
    UiNetHarness h(diag_opts());
    write_plain_scene(h);
    ASSERT_TRUE(h.start());
    lv_obj_t* home = active_screen(h);
    wisp::scene::SceneRuntime* rt = open_plain_scene(h);
    ASSERT_NE(rt, nullptr);
    ASSERT_TRUE(wait_until([&] { return active_screen(h) != home; }, 3000));

    run_on_ui_blocking(h.app->ui(), [&] { h.app->shell()->show(wisp::shell::Page::Home); });
    EXPECT_EQ(active_screen(h), home);
    ASSERT_TRUE(wait_until([&] { return !rt->foreground().has_value(); }, 3000));
    // The view went without loading anything over the home screen.
    EXPECT_TRUE(wait_until(
        [&] {
            bool none = false;
            run_on_ui_blocking(h.app->ui(), [&] { none = rt->ui_current_view() == nullptr; });
            return none;
        },
        3000));
    EXPECT_EQ(active_screen(h), home);
}

// The same over the diagnostics: the scene goes back and the diagnostics
// close too.
TEST(AppBoot, HomeOverDiagnosticsSendsTheSceneBack) {
    UiNetHarness h(diag_opts());
    write_plain_scene(h);
    ASSERT_TRUE(h.start());
    lv_obj_t* home = active_screen(h);
    lv_obj_t* diag = open_diagnostics(h);
    ASSERT_NE(diag, home);
    wisp::scene::SceneRuntime* rt = open_plain_scene(h);
    ASSERT_NE(rt, nullptr);
    ASSERT_TRUE(wait_until([&] { return active_screen(h) != diag; }, 3000));

    run_on_ui_blocking(h.app->ui(), [&] { h.app->shell()->show(wisp::shell::Page::Home); });
    EXPECT_EQ(active_screen(h), home);
    EXPECT_FALSE(diagnostics_open(h));
    ASSERT_TRUE(wait_until([&] { return !rt->foreground().has_value(); }, 3000));
    EXPECT_TRUE(wait_until([&] { return imu_holders(h).empty(); }, 3000));
    EXPECT_EQ(active_screen(h), home);
}

// Opening the diagnostics with a scene in front sends it back first, so
// the back swipe from the diagnostics reaches the home screen.
TEST(AppBoot, DiagnosticsSendTheSceneInFrontBack) {
    UiNetHarness h(diag_opts());
    write_plain_scene(h);
    ASSERT_TRUE(h.start());
    lv_obj_t* home = active_screen(h);
    wisp::scene::SceneRuntime* rt = open_plain_scene(h);
    ASSERT_NE(rt, nullptr);
    ASSERT_TRUE(wait_until([&] { return active_screen(h) != home; }, 3000));

    lv_obj_t* diag = open_diagnostics(h);
    EXPECT_TRUE(diagnostics_open(h));
    ASSERT_TRUE(wait_until([&] { return !rt->foreground().has_value(); }, 3000));
    EXPECT_EQ(active_screen(h), diag);

    back_swipe(h);
    ASSERT_TRUE(wait_until([&] { return active_screen(h) == home; }, 3000));
    EXPECT_FALSE(diagnostics_open(h));
}
