#include "wisp/app/power_manager.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "support/fake_devices.hpp"
#include "support/temp_dir.hpp"
#include "support/wait.hpp"
#include "wisp/app/power_key_controller.hpp"
#include "wisp/core/device_registry.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/core/log.hpp"
#include "wisp/core/power_monitor.hpp"
#include "wisp/core/settings.hpp"
#include "wisp/core/storage_manager.hpp"
#include "wisp/core/wakelock.hpp"
#include "wisp/hal/device.hpp"
#include "wisp/os/os.hpp"
#include "wisp/port/sim/settings_json_file.hpp"
#include "wisp/ui/ui_runtime.hpp"

using wisp::app::PowerKeyController;
using wisp::app::PowerManager;
using wisp::app::PowerManagerDeps;
using wisp::core::ButtonService;
using wisp::core::DeviceRegistry;
using wisp::core::Event;
using wisp::core::EventBus;
using wisp::core::PowerMonitor;
using wisp::core::Settings;
using wisp::core::StorageManager;
using wisp::core::WakeLockKind;
using wisp::core::WakeLockRegistry;
using wisp::hal::DeviceClass;
using wisp::hal::PowerLevel;
using wisp::port::sim::JsonFileSettingsBackend;
using wisp::test::FakeButton;
using wisp::test::FakeDisplay;
using wisp::test::FakeImu;
using wisp::test::FakePower;
using wisp::test::FakeStorage;
using wisp::test::FakeTouch;
using wisp::test::temp_dir;
using wisp::test::wait_until;
using wisp::ui::UiRuntime;

namespace {

// Everything a PowerManager test needs: a real (but Fake-backed) Display/
// Touch/UiRuntime/ButtonService/EventBus, plus a JSON-file Settings
// backend under a fresh temp dir -- see tests/core/settings_test.cpp for
// the same backend used the same way. `light_sleep_calls` records every
// Platform::set_light_sleep() call PowerManager makes. `btn` is
// "button.power"; `vol_btn` ("button.vol_up") exists so the power-key-
// ownership integration tests below can distinguish "woken via the
// owned power key path" from "woken via PowerManager's own generic
// any-button-wakes-it path".
struct Fixture {
    // `with_storage=false` passes a null StorageManager* into
    // PowerManager, the same way a headless build would pass null
    // display/touch/ui/buttons -- see
    // NullStorageManagerIsToleratedByStandbyTransitions below. Likewise
    // `with_power_monitor=false` passes a null PowerMonitor*; see
    // NullPowerMonitorIsToleratedByStandbyTransitions below.
    explicit Fixture(int64_t screen_off_s = 60, bool with_storage = true,
                     bool with_power_monitor = true) {
        settings_path = (temp_dir() / "power_settings.json").string();
        backend = std::make_unique<JsonFileSettingsBackend>(settings_path);
        Settings power_settings(*backend, "power");
        power_settings.set("screen_off_s", screen_off_s);

        bus.start();
        // An exact-topic subscribe, not a "power."-prefix one -- these
        // tests only care about PowerManager's own "power.state"
        // topic, and a prefix subscribe would also pick up
        // wisp::core::PowerMonitor's "power.charging.changed"/"power.
        // battery.low" topics on a bus a future test shares with one.
        bus.subscribe("power.state", [this](const Event& e) {
            std::lock_guard<std::mutex> lock(power_events_mutex);
            power_events.push_back(e);
        });

        registry.add(btn);
        registry.add(vol_btn);
        registry.add(display);
        registry.add(touch);
        registry.add(imu);

        // A real (Fake-backed) StorageManager, so PowerManager's
        // set_standby() forwarding can
        // be exercised the same way as display/touch/imu above.
        registry.add(int_storage);
        registry.add(sd_storage);
        storage = std::make_unique<StorageManager>(registry, bus);
        if (!storage->init()) {
            ADD_FAILURE() << "StorageManager::init() failed";
        }

        ui = std::make_unique<UiRuntime>(display, touch);
        if (!ui->start()) {
            ADD_FAILURE() << "UiRuntime::start() failed";
        }

        buttons = std::make_unique<ButtonService>(registry, bus);
        buttons->start();

        // A real PowerMonitor (Fake-backed), so PowerManager's
        // set_standby() forwarding to it can be exercised the same way as
        // storage above -- see
        // EnterAndExitStandbyWidenAndRestorePowerMonitorSampleInterval
        // below. Deliberately never start()ed: these tests only
        // check the standby-period hook (sample_interval_ms()), not the
        // Timer actually firing, so there's no reason to add a real
        // background Timer's scheduling noise to every other test in this
        // file that doesn't care about it either way.
        power_monitor = std::make_unique<PowerMonitor>(bus, power, Settings(*backend, "power"));

        pm = std::make_unique<PowerManager>(PowerManagerDeps{
            .bus = bus,
            .registry = registry,
            .locks = locks,
            .settings = Settings(*backend, "power"),
            .display = display,
            .storage = with_storage ? storage.get() : nullptr,
            .power_monitor = with_power_monitor ? power_monitor.get() : nullptr,
            .ui = ui.get(),
            .buttons = buttons.get(),
            .set_light_sleep = [this](bool on) { light_sleep_calls.push_back(on); },
        });
    }

    ~Fixture() {
        pm.reset();
        buttons->stop();
        ui->stop();
        bus.stop();
    }

    // power_events is written from the EventBus's dispatch thread (the
    // subscriber above) and read from
    // the test thread with no synchronization otherwise -- a real data
    // race (TSan-visible) that happened to "work" only because the races
    // never changed what the already-sampled snapshot looked like by
    // the time a wait_until() loop's predicate re-ran. These two helpers
    // are the only way tests below touch power_events now.
    size_t power_event_count() {
        std::lock_guard<std::mutex> lock(power_events_mutex);
        return power_events.size();
    }
    Event power_event_at(size_t index) {
        std::lock_guard<std::mutex> lock(power_events_mutex);
        return power_events.at(index);
    }

    // Blocks until every job posted to `ui` so far has
    // actually run, by posting one more and waiting for it -- UiRuntime's
    // post queue is FIFO/single-consumer, so this guarantees any earlier
    // tick()-posted enter_standby_if_still_idle() job has already run (or
    // never existed) by the time this returns. Without draining first, a
    // negative assertion (EXPECT_FALSE(standby())) right after tick()
    // can't tell "the idle-timeout logic correctly decided not to enter
    // standby" apart from "it decided to, but the UI task just hasn't
    // gotten to the posted job yet" -- tick() itself only ever posts,
    // never blocks.
    void drain_ui() {
        auto done = std::make_shared<wisp::os::Semaphore>(1, 0);
        while (!ui->post([done] { done->give(); })) {
            wisp::os::sleep_ms(1);
        }
        done->take(wisp::os::kForever);
    }

