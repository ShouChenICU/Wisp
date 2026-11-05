#include "wisp/net/time_sync.hpp"

#include <gtest/gtest.h>

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "support/fake_devices.hpp"
#include "support/fake_sntp.hpp"
#include "wisp/base/error.hpp"
#include "wisp/core/event_bus.hpp"

using wisp::ErrorCode;
using wisp::Result;
using wisp::Value;
using wisp::core::Event;
using wisp::core::EventBus;
using wisp::net::TimeSyncConfig;
using wisp::net::TimeSyncService;
using wisp::test::FakeClock;
using wisp::test::FakeSntp;

namespace {

constexpr int64_t kServerTime = 1767225600;  // 2026-01-01 00:00:00 UTC
constexpr uint64_t kSecond = 1000;
constexpr uint64_t kMinute = 60 * kSecond;
constexpr uint64_t kHour = 60 * kMinute;

class FailingClock : public FakeClock {
public:
    using FakeClock::FakeClock;
    wisp::Result<void> set_unix(int64_t) override {
        ++attempts;
        return wisp::Result<void>::err(ErrorCode::DeviceFailure, "i2c timeout");
    }
    int attempts = 0;
};

struct Fixture {
    explicit Fixture(std::shared_ptr<wisp::hal::Clock> rtc_in = nullptr)
        : rtc(rtc_in ? std::move(rtc_in) : std::make_shared<FakeClock>("clock.main")) {
        bus.start();
        bus.subscribe("system.time", [this](const Event& e) {
            std::lock_guard<std::mutex> lock(mutex);
            events.push_back(e);
        });
        svc = std::make_unique<TimeSyncService>(wisp::net::TimeSyncDeps{
            .sntp = sntp,
            .bus = bus,
            .servers = [this] { return servers; },
            .set_system_time =
                [this](int64_t t) {
                    system_times.push_back(t);
                    if (clock_fails)
                        return Result<void>::err(ErrorCode::IoError, "settimeofday: errno 22");
                    return Result<void>::ok();
                },
            .rtc = rtc,
            .now_ms = [this] { return now; },
        });
    }

    bool clock_fails = false;

    ~Fixture() {
        svc.reset();
        bus.stop();
    }

    // One "wisp.net" tick at the current time; runs the attempt it
    // prepared, as the worker would. True when there was one.
    bool step(bool online) {
        if (!svc->tick(online)) return false;
        svc->run_attempt();
        return true;
    }

    std::vector<Event> take_events() {
        bus.wait_idle(2000);
        std::lock_guard<std::mutex> lock(mutex);
        std::vector<Event> out;
        out.swap(events);
        return out;
    }

    uint64_t now = 5000;
    std::string servers = "pool.ntp.org,ntp.aliyun.com";
    FakeSntp sntp{[this] { return kServerTime + static_cast<int64_t>(now / 1000); }};
    EventBus bus;
    std::shared_ptr<wisp::hal::Clock> rtc;
    std::vector<int64_t> system_times;
    std::mutex mutex;
    std::vector<Event> events;
    std::unique_ptr<TimeSyncService> svc;
};

std::string str(const Event& e, const char* key) {
    auto it = e.payload.find(key);
    return it == e.payload.end() ? std::string("<none>") : it->second.as_string();
}

}  // namespace

TEST(TimeSync, SyncsOnceWhenTheStationGetsAnIp) {
    Fixture f;
    EXPECT_FALSE(f.step(false));
    EXPECT_TRUE(f.sntp.take_calls().empty());
    EXPECT_FALSE(f.svc->status().synced);

    ASSERT_TRUE(f.step(true));
    EXPECT_EQ(f.sntp.take_calls(), (std::vector<std::string>{"pool.ntp.org"}));
    const int64_t t = kServerTime + 5;
    EXPECT_EQ(f.system_times, (std::vector<int64_t>{t}));
    EXPECT_EQ(f.rtc->get_unix().value(), t);
    auto st = f.svc->status();
    EXPECT_TRUE(st.synced);
    EXPECT_EQ(st.last_sync, t);
    EXPECT_EQ(st.server, "pool.ntp.org");
    EXPECT_EQ(f.sntp.timeouts().at(0), TimeSyncConfig{}.query_timeout_ms);

    auto events = f.take_events();
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(str(events[0], "state"), "synced");
    EXPECT_EQ(str(events[0], "server"), "pool.ntp.org");
    EXPECT_EQ(events[0].payload.at("time"), Value(t));

    // Staying online asks nothing more until the resync is due.
    for (int i = 0; i < 100; ++i) {
        f.now += kMinute;
        EXPECT_FALSE(f.step(true));
    }
    EXPECT_TRUE(f.sntp.take_calls().empty());
}

