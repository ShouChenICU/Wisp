#include "wisp/core/device_service.hpp"

#include <gtest/gtest.h>

#include <array>
#include <atomic>
#include <cstdint>
#include <latch>
#include <memory>
#include <string>
#include <thread>

#include "support/fake_devices.hpp"
#include "wisp/core/device_registry.hpp"
#include "wisp/core/log.hpp"
#include "wisp/core/occupancy.hpp"
#include "wisp/hal/device_schema.hpp"

using wisp::ErrorCode;
using wisp::List;
using wisp::Map;
using wisp::Value;
using wisp::core::DeviceRegistry;
using wisp::core::DeviceService;
using wisp::core::OccupancyMode;
using wisp::core::OccupancyRegistry;
using wisp::hal::CmdSpec;
using wisp::hal::DeviceSchema;
using wisp::hal::ParamSpec;
using wisp::hal::ParamType;
using wisp::hal::PowerLevel;
using wisp::test::FakeClock;
using wisp::test::FakeGeneric;
using wisp::test::FakeImu;
using wisp::test::kFakeGenericSchema;

namespace {

size_t count_of(const std::string& haystack, const std::string& needle) {
    size_t n = 0;
    for (size_t pos = haystack.find(needle); pos != std::string::npos;
         pos = haystack.find(needle, pos + 1)) {
        ++n;
    }
    return n;
}

std::string message_of(const wisp::Result<Value>& r) {
    return r ? std::string("<ok>") : r.error().message;
}

class DeviceServiceTest : public ::testing::Test {
protected:
    void SetUp() override {
        wisp::log::reset_for_test();
        wisp::log::init(64 * 1024);
        haptic = std::make_shared<FakeGeneric>("haptic.main", &kFakeGenericSchema);
        ASSERT_TRUE(registry.add(haptic));
    }
    void TearDown() override { wisp::log::reset_for_test(); }

    wisp::Result<Value> pulse(const std::string& holder = "agent", int ms = 150) {
        return service.call("haptic.main", "pulse", Value(Map{{"ms", ms}}), holder);
    }

    std::atomic<uint64_t> t{0};
    DeviceRegistry registry;
    OccupancyRegistry occupancy{[this] { return t.load(); }};
    DeviceService service{registry, &occupancy, [this] { return t.load(); }};
    std::shared_ptr<FakeGeneric> haptic;
};

}  // namespace

TEST_F(DeviceServiceTest, ListsOnlyDevicesThatDescribeThemselves) {
    ASSERT_TRUE(registry.add(std::make_shared<FakeImu>("imu.main")));
    List list = service.list();
    ASSERT_EQ(list.size(), 1u);
    const Map& entry = list[0].as_map();
    EXPECT_EQ(entry.at("id"), Value("haptic.main"));
    EXPECT_EQ(entry.at("class"), Value("other"));
    EXPECT_EQ(entry.at("model"), Value("FAKE"));
    EXPECT_TRUE(entry.at("role").is_null());
    EXPECT_EQ(entry.at("schema"), wisp::hal::render_schema(kFakeGenericSchema));
}

TEST_F(DeviceServiceTest, ListShowsTheRole) {
    ASSERT_TRUE(registry.remove("haptic.main"));
    ASSERT_TRUE(registry.add(std::make_shared<FakeGeneric>("haptic.main", &kFakeGenericSchema,
                                                           wisp::Properties{}, "main")));
    List list = service.list();
    ASSERT_EQ(list.size(), 1u);
    EXPECT_EQ(list[0].as_map().at("role"), Value("main"));
}

// One device at a time, in list()'s order and shape, and the visitor can
// stop the walk.
TEST_F(DeviceServiceTest, ForEachVisitsOneDeviceAtATimeAndStops) {
    ASSERT_TRUE(registry.add(std::make_shared<FakeImu>("imu.main")));
    ASSERT_TRUE(registry.add(std::make_shared<FakeGeneric>("light.main", &kFakeGenericSchema)));
    ASSERT_TRUE(registry.add(std::make_shared<FakeGeneric>("other.x", &kFakeGenericSchema)));
    List seen;
    service.for_each([&](Value entry) {
        seen.push_back(std::move(entry));
        return true;
    });
    EXPECT_EQ(seen, service.list());
    ASSERT_EQ(seen.size(), 3u);

    int visits = 0;
    service.for_each([&](Value) { return ++visits < 2; });
    EXPECT_EQ(visits, 2);
}