    // Runs `fn` on the UI task and blocks until it has returned. Needed
    // for any direct enter_standby()/exit_standby() call from a test:
    // spec 5.3 requires every Display call to come from the UI thread
    // (debug-asserted -- see display.hpp), and both methods call
    // display_->set_power() directly, not via post(). Production code
    // only ever reaches them already on the UI task (PowerManager always
    // posts first when ui_ != nullptr -- see power_manager.cpp); this
    // reproduces that same calling convention for tests that drive them
    // directly instead of through a simulated button press.
    void run_on_ui(std::function<void()> fn) {
        auto done = std::make_shared<wisp::os::Semaphore>(1, 0);
        while (!ui->post([fn, done] {
            fn();
            done->give();
        })) {
            wisp::os::sleep_ms(1);
        }
        done->take(wisp::os::kForever);
    }

    std::string settings_path;
    std::unique_ptr<JsonFileSettingsBackend> backend;

    EventBus bus;
    DeviceRegistry registry;
    WakeLockRegistry locks{bus};

    std::shared_ptr<FakeDisplay> display = std::make_shared<FakeDisplay>("display.main", 480, 480);
    std::shared_ptr<FakeTouch> touch = std::make_shared<FakeTouch>("touch.main");
    std::shared_ptr<FakeImu> imu = std::make_shared<FakeImu>("imu.main");
    std::shared_ptr<FakePower> power = std::make_shared<FakePower>("power.main");
    std::unique_ptr<PowerMonitor> power_monitor;
    std::shared_ptr<FakeButton> btn = std::make_shared<FakeButton>("button.power");
    std::shared_ptr<FakeButton> vol_btn = std::make_shared<FakeButton>("button.vol_up");

    std::shared_ptr<FakeStorage> int_storage =
        std::make_shared<FakeStorage>("storage.int", temp_dir().string(), /*removable=*/false);
    std::shared_ptr<FakeStorage> sd_storage =
        std::make_shared<FakeStorage>("storage.sd", temp_dir().string(), /*removable=*/true);
    std::unique_ptr<StorageManager> storage;

    std::unique_ptr<UiRuntime> ui;
    std::unique_ptr<ButtonService> buttons;
    std::unique_ptr<PowerManager> pm;

    // Written on the UI task; tests read it only after run_on_ui() or
    // drain_ui(), which order those writes before the read.
    std::vector<bool> light_sleep_calls;

private:
    std::mutex power_events_mutex;
    std::vector<Event> power_events;
};

// A minimal replica of self_check_screen.cpp's own
// PowerManager/PowerKeyController
// integration glue (on_button_event()/handle_power_key_action()), just
// enough to exercise the fix end to end without building the full
// self-check LVGL screen. Must stay in sync with that file's actual
// wiring: PowerKeyController::on_short(bool)/on_long(bool) is handed the
// *live* `!pm.standby()` read fresh on the UI thread, and
// PowerManager::set_power_key_owned(true) hands this wiring (not
// PowerManager's own generic "any button wakes it" subscriber)
// exclusive control over what "button.power" does.
class PowerKeyWiring {
public:
    PowerKeyWiring(EventBus& bus, UiRuntime& ui, PowerManager& pm) : bus_(bus), ui_(ui), pm_(pm) {
        pm_.set_power_key_owned(true);
        sub_ = bus_.subscribe("button.", [this](const Event& e) {
            if (e.source != "button.power") {
                return;
            }
            std::string topic = e.topic;
            ui_.post([this, topic] {
                bool screen_on = !pm_.standby();
                PowerKeyController::Action action = PowerKeyController::Action::None;
                if (topic == "button.power.short") {
                    action = pkc_.on_short(screen_on);
                } else if (topic == "button.power.long") {
                    action = pkc_.on_long(screen_on);
                }
                switch (action) {
                    case PowerKeyController::Action::ScreenOff:
                        pm_.enter_standby();
                        break;
                    case PowerKeyController::Action::ScreenOn:
                    case PowerKeyController::Action::ScreenOnAndConfirm:
                        pm_.exit_standby();
                        break;
                    default:
                        break;
                }
            });
        });
    }

    ~PowerKeyWiring() { bus_.unsubscribe(sub_); }

private:
    EventBus& bus_;
    UiRuntime& ui_;
    PowerManager& pm_;
    PowerKeyController pkc_;
    wisp::core::SubscriptionId sub_ = 0;
};

// Simulates one physical short press on `btn` (down, then up+short after
// a few ms -- comfortably under ButtonService's default 1000ms
// long_press_ms) by driving ButtonService::poll() directly with
// synthetic timestamps, the same way tests/core/button_service_test.cpp
// does.
void press_short(ButtonService& buttons, FakeButton& btn) {
    uint64_t t = wisp::os::now_ms();
    btn.down = true;
    buttons.poll(t);
    btn.down = false;
    buttons.poll(t + 5);
}

// Simulates one physical long press on `btn` (down, held past
// ButtonService's default 1000ms long_press_ms, then up -- no trailing
// `short`, since `long` already fired for this press).
void press_long(ButtonService& buttons, FakeButton& btn) {
    uint64_t t = wisp::os::now_ms();
    btn.down = true;
    buttons.poll(t);
    buttons.poll(t + 1001);
    btn.down = false;
    buttons.poll(t + 1010);
}

}  // namespace

TEST(PowerManager, EnterStandbySleepsEverythingAndPublishesState) {
    Fixture f;
    f.run_on_ui([&] { f.pm->enter_standby(); });
    ASSERT_TRUE(wait_until([&] { return f.power_event_count() != 0; }, 2000));

    EXPECT_TRUE(f.pm->standby());
    EXPECT_FALSE(f.display->powered());
    EXPECT_FALSE(f.touch->powered());
    EXPECT_FALSE(f.imu->powered());
    EXPECT_TRUE(f.sd_storage->standby());
    EXPECT_EQ(f.int_storage->set_standby_calls(), 0);
    EXPECT_TRUE(wait_until([&] { return f.ui->paused(); }, 2000));
    EXPECT_EQ(f.buttons->poll_interval_ms(), 50u);
    // Light sleep is for deep standby only (spec 4.1).
    EXPECT_TRUE(f.light_sleep_calls.empty());

    ASSERT_EQ(f.power_event_count(), 1u);
    Event e = f.power_event_at(0);
    EXPECT_EQ(e.topic, "power.state");
    auto state = e.payload.find("state");
    ASSERT_NE(state, e.payload.end());
    EXPECT_EQ(state->second.as_string(), "standby");
}