// The time a scene shows moves on from the last sync without asking again.
TEST(TimeSync, NowFollowsTheLastSync) {
    Fixture f;
    EXPECT_FALSE(f.svc->now_unix().has_value());
    ASSERT_TRUE(f.step(true));
    EXPECT_EQ(f.svc->now_unix(), kServerTime + 5);
    f.now += 90 * kSecond;
    EXPECT_EQ(f.svc->now_unix(), kServerTime + 95);
}

TEST(TimeSync, FallsThroughToTheSecondServer) {
    Fixture f;
    f.sntp.set_down("pool.ntp.org", true);
    ASSERT_TRUE(f.step(true));
    EXPECT_EQ(f.sntp.take_calls(), (std::vector<std::string>{"pool.ntp.org", "ntp.aliyun.com"}));
    EXPECT_EQ(f.svc->status().server, "ntp.aliyun.com");
    auto events = f.take_events();
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(str(events[0], "state"), "synced");
    EXPECT_EQ(str(events[0], "server"), "ntp.aliyun.com");
}

// A setting changed between attempts is used by the next one.
TEST(TimeSync, ReadsTheServersForEachAttempt) {
    Fixture f;
    f.servers = "time.example.com";
    ASSERT_TRUE(f.step(true));
    EXPECT_EQ(f.sntp.take_calls(), (std::vector<std::string>{"time.example.com"}));
}

TEST(TimeSync, FailurePublishesAndBacksOff) {
    Fixture f;
    f.sntp.set_down("pool.ntp.org", true);
    f.sntp.set_down("ntp.aliyun.com", true);
    ASSERT_TRUE(f.step(true));
    EXPECT_EQ(f.sntp.take_calls().size(), 2u);
    EXPECT_TRUE(f.system_times.empty());
    EXPECT_FALSE(f.svc->status().synced);
    auto events = f.take_events();
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(str(events[0], "state"), "failed");
    EXPECT_EQ(str(events[0], "server"), "ntp.aliyun.com");
    EXPECT_EQ(events[0].payload.count("time"), 0u);

    // 30 s, doubling, capped at 30 min.
    std::vector<uint64_t> waits;
    uint64_t last = f.now;
    while (waits.size() < 8) {
        f.now += kSecond;
        if (f.step(true)) {
            waits.push_back(f.now - last);
            last = f.now;
        }
    }
    EXPECT_EQ(waits, (std::vector<uint64_t>{30 * kSecond, 60 * kSecond, 120 * kSecond,
                                            240 * kSecond, 480 * kSecond, 960 * kSecond,
                                            1800 * kSecond, 1800 * kSecond}));
    EXPECT_EQ(f.take_events().size(), 8u);

    // A success ends the backoff; the next attempt is the 6 h resync.
    f.sntp.all_up();
    while (!f.step(true)) f.now += kSecond;
    EXPECT_TRUE(f.svc->status().synced);
    const uint64_t synced_at = f.now;
    while (!f.step(true)) f.now += kMinute;
    EXPECT_EQ(f.now - synced_at, 6 * kHour);
}

TEST(TimeSync, ResyncsEverySixHours) {
    Fixture f;
    ASSERT_TRUE(f.step(true));
    f.sntp.take_calls();
    f.now += 6 * kHour - 1;
    EXPECT_FALSE(f.step(true));
    f.now += 1;
    ASSERT_TRUE(f.step(true));
    EXPECT_EQ(f.sntp.take_calls(), (std::vector<std::string>{"pool.ntp.org"}));
    EXPECT_EQ(f.svc->status().last_sync, kServerTime + static_cast<int64_t>(f.now / 1000));
    EXPECT_EQ(f.take_events().size(), 2u);
}

// Losing the link drops the retry; the next IP syncs at once.
TEST(TimeSync, StopsOnLinkLossAndSyncsOnTheNextIp) {
    Fixture f;
    f.sntp.set_down("pool.ntp.org", true);
    f.sntp.set_down("ntp.aliyun.com", true);
    ASSERT_TRUE(f.step(true));
    f.sntp.take_calls();
    for (int i = 0; i < 120; ++i) {
        f.now += kMinute;
        EXPECT_FALSE(f.step(false));
    }
    EXPECT_TRUE(f.sntp.take_calls().empty());

    f.sntp.all_up();
    ASSERT_TRUE(f.step(true));
    EXPECT_TRUE(f.svc->status().synced);
}

