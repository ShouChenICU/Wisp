#include "wisp/core/power_monitor.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "support/fake_devices.hpp"
#include "support/temp_dir.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/core/settings.hpp"
#include "wisp/port/sim/settings_json_file.hpp"

using wisp::core::Event;
using wisp::core::EventBus;
using wisp::core::PowerMonitor;
using wisp::core::Settings;
using wisp::hal::PowerStatus;
using wisp::port::sim::JsonFileSettingsBackend;
using wisp::test::FakePower;
using wisp::test::temp_dir;

namespace {

// bus is started and subscribed to every "power."-prefixed topic,
// collecting events (in delivery order) into `events` -- same pattern as
// tests/app/power_manager_test.cpp's Fixture. `power` defaults to a
// FakePower("power.main"); pass nullptr to make() to exercise the
// no-device case.
struct Fixture {
    explicit Fixture(
        std::shared_ptr<FakePower> power_dev = std::make_shared<FakePower>("power.main"))
        : power(std::move(power_dev)) {
        settings_path = (temp_dir() / "power_monitor_settings.json").string();
        backend = std::make_unique<JsonFileSettingsBackend>(settings_path);

        bus.start();
        bus.subscribe("power.", [this](const Event& e) {
            std::lock_guard<std::mutex> lock(mutex);
            events.push_back(e);
        });

        mon = std::make_unique<PowerMonitor>(bus, power, Settings(*backend, "power"));
    }

    ~Fixture() {
        mon.reset();
        bus.stop();
    }

    size_t event_count() {
        std::lock_guard<std::mutex> lock(mutex);
        return events.size();
    }
    Event event_at(size_t index) {
        std::lock_guard<std::mutex> lock(mutex);
        return events.at(index);
    }

    void wait_idle() { bus.wait_idle(2000); }

    std::string settings_path;
    std::unique_ptr<JsonFileSettingsBackend> backend;
    EventBus bus;
    std::shared_ptr<FakePower> power;
    std::unique_ptr<PowerMonitor> mon;

private:
    std::mutex mutex;
    std::vector<Event> events;
};

}  // namespace

TEST(PowerMonitor, NoDeviceIsInertAndNeverCrashes) {
    Fixture f(nullptr);
    f.mon->start();  // no-op: no device.
    f.mon->set_standby(true);
    f.mon->set_standby(false);
    f.mon->sample();  // no-op: no device.
    f.mon->stop();

    f.wait_idle();
    EXPECT_EQ(f.event_count(), 0u);
}

TEST(PowerMonitor, FirstSampleNeverPublishesChargingChanged) {
    Fixture f;
    f.power->set_status(PowerStatus{/*battery_percent=*/80, /*battery_mv=*/4000,
                                    /*charging=*/true, /*usb_present=*/true});
    f.mon->sample();
    f.wait_idle();

    EXPECT_EQ(f.event_count(), 0u);
}

TEST(PowerMonitor, ChargingChangeAfterBaselinePublishesEventWithExpectedPayload) {
    Fixture f;
    f.power->set_status(PowerStatus{80, 4000, /*charging=*/false, /*usb_present=*/false});
    f.mon->sample();  // baseline, no event.

    f.power->set_status(PowerStatus{81, 4000, /*charging=*/true, /*usb_present=*/true});
    f.mon->sample();
    f.wait_idle();

    ASSERT_EQ(f.event_count(), 1u);
    Event e = f.event_at(0);
    EXPECT_EQ(e.topic, "power.charging.changed");
    EXPECT_EQ(e.source, "power.main");
    ASSERT_NE(e.payload.find("charging"), e.payload.end());
    EXPECT_TRUE(e.payload.find("charging")->second.as_bool());
    ASSERT_NE(e.payload.find("usb_present"), e.payload.end());
    EXPECT_TRUE(e.payload.find("usb_present")->second.as_bool());
    ASSERT_NE(e.payload.find("battery_percent"), e.payload.end());
    EXPECT_EQ(e.payload.find("battery_percent")->second.as_int(), 81);
}

TEST(PowerMonitor, UsbPresentAloneChangingAlsoPublishes) {
    Fixture f;
    f.power->set_status(PowerStatus{50, 0, /*charging=*/false, /*usb_present=*/false});
    f.mon->sample();

    f.power->set_status(PowerStatus{50, 0, /*charging=*/false, /*usb_present=*/true});
    f.mon->sample();
    f.wait_idle();

    ASSERT_EQ(f.event_count(), 1u);
    EXPECT_EQ(f.event_at(0).topic, "power.charging.changed");
}