TEST(PowerManager, ExitStandbyRestoresEverything) {
    Fixture f;
    f.run_on_ui([&] { f.pm->enter_standby(); });
    ASSERT_TRUE(wait_until([&] { return f.pm->standby(); }, 2000));

    f.run_on_ui([&] { f.pm->exit_standby(); });
    ASSERT_TRUE(wait_until([&] { return !f.pm->standby(); }, 2000));

    EXPECT_TRUE(f.display->powered());
    EXPECT_TRUE(f.touch->powered());
    EXPECT_TRUE(f.imu->powered());
    EXPECT_FALSE(f.sd_storage->standby());
    EXPECT_TRUE(wait_until([&] { return !f.ui->paused(); }, 2000));
    EXPECT_EQ(f.buttons->poll_interval_ms(), 20u);
    EXPECT_TRUE(f.light_sleep_calls.empty());

    ASSERT_TRUE(wait_until([&] { return f.power_event_count() >= 2; }, 2000));
    Event e = f.power_event_at(1);
    auto state = e.payload.find("state");
    ASSERT_NE(state, e.payload.end());
    EXPECT_EQ(state->second.as_string(), "active");
}

TEST(PowerManager, EnterStandbyTwiceOnlyTakesEffectOnce) {
    Fixture f;
    f.run_on_ui([&] { f.pm->enter_standby(); });
    ASSERT_TRUE(wait_until([&] { return f.power_event_count() != 0; }, 2000));
    f.run_on_ui([&] { f.pm->enter_standby(); });
    f.bus.wait_idle(1000);

    EXPECT_EQ(f.display->set_power_calls(), 1);
    EXPECT_EQ(f.touch->set_power_calls(), 1);
    EXPECT_EQ(f.imu->set_power_calls(), 1);
    EXPECT_EQ(f.sd_storage->set_standby_calls(), 1);
    EXPECT_TRUE(f.light_sleep_calls.empty());
    EXPECT_EQ(f.power_event_count(), 1u);
}

TEST(PowerManager, ExitStandbyWhileAlreadyActiveIsANoOp) {
    Fixture f;
    f.run_on_ui([&] { f.pm->exit_standby(); });

    EXPECT_FALSE(f.pm->standby());
    EXPECT_EQ(f.display->set_power_calls(), 0);
    EXPECT_EQ(f.touch->set_power_calls(), 0);
    EXPECT_TRUE(f.light_sleep_calls.empty());
    EXPECT_EQ(f.power_event_count(), 0u);
}

TEST(PowerManager, UnsupportedTouchSleepIsNotAnErrorAndStandbyStillSucceeds) {
    Fixture f;
    f.touch->unsupported = true;

    f.run_on_ui([&] { f.pm->enter_standby(); });
    ASSERT_TRUE(wait_until([&] { return f.pm->standby(); }, 2000));

    EXPECT_TRUE(f.pm->standby());
    EXPECT_FALSE(f.display->powered());
    EXPECT_TRUE(wait_until([&] { return f.ui->paused(); }, 2000));
    EXPECT_EQ(f.buttons->poll_interval_ms(), 50u);
}

// IMU power-down stopgap: mirrors UnsupportedTouchSleepIsNotAnError
// AndStandbyStillSucceeds above -- most of this codebase's -- well, so
// far its only -- IMU driver hasn't actually hit this case (QMI8658
// does support it), but the contract (ErrorCode::Unsupported is
// expected, not an error) is identical to Touch's, for the same reason:
// not every IMU driver can power its sensors down independently.
TEST(PowerManager, UnsupportedImuSleepIsNotAnErrorAndStandbyStillSucceeds) {
    Fixture f;
    f.imu->unsupported = true;

    f.run_on_ui([&] { f.pm->enter_standby(); });
    ASSERT_TRUE(wait_until([&] { return f.pm->standby(); }, 2000));

    EXPECT_TRUE(f.pm->standby());
    EXPECT_FALSE(f.display->powered());
    EXPECT_FALSE(f.touch->powered());
    EXPECT_TRUE(f.imu->powered());  // set_power() failed, so unchanged.
    EXPECT_TRUE(wait_until([&] { return f.ui->paused(); }, 2000));
    EXPECT_EQ(f.buttons->poll_interval_ms(), 50u);
}

// `storage` is nullable (headless-style), just like
// display/touch/imu/ui/buttons already are -- a PowerManager
// built without one (e.g. a board that registers no "storage.int" at
// all) must still complete both transitions normally.
TEST(PowerManager, NullStorageManagerIsToleratedByStandbyTransitions) {
    Fixture f(/*screen_off_s=*/60, /*with_storage=*/false);

    f.run_on_ui([&] { f.pm->enter_standby(); });
    ASSERT_TRUE(wait_until([&] { return f.pm->standby(); }, 2000));
    EXPECT_FALSE(f.display->powered());
    // Nothing reached FakeStorage below PowerManager at all -- confirms
    // this isn't accidentally still forwarding through some other path.
    EXPECT_EQ(f.sd_storage->set_standby_calls(), 0);

    f.run_on_ui([&] { f.pm->exit_standby(); });
    ASSERT_TRUE(wait_until([&] { return !f.pm->standby(); }, 2000));
    EXPECT_TRUE(f.display->powered());
    EXPECT_EQ(f.sd_storage->set_standby_calls(), 0);
}

// PowerMonitor* is nullable the same way StorageManager* is (see
// NullStorageManagerIsToleratedByStandbyTransitions above) -- a PowerManager
// built without a "power.main" device must still complete both
// transitions normally.
TEST(PowerManager, NullPowerMonitorIsToleratedByStandbyTransitions) {
    Fixture f(/*screen_off_s=*/60, /*with_storage=*/true, /*with_power_monitor=*/false);

    f.run_on_ui([&] { f.pm->enter_standby(); });
    ASSERT_TRUE(wait_until([&] { return f.pm->standby(); }, 2000));
    EXPECT_FALSE(f.display->powered());

    f.run_on_ui([&] { f.pm->exit_standby(); });
    ASSERT_TRUE(wait_until([&] { return !f.pm->standby(); }, 2000));
    EXPECT_TRUE(f.display->powered());
}

// spec 6.3/6.7: enter_standby()/exit_standby() must widen/
// restore the power monitor's sample period the same way they already do
// for ButtonService's poll period -- see power_manager.cpp's
// PowerMonitor::set_standby() hook.
TEST(PowerManager, EnterAndExitStandbyWidenAndRestorePowerMonitorSampleInterval) {
    Fixture f;
    EXPECT_EQ(f.power_monitor->sample_interval_ms(), 1000u);

    f.run_on_ui([&] { f.pm->enter_standby(); });
    ASSERT_TRUE(wait_until([&] { return f.pm->standby(); }, 2000));
    EXPECT_EQ(f.power_monitor->sample_interval_ms(), 30000u);

    f.run_on_ui([&] { f.pm->exit_standby(); });
    ASSERT_TRUE(wait_until([&] { return !f.pm->standby(); }, 2000));
    EXPECT_EQ(f.power_monitor->sample_interval_ms(), 1000u);
}