// The link going down while a query runs: the outcome still counts, but no
// retry is scheduled until the next IP.
TEST(TimeSync, LinkLossDuringAnAttemptSchedulesNothing) {
    Fixture f;
    f.sntp.set_down("pool.ntp.org", true);
    f.sntp.set_down("ntp.aliyun.com", true);
    ASSERT_TRUE(f.svc->tick(true));
    EXPECT_FALSE(f.svc->tick(false));
    f.svc->run_attempt();
    f.now += 2 * kHour;
    EXPECT_FALSE(f.step(false));
    EXPECT_EQ(f.sntp.take_calls().size(), 2u);
}

// The link went and came back while an attempt was running: the new IP
// still gets its immediate attempt, whatever the old one's outcome, and a
// failure there starts the backoff afresh at 30 s.
TEST(TimeSync, IpRegainedDuringAFailingAttemptStillSyncsAtOnce) {
    Fixture f;
    f.sntp.set_down("pool.ntp.org", true);
    f.sntp.set_down("ntp.aliyun.com", true);
    ASSERT_TRUE(f.svc->tick(true));
    EXPECT_FALSE(f.svc->tick(false));
    EXPECT_FALSE(f.svc->tick(true));
    f.svc->run_attempt();
    EXPECT_EQ(f.sntp.take_calls().size(), 2u);
    ASSERT_TRUE(f.step(true));
    EXPECT_EQ(f.sntp.take_calls().size(), 2u);
    f.now += 30 * kSecond - 1;
    EXPECT_FALSE(f.step(true));
    f.now += 1;
    EXPECT_TRUE(f.step(true));
}

TEST(TimeSync, IpRegainedDuringASucceedingAttemptStillSyncsAtOnce) {
    Fixture f;
    ASSERT_TRUE(f.svc->tick(true));
    EXPECT_FALSE(f.svc->tick(false));
    EXPECT_FALSE(f.svc->tick(true));
    f.svc->run_attempt();
    EXPECT_TRUE(f.svc->status().synced);
    ASSERT_TRUE(f.step(true));
    EXPECT_EQ(f.sntp.take_calls().size(), 2u);
}

// Lost during an attempt that then fails: the next IP still starts the
// backoff at 30 s.
TEST(TimeSync, LinkLossDuringAFailingAttemptKeepsTheBackoffAtItsStart) {
    Fixture f;
    f.sntp.set_down("pool.ntp.org", true);
    f.sntp.set_down("ntp.aliyun.com", true);
    ASSERT_TRUE(f.svc->tick(true));
    EXPECT_FALSE(f.svc->tick(false));
    f.svc->run_attempt();
    ASSERT_TRUE(f.step(true));
    f.now += 30 * kSecond;
    EXPECT_TRUE(f.step(true));
}

// cancel() (shutdown) stops an attempt before its next server and
// publishes nothing.
TEST(TimeSync, CancelStopsAnAttemptBetweenServers) {
    Fixture f;
    f.sntp.set_down("pool.ntp.org", true);
    f.sntp.on_query = [&f](const std::string&) { f.svc->cancel(); };
    ASSERT_TRUE(f.svc->tick(true));
    f.svc->run_attempt();
    EXPECT_EQ(f.sntp.take_calls(), (std::vector<std::string>{"pool.ntp.org"}));
    EXPECT_TRUE(f.take_events().empty());
    EXPECT_TRUE(f.system_times.empty());
    EXPECT_FALSE(f.svc->status().synced);
}

// The system clock refusing the time fails the attempt: nothing reports a
// time that is not there, the RTC is left alone and the backoff starts.
TEST(TimeSync, SystemClockRefusalFailsTheAttempt) {
    auto rtc = std::make_shared<FailingClock>("clock.main");
    Fixture f(rtc);
    f.clock_fails = true;
    ASSERT_TRUE(f.step(true));
    EXPECT_EQ(f.system_times.size(), 1u);
    EXPECT_EQ(rtc->attempts, 0);
    EXPECT_FALSE(f.svc->status().synced);
    const auto events = f.take_events();
    ASSERT_EQ(events.size(), 1u);
    EXPECT_EQ(str(events.at(0), "state"), "failed");
    f.clock_fails = false;
    f.now += 30 * kSecond;
    EXPECT_TRUE(f.step(true));
    EXPECT_TRUE(f.svc->status().synced);
}

// A failed RTC write is only a warning: the system clock is set and the
// sync counts.
TEST(TimeSync, WritesTheRtcAndSurvivesItsFailure) {
    auto rtc = std::make_shared<FailingClock>("clock.main");
    Fixture f(rtc);
    ASSERT_TRUE(f.step(true));
    EXPECT_EQ(rtc->attempts, 1);
    EXPECT_EQ(f.system_times.size(), 1u);
    EXPECT_TRUE(f.svc->status().synced);
    EXPECT_EQ(str(f.take_events().at(0), "state"), "synced");
}