TEST_F(DeviceServiceTest, CallValidatesThenInvokes) {
    auto r = pulse();
    ASSERT_TRUE(r) << message_of(r);
    auto calls = haptic->invocations();
    ASSERT_EQ(calls.size(), 1u);
    EXPECT_EQ(calls[0].first, "pulse");
    EXPECT_EQ(calls[0].second, (Map{{"ms", 150}, {"strength", 80}}));
}

TEST_F(DeviceServiceTest, GetReadsAProperty) {
    haptic->on_get = [](std::string_view) { return wisp::Result<Value>::ok(Value(42)); };
    auto r = service.get("haptic.main", "level");
    ASSERT_TRUE(r) << message_of(r);
    EXPECT_EQ(r.value(), Value(42));

    auto missing = service.get("haptic.main", "nope");
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().code, ErrorCode::NotFound);
    EXPECT_EQ(missing.error().message, "'haptic.main' has no property 'nope'");
    EXPECT_EQ(haptic->gets().size(), 1u);
}

TEST_F(DeviceServiceTest, RefusalsNeverReachTheDriver) {
    auto unknown_id = service.call("haptic.nope", "pulse", Value(Map{{"ms", 150}}), "agent");
    ASSERT_FALSE(unknown_id);
    EXPECT_EQ(unknown_id.error().code, ErrorCode::NotFound);
    EXPECT_EQ(unknown_id.error().message, "no device 'haptic.nope'");

    auto unknown_cmd = service.call("haptic.main", "buzz", Value(Map{}), "agent");
    ASSERT_FALSE(unknown_cmd);
    EXPECT_EQ(unknown_cmd.error().code, ErrorCode::NotFound);
    EXPECT_EQ(unknown_cmd.error().message, "'haptic.main' has no command 'buzz'");

    auto too_short = pulse("agent", 5);
    ASSERT_FALSE(too_short);
    EXPECT_EQ(too_short.error().code, ErrorCode::InvalidArgument);

    auto extra = service.call("haptic.main", "pulse", Value(Map{{"ms", 150}, {"x", 1}}), "agent");
    ASSERT_FALSE(extra);
    EXPECT_EQ(extra.error().code, ErrorCode::InvalidArgument);

    EXPECT_TRUE(haptic->invocations().empty());
}

TEST_F(DeviceServiceTest, OverlongIdIsNotFound) {
    const std::string id(DeviceService::kMaxIdBytes + 1, 'a');
    auto r = service.call(id, "pulse", Value(Map{{"ms", 150}}), "agent");
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, ErrorCode::NotFound);
    EXPECT_EQ(service.get(id, "level").error().code, ErrorCode::NotFound);
}

TEST_F(DeviceServiceTest, MinIntervalIsPerCommandAcrossHolders) {
    t = 0;
    ASSERT_TRUE(pulse("agent"));
    t = 50;
    auto again = pulse("scene.a");
    ASSERT_FALSE(again);
    EXPECT_EQ(again.error().code, ErrorCode::RateLimited);
    EXPECT_EQ(again.error().message,
              "'haptic.main.pulse' called again too soon; retry_after_ms=50");
    t = 99;
    auto almost = pulse("scene.a");
    ASSERT_FALSE(almost);
    EXPECT_NE(almost.error().message.find("retry_after_ms=1"), std::string::npos);
    t = 100;  // exactly the interval: allowed
    ASSERT_TRUE(pulse("scene.a"));
    ASSERT_TRUE(service.call("haptic.main", "set_mode", Value(Map{{"mode", "soft"}}), "agent"));
    EXPECT_EQ(haptic->invocations().size(), 3u);
}

TEST_F(DeviceServiceTest, ARefusedCallDoesNotStartTheInterval) {
    t = 1000;
    ASSERT_TRUE(occupancy.acquire("haptic.main", "scene.b", OccupancyMode::Exclusive));
    auto busy = pulse("agent");
    ASSERT_FALSE(busy);
    EXPECT_EQ(busy.error().code, ErrorCode::Busy);
    EXPECT_EQ(busy.error().message, "'haptic.main' is held by scene.b");
    occupancy.release("haptic.main", "scene.b");

    // Neither does one refused by validation or for standby.
    ASSERT_FALSE(pulse("agent", 5));
    ASSERT_TRUE(registry.remove("haptic.main"));
    auto off = std::make_shared<FakeGeneric>("haptic.main", &kFakeGenericSchema, wisp::Properties{},
                                             std::string{}, true);
    ASSERT_TRUE(registry.add(off));
    service.set_power_level(PowerLevel::Standby);
    ASSERT_FALSE(pulse("agent"));
    service.set_power_level(PowerLevel::Active);

    ASSERT_TRUE(pulse("agent"));
    EXPECT_EQ(off->invocations().size(), 1u);
}