TEST(PowerManager, TickEntersStandbyAfterIdleTimeoutAndActivityResetsIt) {
    Fixture f(/*screen_off_s=*/1);
    f.pm->note_activity();
    uint64_t t0 = wisp::os::now_ms();

    f.pm->tick(t0);
    f.drain_ui();  // so a buggy false-positive post would already show up.
    EXPECT_FALSE(f.pm->standby());
    f.pm->tick(t0 + 999);
    f.drain_ui();
    EXPECT_FALSE(f.pm->standby());

    f.pm->tick(t0 + 1001);
    EXPECT_TRUE(wait_until([&] { return f.pm->standby(); }, 2000));
}

// A tick() at t0 + 999, right after the second note_activity(), is
// already < 1000ms past the *first* note_activity() (~t0) too, so it
// passed even with the reset deleted entirely (idle_ms never reached
// the threshold
// either way). This version picks a tick() time that is past the first
// activity's own deadline (proving a reset is actually needed to stay
// active) but still short of the second activity's deadline (proving
// the reset moved the clock forward, not just no-opped).
TEST(PowerManager, NoteActivityResetsTheIdleClock) {
    Fixture f(/*screen_off_s=*/1);
    f.pm->note_activity();
    uint64_t t0 = wisp::os::now_ms();

    wisp::os::sleep_ms(300);
    f.pm->note_activity();  // resets the clock to ~t0+300, not t0.

    // t0+1000 is >= the *first* activity's 1000ms deadline (a reset that
    // didn't actually happen would enter standby here), but well short
    // of the *second* activity's deadline (~t0+1300).
    f.pm->tick(t0 + 1000);
    // tick() only ever posts the decision onto the UI task (see
    // tick()'s doc comment) -- without draining the queue
    // first, this assertion can't tell "the reset correctly kept this
    // idle" apart from "it didn't, but the posted enter_standby_if_
    // still_idle() job just hasn't run yet", which would pass even with
    // the second note_activity() call above deleted entirely (verified:
    // reverting this fix alone makes that regression pass undetected).
    f.drain_ui();
    EXPECT_FALSE(f.pm->standby());
}

TEST(PowerManager, ZeroScreenOffSecondsDisablesAutoStandby) {
    Fixture f(/*screen_off_s=*/0);
    f.pm->note_activity();
    uint64_t t0 = wisp::os::now_ms();

    f.pm->tick(t0 + 1000000);
    // See NoteActivityResetsTheIdleClock's comment above -- without this,
    // deleting the `screen_off_s <= 0` early-return above would still
    // pass this assertion (the posted job just wouldn't have run yet).
    f.drain_ui();
    EXPECT_FALSE(f.pm->standby());
}

// Spec 6.7/8: pressing any button (not just the power key) while in
// standby wakes the device.
TEST(PowerManager, AnyButtonEventWhileInStandbyWakesIt) {
    Fixture f;
    f.run_on_ui([&] { f.pm->enter_standby(); });
    ASSERT_TRUE(wait_until([&] { return f.pm->standby(); }, 2000));

    f.btn->down = true;
    f.buttons->poll(wisp::os::now_ms());
    f.btn->down = false;

    EXPECT_TRUE(wait_until([&] { return !f.pm->standby(); }, 2000));
}

// The bug this guards against: a single physical short press on the
// power key, while in an *automatic*
// idle-timeout standby, produces "button.power.down" then
// "button.power.short" in quick succession. Before this fix,
// PowerManager's own generic handler woke the device on `down` (as it
// does for every other button), and PowerKeyController -- tracking
// "screen on" itself, and still believing it was on from before the
// auto-standby (which it was never told about) -- reacted to the
// following `short` by deciding "turn it off", producing a visible
// wake-then-immediately-resleep flash for what the user experienced as
// one single press. With set_power_key_owned(true), PowerManager's
// generic handler leaves "button.power" alone entirely, so only this
// wiring's own on_short() (reading PowerManager::standby() live) decides
// -- correctly, ScreenOn -- and nothing then reverses it.
TEST(PowerManager, AutoScreenOffThenPowerKeyPressWakesWithoutFlashingBackOff) {
    Fixture f(/*screen_off_s=*/1);
    PowerKeyWiring wiring(f.bus, *f.ui, *f.pm);

    f.pm->note_activity();
    uint64_t t0 = wisp::os::now_ms();
    f.pm->tick(t0 + 1001);
    ASSERT_TRUE(wait_until([&] { return f.pm->standby(); }, 2000));

    press_short(*f.buttons, *f.btn);

    ASSERT_TRUE(wait_until([&] { return !f.pm->standby(); }, 2000));
    // Give any erroneous second transition a real chance to happen
    // before asserting it didn't.
    wisp::os::sleep_ms(150);
    EXPECT_FALSE(f.pm->standby());
    // Exactly two transitions total: auto-standby, then this one wake --
    // not a third (the flash this test is named for).
    EXPECT_EQ(f.power_event_count(), 2u);
    Event last = f.power_event_at(1);
    auto state = last.payload.find("state");
    ASSERT_NE(state, last.payload.end());
    EXPECT_EQ(state->second.as_string(), "active");
}

// Waking via a button PowerKeyController never sees (a volume key,
// handled entirely by PowerManager's own generic subscriber) must not
// leave the owned power-key wiring with a stale
// idea of "screen on": the very next power-key short press must still
// correctly turn it back off.
TEST(PowerManager, PowerKeyScreenOffThenVolumeKeyWakeThenNextPowerShortTurnsItOff) {
    Fixture f;
    PowerKeyWiring wiring(f.bus, *f.ui, *f.pm);

    // Screen starts on; a power-key short press turns it off.
    press_short(*f.buttons, *f.btn);
    ASSERT_TRUE(wait_until([&] { return f.pm->standby(); }, 2000));

    // A *different* button -- never routed through the owned wiring --
    // wakes it via PowerManager's own generic handler.
    f.vol_btn->down = true;
    f.buttons->poll(wisp::os::now_ms());
    f.vol_btn->down = false;
    ASSERT_TRUE(wait_until([&] { return !f.pm->standby(); }, 2000));

    // The next power-key short press must still see the screen as on
    // (PowerManager::standby()==false) and correctly turn it off --
    // not treat it as still "off" from its own last action.
    press_short(*f.buttons, *f.btn);
    EXPECT_TRUE(wait_until([&] { return f.pm->standby(); }, 2000));
}