TEST(TimeSync, WorksWithoutAnRtc) {
    Fixture f;
    f.svc.reset();
    f.svc = std::make_unique<TimeSyncService>(wisp::net::TimeSyncDeps{
        .sntp = f.sntp,
        .bus = f.bus,
        .servers = [&f] { return f.servers; },
        .set_system_time = {},
        .rtc = nullptr,
        .now_ms = [&f] { return f.now; },
    });
    ASSERT_TRUE(f.step(true));
    EXPECT_TRUE(f.svc->status().synced);
}

TEST(TimeSync, SyncNowRunsAnAttemptAtOnce) {
    Fixture f;
    int wakes = 0;
    f.svc->set_wake([&wakes] { ++wakes; });
    // No network: refused.
    EXPECT_FALSE(f.step(false));
    EXPECT_FALSE(f.svc->sync_now());
    EXPECT_EQ(wakes, 0);

    ASSERT_TRUE(f.step(true));
    f.sntp.take_calls();
    f.now += kMinute;
    EXPECT_FALSE(f.step(true));
    EXPECT_TRUE(f.svc->sync_now());
    EXPECT_EQ(wakes, 1);
    ASSERT_TRUE(f.step(true));
    EXPECT_EQ(f.sntp.take_calls(), (std::vector<std::string>{"pool.ntp.org"}));
    EXPECT_EQ(f.take_events().size(), 2u);
    // Asked once, run once.
    f.now += kSecond;
    EXPECT_FALSE(f.step(true));

    // While an attempt is under way a request joins it.
    f.now += kMinute;
    EXPECT_TRUE(f.svc->sync_now());
    ASSERT_TRUE(f.svc->tick(true));
    EXPECT_TRUE(f.svc->sync_now());
    f.svc->run_attempt();
    EXPECT_FALSE(f.step(true));
}

// A request during backoff skips the rest of the wait.
TEST(TimeSync, SyncNowCutsTheBackoffShort) {
    Fixture f;
    f.sntp.set_down("pool.ntp.org", true);
    f.sntp.set_down("ntp.aliyun.com", true);
    ASSERT_TRUE(f.step(true));
    f.sntp.all_up();
    f.now += kSecond;
    EXPECT_FALSE(f.step(true));
    EXPECT_TRUE(f.svc->sync_now());
    ASSERT_TRUE(f.step(true));
    EXPECT_TRUE(f.svc->status().synced);
}

// Each finished attempt is counted with its outcome; sync_now() returns
// the count at the moment it was accepted, so the attempt that answers it
// is the first to finish after that.
TEST(TimeSync, CountsFinishedAttempts) {
    Fixture f;
    EXPECT_EQ(f.svc->status().attempts, 0u);
    EXPECT_EQ(f.svc->status().last_result, "");
    f.sntp.set_down("pool.ntp.org", true);
    f.sntp.set_down("ntp.aliyun.com", true);
    ASSERT_TRUE(f.step(true));
    EXPECT_EQ(f.svc->status().attempts, 1u);
    EXPECT_EQ(f.svc->status().last_result, "failed");

    std::optional<uint64_t> after = f.svc->sync_now();
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(*after, 1u);
    f.sntp.all_up();
    ASSERT_TRUE(f.step(true));
    EXPECT_EQ(f.svc->status().attempts, 2u);
    EXPECT_EQ(f.svc->status().last_result, "synced");

    // Joining one under way: still answered by the next to finish.
    f.now += kMinute;
    ASSERT_TRUE(f.svc->sync_now().has_value());
    ASSERT_TRUE(f.svc->tick(true));
    after = f.svc->sync_now();
    ASSERT_TRUE(after.has_value());
    EXPECT_EQ(*after, 2u);
    f.svc->run_attempt();
    EXPECT_EQ(f.svc->status().attempts, 3u);
}

TEST(TimeSync, SplitsTheServerList) {
    EXPECT_EQ(wisp::net::split_servers("a.example,b.example"),
              (std::vector<std::string>{"a.example", "b.example"}));
    EXPECT_EQ(wisp::net::split_servers("10.0.0.1"), (std::vector<std::string>{"10.0.0.1"}));
    EXPECT_EQ(wisp::net::split_servers(",a,,b,"), (std::vector<std::string>{"a", "b"}));
    EXPECT_TRUE(wisp::net::split_servers("").empty());
}
