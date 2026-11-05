// The authenticated business routes: device, devices, settings, Wi-Fi,
// clients and system (spec 6.1). The model routes are in routes_ai_test.cpp.

#include <gtest/gtest.h>

#include <memory>
#include <ostream>
#include <string>
#include <vector>

#include "admin/admin_fixture.hpp"
#include "support/fake_http.hpp"
#include "support/wait.hpp"
#include "wisp/base/json.hpp"
#include "wisp/net/http.hpp"

// Readable gtest failure output for Value comparisons.
namespace wisp {
void PrintTo(const Value& v, std::ostream* os) { *os << to_json(v); }
}  // namespace wisp

using wisp::List;
using wisp::Map;
using wisp::Value;
using wisp::net::HttpRequest;
using wisp::net::HttpResponse;
using wisp::net::Method;
using wisp::test::AdminFixture;
using wisp::test::body_json;
using wisp::test::contains;
using wisp::test::error_code;
using wisp::test::every_authenticated_get_route;
using wisp::test::make_req;
using wisp::test::with_csrf;
using wisp::test::with_token;

namespace {

constexpr Method Get = Method::Get;
constexpr Method Post = Method::Post;
constexpr Method Put = Method::Put;
constexpr Method Delete = Method::Delete;

// An authenticated request; mutating methods also carry X-Wisp.
HttpRequest authed(const std::string& token, Method m, const std::string& path,
                   std::string body = {}) {
    HttpRequest req = with_token(make_req(m, path, std::move(body)), token);
    if (m != Get) req = with_csrf(std::move(req));
    return req;
}

const Value& field(const Value& v, const char* key) {
    static const Value null;
    const Value* f = v.find(key);
    EXPECT_NE(f, nullptr) << "missing \"" << key << "\" in " << wisp::to_json(v);
    return f != nullptr ? *f : null;
}

// A typed read that fails the test instead of asserting on a wrong type.
bool is_true(const Value& v) {
    EXPECT_EQ(v.type(), Value::Type::Bool) << wisp::to_json(v);
    return v.type() == Value::Type::Bool && v.as_bool();
}

TEST(Routes, BusinessRoutesNeedAToken) {
    AdminFixture f;
    for (const std::string& path : every_authenticated_get_route()) {
        if (path == "/api/v1/info") continue;  // public
        EXPECT_EQ(f.srv->handle(make_req(Get, path)).status, 401) << path;
    }
    EXPECT_EQ(f.srv->handle(with_csrf(make_req(Post, "/api/v1/system/reboot"))).status, 401);
    EXPECT_EQ(f.reboots.load(), 0);
}

TEST(Routes, WifiStatusNeverContainsApPassword) {
    AdminFixture f;
    const std::string token = f.token();
    auto session = f.wifi->start_provisioning(f.now);
    ASSERT_TRUE(session);
    const std::string password = session.value().password;
    ASSERT_EQ(password.size(), 8u);
    f.fake_wifi->ap_client_count = 1;
    f.wifi->tick(f.now);

    HttpResponse res = f.srv->handle(authed(token, Get, "/api/v1/wifi"));
    EXPECT_EQ(res.status, 200);
    EXPECT_FALSE(contains(res.body, password)) << res.body;
    Value v = body_json(res);
    EXPECT_EQ(field(v, "ap"), Value(Map{{"active", Value(true)},
                                        {"ssid", Value(session.value().ssid)},
                                        {"clients", Value(1)}}));
    EXPECT_EQ(field(v, "state"), Value("off"));
    EXPECT_EQ(field(v, "ssid"), Value(""));
    EXPECT_EQ(field(v, "ip"), Value(""));
    EXPECT_EQ(field(v, "rssi"), Value(0));
    EXPECT_EQ(field(v, "reason"), Value(""));
    EXPECT_EQ(field(v, "saved"), Value(List{}));

    for (const std::string& path : every_authenticated_get_route()) {
        EXPECT_FALSE(contains(f.srv->handle(authed(token, Get, path)).body, password)) << path;
    }
}

TEST(Routes, WifiAddScanAndConnect) {
    AdminFixture f;
    const std::string token = f.token();
    f.fake_wifi->networks = {{"Home", -50, true}, {"Cafe", -70, false}};
    f.fake_wifi->passwords["Home"] = "correct-horse";

    HttpResponse add = f.srv->handle(authed(token, Post, "/api/v1/wifi/networks",
                                            R"({"ssid":"Home","password":"correct-horse"})"));
    EXPECT_EQ(add.status, 202);
    EXPECT_FALSE(contains(add.body, "correct-horse"));
    EXPECT_EQ(f.srv->handle(authed(token, Post, "/api/v1/wifi/networks", R"({"ssid":"x"})")).status,
              202);  // open network: password optional
    EXPECT_EQ(f.srv
                  ->handle(authed(token, Post, "/api/v1/wifi/networks",
                                  R"({"ssid":"Home","password":"short"})"))
                  .status,
              400);
    EXPECT_EQ(
        f.srv->handle(authed(token, Post, "/api/v1/wifi/networks", R"({"password":"12345678"})"))
            .status,
        400);
    EXPECT_EQ(f.srv
                  ->handle(authed(token, Post, "/api/v1/wifi/networks",
                                  R"({"ssid":"Home","password":12345678})"))
                  .status,
              400);
    EXPECT_EQ(f.wifi->saved_ssids(), (std::vector<std::string>{"Home", "x"}));

    // Re-adding Home makes it the next network tried.
    ASSERT_EQ(f.srv
                  ->handle(authed(token, Post, "/api/v1/wifi/networks",
                                  R"({"ssid":"Home","password":"correct-horse"})"))
                  .status,
              202);
    f.wifi->tick(f.now);  // connect()
    f.wifi->tick(f.now);  // link() reports the result
    Value status = body_json(f.srv->handle(authed(token, Get, "/api/v1/wifi")));
    EXPECT_EQ(field(status, "state"), Value("connected"));
    EXPECT_EQ(field(status, "ssid"), Value("Home"));
    EXPECT_EQ(field(status, "ip"), Value("192.168.1.50"));
    EXPECT_EQ(field(status, "rssi"), Value(-50));
    EXPECT_EQ(field(status, "ssid_hex"), Value("486f6d65"));
    EXPECT_EQ(field(status, "saved"), Value(List{Value(Map{{"ssid", Value("Home")},
                                                           {"ssid_hex", Value("486f6d65")},
                                                           {"auth_failed", Value(false)}}),
                                                 Value(Map{{"ssid", Value("x")},
                                                           {"ssid_hex", Value("78")},
                                                           {"auth_failed", Value(false)}})}));
    EXPECT_FALSE(contains(wisp::to_json(status), "correct-horse"));

    Value scan = body_json(f.srv->handle(authed(token, Get, "/api/v1/wifi/scan")));
    EXPECT_EQ(scan, Value(Map{{"networks", Value(List{Value(Map{{"ssid", Value("Home")},
                                                                {"ssid_hex", Value("486f6d65")},
                                                                {"rssi", Value(-50)},
                                                                {"secured", Value(true)},
                                                                {"saved", Value(true)}}),
                                                      Value(Map{{"ssid", Value("Cafe")},
                                                                {"ssid_hex", Value("43616665")},
                                                                {"rssi", Value(-70)},
                                                                {"secured", Value(false)},
                                                                {"saved", Value(false)}})})}}));
}

TEST(Routes, WifiStatusReportsFailureReason) {
    AdminFixture f;
    const std::string token = f.token();
    f.fake_wifi->networks = {{"Home", -50, true}};
    f.fake_wifi->passwords["Home"] = "correct-horse";
    ASSERT_TRUE(f.wifi->add_network("Home", "wrong-password", f.now));
    uint64_t now = f.now;
    for (int rejection = 1; rejection <= 3; ++rejection) {
        f.wifi->tick(now);  // connect()
        f.wifi->tick(now);  // link() reports the failure
        Value status = body_json(f.srv->handle(authed(token, Get, "/api/v1/wifi")));
        EXPECT_EQ(field(status, "state"), Value("backoff"));
        EXPECT_EQ(field(status, "reason"), Value("auth_failed"));
        if (rejection < 3) {
            EXPECT_EQ(field(field(status, "saved").as_list().at(0), "auth_failed"), Value(false));
        }
        now = f.wifi->status().retry_at_ms;
    }

    // After the third rejection in a row the network says so, so the page
    // can ask for the password.
    Value status = body_json(f.srv->handle(authed(token, Get, "/api/v1/wifi")));
    EXPECT_EQ(field(status, "saved"), Value(List{Value(Map{{"ssid", Value("Home")},
                                                           {"ssid_hex", Value("486f6d65")},
                                                           {"auth_failed", Value(true)}})}));
}

// SSIDs are raw bytes and need not be UTF-8 (GBK routers are common), but
// JSON strings can only carry UTF-8: "ssid" is lossy for such networks, so
// every SSID also travels as "ssid_hex" and can be sent back that way.
TEST(Routes, WifiNonUtf8SsidRoundTripsThroughHex) {
    AdminFixture f;
    const std::string token = f.token();
    const std::string gbk = "\xc4\xe3\xba\xc3";  // "你好" in GBK
    f.fake_wifi->networks = {{gbk, -50, true}};
    f.fake_wifi->passwords[gbk] = "correct-horse";

    Value scan = body_json(f.srv->handle(authed(token, Get, "/api/v1/wifi/scan")));
    const Value& entry = field(scan, "networks").as_list().at(0);
    EXPECT_EQ(field(entry, "ssid_hex"), Value("c4e3bac3"));
    EXPECT_NE(field(entry, "ssid"), Value(gbk));  // the JSON string is lossy

    ASSERT_EQ(f.srv
                  ->handle(authed(token, Post, "/api/v1/wifi/networks",
                                  R"({"ssid_hex":"c4e3bac3","password":"correct-horse"})"))
                  .status,
              202);
    EXPECT_EQ(f.wifi->saved_ssids(), (std::vector<std::string>{gbk}));
    f.wifi->tick(f.now);
    f.wifi->tick(f.now);
    Value status = body_json(f.srv->handle(authed(token, Get, "/api/v1/wifi")));
    EXPECT_EQ(field(status, "state"), Value("connected"));
    EXPECT_EQ(field(status, "ssid_hex"), Value("c4e3bac3"));
    const Value& saved = field(status, "saved").as_list().at(0);
    EXPECT_EQ(field(saved, "ssid_hex"), Value("c4e3bac3"));

    // Uppercase hex is accepted too; the path form takes the raw bytes.
    ASSERT_EQ(f.srv
                  ->handle(authed(token, Post, "/api/v1/wifi/networks",
                                  R"({"ssid_hex":"C4E3BAC3","password":"correct-horse"})"))
                  .status,
              202);
    EXPECT_EQ(f.wifi->saved_ssids(), (std::vector<std::string>{gbk}));
    EXPECT_EQ(f.srv->handle(authed(token, Delete, "/api/v1/wifi/networks/%C4%E3%BA%C3")).status,
              204);
    EXPECT_TRUE(f.wifi->saved_ssids().empty());
}

TEST(Routes, WifiAddNeedsExactlyOneSsidForm) {
    AdminFixture f;
    const std::string token = f.token();
    for (const char* body : {
             R"({"ssid":"Home","ssid_hex":"486f6d65","password":"12345678"})",  // both
             R"({"password":"12345678"})",                                      // neither
             R"({"ssid_hex":"486f6d6"})",                                       // odd length
             R"({"ssid_hex":"486f6d6g"})",                                      // not hex
             R"({"ssid_hex":""})",                                              // empty SSID
             R"({"ssid_hex":486})",                                             // not a string
         }) {
        HttpResponse r = f.srv->handle(authed(token, Post, "/api/v1/wifi/networks", body));
        EXPECT_EQ(r.status, 400) << body;
        EXPECT_EQ(error_code(r), "invalid_argument") << body;
    }
    EXPECT_TRUE(f.wifi->saved_ssids().empty());
}

TEST(Routes, WifiNetworkDeleteDecodesSsid) {
    AdminFixture f;
    const std::string token = f.token();
    ASSERT_TRUE(f.wifi->add_network("My Home", "", f.now));
    ASSERT_TRUE(f.wifi->add_network("咖啡馆", "", f.now));
    ASSERT_TRUE(f.wifi->add_network("a/b", "", f.now));

    HttpResponse res = f.srv->handle(authed(token, Delete, "/api/v1/wifi/networks/My%20Home"));
    EXPECT_EQ(res.status, 204);
    EXPECT_TRUE(res.body.empty());
    EXPECT_EQ(
        f.srv->handle(authed(token, Delete, "/api/v1/wifi/networks/%E5%92%96%E5%95%A1%E9%A6%86"))
            .status,
        204);
    EXPECT_EQ(f.srv->handle(authed(token, Delete, "/api/v1/wifi/networks/a%2fb")).status, 204);
    EXPECT_TRUE(f.wifi->saved_ssids().empty());

    EXPECT_EQ(f.srv->handle(authed(token, Delete, "/api/v1/wifi/networks/Nope")).status, 404);
    for (const char* bad : {"/api/v1/wifi/networks/bad%2", "/api/v1/wifi/networks/bad%zz",
                            "/api/v1/wifi/networks/%"}) {
        HttpResponse r = f.srv->handle(authed(token, Delete, bad));
        EXPECT_EQ(r.status, 400) << bad;
        EXPECT_EQ(error_code(r), "invalid_argument") << bad;
    }
}

TEST(Routes, SettingsPutValidatesAndApplies) {
    AdminFixture f;
    std::vector<uint8_t> applied;
    f.srv.reset();
    f.settings = std::make_unique<wisp::core::SystemSettings>(
        *f.backend, [&applied](uint8_t level) { applied.push_back(level); });
    f.srv = std::make_unique<wisp::admin::AdminServer>(f.make_deps());
    const std::string token = f.token();

    EXPECT_EQ(body_json(f.srv->handle(authed(token, Get, "/api/v1/settings"))),
              Value(Map{{"brightness", Value(200)},
                        {"volume", Value(60)},
                        {"screen_off_s", Value(60)},
                        {"language", Value("zh")},
                        {"utc_offset_min", Value(480)},
                        {"ntp_servers", Value("pool.ntp.org,ntp.aliyun.com")}}));

    HttpResponse put = f.srv->handle(
        authed(token, Put, "/api/v1/settings",
               R"({"brightness":100,"volume":30,"utc_offset_min":-210,"ntp_servers":"10.0.0.1"})"));
    EXPECT_EQ(put.status, 200);
    Value now(Map{{"brightness", Value(100)},
                  {"volume", Value(30)},
                  {"screen_off_s", Value(60)},
                  {"language", Value("zh")},
                  {"utc_offset_min", Value(-210)},
                  {"ntp_servers", Value("10.0.0.1")}});
    EXPECT_EQ(body_json(put), now);
    EXPECT_EQ(applied, (std::vector<uint8_t>{100}));
    EXPECT_EQ(body_json(f.srv->handle(authed(token, Get, "/api/v1/settings"))), now);

    // Each rejected batch leaves every value as it was.
    for (const char* bad :
         {R"({"brightness":5})", R"({"volume":30.5})", R"({"volume":"30"})",
          R"({"screen_off_s":true})", R"({"color":1})", R"({"language":"fr"})", R"({"language":1})",
          R"({"language":"ja","volume":101})", R"({"utc_offset_min":900})",
          R"({"utc_offset_min":-721})", R"({"utc_offset_min":"8"})", R"({"utc_offset_min":5.5})",
          R"({"ntp_servers":""})", R"({"ntp_servers":"a,b,c"})", R"({"ntp_servers":"a b"})",
          R"({"ntp_servers":1})", "[1]", "3"}) {
        HttpResponse r = f.srv->handle(authed(token, Put, "/api/v1/settings", bad));
        EXPECT_EQ(r.status, 400) << bad;
        EXPECT_EQ(error_code(r), "invalid_argument") << bad;
    }
    EXPECT_EQ(f.srv->handle(authed(token, Put, "/api/v1/settings")).status, 400);
    EXPECT_EQ(body_json(f.srv->handle(authed(token, Get, "/api/v1/settings"))), now);
    EXPECT_EQ(applied.size(), 1u);
}

TEST(Routes, SettingsLanguageRoundTrips) {
    AdminFixture f;
    const std::string token = f.token();

    HttpResponse put =
        f.srv->handle(authed(token, Put, "/api/v1/settings", R"({"language":"ja"})"));
    EXPECT_EQ(put.status, 200);
    EXPECT_EQ(field(body_json(put), "language"), Value("ja"));
    EXPECT_EQ(field(body_json(f.srv->handle(authed(token, Get, "/api/v1/settings"))), "language"),
              Value("ja"));

    HttpResponse bad =
        f.srv->handle(authed(token, Put, "/api/v1/settings", R"({"language":"fr"})"));
    EXPECT_EQ(bad.status, 400);
    EXPECT_EQ(error_code(bad), "invalid_argument");
    EXPECT_EQ(field(body_json(f.srv->handle(authed(token, Get, "/api/v1/settings"))), "language"),
              Value("ja"));
}

TEST(Routes, ClientsListMarksCurrent) {
    AdminFixture f;
    const std::string mine = f.token();
    const std::string other = f.token();
    ASSERT_EQ(f.pairing->clients().size(), 2u);
    const std::string other_id = f.pairing->clients()[1].id;

    Value v = body_json(f.srv->handle(authed(mine, Get, "/api/v1/clients")));
    const Value& list = field(v, "clients");
    ASSERT_EQ(list.type(), Value::Type::List);
    ASSERT_EQ(list.as_list().size(), 2u);
    const Value& first = list.as_list()[0];
    const Value& second = list.as_list()[1];
    EXPECT_EQ(field(first, "id"), Value(f.pairing->clients()[0].id));
    EXPECT_TRUE(is_true(field(first, "current")));
    EXPECT_FALSE(is_true(field(second, "current")));
    EXPECT_EQ(field(second, "id"), Value(other_id));
    EXPECT_EQ(field(first, "label").type(), Value::Type::String);
    EXPECT_EQ(field(first, "created"), Value(int64_t{1767225600}));
    EXPECT_EQ(field(first, "last_seen"), Value(int64_t{1767225600}));
    EXPECT_FALSE(contains(wisp::to_json(v), mine));
    EXPECT_FALSE(contains(wisp::to_json(v), other));

    HttpResponse del = f.srv->handle(authed(mine, Delete, "/api/v1/clients/" + other_id));
    EXPECT_EQ(del.status, 204);
    EXPECT_EQ(f.srv->handle(authed(other, Get, "/api/v1/clients")).status, 401);
    EXPECT_EQ(f.srv->handle(authed(mine, Delete, "/api/v1/clients/" + other_id)).status, 404);
}

TEST(Routes, DeviceAndDevices) {
    AdminFixture f;
    const std::string token = f.token();
    f.report.record({"net.wifi", wisp::hal::DeviceClass::Network, wisp::core::DeviceState::Ok, ""});
    f.report.record({"imu.main", wisp::hal::DeviceClass::Imu, wisp::core::DeviceState::Missing,
                     "not detected"});
    f.registry.add(f.fake_wifi);

    Value device = body_json(f.srv->handle(authed(token, Get, "/api/v1/device")));
    EXPECT_EQ(field(device, "manifest"), f.registry.manifest("sim"));
    EXPECT_EQ(field(device, "boot"), f.report.to_value("sim", "0.2.0-test"));
    EXPECT_EQ(field(device, "status"), Value(Map{}));

    Value devices = body_json(f.srv->handle(authed(token, Get, "/api/v1/devices")));
    const Value& list = field(devices, "devices");
    ASSERT_EQ(list.type(), Value::Type::List);
    // Registered devices (sorted by id), then devices that never made it
    // into the registry.
    ASSERT_EQ(list.as_list().size(), 3u);
    const Value& wifi = list.as_list()[0];
    EXPECT_EQ(wifi, Value(Map{{"id", Value("net.wifi")},
                              {"class", Value("network")},
                              {"model", Value("FAKE")},
                              {"state", Value("ok")},
                              {"caps", f.fake_wifi->info().caps}}));
    EXPECT_EQ(field(list.as_list()[1], "id"), Value("storage.int"));
    EXPECT_EQ(field(list.as_list()[1], "state"), Value("ok"));  // registered, not in the report
    EXPECT_EQ(list.as_list()[2], Value(Map{{"id", Value("imu.main")},
                                           {"class", Value("imu")},
                                           {"model", Value("")},
                                           {"state", Value("missing")},
                                           {"caps", Value(Map{})}}));
}

TEST(Routes, FactoryResetDelegatesToDevice) {
    AdminFixture f;
    const std::string token = f.token();
    ASSERT_TRUE(f.wifi->add_network("Home", "", f.now));
    ASSERT_TRUE(f.models->create(
        {.kind = wisp::core::ModelKind::Llm, .name = "x", .preset = "openai", .api_key = "sk-1"}));

    HttpResponse res = f.srv->handle(authed(token, Post, "/api/v1/system/factory-reset"));
    EXPECT_EQ(res.status, 202);
    EXPECT_EQ(f.factory_resets.load(), 1);
    // The device asks for confirmation; the route itself erases nothing.
    EXPECT_EQ(f.wifi->saved_ssids().size(), 1u);
    EXPECT_EQ(f.models->get().profiles.size(), 1u);
    EXPECT_EQ(f.pairing->clients().size(), 1u);
    EXPECT_EQ(f.reboots.load(), 0);
}

TEST(Routes, RebootRepliesFirstThenReboots) {
    AdminFixture f;
    const std::string token = f.token();
    const uint64_t start = wisp::os::now_ms();
    HttpResponse res = f.srv->handle(authed(token, Post, "/api/v1/system/reboot"));
    EXPECT_EQ(res.status, 202);
    EXPECT_EQ(f.reboots.load(), 0);  // the reply goes out first
    // A second request while one is pending does not stack.
    EXPECT_EQ(f.srv->handle(authed(token, Post, "/api/v1/system/reboot")).status, 202);
    EXPECT_TRUE(wisp::test::wait_until([&] { return f.reboots.load() == 1; }, 3000));
    EXPECT_GE(wisp::os::now_ms() - start, 400u);
    EXPECT_FALSE(wisp::test::wait_until([&] { return f.reboots.load() > 1; }, 1000));
}

TEST(Routes, DestroyingTheServerCancelsAPendingReboot) {
    AdminFixture f;
    const std::string token = f.token();
    EXPECT_EQ(f.srv->handle(authed(token, Post, "/api/v1/system/reboot")).status, 202);
    f.srv.reset();
    wisp::os::sleep_ms(700);
    EXPECT_EQ(f.reboots.load(), 0);
}

}  // namespace