// A confirm dialog left open when automatic standby kicks in must not
// swallow the next power-key press as a dialog
// decision -- spec 6.7's "待机中按任意键或长按电源键都会唤醒" requires
// a short press (or a long press) to wake the screen first regardless
// (the dialog itself may stay open; see power_key_controller_test.cpp's
// own tests for that half). Before this fix, PowerKeyController checked
// `confirming_` before `screen_on`, so the first post-standby press just
// cancelled/no-opped on the dialog instead of waking anything at all,
// needing a second press.
TEST(PowerManager, ConfirmDialogOpenThenAutoStandbyThenPowerKeyShortWakes) {
    Fixture f(/*screen_off_s=*/1);
    PowerKeyWiring wiring(f.bus, *f.ui, *f.pm);

    // Open the power-off confirm dialog with a long press while active.
    press_long(*f.buttons, *f.btn);
    // Wait for the bus to have dispatched (and, inside that dispatch,
    // posted to the UI task) the resulting on_long() decision, so it is
    // guaranteed to already be queued -- and so guaranteed to run --
    // ahead of the auto-standby tick() below's own posted job (the UI
    // task's post queue is FIFO).
    f.bus.wait_idle(1000);

    // Idle out into automatic standby while the dialog is still "open".
    f.pm->note_activity();
    uint64_t t0 = wisp::os::now_ms();
    f.pm->tick(t0 + 1001);
    ASSERT_TRUE(wait_until([&] { return f.pm->standby(); }, 2000));

    // A short press must wake the screen, not act on the dialog.
    press_short(*f.buttons, *f.btn);
    EXPECT_TRUE(wait_until([&] { return !f.pm->standby(); }, 2000));
}

// Same scenario, but the wake comes from a long press instead.
TEST(PowerManager, ConfirmDialogOpenThenAutoStandbyThenPowerKeyLongWakes) {
    Fixture f(/*screen_off_s=*/1);
    PowerKeyWiring wiring(f.bus, *f.ui, *f.pm);

    press_long(*f.buttons, *f.btn);
    f.bus.wait_idle(1000);

    f.pm->note_activity();
    uint64_t t0 = wisp::os::now_ms();
    f.pm->tick(t0 + 1001);
    ASSERT_TRUE(wait_until([&] { return f.pm->standby(); }, 2000));

    press_long(*f.buttons, *f.btn);
    EXPECT_TRUE(wait_until([&] { return !f.pm->standby(); }, 2000));
}

TEST(PowerManager, SetBrightnessWhileActiveAppliesImmediately) {
    Fixture f;
    // Called from a non-UI thread (as the admin HTTP handler would): the
    // write is handed to the UI task, so the owner-thread assert holds.
    f.pm->set_brightness(80);
    f.drain_ui();
    EXPECT_EQ(f.display->brightness(), 80);
    EXPECT_EQ(f.display->set_brightness_calls(), 1);

    f.run_on_ui([&] { f.pm->set_brightness(90); });
    EXPECT_EQ(f.display->brightness(), 90);
}

TEST(PowerManager, BrightnessChangedInStandbyAppliesOnWake) {
    Fixture f;
    f.run_on_ui([&] { f.pm->enter_standby(); });
    ASSERT_TRUE(f.pm->standby());
    int power_calls = f.display->set_power_calls();

    f.pm->set_brightness(50);
    f.drain_ui();
    EXPECT_EQ(f.display->set_brightness_calls(), 0);
    EXPECT_FALSE(f.display->powered());
    EXPECT_EQ(f.display->set_power_calls(), power_calls);
    EXPECT_TRUE(f.pm->standby());

    // Only the last value set during standby is applied, once.
    f.pm->set_brightness(60);
    f.drain_ui();
    f.run_on_ui([&] { f.pm->exit_standby(); });
    EXPECT_TRUE(f.display->powered());
    EXPECT_EQ(f.display->brightness(), 60);
    EXPECT_EQ(f.display->set_brightness_calls(), 1);

    // A second standby cycle with no change does not re-apply anything.
    f.run_on_ui([&] { f.pm->enter_standby(); });
    f.run_on_ui([&] { f.pm->exit_standby(); });
    EXPECT_EQ(f.display->set_brightness_calls(), 1);
}

TEST(PowerManager, HeadlessSetBrightnessHonoursStandby) {
    EventBus bus;
    DeviceRegistry registry;
    WakeLockRegistry locks(bus);
    auto backend =
        std::make_unique<JsonFileSettingsBackend>((temp_dir() / "headless.json").string());
    auto display = std::make_shared<FakeDisplay>("display.main", 480, 480);
    registry.add(display);
    PowerManager pm(PowerManagerDeps{.bus = bus,
                                     .registry = registry,
                                     .locks = locks,
                                     .settings = Settings(*backend, "power"),
                                     .display = display});
    pm.set_brightness(70);
    EXPECT_EQ(display->brightness(), 70);

    pm.enter_standby();
    pm.set_brightness(40);
    EXPECT_EQ(display->brightness(), 70);
    EXPECT_FALSE(display->powered());
    pm.exit_standby();
    EXPECT_EQ(display->brightness(), 40);

    // Headless PowerManager owns no display at all: set_brightness is a no-op.
    DeviceRegistry empty;
    PowerManager no_display(PowerManagerDeps{
        .bus = bus, .registry = empty, .locks = locks, .settings = Settings(*backend, "power")});
    no_display.set_brightness(10);
}

TEST(PowerManager, SetBrightnessCoalescesWhileUiIsBusy) {
    Fixture f;
    // Park the UI task inside a job so nothing posted afterwards can run.
    auto started = std::make_shared<wisp::os::Semaphore>(1, 0);
    auto gate = std::make_shared<wisp::os::Semaphore>(1, 0);
    ASSERT_TRUE(f.ui->post([started, gate] {
        started->give();
        gate->take(wisp::os::kForever);
    }));
    started->take(wisp::os::kForever);

    // Far more calls than the post queue holds; none may block.
    std::atomic<bool> done{false};
    std::thread setter([&] {
        for (int level = 1; level <= 100; ++level) {
            f.pm->set_brightness(static_cast<uint8_t>(level));
        }
        done = true;
    });
    bool finished = wait_until([&] { return done.load(); }, 2000);

    // Everything still free in the queue: at most one slot may be taken by
    // the brightness calls above.
    int free_slots = 0;
    while (f.ui->post([] {})) {
        ++free_slots;
    }
    gate->give();
    setter.join();
    ASSERT_TRUE(finished) << "set_brightness blocked while the UI task was busy";
    EXPECT_GE(free_slots, 31);

    f.drain_ui();
    EXPECT_EQ(f.display->brightness(), 100);
    EXPECT_EQ(f.display->set_brightness_calls(), 1);
}

TEST(PowerManager, UiThreadBrightnessIsNotOverwrittenByAnOlderQueuedJob) {
    Fixture f;
    // Park the UI task; once released it sets 50 directly on the UI thread.
    auto started = std::make_shared<wisp::os::Semaphore>(1, 0);
    auto gate = std::make_shared<wisp::os::Semaphore>(1, 0);
    PowerManager* pm = f.pm.get();
    ASSERT_TRUE(f.ui->post([started, gate, pm] {
        started->give();
        gate->take(wisp::os::kForever);
        pm->set_brightness(50);
    }));
    started->take(wisp::os::kForever);

    // Queued behind the parked job, so it runs after the UI-thread call
    // above even though it was requested earlier.
    f.pm->set_brightness(100);
    gate->give();
    f.drain_ui();

    EXPECT_EQ(f.display->brightness(), 50);
}

