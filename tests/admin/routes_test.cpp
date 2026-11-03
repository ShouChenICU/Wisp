// The authenticated business routes: device, devices, settings, Wi-Fi,
// LLM, clients and system (spec 6.1).

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
using wisp::net::TransportError;
using wisp::test::AdminFixture;
using wisp::test::body_json;
using wisp::test::contains;
using wisp::test::error_code;
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

const std::vector<std::string>& every_authenticated_get_route() {
    static const std::vector<std::string> paths = {
        "/api/v1/info", "/api/v1/device",    "/api/v1/devices", "/api/v1/settings",
        "/api/v1/wifi", "/api/v1/wifi/scan", "/api/v1/llm",     "/api/v1/clients",
    };
    return paths;
}

const Value& field(const Value& v, const char* key) {
    static const Value null;
    const Value* f = v.find(key);
    EXPECT_NE(f, nullptr) << "missing \"" << key << "\" in " << wisp::to_json(v);
    return f != nullptr ? *f : null;
}

// Typed reads that fail the test instead of asserting on a wrong type.
bool is_true(const Value& v) {
    EXPECT_EQ(v.type(), Value::Type::Bool) << wisp::to_json(v);
    return v.type() == Value::Type::Bool && v.as_bool();
}

int64_t int_of(const Value& v) {
    EXPECT_EQ(v.type(), Value::Type::Int) << wisp::to_json(v);
    return v.type() == Value::Type::Int ? v.as_int() : -1;
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

TEST(Routes, NoResponseEverContainsApiKey) {
    AdminFixture f;
    const std::string token = f.token();
    ASSERT_TRUE(f.llm->set({.base_url = "https://x/v1", .api_key = "sk-NEVERLEAK"}));
    f.http.next = {.status = 401, .body = R"({"error":"bad key sk-NEVERLEAK"})"};

    std::vector<HttpRequest> reqs;
    for (const std::string& path : every_authenticated_get_route()) {
        reqs.push_back(authed(token, Get, path));
    }
    reqs.push_back(authed(token, Post, "/api/v1/llm/test"));
    reqs.push_back(authed(token, Put, "/api/v1/llm", R"({"model":"gpt-x"})"));
    // A rejected key must not be echoed back in the error either.
    reqs.push_back(authed(token, Put, "/api/v1/llm", R"({"api_key":"sk-NEVERLEAK x"})"));
    for (const HttpRequest& req : reqs) {
        HttpResponse res = f.srv->handle(req);
        EXPECT_LT(res.status, 500) << req.path;
        EXPECT_EQ(res.body.find("NEVERLEAK"), std::string::npos) << req.path << ": " << res.body;
        for (const auto& [k, v] : res.headers) {
            EXPECT_EQ(v.find("NEVERLEAK"), std::string::npos) << req.path << " header " << k;
        }
    }
    // The probe really ran, and its key went only into the header.
    auto sent = f.http.last();
    ASSERT_TRUE(sent.has_value());
    EXPECT_EQ(sent->url, "https://x/v1/models");
}

TEST(Routes, LlmGetPutAndTest) {
    AdminFixture f;
    const std::string token = f.token();

    Value empty = body_json(f.srv->handle(authed(token, Get, "/api/v1/llm")));
    EXPECT_EQ(empty,
              Value(Map{{"base_url", Value("")}, {"model", Value("")}, {"has_key", Value(false)}}));

    HttpResponse put = f.srv->handle(
        authed(token, Put, "/api/v1/llm",
               R"({"base_url":"https://api.example.com/v1/","model":"m1","api_key":"sk-1"})"));
    EXPECT_EQ(put.status, 200);
    Value expected(Map{{"base_url", Value("https://api.example.com/v1")},
                       {"model", Value("m1")},
                       {"has_key", Value(true)}});
    EXPECT_EQ(body_json(put), expected);
    EXPECT_EQ(body_json(f.srv->handle(authed(token, Get, "/api/v1/llm"))), expected);

    // Validation failures change nothing.
    EXPECT_EQ(f.srv->handle(authed(token, Put, "/api/v1/llm", R"({"base_url":"ftp://x"})")).status,
              400);
    EXPECT_EQ(f.srv->handle(authed(token, Put, "/api/v1/llm", R"({"model":5})")).status, 400);
    EXPECT_EQ(f.srv->handle(authed(token, Put, "/api/v1/llm", R"({"temperature":"1"})")).status,
              400);
    EXPECT_EQ(f.srv->handle(authed(token, Put, "/api/v1/llm", "[]")).status, 400);
    EXPECT_EQ(f.srv->handle(authed(token, Put, "/api/v1/llm")).status, 400);
    EXPECT_EQ(f.llm->get().base_url, "https://api.example.com/v1");

    f.http.next = {.status = 200, .body = R"({"data":[{"id":"m1"},{"id":"m2"}]})"};
    HttpResponse ok = f.srv->handle(authed(token, Post, "/api/v1/llm/test"));
    EXPECT_EQ(ok.status, 200);
    Value okv = body_json(ok);
    EXPECT_TRUE(is_true(field(okv, "ok")));
    EXPECT_TRUE(field(okv, "error").is_null());
    EXPECT_EQ(int_of(field(okv, "http_status")), 200);
    EXPECT_EQ(field(okv, "models"), Value(List{Value("m1"), Value("m2")}));
    EXPECT_EQ(field(okv, "latency_ms").type(), Value::Type::Int);

    struct Case {
        TransportError transport;
        int status;
        const char* error;
    };
    for (const Case& c :
         {Case{TransportError::Dns, 0, "dns"}, Case{TransportError::Connect, 0, "connect"},
          Case{TransportError::Tls, 0, "tls"}, Case{TransportError::Timeout, 0, "timeout"},
          Case{TransportError::Other, 0, "other"}, Case{TransportError::None, 503, "http"}}) {
        f.http.next = {.transport = c.transport, .status = c.status, .body = "upstream-body"};
        HttpResponse res = f.srv->handle(authed(token, Post, "/api/v1/llm/test"));
        EXPECT_EQ(res.status, 200);
        Value v = body_json(res);
        EXPECT_FALSE(is_true(field(v, "ok")));
        EXPECT_EQ(field(v, "error"), Value(c.error));
        EXPECT_EQ(int_of(field(v, "http_status")), c.status);
        EXPECT_EQ(field(v, "models"), Value(List{}));
        EXPECT_FALSE(contains(res.body, "upstream-body"));
    }
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
    EXPECT_EQ(field(status, "saved"), Value(List{Value("Home"), Value("x")}));
    EXPECT_FALSE(contains(wisp::to_json(status), "correct-horse"));

    Value scan = body_json(f.srv->handle(authed(token, Get, "/api/v1/wifi/scan")));
    EXPECT_EQ(scan, Value(Map{{"networks", Value(List{Value(Map{{"ssid", Value("Home")},
                                                                {"rssi", Value(-50)},
                                                                {"secured", Value(true)},
                                                                {"saved", Value(true)}}),
                                                      Value(Map{{"ssid", Value("Cafe")},
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
    f.wifi->tick(f.now);  // connect()
    f.wifi->tick(f.now);  // link() reports the failure

    Value status = body_json(f.srv->handle(authed(token, Get, "/api/v1/wifi")));
    EXPECT_EQ(field(status, "state"), Value("backoff"));
    EXPECT_EQ(field(status, "reason"), Value("auth_failed"));
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

    EXPECT_EQ(
        body_json(f.srv->handle(authed(token, Get, "/api/v1/settings"))),
        Value(Map{{"brightness", Value(200)}, {"volume", Value(60)}, {"screen_off_s", Value(60)}}));

    HttpResponse put =
        f.srv->handle(authed(token, Put, "/api/v1/settings", R"({"brightness":100,"volume":30})"));
    EXPECT_EQ(put.status, 200);
    Value now(Map{{"brightness", Value(100)}, {"volume", Value(30)}, {"screen_off_s", Value(60)}});
    EXPECT_EQ(body_json(put), now);
    EXPECT_EQ(applied, (std::vector<uint8_t>{100}));
    EXPECT_EQ(body_json(f.srv->handle(authed(token, Get, "/api/v1/settings"))), now);

    // Each rejected batch leaves every value as it was.
    for (const char* bad : {R"({"brightness":5})", R"({"volume":30.5})", R"({"volume":"30"})",
                            R"({"screen_off_s":true})", R"({"color":1})", "[1]", "3"}) {
        HttpResponse r = f.srv->handle(authed(token, Put, "/api/v1/settings", bad));
        EXPECT_EQ(r.status, 400) << bad;
        EXPECT_EQ(error_code(r), "invalid_argument") << bad;
    }
    EXPECT_EQ(f.srv->handle(authed(token, Put, "/api/v1/settings")).status, 400);
    EXPECT_EQ(body_json(f.srv->handle(authed(token, Get, "/api/v1/settings"))), now);
    EXPECT_EQ(applied.size(), 1u);
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
    ASSERT_TRUE(f.llm->set({.base_url = "https://x/v1", .api_key = "sk-1"}));

    HttpResponse res = f.srv->handle(authed(token, Post, "/api/v1/system/factory-reset"));
    EXPECT_EQ(res.status, 202);
    EXPECT_EQ(f.factory_resets.load(), 1);
    // The device asks for confirmation; the route itself erases nothing.
    EXPECT_EQ(f.wifi->saved_ssids().size(), 1u);
    EXPECT_TRUE(f.llm->get().has_key);
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
    EXPECT_TRUE(wisp::test::wait_until([&] { return f.reboots.load() == 1; }, 3000));
    EXPECT_GE(wisp::os::now_ms() - start, 400u);
    // A second request while one is pending (or done) does not stack.
    EXPECT_EQ(f.reboots.load(), 1);
}

TEST(Routes, DestroyingTheServerCancelsAPendingReboot) {
    AdminFixture f;
    const std::string token = f.token();
    EXPECT_EQ(f.srv->handle(authed(token, Post, "/api/v1/system/reboot")).status, 202);
    f.srv.reset();
    wisp::os::sleep_ms(700);
    EXPECT_EQ(f.reboots.load(), 0);
}

TEST(Routes, LlmTestWithoutHttpClientIsUnavailable) {
    AdminFixture f;
    f.srv.reset();
    auto deps = f.make_deps();
    deps.http_client = nullptr;
    f.srv = std::make_unique<wisp::admin::AdminServer>(std::move(deps));
    const std::string token = f.token();
    HttpResponse res = f.srv->handle(authed(token, Post, "/api/v1/llm/test"));
    EXPECT_EQ(res.status, 503);
}

}  // namespace
