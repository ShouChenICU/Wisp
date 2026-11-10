// Unit tests for the pure light-sleep session bookkeeping behind the
// firmware's `sleep` console command (wisp/port/esp/sleep_record.hpp).
// Compiles on the host without any ESP-IDF include.

#include "wisp/port/esp/sleep_record.hpp"

#include <gtest/gtest.h>

#include <cstring>
#include <string>

using wisp::port::esp::SleepBattery;
using wisp::port::esp::SleepRecord;
using wisp::port::esp::SleepSession;
namespace sr = wisp::port::esp::sleep_record;

namespace {

constexpr int64_t kMs = 1000;
constexpr int64_t kSec = 1000 * kMs;
constexpr int64_t kHour = 3600 * kSec;

const char* cause_name(int bit) {
    switch (bit) {
        case 0:
            return "undefined";
        case 4:
            return "timer";
        case 7:
            return "gpio";
        default:
            return nullptr;
    }
}

SleepBattery battery(int percent, uint16_t mv, bool usb = false) {
    SleepBattery b{};
    b.valid = 1;
    b.percent = static_cast<int16_t>(percent);
    b.mv = mv;
    b.usb_present = usb ? 1 : 0;
    return b;
}

SleepRecord fresh() {
    SleepRecord r;
    std::memset(&r, 0xA5, sizeof(r));  // what RTC_NOINIT memory holds after power-on
    sr::boot(r);
    return r;
}

}  // namespace

TEST(SleepRecord, BucketsBySleepLength) {
    EXPECT_EQ(sr::bucket(1), 0);
    EXPECT_EQ(sr::bucket(9999), 0);
    EXPECT_EQ(sr::bucket(10 * kMs), 1);
    EXPECT_EQ(sr::bucket(49 * kMs), 1);
    EXPECT_EQ(sr::bucket(50 * kMs), 2);
    EXPECT_EQ(sr::bucket(199 * kMs), 2);
    EXPECT_EQ(sr::bucket(200 * kMs), 3);
    EXPECT_EQ(sr::bucket(999 * kMs), 3);
    EXPECT_EQ(sr::bucket(1 * kSec), 4);
    EXPECT_EQ(sr::bucket(5 * kSec - 1), 4);
    EXPECT_EQ(sr::bucket(5 * kSec), 5);
    EXPECT_EQ(sr::bucket(3 * kHour), 5);
}

TEST(SleepRecord, GarbageIsResetOnBoot) {
    SleepRecord r = fresh();
    EXPECT_TRUE(sr::valid(r));
    EXPECT_FALSE(r.current.flags & sr::kActive);
    EXPECT_EQ(r.last.sleeps, 0u);
    EXPECT_EQ(r.current.sleeps, 0u);
}

TEST(SleepRecord, CorruptedSessionIsDetected) {
    SleepRecord r = fresh();
    sr::begin(r, 0, battery(100, 4200), false);
    sr::note_sleep(r.current, 50 * kMs, 40 * kMs, 1u << 4);
    EXPECT_TRUE(sr::valid(r));
    r.current.sleeps += 1;  // a write that never finished its checksum
    EXPECT_FALSE(sr::valid(r));
    sr::boot(r);
    EXPECT_TRUE(sr::valid(r));
    EXPECT_EQ(r.current.sleeps, 0u);
}

TEST(SleepRecord, NoteSleepAccumulates) {
    SleepRecord r = fresh();
    sr::begin(r, 5 * kSec, battery(90, 4000), false);
    sr::note_sleep(r.current, 5 * kSec + 100 * kMs, 40 * kMs, 1u << 4);
    sr::note_sleep(r.current, 5 * kSec + 200 * kMs, 3 * kMs, (1u << 4) | (1u << 7));
    sr::note_sleep(r.current, 5 * kSec + 9 * kSec, 6 * kSec, 1u << 7);
    const SleepSession& s = r.current;
    EXPECT_EQ(s.sleeps, 3u);
    EXPECT_EQ(s.asleep_us, 40 * kMs + 3 * kMs + 6 * kSec);
    EXPECT_EQ(s.shortest_us, 3 * kMs);
    EXPECT_EQ(s.longest_us, 6 * kSec);
    EXPECT_EQ(s.buckets[0], 1u);
    EXPECT_EQ(s.buckets[1], 1u);
    EXPECT_EQ(s.buckets[5], 1u);
    EXPECT_EQ(s.causes[4], 2u);
    EXPECT_EQ(s.causes[7], 2u);
    EXPECT_EQ(s.elapsed_us, 9 * kSec);
    EXPECT_TRUE(sr::valid(r));
}