namespace {

// Every set_power() call the probes below receive, as "<id>:<level>", in
// call order across all of them.
struct CallLog {
    void add(std::string entry) {
        std::lock_guard<std::mutex> lock(mutex);
        calls.push_back(std::move(entry));
    }
    std::vector<std::string> take() {
        std::lock_guard<std::mutex> lock(mutex);
        return std::exchange(calls, {});
    }
    std::mutex mutex;
    std::vector<std::string> calls;
};

// A device that declares caps "standby" = `standby` (none when null) and
// records every set_power() call. `busy` makes the next that many calls
// fail with ErrorCode::Busy, as the ESP audio devices do while a stream is
// open; `unsupported` makes every call fail with ErrorCode::Unsupported.
class Probe : public wisp::hal::Device {
public:
    Probe(std::string id, DeviceClass cls, const char* standby, std::shared_ptr<CallLog> log)
        : log_(std::move(log)) {
        info_.id = std::move(id);
        info_.cls = cls;
        info_.model = "PROBE";
        if (standby != nullptr) info_.caps["standby"] = wisp::Value(standby);
    }

    const wisp::hal::DeviceInfo& info() const override { return info_; }

    wisp::Result<void> set_power(PowerLevel level) override {
        log_->add(info_.id + ":" + wisp::hal::to_string(level));
        if (unsupported.load()) {
            return wisp::Result<void>::err(wisp::ErrorCode::Unsupported, "probe: no standby");
        }
        if (busy.load() > 0) {
            busy.fetch_sub(1);
            return wisp::Result<void>::err(wisp::ErrorCode::Busy, "probe: in use");
        }
        powered_.store(wisp::hal::powered_at(wisp::hal::standby_policy(info_), level));
        return wisp::Result<void>::ok();
    }

    bool powered() const { return powered_.load(); }

    std::atomic<int> busy{0};
    std::atomic<bool> unsupported{false};

private:
    wisp::hal::DeviceInfo info_;
    std::shared_ptr<CallLog> log_;
    std::atomic<bool> powered_{true};
};

// Collects every Warn-or-worse line logged with PowerManager's tag.
class WarnSink : public wisp::LogSink {
public:
    WarnSink() {
        wisp::log::init();
        wisp::log::add_sink(this);
    }
    ~WarnSink() override { wisp::log::remove_sink(this); }

    void write(const wisp::LogRecord& record) override {
        if (record.level < wisp::LogLevel::Warn) return;
        if (std::string(record.tag) != "power_manager") return;
        std::lock_guard<std::mutex> lock(mutex_);
        lines_.push_back(record.msg);
    }
    std::vector<std::string> lines() {
        std::lock_guard<std::mutex> lock(mutex_);
        return lines_;
    }

private:
    std::mutex mutex_;
    std::vector<std::string> lines_;
};

// A PowerManager with no UiRuntime, so every transition runs on the
// calling thread and each assertion can follow the call directly.
struct Headless {
    explicit Headless(int64_t screen_off_s = 60, int64_t deep_after_s = 300,
                      bool with_hook = true) {
        backend = std::make_unique<JsonFileSettingsBackend>((temp_dir() / "power.json").string());
        Settings power(*backend, "power");
        power.set("screen_off_s", screen_off_s);
        power.set("deep_after_s", deep_after_s);

        bus.start();
        bus.subscribe("power.state", [this](const Event& e) {
            std::lock_guard<std::mutex> lock(mutex);
            states_.push_back(e.payload.at("state").as_string());
        });

        PowerManagerDeps deps{.bus = bus,
                              .registry = registry,
                              .locks = locks,
                              .settings = Settings(*backend, "power")};
        if (with_hook) {
            deps.set_light_sleep = [this](bool on) {
                std::lock_guard<std::mutex> lock(mutex);
                light_sleep_.push_back(on);
            };
        }
        pm = std::make_unique<PowerManager>(std::move(deps));
    }

    ~Headless() {
        pm.reset();
        bus.stop();
    }

    std::shared_ptr<Probe> add_probe(std::string id, DeviceClass cls, const char* standby) {
        auto probe = std::make_shared<Probe>(std::move(id), cls, standby, log);
        registry.add(probe);
        return probe;
    }

    // Reads the clock, then restarts the idle clock: a tick at t0 + x has
    // seen at most x ms of idleness.
    uint64_t start_idle() {
        uint64_t t0 = wisp::os::now_ms();
        pm->note_activity();
        return t0;
    }

    // Goes Active -> Standby -> DeepStandby by idle ticks from a fresh
    // idle clock; returns when deep standby was reached.
    uint64_t go_deep() {
        uint64_t t0 = start_idle();
        pm->tick(t0 + 60050);
        pm->tick(t0 + 60050 + 300000);
        return t0 + 60050 + 300000;
    }

    std::vector<std::string> states() {
        bus.wait_idle(2000);
        std::lock_guard<std::mutex> lock(mutex);
        return states_;
    }
    std::vector<bool> light_sleep() {
        std::lock_guard<std::mutex> lock(mutex);
        return light_sleep_;
    }

    std::unique_ptr<JsonFileSettingsBackend> backend;
    EventBus bus;
    DeviceRegistry registry;
    WakeLockRegistry locks{bus};
    std::shared_ptr<CallLog> log = std::make_shared<CallLog>();
    std::unique_ptr<PowerManager> pm;

private:
    std::mutex mutex;
    std::vector<std::string> states_;
    std::vector<bool> light_sleep_;
};

using Strings = std::vector<std::string>;

}  // namespace

// spec 4.1: dim comes 10 s before the screen goes off, at 30% of the set
// brightness; any activity brings the screen back to full brightness.
TEST(PowerManager, DimTenSecondsBeforeScreenOff) {
    Fixture f(/*screen_off_s=*/60);
    f.pm->set_brightness(200);
    f.drain_ui();
    uint64_t t0 = wisp::os::now_ms();
    f.pm->note_activity();

    f.pm->tick(t0 + 49000);
    f.drain_ui();
    EXPECT_EQ(f.pm->level(), PowerLevel::Active);
    EXPECT_EQ(f.display->brightness(), 200);

    f.pm->tick(t0 + 50050);
    f.drain_ui();
    EXPECT_EQ(f.pm->level(), PowerLevel::Dim);
    EXPECT_FALSE(f.pm->standby());
    EXPECT_EQ(f.display->brightness(), 60);
    EXPECT_TRUE(f.display->powered());  // dim only lowers the brightness
    EXPECT_FALSE(f.ui->paused());

    f.pm->note_activity();
    f.drain_ui();
    EXPECT_EQ(f.pm->level(), PowerLevel::Active);
    EXPECT_EQ(f.display->brightness(), 200);

    ASSERT_TRUE(wait_until([&] { return f.power_event_count() >= 2; }, 2000));
    EXPECT_EQ(f.power_event_at(0).payload.at("state").as_string(), "dim");
    EXPECT_EQ(f.power_event_at(1).payload.at("state").as_string(), "active");
}

