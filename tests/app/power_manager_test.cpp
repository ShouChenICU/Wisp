#include "wisp/app/power_manager.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "support/fake_devices.hpp"
#include "support/temp_dir.hpp"
#include "support/wait.hpp"
#include "wisp/core/device_registry.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/core/settings.hpp"
#include "wisp/os/os.hpp"
#include "wisp/port/sim/settings_json_file.hpp"
#include "wisp/ui/ui_runtime.hpp"

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
// Platform::set_standby() call PowerManager makes.
struct Fixture {
    explicit Fixture(int64_t screen_off_s = 60) {
        settings_path = (temp_dir() / "power_settings.json").string();
        backend = std::make_unique<JsonFileSettingsBackend>(settings_path);
        Settings power_settings(*backend, "power");
        power_settings.set("screen_off_s", screen_off_s);

        bus.start();
        bus.subscribe("power/", [this](const Event& e) { power_events.push_back(e); });

        registry.add(btn);

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

    std::string settings_path;
    std::unique_ptr<JsonFileSettingsBackend> backend;

    EventBus bus;
    DeviceRegistry registry;

    std::shared_ptr<FakeDisplay> display = std::make_shared<FakeDisplay>("display.main", 480, 480);
    std::shared_ptr<FakeTouch> touch = std::make_shared<FakeTouch>("touch.main");
    std::shared_ptr<FakeButton> btn = std::make_shared<FakeButton>("button.power");

    std::unique_ptr<UiRuntime> ui;
    std::unique_ptr<ButtonService> buttons;
    std::unique_ptr<PowerManager> pm;

    std::vector<bool> set_standby_calls;
    std::vector<Event> power_events;
};

}  // namespace

TEST(PowerManager, EnterStandbySleepsEverythingAndPublishesState) {
    Fixture f;
    f.pm->enter_standby();
    ASSERT_TRUE(wait_until([&] { return !f.power_events.empty(); }, 2000));

    EXPECT_TRUE(f.pm->standby());
    EXPECT_FALSE(f.display->powered());
    EXPECT_FALSE(f.touch->powered());
    EXPECT_TRUE(wait_until([&] { return f.ui->paused(); }, 2000));
    EXPECT_EQ(f.buttons->poll_interval_ms(), 50u);
    ASSERT_EQ(f.set_standby_calls.size(), 1u);
    EXPECT_TRUE(f.set_standby_calls[0]);

    ASSERT_EQ(f.power_events.size(), 1u);
    EXPECT_EQ(f.power_events[0].topic, "power/state");
    auto state = f.power_events[0].payload.find("state");
    ASSERT_NE(state, f.power_events[0].payload.end());
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

    ASSERT_TRUE(wait_until([&] { return f.power_events.size() >= 2; }, 2000));
    auto state = f.power_events[1].payload.find("state");
    ASSERT_NE(state, f.power_events[1].payload.end());
    EXPECT_EQ(state->second.as_string(), "active");
}

TEST(PowerManager, EnterStandbyTwiceOnlyTakesEffectOnce) {
    Fixture f;
    f.pm->enter_standby();
    ASSERT_TRUE(wait_until([&] { return !f.power_events.empty(); }, 2000));
    f.pm->enter_standby();
    f.bus.wait_idle(1000);

    EXPECT_EQ(f.display->set_power_calls(), 1);
    EXPECT_EQ(f.touch->set_power_calls(), 1);
    EXPECT_EQ(f.set_standby_calls.size(), 1u);
    EXPECT_EQ(f.power_events.size(), 1u);
}

TEST(PowerManager, ExitStandbyWhileAlreadyActiveIsANoOp) {
    Fixture f;
    f.pm->exit_standby();

    EXPECT_FALSE(f.pm->standby());
    EXPECT_EQ(f.display->set_power_calls(), 0);
    EXPECT_EQ(f.touch->set_power_calls(), 0);
    EXPECT_EQ(f.set_standby_calls.size(), 0u);
    EXPECT_EQ(f.power_events.size(), 0u);
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

TEST(PowerManager, NoteActivityResetsTheIdleClock) {
    Fixture f(/*screen_off_s=*/1);
    f.pm->note_activity();
    uint64_t t0 = wisp::os::now_ms();

    f.pm->tick(t0 + 500);
    EXPECT_FALSE(f.pm->standby());

    f.pm->note_activity();  // resets the clock to "now" (~t0), not t0+500
    f.pm->tick(t0 + 999);
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