TEST_F(DeviceServiceTest, AFailingInvokeStillStartsTheInterval) {
    haptic->on_invoke = [](std::string_view, const Map&) {
        return wisp::Result<Value>::err(ErrorCode::DeviceFailure, "bus");
    };
    ASSERT_FALSE(pulse());
    auto again = pulse();
    ASSERT_FALSE(again);
    EXPECT_EQ(again.error().code, ErrorCode::RateLimited);
    EXPECT_EQ(haptic->invocations().size(), 1u);
}

TEST_F(DeviceServiceTest, ExclusiveHolderMayCallOthersAreBusy) {
    ASSERT_TRUE(occupancy.acquire("haptic.main", "scene.b", OccupancyMode::Exclusive));
    ASSERT_TRUE(pulse("scene.b"));
    t = 1000;
    auto r = pulse("agent");
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, ErrorCode::Busy);
    EXPECT_EQ(haptic->invocations().size(), 1u);
}

TEST_F(DeviceServiceTest, SharedHoldersDoNotBlock) {
    ASSERT_TRUE(occupancy.acquire("haptic.main", "scene.b", OccupancyMode::Shared));
    EXPECT_TRUE(pulse("agent"));
}

TEST_F(DeviceServiceTest, OffForStandbyIsBusyAndNotWoken) {
    ASSERT_TRUE(registry.remove("haptic.main"));
    auto off = std::make_shared<FakeGeneric>("haptic.main", &kFakeGenericSchema, wisp::Properties{},
                                             std::string{}, true);
    ASSERT_TRUE(registry.add(off));

    for (PowerLevel level : {PowerLevel::Standby, PowerLevel::DeepStandby}) {
        service.set_power_level(level);
        auto call = pulse();
        ASSERT_FALSE(call);
        EXPECT_EQ(call.error().code, ErrorCode::Busy);
        EXPECT_EQ(call.error().message, "'haptic.main' is off while the screen is off");
        auto get = service.get("haptic.main", "level");
        ASSERT_FALSE(get);
        EXPECT_EQ(get.error().code, ErrorCode::Busy);
    }
    EXPECT_EQ(off->set_power_calls(), 0);
    EXPECT_TRUE(off->invocations().empty());
    EXPECT_TRUE(off->gets().empty());

    service.set_power_level(PowerLevel::Dim);
    EXPECT_TRUE(service.get("haptic.main", "level"));
    service.set_power_level(PowerLevel::Active);
    EXPECT_TRUE(pulse());
}

TEST_F(DeviceServiceTest, DevicesWithoutStandbyOffStayReachable) {
    service.set_power_level(PowerLevel::DeepStandby);
    EXPECT_TRUE(pulse());
    EXPECT_TRUE(service.get("haptic.main", "level"));
}

namespace {

constexpr ParamSpec kImuProps[] = {
    {.name = "temp_c", .type = ParamType::Number, .unit = "°C"},
};
constexpr CmdSpec kImuCmds[] = {{.name = "calibrate", .desc = "Calibrate"}};
constexpr DeviceSchema kImuPropsOnly{.props = kImuProps};
constexpr DeviceSchema kImuWithCmds{.props = kImuProps, .cmds = kImuCmds};

// add() refuses a typed device that describes commands, so this one
// describes them only after it is registered: the service's own check is
// the second line.
class DescribingImu : public FakeImu {
public:
    using FakeImu::FakeImu;
    const DeviceSchema* describe() const override {
        return with_cmds.load() ? &kImuWithCmds : &kImuPropsOnly;
    }
    wisp::Result<Value> get(std::string_view) override {
        return wisp::Result<Value>::ok(Value(31.5));
    }
    wisp::Result<Value> invoke(std::string_view, const Map&) override {
        ++invokes;
        return wisp::Result<Value>::ok(Value());
    }
    std::atomic<bool> with_cmds{false};
    std::atomic<int> invokes{0};
};

}  // namespace

