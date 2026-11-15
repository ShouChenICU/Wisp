#include "wisp/core/power_monitor.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "support/bus_flood.hpp"
#include "support/fake_devices.hpp"
#include "support/manual_dispatcher.hpp"
#include "wisp/base/error.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/core/settings_schema.hpp"
#include "wisp/core/state_cell.hpp"
#include "wisp/core/topics.hpp"
#include "wisp/os/os.hpp"

using wisp::ErrorCode;
using wisp::core::Event;
using wisp::core::EventBus;
using wisp::core::PowerMonitor;
using wisp::core::PowerSupply;
using wisp::core::SettingsSnapshot;
using wisp::core::StateCell;
using wisp::hal::PowerStatus;
using wisp::test::FakePower;

namespace {

// bus is started and subscribed to every "power."-prefixed topic,
// collecting events (in delivery order) into `events` -- same pattern as
// tests/app/power_manager_test.cpp's Fixture. `power` defaults to a
// FakePower("power.main"); pass nullptr to make() to exercise the
// no-device case. `settings` stands in for SystemSettings::cell().
struct Fixture {
    explicit Fixture(
        std::shared_ptr<FakePower> power_dev = std::make_shared<FakePower>("power.main"))
        : power(std::move(power_dev)) {
        bus.start();
        bus.subscribe("power.", [this](const Event& e) {
            std::lock_guard<std::mutex> lock(mutex);
            events.push_back(e);
        });

        mon = std::make_unique<PowerMonitor>(bus, power, &settings);
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

    void set_low_batt_pct(int pct) {
        SettingsSnapshot v = settings.get();
        v.low_batt_pct = pct;
        settings.set(v);
    }

    // Polls event_count() until it reaches at least `n` or `timeout_ms`
    // elapses -- unlike wait_idle() (which only waits for an *already
    // in-flight* publish to finish dispatching, so it returns
    // immediately, without waiting at all, when nothing has been
    // published yet), this is for waiting on an event a background
    // os::Timer callback hasn't fired yet, which is what the real-Timer
    // test below needs. Returns whether the count was actually reached.
    bool wait_for_event_count(size_t n, uint32_t timeout_ms) {
        uint64_t start = wisp::os::now_ms();
        while (event_count() < n) {
            if (wisp::os::now_ms() - start >= timeout_ms) {
                return event_count() >= n;
            }
            wisp::os::sleep_ms(20);
        }
        return true;
    }

    EventBus bus;
    StateCell<SettingsSnapshot> settings{bus, SettingsSnapshot{}};
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
    EXPECT_EQ(e.source, wisp::core::kPowerMonitorSource);
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
    EXPECT_EQ(e.source, wisp::core::kPowerMonitorSource);
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

TEST(PowerMonitor, BatteryLowDoesNotRearmExactlyAtHysteresisBoundary) {
    // The rearm check is battery_percent > threshold + hysteresis
    // (strictly greater), so landing exactly on threshold + 5 (20, at
    // the default threshold of 15) must not rearm -- a `>=` bug here
    // wouldn't be caught by the other rearm test's use of 18 (below the
    // boundary) and 21 (above it).
    Fixture f;
    f.power->set_status(PowerStatus{15, 0, false, false});
    f.mon->sample();
    f.wait_idle();
    ASSERT_EQ(f.event_count(), 1u);

    f.power->set_status(PowerStatus{20, 0, false, false});
    f.mon->sample();
    f.power->set_status(PowerStatus{15, 0, false, false});
    f.mon->sample();
    f.wait_idle();
    EXPECT_EQ(f.event_count(), 1u);  // still not rearmed -- no second event.
}

TEST(PowerMonitor, UnknownBatteryPercentAfterLowDoesNotRearmOrRefire) {
    // Once low_fired_ is true, a battery_percent of -1 (unknown) must be
    // skipped entirely for the rearm check (not treated as "clearly
    // above threshold" and rearmed), and a subsequent still-low real
    // reading must not fire a second event.
    Fixture f;
    f.power->set_status(PowerStatus{15, 0, false, false});
    f.mon->sample();
    f.wait_idle();
    ASSERT_EQ(f.event_count(), 1u);

    f.power->set_status(PowerStatus{-1, 0, false, false});
    f.mon->sample();
    f.power->set_status(PowerStatus{15, 0, false, false});
    f.mon->sample();
    f.wait_idle();
    EXPECT_EQ(f.event_count(), 1u);  // no rearm via -1, no second low event.
}

TEST(PowerMonitor, FailedStatusDoesNotUpdateBaselineOrLowFiredState) {
    // A failed status() read must leave have_sample_/last_charging_/
    // last_usb_present_/low_fired_ exactly as they were -- it is neither
    // "no change" against the next good sample, nor does it silently
    // re-arm/disarm the low-battery check.
    Fixture f;
    f.power->set_status(PowerStatus{50, 0, /*charging=*/false, /*usb_present=*/false});
    f.mon->sample();  // baseline.
    f.wait_idle();
    EXPECT_EQ(f.event_count(), 0u);

    f.power->set_next_error(ErrorCode::DeviceFailure, "injected");
    f.mon->sample();  // must no-op: baseline left at charging=false/usb_present=false.
    f.wait_idle();
    EXPECT_EQ(f.event_count(), 0u);

    // A real sample with charging now true must still be seen as a
    // change relative to the *original* baseline (false), not skipped
    // because the failed sample in between was wrongly treated as an
    // update.
    f.power->set_status(PowerStatus{50, 0, /*charging=*/true, /*usb_present=*/false});
    f.mon->sample();
    f.wait_idle();
    ASSERT_EQ(f.event_count(), 1u);
    EXPECT_EQ(f.event_at(0).topic, "power.charging.changed");
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

// The threshold is the settings cell's low_batt_pct, read on every sample:
// a change applies to the next crossing without restarting the monitor.
TEST(PowerMonitor, ThresholdFollowsTheSettingsCell) {
    Fixture f;
    f.set_low_batt_pct(30);
    f.power->set_status(PowerStatus{25, 0, false, false});
    f.mon->sample();  // 25 <= 30 (the default 15 would not fire)
    f.wait_idle();
    ASSERT_EQ(f.event_count(), 1u);
    EXPECT_EQ(f.event_at(0).topic, "power.battery.low");
    EXPECT_EQ(f.event_at(0).payload.find("battery_percent")->second.as_int(), 25);

    f.set_low_batt_pct(20);
    f.power->set_status(PowerStatus{40, 0, false, false});
    f.mon->sample();  // 40 > 20 + 5: re-armed
    f.power->set_status(PowerStatus{25, 0, false, false});
    f.mon->sample();  // 25 > 20: no event (30 would fire)
    f.wait_idle();
    EXPECT_EQ(f.event_count(), 1u);

    f.power->set_status(PowerStatus{20, 0, false, false});
    f.mon->sample();
    f.wait_idle();
    ASSERT_EQ(f.event_count(), 2u);
    EXPECT_EQ(f.event_at(1).payload.find("battery_percent")->second.as_int(), 20);
}

// Without a settings cell the threshold is the schema's default.
TEST(PowerMonitor, NoCellUsesTheSchemaDefault) {
    const int def = wisp::core::setting_default_int(wisp::core::SettingId::LowBattPct);
    {
        Fixture f;
        f.mon = std::make_unique<PowerMonitor>(f.bus, f.power, nullptr);
        f.power->set_status(PowerStatus{def, 0, false, false});
        f.mon->sample();
        f.wait_idle();
        ASSERT_EQ(f.event_count(), 1u);
        EXPECT_EQ(f.event_at(0).topic, "power.battery.low");
    }
    {
        Fixture f;
        f.mon = std::make_unique<PowerMonitor>(f.bus, f.power, nullptr);
        f.power->set_status(PowerStatus{def + 1, 0, false, false});
        f.mon->sample();
        f.wait_idle();
        EXPECT_EQ(f.event_count(), 0u);
    }
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

TEST(PowerMonitor, StartedTimerActuallySamplesAndSetStandbyActuallyRetunesIt) {
    // Every other test above calls sample() directly, so none of them
    // would notice if start() failed to build a real os::Timer, or if
    // set_standby() only flipped a flag without actually retuning the
    // Timer's period (SampleIntervalSwitchesBetweenActiveAndStandby
    // above only reads sample_interval_ms(), which is exactly that
    // flag). This test starts a real Timer and only ever drives it by
    // mutating the FakePower device and waiting for the resulting event,
    // proving both that start() really polls on its own and that
    // set_standby() really changes how often it does.
    Fixture f;
    f.power->set_status(PowerStatus{50, 0, /*charging=*/false, /*usb_present=*/false});
    // Establishes the baseline deterministically via a direct call
    // (sample()'s own doc comment: safe to call without ever calling
    // start()) -- otherwise the baseline would race against the real
    // Timer's own first tick below, and a change made too close to it
    // could be folded into the baseline itself instead of detected as a
    // change.
    f.mon->sample();
    ASSERT_EQ(f.event_count(), 0u);
    f.mon->start();

    // Active period is ~1s -- a change should be observed well within
    // 2.5s if the Timer is really running sample() on its own.
    f.power->set_status(PowerStatus{50, 0, /*charging=*/true, /*usb_present=*/false});
    ASSERT_TRUE(f.wait_for_event_count(1, 2500));
    EXPECT_EQ(f.event_at(0).topic, "power.charging.changed");

    // Standby period is ~30s -- if set_standby(true) actually retuned
    // the Timer (rather than just flipping a flag some other code path
    // reads), a change made right after it must NOT show up within 1.5s.
    f.mon->set_standby(true);
    f.power->set_status(PowerStatus{50, 0, /*charging=*/false, /*usb_present=*/false});
    EXPECT_FALSE(f.wait_for_event_count(2, 1500));

    // Back to active -- the still-pending change above should now be
    // picked up well within 2.5s.
    f.mon->set_standby(false);
    ASSERT_TRUE(f.wait_for_event_count(2, 2500));
    EXPECT_EQ(f.event_at(1).topic, "power.charging.changed");
}

// The admin page's GET /device serves the last sample instead of reading
// the power chip itself (the service layer owns the device, AGENTS.md 3).
TEST(PowerMonitor, LastStatusIsTheLatestSample) {
    Fixture f;
    EXPECT_FALSE(f.mon->last_status().has_value());  // nothing sampled yet

    f.power->set_status(PowerStatus{/*battery_percent=*/64, /*battery_mv=*/3900,
                                    /*charging=*/true, /*usb_present=*/true});
    f.mon->sample();
    auto last = f.mon->last_status();
    ASSERT_TRUE(last.has_value());
    EXPECT_EQ(last->battery_percent, 64);
    EXPECT_TRUE(last->charging);
    EXPECT_TRUE(last->usb_present);

    // A failed read means the battery state is unknown again.
    f.power->set_next_error(ErrorCode::DeviceFailure, "injected");
    f.mon->sample();
    EXPECT_FALSE(f.mon->last_status().has_value());

    Fixture none(nullptr);
    none.mon->sample();
    EXPECT_FALSE(none.mon->last_status().has_value());
}

// The supply cell beside the two events: its first value is the first
// sample, which was never announced before the cell either.
TEST(PowerMonitor, FirstSampleSeedsTheSupplyWithoutAnEvent) {
    Fixture f;
    f.power->set_status(PowerStatus{80, 4000, /*charging=*/true, /*usb_present=*/true});
    f.mon->sample();
    f.wait_idle();

    EXPECT_EQ(f.mon->supply().get(), (PowerSupply{true, true, 80, false}));
    EXPECT_EQ(f.event_count(), 0u);
}

TEST(PowerMonitor, LowBatteryAtTheFirstSampleStillPublishes) {
    Fixture f;
    f.power->set_status(PowerStatus{10, 0, false, false});
    f.mon->sample();
    f.wait_idle();

    EXPECT_EQ(f.mon->supply().get(), (PowerSupply{false, false, 10, true}));
    ASSERT_EQ(f.event_count(), 1u);
    Event e = f.event_at(0);
    EXPECT_EQ(e.topic, wisp::core::topics::kPowerBatteryLow);
    EXPECT_EQ(e.source, wisp::core::kPowerMonitorSource);
    EXPECT_EQ(e.payload, (wisp::Properties{{"battery_percent", wisp::Value(int64_t{10})}}));
}

// A first sample equal to the cell's initial value moves nothing, yet it is
// still the baseline the next sample is compared with.
TEST(PowerMonitor, FirstSampleEqualToTheDefaultIsStillTheBaseline) {
    Fixture f;
    f.power->set_status(PowerStatus{-1, 0, false, false});
    f.mon->sample();
    EXPECT_EQ(f.mon->supply().revision(), 1u);

    f.power->set_status(PowerStatus{-1, 0, true, true});
    f.mon->sample();
    f.wait_idle();
    ASSERT_EQ(f.event_count(), 1u);
    EXPECT_EQ(f.event_at(0).topic, wisp::core::topics::kPowerChargingChanged);
    EXPECT_EQ(f.mon->supply().get(), (PowerSupply{true, true, -1, false}));
}

// The percent alone is a change of the cell, never an event.
TEST(PowerMonitor, PercentChangeMovesTheCellButPublishesNothing) {
    wisp::test::ManualDispatcher md;
    Fixture f;
    f.power->set_status(PowerStatus{80, 0, false, false});
    f.mon->sample();
    std::vector<int> seen;
    wisp::core::CellSubscription sub = f.mon->supply().subscribe(
        [&](const PowerSupply& s) { seen.push_back(s.battery_percent); }, &md);
    md.run_all();
    uint32_t before = f.mon->supply().revision();

    f.power->set_status(PowerStatus{79, 0, false, false});
    f.mon->sample();
    md.run_all();
    f.wait_idle();

    EXPECT_NE(f.mon->supply().revision(), before);
    EXPECT_EQ(seen, (std::vector<int>{80, 79}));
    EXPECT_EQ(f.event_count(), 0u);
}

TEST(PowerMonitor, ChargingAndLowInOneSamplePublishInTodaysOrder) {
    Fixture f;
    f.power->set_status(PowerStatus{20, 0, false, false});
    f.mon->sample();
    f.power->set_status(PowerStatus{12, 0, true, true});
    f.mon->sample();
    f.wait_idle();

    ASSERT_EQ(f.event_count(), 2u);
    EXPECT_EQ(f.event_at(0).topic, wisp::core::topics::kPowerChargingChanged);
    EXPECT_EQ(f.event_at(0).payload, (wisp::Properties{
                                         {"charging", wisp::Value(true)},
                                         {"usb_present", wisp::Value(true)},
                                         {"battery_percent", wisp::Value(int64_t{12})},
                                     }));
    EXPECT_EQ(f.event_at(1).topic, wisp::core::topics::kPowerBatteryLow);
    EXPECT_EQ(f.mon->supply().get(), (PowerSupply{true, true, 12, true}));
}

TEST(PowerMonitor, FailedReadLeavesTheSupply) {
    Fixture f;
    f.power->set_status(PowerStatus{50, 0, true, false});
    f.mon->sample();
    uint32_t revision = f.mon->supply().revision();

    f.power->set_next_error(ErrorCode::DeviceFailure, "injected");
    f.mon->sample();

    EXPECT_EQ(f.mon->supply().revision(), revision);
    EXPECT_EQ(f.mon->supply().get(), (PowerSupply{true, false, 50, false}));
}

// An unknown percent neither sets nor clears `low`: the cell keeps the
// state the hysteresis left.
TEST(PowerMonitor, UnknownPercentKeepsTheLowState) {
    Fixture f;
    f.power->set_status(PowerStatus{10, 0, false, false});
    f.mon->sample();
    f.power->set_status(PowerStatus{-1, 0, false, false});
    f.mon->sample();
    EXPECT_EQ(f.mon->supply().get(), (PowerSupply{false, false, -1, true}));

    f.power->set_status(PowerStatus{30, 0, false, false});
    f.mon->sample();  // past threshold + 5: re-armed, no event
    EXPECT_FALSE(f.mon->supply().get().low);
    f.power->set_status(PowerStatus{-1, 0, false, false});
    f.mon->sample();
    EXPECT_FALSE(f.mon->supply().get().low);
    f.wait_idle();
    EXPECT_EQ(f.event_count(), 1u);  // the first sample's low only
}

// A power.battery.low the saturated bus drops has no other path; the cell
// still holds the low state for whoever follows it.
TEST(PowerMonitor, SupplyCellHoldsLowWhenTheEventIsDropped) {
    Fixture f;
    f.power->set_status(PowerStatus{40, 0, false, false});
    f.mon->sample();
    f.wait_idle();
    {
        wisp::test::BusFlood flood(f.bus);
        flood.hold();
        f.power->set_status(PowerStatus{10, 0, false, false});
        f.mon->sample();
        flood.flood(128);
        flood.release();
    }
    f.wait_idle();

    EXPECT_EQ(f.event_count(), 0u);
    EXPECT_TRUE(f.mon->supply().get().low);
}
