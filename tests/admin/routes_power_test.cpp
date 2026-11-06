// GET /system/power and GET /system/occupancy (scene-device spec 4.5,
// 6.3): the power level, light sleep and the deferred scene requests as
// AdminDeps::power_status reports them, every wake lock with how long it
// has been held, and who uses which device. Both registries run on the
// fixture's clock, so the held times are exact.

#include <gtest/gtest.h>

#include <string>
#include <utility>

#include "admin/admin_fixture.hpp"
#include "support/fake_http.hpp"
#include "wisp/core/occupancy.hpp"
#include "wisp/core/wakelock.hpp"

using wisp::List;
using wisp::Map;
using wisp::Value;
using wisp::core::OccupancyMode;
using wisp::core::OccupancyRegistry;
using wisp::core::WakeLockKind;
using wisp::core::WakeLockRegistry;
using wisp::net::HttpResponse;
using wisp::net::Method;
using wisp::test::AdminFixture;
using wisp::test::body_json;
using wisp::test::error_code;
using wisp::test::make_req;
using wisp::test::with_token;

namespace {

constexpr const char* kPower = "/api/v1/system/power";
constexpr const char* kOccupancy = "/api/v1/system/occupancy";

struct PowerFixture {
    struct Options {
        bool with_locks = true;
        bool with_occupancy = true;
        bool with_status = false;
    };

    PowerFixture() : PowerFixture(Options{}) {}
    explicit PowerFixture(Options o) {
        f.srv.reset();
        wisp::admin::AdminDeps deps = f.make_deps();
        deps.wakelocks = o.with_locks ? &locks : nullptr;
        deps.occupancy = o.with_occupancy ? &occupancy : nullptr;
        if (o.with_status) deps.power_status = [this] { return status; };
        f.srv = std::make_unique<wisp::admin::AdminServer>(std::move(deps));
        token = f.token();
    }

    // The server holds a lease in `locks`: it goes first.
    ~PowerFixture() { f.srv.reset(); }

    HttpResponse get(const char* path) {
        return f.srv->handle(with_token(make_req(Method::Get, path), token));
    }

    AdminFixture f;
    WakeLockRegistry locks{f.bus, [this] { return f.now.load(); }};
    OccupancyRegistry occupancy{[this] { return f.now.load(); }};
    Value status;
    std::string token;
};

const Value& at(const Value& v, const char* key) {
    static const Value null;
    const Value* found = v.find(key);
    EXPECT_NE(found, nullptr) << "missing \"" << key << "\" in " << wisp::to_json(v);
    return found != nullptr ? *found : null;
}

// The lock entry of `holder`, or Null.
Value lock_of(const Value& locks, const std::string& holder) {
    for (const Value& l : locks.as_list()) {
        if (at(l, "holder") == Value(holder)) return l;
    }
    return Value();
}

}  // namespace

TEST(AdminPower, ReportsLevelLightSleepAndLocks) {
    PowerFixture fx;
    const uint32_t ntp = fx.locks.acquire("system.ntp", WakeLockKind::Network, "time sync");
    fx.f.now += 1500;
    fx.locks.acquire("scene.music", WakeLockKind::Cpu, "playing");
    fx.f.now += 500;

    HttpResponse res = fx.get(kPower);
    ASSERT_EQ(res.status, 200) << res.body;
    Value v = body_json(res);
    // Without a power_status hook there is no PowerManager, so no light
    // sleep hook either.
    EXPECT_EQ(at(v, "level"), Value("active"));
    EXPECT_EQ(at(v, "light_sleep"), Value("unsupported"));
    EXPECT_EQ(at(v, "deferred"), Value(int64_t{0}));
    const Value& locks = at(v, "locks");
    ASSERT_EQ(locks.type(), Value::Type::List);
    // The two locks, plus the lease this very request took.
    ASSERT_EQ(locks.as_list().size(), 3u) << res.body;
    const Value n = lock_of(locks, "system.ntp");
    EXPECT_EQ(at(n, "id"), Value(static_cast<int64_t>(ntp)));
    EXPECT_EQ(at(n, "kind"), Value("network"));
    EXPECT_EQ(at(n, "reason"), Value("time sync"));
    EXPECT_EQ(at(n, "held_ms"), Value(int64_t{2000}));
    const Value m = lock_of(locks, "scene.music");
    EXPECT_EQ(at(m, "kind"), Value("cpu"));
    EXPECT_EQ(at(m, "reason"), Value("playing"));
    EXPECT_EQ(at(m, "held_ms"), Value(int64_t{500}));
    const Value admin = lock_of(locks, "system.admin");
    EXPECT_EQ(at(admin, "held_ms"), Value(int64_t{0}));

    // held_ms follows the clock.
    fx.f.now += 10000;
    Value later = body_json(fx.get(kPower));
    EXPECT_EQ(at(lock_of(at(later, "locks"), "system.ntp"), "held_ms"), Value(int64_t{12000}));
    EXPECT_EQ(at(lock_of(at(later, "locks"), "scene.music"), "held_ms"), Value(int64_t{10500}));

    // Released locks are gone.
    fx.locks.release(ntp);
    Value after = body_json(fx.get(kPower));
    EXPECT_EQ(lock_of(at(after, "locks"), "system.ntp"), Value());
}