TEST_F(DeviceServiceTest, CommandsOnTypedDevicesAreRefused) {
    auto imu = std::make_shared<DescribingImu>("imu.main");
    ASSERT_TRUE(registry.add(imu));
    auto temp = service.get("imu.main", "temp_c");
    ASSERT_TRUE(temp) << message_of(temp);
    EXPECT_EQ(temp.value(), Value(31.5));

    imu->with_cmds = true;
    auto r = service.call("imu.main", "calibrate", Value(Map{}), "agent");
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, ErrorCode::PermissionDenied);
    EXPECT_EQ(r.error().message, "commands are only for generic devices");
    EXPECT_EQ(imu->invokes.load(), 0);
}

TEST_F(DeviceServiceTest, TypedDeviceWithoutSchemaIsNotFound) {
    ASSERT_TRUE(registry.add(std::make_shared<FakeClock>("clock.main")));
    auto r = service.call("clock.main", "sync", Value(Map{}), "agent");
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, ErrorCode::NotFound);
    EXPECT_EQ(r.error().message, "'clock.main' describes nothing");
    auto g = service.get("clock.main", "unix");
    ASSERT_FALSE(g);
    EXPECT_EQ(g.error().code, ErrorCode::NotFound);
}

TEST_F(DeviceServiceTest, ResultLargerThan1KiBIsAFailure) {
    haptic->on_invoke = [](std::string_view, const Map&) {
        return wisp::Result<Value>::ok(Value(std::string(2000, 'x')));
    };
    auto r = pulse();
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, ErrorCode::DeviceFailure);
    EXPECT_EQ(r.error().message, "result too large (2002 bytes)");

    haptic->on_get = [](std::string_view) {
        return wisp::Result<Value>::ok(Value(std::string(2000, 'x')));
    };
    auto g = service.get("haptic.main", "level");
    ASSERT_FALSE(g);
    EXPECT_EQ(g.error().code, ErrorCode::DeviceFailure);
}

TEST_F(DeviceServiceTest, ResultOfExactly1KiBPasses) {
    haptic->on_invoke = [](std::string_view, const Map&) {
        // Two quotes around 1022 bytes: 1024 bytes of JSON.
        return wisp::Result<Value>::ok(Value(std::string(DeviceService::kMaxResultBytes - 2, 'x')));
    };
    EXPECT_TRUE(pulse());
}

TEST_F(DeviceServiceTest, SlowCallIsLoggedOnce) {
    haptic->on_invoke = [this](std::string_view, const Map&) {
        t += 80;
        return wisp::Result<Value>::ok(Value());
    };
    ASSERT_TRUE(pulse());
    t += 200;
    ASSERT_TRUE(pulse());
    const std::string ring = wisp::log::ring_snapshot();
    EXPECT_EQ(count_of(ring, "haptic.main.pulse took 80 ms"), 1u) << ring;

    // Within the budget: no warning.
    haptic->on_invoke = [this](std::string_view, const Map&) {
        t += wisp::hal::kDeviceCallBudgetMs;
        return wisp::Result<Value>::ok(Value());
    };
    ASSERT_TRUE(service.call("haptic.main", "set_mode", Value(Map{{"mode", "hard"}}), "agent"));
    EXPECT_EQ(count_of(wisp::log::ring_snapshot(), "haptic.main.set_mode took"), 0u);
}

TEST_F(DeviceServiceTest, SlowReadIsLoggedOnce) {
    haptic->on_get = [this](std::string_view) {
        t += 60;
        return wisp::Result<Value>::ok(Value(1));
    };
    ASSERT_TRUE(service.get("haptic.main", "level"));
    ASSERT_TRUE(service.get("haptic.main", "level"));
    EXPECT_EQ(count_of(wisp::log::ring_snapshot(), "haptic.main.level took 60 ms"), 1u);
}

TEST_F(DeviceServiceTest, StateChangesAreLoggedWithTheHolder) {
    ASSERT_TRUE(pulse("scene.demo"));
    std::string ring = wisp::log::ring_snapshot();
    EXPECT_EQ(count_of(ring, "device haptic.main.pulse by scene.demo"), 1u) << ring;
    // Argument values never reach the log.
    EXPECT_EQ(ring.find("150"), std::string::npos) << ring;

    ASSERT_TRUE(service.call("haptic.main", "read_raw", Value(Map{{"reg", 7}}), "agent"));
    ring = wisp::log::ring_snapshot();
    EXPECT_EQ(count_of(ring, "device haptic.main.read_raw"), 0u) << ring;
}