TEST(PowerManager, DimWithShortestTimeout) {
    Headless h(/*screen_off_s=*/15);
    uint64_t t0 = h.start_idle();
    h.pm->tick(t0 + 4999);
    EXPECT_EQ(h.pm->level(), PowerLevel::Active);
    h.pm->tick(t0 + 5050);
    EXPECT_EQ(h.pm->level(), PowerLevel::Dim);
    h.pm->tick(t0 + 15050);
    EXPECT_EQ(h.pm->level(), PowerLevel::Standby);
}

// spec 4.2: every device with caps "standby" gets set_power(); the policy
// decides what it does. A device without the cap is never called, and
// Unsupported is expected, not a warning.
TEST(PowerManager, StandbyWalksDevicesByPolicy) {
    WarnSink warns;
    Headless h;
    auto off = h.add_probe("imu.off", DeviceClass::Imu, "off");
    auto wake = h.add_probe("button.wake", DeviceClass::Button, "wake_source");
    auto none = h.add_probe("clock.none", DeviceClass::Clock, nullptr);
    auto unsup = h.add_probe("touch.unsup", DeviceClass::Touch, "off");
    unsup->unsupported = true;

    h.pm->enter_standby();
    EXPECT_EQ(h.pm->level(), PowerLevel::Standby);
    EXPECT_EQ(h.log->take(),
              (Strings{"button.wake:standby", "imu.off:standby", "touch.unsup:standby"}));
    EXPECT_FALSE(off->powered());
    EXPECT_TRUE(wake->powered());
    EXPECT_TRUE(none->powered());

    h.pm->exit_standby();
    EXPECT_EQ(h.pm->level(), PowerLevel::Active);
    EXPECT_EQ(h.log->take(),
              (Strings{"touch.unsup:active", "imu.off:active", "button.wake:active"}));
    EXPECT_TRUE(off->powered());
    EXPECT_TRUE(wake->powered());
    EXPECT_TRUE(warns.lines().empty());
}

// The audio pair goes down input first and comes back output first,
// wherever the two sit in the registry: in a duplex pair the output
// clocks the input (wisp/port/esp/i2s_power.hpp).
TEST(PowerManager, AudioGoesDownInputFirstAndUpOutputFirst) {
    Headless h;
    h.add_probe("audio_out.main", DeviceClass::AudioOut, "off");
    h.add_probe("touch.main", DeviceClass::Touch, "off");
    h.add_probe("audio_in.main", DeviceClass::AudioIn, "off");
    h.add_probe("imu.main", DeviceClass::Imu, "off");

    h.pm->enter_standby();
    EXPECT_EQ(h.log->take(), (Strings{"imu.main:standby", "touch.main:standby",
                                      "audio_in.main:standby", "audio_out.main:standby"}));
    h.pm->exit_standby();
    EXPECT_EQ(h.log->take(), (Strings{"audio_out.main:active", "audio_in.main:active",
                                      "touch.main:active", "imu.main:active"}));
}

// A device still in use answers Busy and stays on; PowerManager logs it
// and asks again on the next tick until it agrees.
TEST(PowerManager, BusyDeviceIsRetriedOnTheNextTick) {
    WarnSink warns;
    Headless h;
    auto in = h.add_probe("audio_in.main", DeviceClass::AudioIn, "off");
    auto out = h.add_probe("audio_out.main", DeviceClass::AudioOut, "off");
    in->busy = 2;
    out->busy = 1;

    h.pm->enter_standby();
    EXPECT_EQ(h.log->take(), (Strings{"audio_in.main:standby", "audio_out.main:standby"}));
    EXPECT_TRUE(in->powered());
    EXPECT_TRUE(out->powered());

    uint64_t now = wisp::os::now_ms();
    h.pm->tick(now);
    // Still the same order: the input is still busy, so the output is
    // asked again (and would still refuse on hardware while the input
    // runs); here it agrees.
    EXPECT_EQ(h.log->take(), (Strings{"audio_in.main:standby", "audio_out.main:standby"}));
    EXPECT_TRUE(in->powered());
    EXPECT_FALSE(out->powered());

    h.pm->tick(now + 1000);
    EXPECT_EQ(h.log->take(), (Strings{"audio_in.main:standby"}));
    EXPECT_FALSE(in->powered());

    h.pm->tick(now + 2000);
    EXPECT_TRUE(h.log->take().empty());  // nothing left to retry

    // Busy on the way up is retried the same way.
    out->busy = 1;
    h.pm->exit_standby();
    EXPECT_EQ(h.log->take(), (Strings{"audio_out.main:active", "audio_in.main:active"}));
    EXPECT_FALSE(out->powered());
    h.pm->tick(wisp::os::now_ms());
    EXPECT_EQ(h.log->take(), (Strings{"audio_out.main:active"}));
    EXPECT_TRUE(out->powered());
    EXPECT_TRUE(warns.lines().empty());
}

TEST(PowerManager, DeepStandbyAfterDeepAfterS) {
    Headless h(/*screen_off_s=*/60, /*deep_after_s=*/300);
    auto off = h.add_probe("imu.main", DeviceClass::Imu, "off");
    uint64_t t0 = h.start_idle();

    h.pm->tick(t0 + 60050);
    EXPECT_EQ(h.pm->level(), PowerLevel::Standby);
    h.pm->tick(t0 + 60050 + 299000);
    EXPECT_EQ(h.pm->level(), PowerLevel::Standby);
    h.pm->tick(t0 + 60050 + 300000);
    EXPECT_EQ(h.pm->level(), PowerLevel::DeepStandby);
    EXPECT_TRUE(h.pm->standby());

    EXPECT_EQ(h.states(), (Strings{"standby", "deep_standby"}));
    EXPECT_EQ(h.log->take(), (Strings{"imu.main:standby", "imu.main:deep_standby"}));
    EXPECT_FALSE(off->powered());
    EXPECT_EQ(h.light_sleep(), (std::vector<bool>{true}));
    EXPECT_TRUE(h.pm->light_sleep_on());
}

TEST(PowerManager, DeepAfterZeroNeverGoesDeep) {
    Headless h(/*screen_off_s=*/60, /*deep_after_s=*/0);
    uint64_t t0 = h.start_idle();
    h.pm->tick(t0 + 60050);
    h.pm->tick(t0 + 10ull * 3600 * 1000);
    EXPECT_EQ(h.pm->level(), PowerLevel::Standby);
    EXPECT_TRUE(h.light_sleep().empty());
}

