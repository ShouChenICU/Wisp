#include "wisp/app/power_manager.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "support/fake_devices.hpp"
#include "support/temp_dir.hpp"
#include "support/wait.hpp"
#include "wisp/app/power_key_controller.hpp"
#include "wisp/core/device_registry.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/core/settings.hpp"
#include "wisp/os/os.hpp"
#include "wisp/port/sim/settings_json_file.hpp"
#include "wisp/ui/ui_runtime.hpp"

using wisp::app::PowerKeyController;
using wisp::app::PowerManager;
using wisp::core::ButtonService;
using wisp::core::DeviceRegistry;
using wisp::core::Event;
using wisp::core::EventBus;
using wisp::core::Settings;
using wisp::port::sim::JsonFileSettingsBackend;
using wisp::test::FakeButton;
using wisp::test::FakeDisplay;
using wisp::test::FakeTouch;
using wisp::test::temp_dir;
using wisp::test::wait_until;
using wisp::ui::UiRuntime;

namespace {

// Everything a PowerManager test needs: a real (but Fake-backed) Display/
// Touch/UiRuntime/ButtonService/EventBus, plus a JSON-file Settings
// backend under a fresh temp dir -- see tests/core/settings_test.cpp for
// the same backend used the same way. `set_standby_calls` records every
// Platform::set_standby() call PowerManager makes. `btn` is
// "button.power"; `vol_btn` ("button.vol_up") exists so C1's integration
// tests can distinguish "woken via the owned power key path" from
// "woken via PowerManager's own generic any-button-wakes-it path".
struct Fixture {
    explicit Fixture(int64_t screen_off_s = 60) {
        settings_path = (temp_dir() / "power_settings.json").string();
        backend = std::make_unique<JsonFileSettingsBackend>(settings_path);
        Settings power_settings(*backend, "power");
        power_settings.set("screen_off_s", screen_off_s);

        bus.start();
        bus.subscribe("power/", [this](const Event& e) {
            std::lock_guard<std::mutex> lock(power_events_mutex);
            power_events.push_back(e);
        });

        registry.add(btn);
        registry.add(vol_btn);

        ui = std::make_unique<UiRuntime>(display, touch);
        if (!ui->start()) {
            ADD_FAILURE() << "UiRuntime::start() failed";
        }

        buttons = std::make_unique<ButtonService>(registry, bus);
        buttons->start();

        pm = std::make_unique<PowerManager>(
            bus, display, touch, ui.get(), buttons.get(),
            [this](bool standby) { set_standby_calls.push_back(standby); },
            Settings(*backend, "power"));
    }

    ~Fixture() {
        pm.reset();
        buttons->stop();
        ui->stop();
        bus.stop();
    }

    // Task 18b review round 1 (M4): power_events is written from the
    // EventBus's dispatch thread (the subscriber above) and read from
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

    std::string settings_path;
    std::unique_ptr<JsonFileSettingsBackend> backend;

    EventBus bus;
    DeviceRegistry registry;

    std::shared_ptr<FakeDisplay> display = std::make_shared<FakeDisplay>("display.main", 480, 480);
    std::shared_ptr<FakeTouch> touch = std::make_shared<FakeTouch>("touch.main");
    std::shared_ptr<FakeButton> btn = std::make_shared<FakeButton>("button.power");
    std::shared_ptr<FakeButton> vol_btn = std::make_shared<FakeButton>("button.vol_up");

    std::unique_ptr<UiRuntime> ui;
    std::unique_ptr<ButtonService> buttons;
    std::unique_ptr<PowerManager> pm;

    std::vector<bool> set_standby_calls;

private:
    std::mutex power_events_mutex;
    std::vector<Event> power_events;
};

// Task 18b review round 1 (C1): a minimal replica of
// self_check_screen.cpp's own PowerManager/PowerKeyController
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
    f.pm->enter_standby();
    ASSERT_TRUE(wait_until([&] { return f.power_event_count() != 0; }, 2000));

    EXPECT_TRUE(f.pm->standby());
    EXPECT_FALSE(f.display->powered());
    EXPECT_FALSE(f.touch->powered());
    EXPECT_TRUE(wait_until([&] { return f.ui->paused(); }, 2000));
    EXPECT_EQ(f.buttons->poll_interval_ms(), 50u);
    ASSERT_EQ(f.set_standby_calls.size(), 1u);
    EXPECT_TRUE(f.set_standby_calls[0]);

    ASSERT_EQ(f.power_event_count(), 1u);
    Event e = f.power_event_at(0);
    EXPECT_EQ(e.topic, "power/state");
    auto state = e.payload.find("state");
    ASSERT_NE(state, e.payload.end());
    EXPECT_EQ(state->second.as_string(), "standby");
}