TEST_F(DeviceServiceTest, RemovedDeviceStaysAliveForTheRunningCall) {
    std::weak_ptr<FakeGeneric> weak = haptic;
    haptic->on_invoke = [this, weak](std::string_view, const Map&) {
        EXPECT_TRUE(registry.remove("haptic.main"));
        EXPECT_FALSE(weak.expired());
        return wisp::Result<Value>::ok(Value());
    };
    haptic.reset();
    ASSERT_TRUE(pulse());
    EXPECT_TRUE(weak.expired());
    t = 1000;
    auto r = pulse();
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, ErrorCode::NotFound);
}

TEST_F(DeviceServiceTest, DriverErrorsPassThrough) {
    haptic->on_invoke = [](std::string_view, const Map&) {
        return wisp::Result<Value>::err(ErrorCode::DeviceFailure, "i2c nack");
    };
    auto r = pulse();
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, ErrorCode::DeviceFailure);
    EXPECT_EQ(r.error().message, "i2c nack");
}

namespace {

constexpr const char* kCmdNames[16] = {"c0", "c1", "c2",  "c3",  "c4",  "c5",  "c6",  "c7",
                                       "c8", "c9", "c10", "c11", "c12", "c13", "c14", "c15"};

// Sixteen argument-less commands: c0 every `first` ms, the rest every
// `rest` ms.
template <uint32_t First, uint32_t Rest>
constexpr std::array<CmdSpec, 16> make_cmds() {
    std::array<CmdSpec, 16> cmds{};
    for (size_t i = 0; i < cmds.size(); ++i) {
        cmds[i] = CmdSpec{
            .name = kCmdNames[i], .desc = "", .args = {}, .min_interval_ms = i == 0 ? First : Rest};
    }
    return cmds;
}

constexpr auto kMixedCmds = make_cmds<5000, 10>();
constexpr auto kLongCmds = make_cmds<5000, 5000>();
constexpr DeviceSchema kMixedSchema{.cmds = kMixedCmds};
constexpr DeviceSchema kLongSchema{.cmds = kLongCmds};

wisp::Result<Value> run(DeviceService& s, const std::string& id, const char* cmd) {
    return s.call(id, cmd, Value(), "agent");
}

}  // namespace

// 32 (id, cmd) pairs fill the table; a 33rd replaces an expired entry
// before it touches one whose interval still runs.
TEST_F(DeviceServiceTest, AFullRateTableReplacesAnExpiredEntryFirst) {
    for (const char* id : {"relay.a", "relay.b", "relay.c"}) {
        ASSERT_TRUE(registry.add(std::make_shared<FakeGeneric>(id, &kMixedSchema)));
    }
    t = 0;
    ASSERT_TRUE(run(service, "relay.a", "c0"));
    for (const char* id : {"relay.a", "relay.b"}) {
        for (size_t i = 0; i < 16; ++i) {
            if (std::string(id) == "relay.a" && i == 0) continue;
            t += 1;
            ASSERT_TRUE(run(service, id, kCmdNames[i]));
        }
    }
    t = 100;
    ASSERT_TRUE(run(service, "relay.c", "c1"));
    auto r = run(service, "relay.a", "c0");
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, ErrorCode::RateLimited);
    EXPECT_NE(r.error().message.find("retry_after_ms=4900"), std::string::npos);
}

constexpr auto kSlowFirstCmds = make_cmds<60000, 5000>();
constexpr DeviceSchema kSlowFirstSchema{.cmds = kSlowFirstCmds};