TEST(AdminPower, PowerStatusComesFromTheHook) {
    PowerFixture fx(PowerFixture::Options{.with_status = true});
    fx.status = Value(Map{{"level", Value("deep_standby")},
                          {"light_sleep", Value("on")},
                          {"deferred", Value(int64_t{3})}});
    Value v = body_json(fx.get(kPower));
    EXPECT_EQ(at(v, "level"), Value("deep_standby"));
    EXPECT_EQ(at(v, "light_sleep"), Value("on"));
    EXPECT_EQ(at(v, "deferred"), Value(int64_t{3}));
    EXPECT_EQ(at(v, "locks").type(), Value::Type::List);
}

TEST(AdminPower, WithoutWakeLocksIsUnavailable) {
    PowerFixture fx(PowerFixture::Options{.with_locks = false, .with_occupancy = false});
    HttpResponse power = fx.get(kPower);
    EXPECT_EQ(power.status, 503);
    EXPECT_EQ(error_code(power), "unavailable");
    HttpResponse occ = fx.get(kOccupancy);
    EXPECT_EQ(occ.status, 503);
    EXPECT_EQ(error_code(occ), "unavailable");
}

TEST(AdminPower, OccupancyList) {
    PowerFixture fx;
    Value empty = body_json(fx.get(kOccupancy));
    EXPECT_EQ(at(empty, "items"), Value(List{}));

    ASSERT_TRUE(fx.occupancy.acquire("imu.main", "scene.level", OccupancyMode::Shared));
    fx.f.now += 250;
    ASSERT_TRUE(fx.occupancy.acquire("imu.main", "system.selfcheck", OccupancyMode::Shared));
    ASSERT_TRUE(fx.occupancy.acquire("audio_in.main", "scene.rec", OccupancyMode::Exclusive));
    fx.f.now += 750;

    HttpResponse res = fx.get(kOccupancy);
    ASSERT_EQ(res.status, 200) << res.body;
    const Value body = body_json(res);
    const Value& items = at(body, "items");
    ASSERT_EQ(items.type(), Value::Type::List);
    ASSERT_EQ(items.as_list().size(), 3u);
    const Value& first = items.as_list()[0];
    EXPECT_EQ(at(first, "device"), Value("imu.main"));
    EXPECT_EQ(at(first, "holder"), Value("scene.level"));
    EXPECT_EQ(at(first, "mode"), Value("shared"));
    EXPECT_EQ(at(first, "held_ms"), Value(int64_t{1000}));
    const Value& third = items.as_list()[2];
    EXPECT_EQ(at(third, "device"), Value("audio_in.main"));
    EXPECT_EQ(at(third, "holder"), Value("scene.rec"));
    EXPECT_EQ(at(third, "mode"), Value("exclusive"));
    EXPECT_EQ(at(third, "held_ms"), Value(int64_t{750}));
}

TEST(AdminPower, RequiresToken) {
    PowerFixture fx;
    fx.locks.acquire("system.ntp", WakeLockKind::Network, "time sync");
    for (const char* path : {kPower, kOccupancy}) {
        HttpResponse res = fx.f.srv->handle(make_req(Method::Get, path));
        EXPECT_EQ(res.status, 401) << path;
        EXPECT_EQ(error_code(res), "unauthorized") << path;
        // Nothing about the locks leaks to an unpaired caller.
        EXPECT_EQ(res.body.find("system.ntp"), std::string::npos) << path;
    }
}