TEST(PowerManager, ExitStandbyRestoresEverything) {
    Fixture f;
    f.pm->enter_standby();
    ASSERT_TRUE(wait_until([&] { return f.pm->standby(); }, 2000));

    f.pm->exit_standby();
    ASSERT_TRUE(wait_until([&] { return !f.pm->standby(); }, 2000));

    EXPECT_TRUE(f.display->powered());
    EXPECT_TRUE(f.touch->powered());
    EXPECT_TRUE(wait_until([&] { return !f.ui->paused(); }, 2000));
    EXPECT_EQ(f.buttons->poll_interval_ms(), 20u);
    ASSERT_EQ(f.set_standby_calls.size(), 2u);
    EXPECT_TRUE(f.set_standby_calls[0]);
    EXPECT_FALSE(f.set_standby_calls[1]);

    ASSERT_TRUE(wait_until([&] { return f.power_event_count() >= 2; }, 2000));
    Event e = f.power_event_at(1);
    auto state = e.payload.find("state");
    ASSERT_NE(state, e.payload.end());
    EXPECT_EQ(state->second.as_string(), "active");
}

TEST(PowerManager, EnterStandbyTwiceOnlyTakesEffectOnce) {
    Fixture f;
    f.pm->enter_standby();
    ASSERT_TRUE(wait_until([&] { return f.power_event_count() != 0; }, 2000));
    f.pm->enter_standby();
    f.bus.wait_idle(1000);

    EXPECT_EQ(f.display->set_power_calls(), 1);
    EXPECT_EQ(f.touch->set_power_calls(), 1);
    EXPECT_EQ(f.set_standby_calls.size(), 1u);
    EXPECT_EQ(f.power_event_count(), 1u);
}

TEST(PowerManager, ExitStandbyWhileAlreadyActiveIsANoOp) {
    Fixture f;
    f.pm->exit_standby();

    EXPECT_FALSE(f.pm->standby());
    EXPECT_EQ(f.display->set_power_calls(), 0);
    EXPECT_EQ(f.touch->set_power_calls(), 0);
    EXPECT_EQ(f.set_standby_calls.size(), 0u);
    EXPECT_EQ(f.power_event_count(), 0u);
}

TEST(PowerManager, UnsupportedTouchSleepIsNotAnErrorAndStandbyStillSucceeds) {
    Fixture f;
    f.touch->unsupported = true;

    f.pm->enter_standby();
    ASSERT_TRUE(wait_until([&] { return f.pm->standby(); }, 2000));

    EXPECT_TRUE(f.pm->standby());
    EXPECT_FALSE(f.display->powered());
    EXPECT_TRUE(wait_until([&] { return f.ui->paused(); }, 2000));
    EXPECT_EQ(f.buttons->poll_interval_ms(), 50u);
}

TEST(PowerManager, TickEntersStandbyAfterIdleTimeoutAndActivityResetsIt) {
    Fixture f(/*screen_off_s=*/1);
    f.pm->note_activity();
    uint64_t t0 = wisp::os::now_ms();

    f.pm->tick(t0);
    EXPECT_FALSE(f.pm->standby());
    f.pm->tick(t0 + 999);
    EXPECT_FALSE(f.pm->standby());

    f.pm->tick(t0 + 1001);
    EXPECT_TRUE(wait_until([&] { return f.pm->standby(); }, 2000));
}

// Task 18b review round 1 (M4): the previous version of this test called
// tick(t0 + 999) after the second note_activity() -- but that is already
// < 1000ms past the *first* note_activity() (~t0) too, so it passed even
// with the reset deleted entirely (idle_ms never reached the threshold
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
    EXPECT_FALSE(f.pm->standby());
}

TEST(PowerManager, ZeroScreenOffSecondsDisablesAutoStandby) {
    Fixture f(/*screen_off_s=*/0);
    f.pm->note_activity();
    uint64_t t0 = wisp::os::now_ms();

    f.pm->tick(t0 + 1000000);
    EXPECT_FALSE(f.pm->standby());
}

// Spec 6.7/8: pressing any button (not just the power key) while in
// standby wakes the device.
TEST(PowerManager, AnyButtonEventWhileInStandbyWakesIt) {
    Fixture f;
    f.pm->enter_standby();
    ASSERT_TRUE(wait_until([&] { return f.pm->standby(); }, 2000));

    f.btn->down = true;
    f.buttons->poll(wisp::os::now_ms());
    f.btn->down = false;

    EXPECT_TRUE(wait_until([&] { return !f.pm->standby(); }, 2000));
}

// Task 18b review round 1 (C1): the bug this guards against. A single
// physical short press on the power key, while in an *automatic*
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

// Task 18b review round 1 (C1): the other scenario the review's fix
// explicitly calls out -- waking via a button PowerKeyController never
// sees (a volume key, handled entirely by PowerManager's own generic
// subscriber) must not leave the owned power-key wiring with a stale
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

// Re-review 1 (N2): a confirm dialog left open when automatic standby
// kicks in must not swallow the next power-key press as a dialog
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
