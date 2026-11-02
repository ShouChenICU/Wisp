#include "wisp/core/power_monitor.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "support/fake_devices.hpp"
#include "support/temp_dir.hpp"
#include "wisp/base/error.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/core/settings.hpp"
#include "wisp/os/os.hpp"
#include "wisp/port/sim/settings_json_file.hpp"

using wisp::ErrorCode;
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

TEST(PowerMonitor, SettingsOverrideLowBattThreshold) {
    // Does not reconstruct `mon`: the header's constructor doc comment
    // promises that a Settings change takes effect without
    // reconstructing this object (the threshold cache's counter starts
    // at 0, so this is `mon`'s very first sample() ever, which always
    // reads a fresh value -- see sample_mutex_'s doc comment). 25 is
    // above the default threshold (15) but at/below the overridden one
    // (30), so a higher override taking effect is what this actually
    // exercises.
    Fixture f;
    Settings power_settings(*f.backend, "power");
    power_settings.set("low_batt_pct", int64_t{30});

    f.power->set_status(PowerStatus{25, 0, false, false});
    f.mon->sample();
    f.wait_idle();

    ASSERT_EQ(f.event_count(), 1u);
    EXPECT_EQ(f.event_at(0).topic, "power.battery.low");
}

TEST(PowerMonitor, ThresholdCacheIsNotRereadOnTheVeryNextSample) {
    // The threshold is cached, not re-read from Settings on every
    // sample() -- proven by changing it to a value that would flip the
    // outcome, then showing the very next sample() (guaranteed not to
    // land on a cache-refresh point, since only one sample has happened
    // since construction) still uses the old cached value, not the new
    // one.
    Fixture f;
    f.power->set_status(PowerStatus{20, 0, false, false});
    f.mon->sample();  // baseline + the one-time initial threshold read (default 15).
    f.wait_idle();
    EXPECT_EQ(f.event_count(), 0u);  // 20 > 15 -- no low event yet.

    Settings power_settings(*f.backend, "power");
    power_settings.set("low_batt_pct", int64_t{50});  // would make 20 <= 50 trigger if re-read now.
    f.mon->sample();
    f.wait_idle();
    EXPECT_EQ(f.event_count(), 0u);  // still the cached 15 -- no event.
}

TEST(PowerMonitor, ThresholdCacheIsRereadAfterRefreshWindow) {
    // The flip side of ThresholdCacheIsNotRereadOnTheVeryNextSample
    // above: the cache is re-read once every kThresholdRefreshSamples
    // (20, power_monitor.cpp) samples, not just once at construction and
    // then never again. Deleting the periodic-refresh half of that
    // logic (while keeping the "not on the very next sample" half) would
    // pass every other test in this file but fail this one.
    Fixture f;
    f.power->set_status(PowerStatus{20, 0, false, false});

    // Call #1 is the construction-time forced refresh (reads the
    // default 15); 20 > 15, so no event yet.
    f.mon->sample();
    f.wait_idle();
    ASSERT_EQ(f.event_count(), 0u);

    Settings power_settings(*f.backend, "power");
    power_settings.set("low_batt_pct", int64_t{50});  // would fire if re-read now.

    // 19 more calls (#2-#20) stay on the stale cached 15: still no
    // event, even though Settings now says 50.
    for (int i = 0; i < 19; ++i) {
        f.mon->sample();
    }
    f.wait_idle();
    EXPECT_EQ(f.event_count(), 0u);

    // Call #21 crosses the refresh window and re-reads "low_batt_pct":
    // now 50, so 20 <= 50 fires.
    f.mon->sample();
    f.wait_idle();

    ASSERT_EQ(f.event_count(), 1u);
    EXPECT_EQ(f.event_at(0).topic, "power.battery.low");
}

TEST(PowerMonitor, ThresholdCacheIsRereadAfterStart) {
    // start() forces the very next sample() to re-read "low_batt_pct",
    // same as the construction-time forced refresh -- proven the same
    // way as the refresh-window test above: a Settings change that only
    // takes effect if actually re-read, shown to take effect right after
    // start() without waiting out the refresh window. This test runs
    // fast enough (no waiting) that start()'s real ~1s os::Timer never
    // gets a chance to fire sample() on its own before the assertions
    // below run, so it can't interfere with the manual sample() calls.
    Fixture f;
    f.power->set_status(PowerStatus{20, 0, false, false});
    f.mon->sample();  // construction-time forced refresh (default 15); 20 > 15, no event.
    f.wait_idle();
    ASSERT_EQ(f.event_count(), 0u);

    Settings power_settings(*f.backend, "power");
    power_settings.set("low_batt_pct", int64_t{50});

    f.mon->sample();  // no start() yet -- still the stale cached 15: no event.
    f.wait_idle();
    ASSERT_EQ(f.event_count(), 0u);

    f.mon->start();   // forces the next sample() to re-read.
    f.mon->sample();  // picks up the new 50: 20 <= 50 fires.
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