// spec 4.3: a network lock keeps standby from going deep, pulls deep
// standby back to standby, and on release deep standby returns at once:
// the deep timer runs from entering standby, whatever the locks did.
TEST(PowerManager, NetworkLockBlocksAndLeavesDeepStandby) {
    Headless h;
    uint32_t first = h.locks.acquire("system.admin", WakeLockKind::Network, "admin session");
    h.go_deep();
    EXPECT_EQ(h.pm->level(), PowerLevel::Standby);

    h.locks.release(first);
    EXPECT_EQ(h.pm->level(), PowerLevel::DeepStandby);

    uint32_t window = h.locks.acquire("system.netwindow", WakeLockKind::Network, "deferred");
    EXPECT_EQ(h.pm->level(), PowerLevel::Standby);
    EXPECT_FALSE(h.pm->light_sleep_on());
    h.locks.release(window);
    EXPECT_EQ(h.pm->level(), PowerLevel::DeepStandby);
    EXPECT_TRUE(h.pm->light_sleep_on());

    EXPECT_EQ(h.states(), (Strings{"standby", "deep_standby", "standby", "deep_standby"}));
}

TEST(PowerManager, LightSleepFollowsCpuLockAndSetting) {
    Headless h;
    EXPECT_TRUE(h.pm->light_sleep_supported());
    h.go_deep();
    EXPECT_EQ(h.light_sleep(), (std::vector<bool>{true}));

    uint32_t cpu = h.locks.acquire("system.ota", WakeLockKind::Cpu, "firmware update");
    EXPECT_EQ(h.light_sleep(), (std::vector<bool>{true, false}));
    EXPECT_EQ(h.pm->level(), PowerLevel::DeepStandby);  // cpu locks keep Wi-Fi off
    h.locks.release(cpu);
    EXPECT_EQ(h.light_sleep(), (std::vector<bool>{true, false, true}));

    h.pm->exit_standby();
    EXPECT_EQ(h.light_sleep(), (std::vector<bool>{true, false, true, false}));
    EXPECT_FALSE(h.pm->light_sleep_on());

    // With power/light_sleep off, deep standby never turns it on.
    ASSERT_TRUE(Settings(*h.backend, "power").set("light_sleep", false));
    h.go_deep();
    EXPECT_EQ(h.pm->level(), PowerLevel::DeepStandby);
    EXPECT_EQ(h.light_sleep().size(), 4u);
    EXPECT_FALSE(h.pm->light_sleep_on());

    // Turning it back on takes effect at the next tick.
    ASSERT_TRUE(Settings(*h.backend, "power").set("light_sleep", true));
    h.pm->tick(wisp::os::now_ms());
    EXPECT_EQ(h.light_sleep().back(), true);
    EXPECT_TRUE(h.pm->light_sleep_on());
}

TEST(PowerManager, NoLightSleepHookStillGoesDeep) {
    Headless h(/*screen_off_s=*/60, /*deep_after_s=*/300, /*with_hook=*/false);
    EXPECT_FALSE(h.pm->light_sleep_supported());
    h.go_deep();
    EXPECT_EQ(h.pm->level(), PowerLevel::DeepStandby);
    EXPECT_FALSE(h.pm->light_sleep_on());
}

// spec 4.3: a screen lock keeps the screen at active, but a short press of
// the power key (enter_standby()) still turns it off and leaves the lock.
TEST(PowerManager, ScreenLockKeepsActiveButPowerKeyStillSleeps) {
    Headless h;
    uint32_t id = h.locks.acquire("scene.reader", WakeLockKind::Screen, "reading");
    uint64_t t0 = h.start_idle();
    h.pm->tick(t0 + 50050);
    EXPECT_EQ(h.pm->level(), PowerLevel::Active);
    h.pm->tick(t0 + 3600000);
    EXPECT_EQ(h.pm->level(), PowerLevel::Active);

    h.pm->enter_standby();
    EXPECT_EQ(h.pm->level(), PowerLevel::Standby);
    EXPECT_TRUE(h.locks.held(WakeLockKind::Screen));
    h.pm->exit_standby();

    // A screen lock taken while dimmed brings the screen straight back.
    h.locks.release(id);
    uint64_t t1 = h.start_idle();
    h.pm->tick(t1 + 50050);
    EXPECT_EQ(h.pm->level(), PowerLevel::Dim);
    h.locks.acquire("scene.reader", WakeLockKind::Screen, "reading");
    EXPECT_EQ(h.pm->level(), PowerLevel::Active);
}

// Waking from deep standby undoes everything: light sleep off first, the
// devices back up in reverse order (audio output before input), the UI
// resumed last.
TEST(PowerManager, WakeFromDeepStandbyRestoresAll) {
    Fixture f(/*screen_off_s=*/60);
    auto log = std::make_shared<CallLog>();
    auto in = std::make_shared<Probe>("audio_in.main", DeviceClass::AudioIn, "off", log);
    auto out = std::make_shared<Probe>("audio_out.main", DeviceClass::AudioOut, "off", log);
    f.registry.add(in);
    f.registry.add(out);

    uint64_t t0 = wisp::os::now_ms();
    f.pm->note_activity();
    f.pm->tick(t0 + 60050);
    f.drain_ui();
    f.pm->tick(t0 + 60050 + 300000);
    f.drain_ui();
    ASSERT_EQ(f.pm->level(), PowerLevel::DeepStandby);
    EXPECT_FALSE(f.display->powered());
    EXPECT_FALSE(f.touch->powered());
    EXPECT_FALSE(f.imu->powered());
    EXPECT_TRUE(f.ui->paused());
    EXPECT_EQ(f.light_sleep_calls, (std::vector<bool>{true}));

    f.run_on_ui([&] { f.pm->exit_standby(); });
    EXPECT_EQ(f.pm->level(), PowerLevel::Active);
    EXPECT_TRUE(f.display->powered());
    EXPECT_TRUE(f.touch->powered());
    EXPECT_TRUE(f.imu->powered());
    EXPECT_TRUE(in->powered());
    EXPECT_TRUE(out->powered());
    EXPECT_FALSE(f.ui->paused());
    EXPECT_FALSE(f.sd_storage->standby());
    EXPECT_EQ(f.buttons->poll_interval_ms(), 20u);
    EXPECT_EQ(f.light_sleep_calls, (std::vector<bool>{true, false}));
    EXPECT_EQ(log->take(), (Strings{"audio_in.main:standby", "audio_out.main:standby",
                                    "audio_in.main:deep_standby", "audio_out.main:deep_standby",
                                    "audio_out.main:active", "audio_in.main:active"}));

    ASSERT_TRUE(wait_until([&] { return f.power_event_count() >= 3; }, 2000));
    EXPECT_EQ(f.power_event_at(0).payload.at("state").as_string(), "standby");
    EXPECT_EQ(f.power_event_at(1).payload.at("state").as_string(), "deep_standby");
    EXPECT_EQ(f.power_event_at(2).payload.at("state").as_string(), "active");
}

TEST(PowerManager, StateEventCarriesEveryLevel) {
    Headless h;
    uint64_t t0 = h.start_idle();
    h.pm->tick(t0 + 50050);
    h.pm->tick(t0 + 60050);
    h.pm->tick(t0 + 60050 + 300000);
    h.pm->exit_standby();
    EXPECT_EQ(h.states(), (Strings{"dim", "standby", "deep_standby", "active"}));
}