TEST(PowerMonitor, NoChangeNeverPublishes) {
    Fixture f;
    f.power->set_status(PowerStatus{50, 0, true, true});
    f.mon->sample();
    f.mon->sample();
    f.mon->sample();
    f.wait_idle();

    EXPECT_EQ(f.event_count(), 0u);
}

TEST(PowerMonitor, BatteryLowFiresOnceAtDefaultThreshold) {
    Fixture f;
    f.power->set_status(PowerStatus{20, 0, false, false});
    f.mon->sample();  // above threshold (15), no low event.
    f.wait_idle();
    EXPECT_EQ(f.event_count(), 0u);

    f.power->set_status(PowerStatus{15, 0, false, false});
    f.mon->sample();
    f.wait_idle();
    ASSERT_EQ(f.event_count(), 1u);
    Event e = f.event_at(0);
    EXPECT_EQ(e.topic, "power.battery.low");
    EXPECT_EQ(e.source, "power.main");
    ASSERT_NE(e.payload.find("battery_percent"), e.payload.end());
    EXPECT_EQ(e.payload.find("battery_percent")->second.as_int(), 15);

    // Stays low -- must not fire a second time.
    f.power->set_status(PowerStatus{14, 0, false, false});
    f.mon->sample();
    f.wait_idle();
    EXPECT_EQ(f.event_count(), 1u);
}

TEST(PowerMonitor, BatteryLowRearmsOnlyAfterRisingPastHysteresis) {
    Fixture f;
    f.power->set_status(PowerStatus{15, 0, false, false});
    f.mon->sample();
    f.wait_idle();
    ASSERT_EQ(f.event_count(), 1u);

    // Rises, but not past threshold+5 (20) -- must not re-arm.
    f.power->set_status(PowerStatus{18, 0, false, false});
    f.mon->sample();
    f.power->set_status(PowerStatus{15, 0, false, false});
    f.mon->sample();
    f.wait_idle();
    EXPECT_EQ(f.event_count(), 1u);

    // Rises past threshold+5 -- re-arms.
    f.power->set_status(PowerStatus{21, 0, false, false});
    f.mon->sample();
    f.power->set_status(PowerStatus{15, 0, false, false});
    f.mon->sample();
    f.wait_idle();
    EXPECT_EQ(f.event_count(), 2u);
    EXPECT_EQ(f.event_at(1).topic, "power.battery.low");
}

TEST(PowerMonitor, UnknownBatteryPercentNeverTriggersLow) {
    Fixture f;
    f.power->set_status(PowerStatus{-1, 0, true, true});
    f.mon->sample();
    f.power->set_status(PowerStatus{-1, 0, false, false});
    f.mon->sample();
    f.wait_idle();

    // The charging change still fires; the low-battery check must not.
    ASSERT_EQ(f.event_count(), 1u);
    EXPECT_EQ(f.event_at(0).topic, "power.charging.changed");
}

TEST(PowerMonitor, SettingsOverrideLowBattThreshold) {
    Fixture f;
    Settings power_settings(*f.backend, "power");
    power_settings.set("low_batt_pct", int64_t{30});
    // Rebuild mon so it's constructed after the setting -- Settings
    // itself re-reads the backend fresh on every get_int() call, so a
    // fresh PowerMonitor isn't actually required, but this keeps the
    // test's intent explicit (a lower threshold triggers sooner).
    f.mon = std::make_unique<PowerMonitor>(f.bus, f.power, Settings(*f.backend, "power"));

    f.power->set_status(PowerStatus{25, 0, false, false});
    f.mon->sample();
    f.wait_idle();

    ASSERT_EQ(f.event_count(), 1u);
    EXPECT_EQ(f.event_at(0).topic, "power.battery.low");
}

TEST(PowerMonitor, SampleIntervalSwitchesBetweenActiveAndStandby) {
    Fixture f;
    EXPECT_EQ(f.mon->sample_interval_ms(), 1000u);

    f.mon->set_standby(true);
    EXPECT_EQ(f.mon->sample_interval_ms(), 30000u);

    f.mon->set_standby(false);
    EXPECT_EQ(f.mon->sample_interval_ms(), 1000u);
}

TEST(PowerMonitor, StartStopAreIdempotentAndToleratedWithoutCrashing) {
    Fixture f;
    f.mon->start();
    f.mon->start();  // idempotent.
    f.mon->set_standby(true);
    f.mon->set_standby(true);  // idempotent.
    f.mon->stop();
    f.mon->stop();  // idempotent, even without a prior start().
}