TEST(SleepRecord, ZeroLengthExitIsNotASleep) {
    // The exit callback also runs when the idle time was too short to try.
    SleepRecord r = fresh();
    sr::begin(r, 0, {}, false);
    sr::note_sleep(r.current, 10 * kMs, 0, 0);
    EXPECT_EQ(r.current.sleeps, 0u);
}

TEST(SleepRecord, InactiveSessionIgnoresSleeps) {
    SleepRecord r = fresh();
    sr::note_sleep(r.current, 10 * kMs, 5 * kMs, 1u << 4);
    EXPECT_EQ(r.current.sleeps, 0u);
}

TEST(SleepRecord, EndMovesCurrentToLast) {
    SleepRecord r = fresh();
    sr::begin(r, 1 * kSec, battery(100, 4200), true);
    sr::note_sleep(r.current, 2 * kSec, 30 * kMs, 1u << 4);
    EXPECT_TRUE(sr::end(r, 1 * kSec + 2 * kHour, battery(98, 4150)));
    EXPECT_FALSE(r.current.flags & sr::kActive);
    EXPECT_EQ(r.last.sleeps, 1u);
    EXPECT_EQ(r.last.elapsed_us, 2 * kHour);
    EXPECT_TRUE(r.last.flags & sr::kUsjAtStart);
    EXPECT_EQ(r.last.end_battery.percent, 98);
    EXPECT_TRUE(sr::valid(r));
    EXPECT_FALSE(sr::end(r, 3 * kHour, {}));  // nothing running
}

TEST(SleepRecord, BeginEndsARunningSession) {
    SleepRecord r = fresh();
    sr::begin(r, 0, battery(80, 3900), false);
    sr::note_sleep(r.current, 1 * kSec, 20 * kMs, 1u << 4);
    sr::begin(r, 10 * kSec, battery(79, 3890), false);
    EXPECT_EQ(r.last.sleeps, 1u);
    EXPECT_EQ(r.last.elapsed_us, 10 * kSec);
    EXPECT_EQ(r.current.sleeps, 0u);
    EXPECT_TRUE(r.current.flags & sr::kActive);
}

TEST(SleepRecord, BootClosesARunningSessionAsReset) {
    SleepRecord r = fresh();
    sr::begin(r, 0, battery(100, 4200), false);
    sr::note_sleep(r.current, 3 * kHour, 40 * kMs, 1u << 4);
    sr::boot(r);  // the chip reset; the record survived in RTC memory
    EXPECT_FALSE(r.current.flags & sr::kActive);
    EXPECT_TRUE(r.last.flags & sr::kEndedByReset);
    EXPECT_EQ(r.last.elapsed_us, 3 * kHour);
    EXPECT_FALSE(r.last.end_battery.valid);
    EXPECT_TRUE(sr::valid(r));
}

TEST(SleepRecord, DrainRate) {
    SleepSession s{};
    s.elapsed_us = 9 * kHour;
    s.start_battery = battery(100, 4200);
    s.end_battery = battery(33, 3700);
    auto rate = sr::drain_per_hour(s);
    ASSERT_TRUE(rate.has_value());
    EXPECT_NEAR(*rate, 67.0 / 9.0, 1e-9);
    s.end_battery.valid = 0;
    EXPECT_FALSE(sr::drain_per_hour(s).has_value());
    s.end_battery = battery(-1, 0);
    EXPECT_FALSE(sr::drain_per_hour(s).has_value());
    s.end_battery = battery(90, 4100);
    s.elapsed_us = 0;
    EXPECT_FALSE(sr::drain_per_hour(s).has_value());
}