// A full table of running intervals fails closed: the new pair is refused
// with the soonest any running interval ends, and no running interval is
// evicted, however long it still has to run.
TEST_F(DeviceServiceTest, AFullRateTableRefusesInsteadOfEvicting) {
    ASSERT_TRUE(registry.add(std::make_shared<FakeGeneric>("relay.a", &kSlowFirstSchema)));
    ASSERT_TRUE(registry.add(std::make_shared<FakeGeneric>("relay.b", &kLongSchema)));
    auto relay_c = std::make_shared<FakeGeneric>("relay.c", &kLongSchema);
    ASSERT_TRUE(registry.add(relay_c));
    t = 0;
    ASSERT_TRUE(pulse());                        // 100 ms
    ASSERT_TRUE(run(service, "relay.a", "c0"));  // 60 s
    for (size_t i = 1; i < 16; ++i) {
        t += 1;
        ASSERT_TRUE(run(service, "relay.a", kCmdNames[i]));  // 5 s each
    }
    for (size_t i = 0; i < 15; ++i) {
        t += 1;
        ASSERT_TRUE(run(service, "relay.b", kCmdNames[i]));
    }

    t = 40;
    auto full = run(service, "relay.c", "c0");
    ASSERT_FALSE(full);
    EXPECT_EQ(full.error().code, ErrorCode::RateLimited);
    EXPECT_EQ(full.error().message, "'relay.c.c0' cannot be rate-tracked now; retry_after_ms=60");
    EXPECT_TRUE(relay_c->invocations().empty());
    auto again = pulse();
    ASSERT_FALSE(again);
    EXPECT_NE(again.error().message.find("called again too soon; retry_after_ms=60"),
              std::string::npos);
    EXPECT_FALSE(run(service, "relay.a", "c0"));
    // A command without an interval needs no slot.
    EXPECT_TRUE(service.call("haptic.main", "set_mode", Value(Map{{"mode", "soft"}}), "agent"));

    // Once the shortest interval ends, its slot takes the new pair.
    t = 100;
    EXPECT_TRUE(run(service, "relay.c", "c0"));
    EXPECT_EQ(relay_c->invocations().size(), 1u);
    auto slow = run(service, "relay.a", "c0");
    ASSERT_FALSE(slow);
    EXPECT_NE(slow.error().message.find("retry_after_ms=59900"), std::string::npos);
}

// Equal intervals: the 33rd pair waits for the oldest stamp to run out.
TEST_F(DeviceServiceTest, AFullRateTableWithEqualIntervalsRefusesTheNewPair) {
    for (const char* id : {"relay.a", "relay.b", "relay.c"}) {
        ASSERT_TRUE(registry.add(std::make_shared<FakeGeneric>(id, &kLongSchema)));
    }
    t = 0;
    for (const char* id : {"relay.a", "relay.b"}) {
        for (const char* cmd : kCmdNames) {
            ASSERT_TRUE(run(service, id, cmd));
            t += 1;
        }
    }
    t = 40;
    auto r = run(service, "relay.c", "c0");
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, ErrorCode::RateLimited);
    EXPECT_EQ(r.error().message, "'relay.c.c0' cannot be rate-tracked now; retry_after_ms=4960");
    EXPECT_FALSE(run(service, "relay.a", "c0"));
    t = 5000;
    EXPECT_TRUE(run(service, "relay.c", "c0"));
    EXPECT_FALSE(run(service, "relay.a", "c1"));  // stamped at 1, ends at 5001
}

// Rate entries are keyed by id, not by the device object.
TEST_F(DeviceServiceTest, ADeviceReAddedUnderTheSameIdIsStillLimited) {
    ASSERT_TRUE(pulse());
    ASSERT_TRUE(registry.remove("haptic.main"));
    ASSERT_TRUE(registry.add(std::make_shared<FakeGeneric>("haptic.main", &kFakeGenericSchema)));
    t = 50;
    auto r = pulse();
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, ErrorCode::RateLimited);
}

// Two callers racing the same rate-limited command at one instant: the
// check and the stamp are one critical section, so exactly one reaches the
// driver.
TEST_F(DeviceServiceTest, RacingCallsPassOneAtATime) {
    constexpr int kRounds = 300;
    for (int round = 0; round < kRounds; ++round) {
        t = 1000 + static_cast<uint64_t>(round) * 100;  // the previous interval has ended
        std::latch go(2);
        std::array<wisp::Result<Value>, 2> results{wisp::Result<Value>::ok(Value()),
                                                   wisp::Result<Value>::ok(Value())};
        auto racer = [&](size_t i) {
            go.arrive_and_wait();
            results[i] = pulse(i == 0 ? "agent" : "scene.a");
        };
        std::thread a(racer, 0), b(racer, 1);
        a.join();
        b.join();
        const int ok = (results[0] ? 1 : 0) + (results[1] ? 1 : 0);
        ASSERT_EQ(ok, 1) << "round " << round;
        const auto& refused = results[0] ? results[1] : results[0];
        ASSERT_EQ(refused.error().code, ErrorCode::RateLimited) << "round " << round;
    }
    EXPECT_EQ(haptic->invocations().size(), static_cast<size_t>(kRounds));
}