TEST(SleepRecord, MedianBucket) {
    SleepSession s{};
    EXPECT_EQ(sr::median_bucket(s), -1);
    s.sleeps = 5;
    s.buckets[0] = 2;
    s.buckets[1] = 3;
    EXPECT_EQ(sr::median_bucket(s), 1);
    s.buckets[0] = 3;
    s.buckets[1] = 2;
    EXPECT_EQ(sr::median_bucket(s), 0);
}

TEST(SleepRecord, FormatsDurations) {
    EXPECT_EQ(sr::format_duration(0), "0s");
    EXPECT_EQ(sr::format_duration(45 * kSec + 600 * kMs), "45s");
    EXPECT_EQ(sr::format_duration(12 * 60 * kSec + 5 * kSec), "12m05s");
    EXPECT_EQ(sr::format_duration(7 * kHour + 12 * 60 * kSec + 30 * kSec), "7h12m");
    EXPECT_EQ(sr::format_duration(-5), "0s");
}

TEST(SleepRecord, SummaryLine) {
    SleepRecord r = fresh();
    sr::begin(r, 0, battery(100, 4200), false);
    for (int i = 0; i < 9; ++i) sr::note_sleep(r.current, (i + 1) * kSec, 45 * kMs, 1u << 4);
    sr::note_sleep(r.current, 10 * kSec, 2 * kSec, 1u << 7);
    sr::end(r, 9 * kHour, battery(33, 3700));
    EXPECT_EQ(sr::summary(r.last, cause_name),
              "light sleep session: 9h00m, asleep 0%, 10 sleeps, median 10-50 ms, "
              "wakes: timer 9 gpio 1, battery 100% -> 33% (7.4%/h)");
}

TEST(SleepRecord, SummaryFlagsUsbAndUnknownCauses) {
    SleepRecord r = fresh();
    sr::begin(r, 0, battery(50, 3800, /*usb=*/true), false);
    sr::note_sleep(r.current, 1 * kSec, 1 * kSec, 1u << 12);
    sr::end(r, 2 * kSec, {});
    EXPECT_EQ(sr::summary(r.last, cause_name),
              "light sleep session: 2s, asleep 50%, 1 sleeps, median 1-5 s, "
              "wakes: cause12 1, battery 50% -> ? (USB connected: not a battery figure)");
}

TEST(SleepRecord, ReportOfAnEmptySlot) {
    SleepSession s{};
    EXPECT_EQ(sr::report(s, "last session", cause_name), "last session: none\n");
}

TEST(SleepRecord, ReportOfARunningSession) {
    SleepRecord r = fresh();
    sr::begin(r, 0, battery(100, 4200), false);
    sr::note_sleep(r.current, 1 * kSec, 40 * kMs, 1u << 4);
    sr::note_sleep(r.current, 2 * kSec, 60 * kMs, 1u << 4);
    const std::string text = sr::report(r.current, "current session", cause_name);
    EXPECT_EQ(text,
              "current session (running): 2s\n"
              "  asleep 100 ms (5.0%), 2 sleeps, shortest 40.0 ms, longest 60.0 ms\n"
              "  lengths: <10ms 0, 10-50ms 1, 50-200ms 1, 200ms-1s 0, 1-5s 0, >=5s 0 "
              "(median 10-50 ms)\n"
              "  wakes: timer 2\n"
              "  battery: start 100% 4200 mV, end ?\n");
}

TEST(SleepRecord, ReportFlagsResetAndUsj) {
    SleepRecord r = fresh();
    sr::begin(r, 0, battery(100, 4200), /*usj=*/true);
    sr::note_sleep(r.current, 1 * kHour, 40 * kMs, 1u << 4);
    sr::boot(r);
    const std::string text = sr::report(r.last, "last session", cause_name);
    EXPECT_NE(text.find("last session (ended by a reset, up to the last wake): 1h00m\n"),
              std::string::npos)
        << text;
    EXPECT_NE(text.find("  note: USB debug port connected at start"), std::string::npos) << text;
}
