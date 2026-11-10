// wisp.http on a running SceneRuntime over a SceneHttp and a fake client
// (scene-runtime-core spec 5.3).

#include <gtest/gtest.h>

#include <algorithm>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "support/fake_http.hpp"
#include "support/scene_runtime_fixture.hpp"
#include "support/wait.hpp"
#include "wisp/net/scene_http.hpp"

namespace {

using wisp::core::SceneState;
using wisp::test::wait_until;

const char* kScreen = "return ui.screen { ui.label { text = \"x\" } }\n";

std::string manifest_with_hosts(const std::string& id) {
    return R"({"id":")" + id +
           R"(","name":"Demo","version":"1.0.0","schema_version":1,)"
           R"("permissions":{"http":["api.example.com"]}})";
}

class HttpBinding : public wisp::test::SceneRuntimeFixture {
protected:
    void SetUp() override {
        SceneRuntimeFixture::SetUp();
        http = std::make_unique<wisp::net::SceneHttp>(client);
        ASSERT_TRUE(http->start());
        configure_deps = [this](wisp::scene::SceneRuntimeDeps& d) { d.http = http.get(); };
    }

    void TearDown() override {
        client.hold = false;
        SceneRuntimeFixture::TearDown();
        http.reset();
    }

    void install_http(const std::string& id, const std::string& lua) {
        install(id, lua + kScreen, {{"scene.json", manifest_with_hosts(id)}});
    }

    // What the scene printed: its log without the proxy's "http: ..."
    // lines (a refused request is logged under the scene's tag too).
    std::vector<std::string> printed(const std::string& id) {
        std::vector<std::string> out;
        for (auto& line : scene_log(id))
            if (line.rfind("http: ", 0) != 0) out.push_back(std::move(line));
        return out;
    }

    // Waits until the scene has printed `n` lines and returns them.
    std::vector<std::string> wait_log(const std::string& id, size_t n) {
        EXPECT_TRUE(wait_until([&] { return printed(id).size() >= n; }, 3000))
            << "logged: " << ::testing::PrintToString(scene_log(id));
        return printed(id);
    }

    wisp::test::FakeHttpClient client;
    std::unique_ptr<wisp::net::SceneHttp> http;
};

TEST_F(HttpBinding, GetDeliversJson) {
    start();
    client.next = {.status = 200,
                   .body = R"({"a":1})",
                   .headers = {{"content-type", "application/json; charset=utf-8"},
                               {"x-multi", "1"},
                               {"x-multi", "2"}}};
    install_http("h", R"(
wisp.http.get("https://api.example.com/x", {headers = {["X-Api-Key"] = "k"}}, function(err, res)
    print(err, res.status, math.type(res.status), res.body, res.json.a,
          res.headers["content-type"], res.headers["x-multi"])
end)
)");
    ASSERT_TRUE(rt->open("h"));
    auto lines = wait_log("h", 1);
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_EQ(lines[0], "nil\t200\tinteger\t{\"a\":1}\t1\tapplication/json; charset=utf-8\t1, 2");
    ASSERT_TRUE(client.last());
    EXPECT_EQ(client.last()->method, wisp::net::Method::Get);
    EXPECT_EQ(client.last()->url, "https://api.example.com/x");
    EXPECT_EQ(client.last()->headers,
              (std::vector<std::pair<std::string, std::string>>{{"X-Api-Key", "k"}}));
    EXPECT_EQ(rt->status("h").state, SceneState::Running);
}

TEST_F(HttpBinding, JsonOnlyForJsonThatParses) {
    start();
    client.next = {.status = 200, .body = "not json", .headers = {{"content-type", "text/json"}}};
    install_http("a", R"(
wisp.http.get("https://api.example.com/x", function(err, res) print(err, res.json, res.body) end)
)");
    ASSERT_TRUE(rt->open("a"));
    EXPECT_EQ(wait_log("a", 1), (std::vector<std::string>{"nil\tnil\tnot json"}));

    client.next = {
        .status = 200, .body = R"({"a":1})", .headers = {{"content-type", "text/plain"}}};
    install_http("b", R"(
wisp.http.get("https://api.example.com/x", function(err, res) print(err, res.json, res.body) end)
)");
    ASSERT_TRUE(rt->open("b"));
    EXPECT_EQ(wait_log("b", 1), (std::vector<std::string>{"nil\tnil\t{\"a\":1}"}));
}

TEST_F(HttpBinding, ErrorsAreStrings) {
    start();
    client.next = {.transport = wisp::net::TransportError::Timeout};
    install_http("h", R"(
local function show(err, res) print(err, res) end
wisp.http.get("https://other.example.com/", nil, show)
wisp.http.get("http://api.example.com/", {}, show)
wisp.http.get("https://api.example.com/", {headers = {Cookie = "c"}}, show)
wisp.http.get("https://api.example.com/slow", nil, show)
print(pcall(wisp.http.get, 5, nil, show))
print(pcall(wisp.http.get, "https://api.example.com/", nil, 5))
print(pcall(wisp.http.get, "https://api.example.com/", {headers = {X = 5}}, show))
print(pcall(wisp.http.get, "https://api.example.com/", {headers = 5}, show))
print(pcall(wisp.http.post, "https://api.example.com/", 5, nil, show))
)");
    ASSERT_TRUE(rt->open("h"));
    auto lines = wait_log("h", 9);
    ASSERT_EQ(lines.size(), 9u);
    for (size_t i = 0; i < 5; ++i) EXPECT_EQ(lines[i].rfind("false\t", 0), 0u) << lines[i];
    std::vector<std::string> answers(lines.begin() + 5, lines.end());
    EXPECT_EQ(answers, (std::vector<std::string>{"permission_denied\tnil", "not_https\tnil",
                                                 "permission_denied\tnil", "timeout\tnil"}));
    EXPECT_EQ(client.calls(), 1u);
    // The refusals are in the scene's log, by host only.
    EXPECT_TRUE(log_has("scene.h", "http: permission_denied (host other.example.com)"));
    EXPECT_TRUE(log_has("scene.h", "http: not_https (host api.example.com)"));
}

TEST_F(HttpBinding, TableBodyIsJson) {
    start();
    install_http("h", R"(
wisp.http.post("https://api.example.com/t", {a = 1}, function(err, res) print(err, res.status) end)
)");
    ASSERT_TRUE(rt->open("h"));
    EXPECT_EQ(wait_log("h", 1), (std::vector<std::string>{"nil\t200"}));
    ASSERT_EQ(client.calls(), 1u);
    EXPECT_EQ(client.last()->method, wisp::net::Method::Post);
    EXPECT_EQ(wisp::test::FakeHttpClient::body_of(*client.last()), R"({"a":1})");
    EXPECT_EQ(
        client.last()->headers,
        (std::vector<std::pair<std::string, std::string>>{{"Content-Type", "application/json"}}));

    install_http("bad", R"(
local function show(err, res) print(err, res) end
wisp.http.post("https://api.example.com/t", {f = function() end}, nil, show)
local deep = {}
local t = deep
for i = 1, 40 do t.x = {}; t = t.x end
wisp.http.post("https://api.example.com/t", deep, nil, show)
wisp.http.post("https://api.example.com/t", {s = string.rep("x", 70000)}, nil, show)
)");
    ASSERT_TRUE(rt->open("bad"));
    EXPECT_EQ(wait_log("bad", 3), (std::vector<std::string>{"not_serializable\tnil",
                                                            "too_deep\tnil", "too_large\tnil"}));
    EXPECT_EQ(client.calls(), 1u);
}

TEST_F(HttpBinding, StringBodyAndOwnContentType) {
    start();
    install_http("h", R"(
wisp.http.post("https://api.example.com/s", "raw text", function(err) print(err) end)
)");
    ASSERT_TRUE(rt->open("h"));
    wait_log("h", 1);
    ASSERT_EQ(client.calls(), 1u);
    EXPECT_EQ(wisp::test::FakeHttpClient::body_of(*client.last()), "raw text");
    EXPECT_TRUE(client.last()->headers.empty());

    install_http("j", R"(
wisp.http.post("https://api.example.com/t", {1, 2}, {headers = {["content-type"] = "text/x-json"}},
               function(err) print(err) end)
)");
    ASSERT_TRUE(rt->open("j"));
    wait_log("j", 1);
    ASSERT_EQ(client.calls(), 2u);
    EXPECT_EQ(wisp::test::FakeHttpClient::body_of(*client.last()), "[1,2]");
    EXPECT_EQ(client.last()->headers,
              (std::vector<std::pair<std::string, std::string>>{{"content-type", "text/x-json"}}));
}

TEST_F(HttpBinding, ClosingCancelsInFlight) {
    start();
    client.hold = true;
    install_http("h", R"(
wisp.http.get("https://api.example.com/x", nil, function(err, res) print("late", err) end)
)");
    ASSERT_TRUE(rt->open("h"));
    ASSERT_TRUE(wait_until([&] { return client.held.load() == 1; }, 2000));
    ASSERT_TRUE(rt->close("h"));
    EXPECT_TRUE(wait_until([&] { return client.saw_cancel.load(); }, 2000));
    client.hold = false;
    wisp::os::sleep_ms(100);
    EXPECT_TRUE(printed("h").empty());
}

TEST_F(HttpBinding, CallbackErrorFaultsTheScene) {
    start();
    install_http("h", R"(
wisp.http.get("https://api.example.com/x", nil, function() error("boom") end)
)");
    ASSERT_TRUE(rt->open("h"));
    ASSERT_TRUE(wait_until([&] { return rt->status("h").state == SceneState::Error; }, 3000));
    EXPECT_NE(rt->status("h").error.find("boom"), std::string::npos) << rt->status("h").error;
}

// A request that waited for deep standby's network window says so
// (spec 4.4); one sent at once has no such field.
TEST_F(HttpBinding, DeferredFlagReachesCallback) {
    start();
    http->set_deferring(true);
    install_http("h", R"(
wisp.http.get("https://api.example.com/a", nil, function(err, res) print("a", err, res.deferred) end)
)");
    ASSERT_TRUE(rt->open("h"));
    ASSERT_TRUE(wait_until([&] { return http->deferred_count() == 1; }, 3000));
    EXPECT_EQ(client.calls(), 0u);
    http->set_deferring(false);
    EXPECT_EQ(wait_log("h", 1), (std::vector<std::string>{"a\tnil\ttrue"}));

    install_http("g", R"(
wisp.http.get("https://api.example.com/b", nil, function(err, res) print("b", err, res.deferred) end)
)");
    ASSERT_TRUE(rt->open("g"));
    EXPECT_EQ(wait_log("g", 1), (std::vector<std::string>{"b\tnil\tnil"}));
}

TEST_F(HttpBinding, RequestMethods) {
    start();
    std::mutex mu;
    std::map<std::string, wisp::net::Method> seen;
    client.responder = [&](const wisp::net::HttpClientRequest& req) {
        std::lock_guard<std::mutex> lock(mu);
        seen[req.url] = req.method;
        return wisp::net::HttpClientResponse{.status = 200};
    };
    install_http("h", R"(
local function show(err, res) print(err, res and res.status) end
for _, m in ipairs({"GET", "POST", "PUT", "PATCH", "DELETE", "HEAD"}) do
    wisp.http.request({method = m, url = "https://api.example.com/" .. m, body = "b"}, show)
end
wisp.http.request({url = "https://api.example.com/default"}, show)
wisp.http.request({method = "get", url = "https://other.example.com/"}, show)
wisp.http.request({url = "http://api.example.com/", follow = true, timeout_ms = 2000}, show)
print(pcall(wisp.http.request, {method = "TRACE", url = "https://api.example.com/"}, show))
print(pcall(wisp.http.request, {method = "GET"}, show))
print(pcall(wisp.http.request, "https://api.example.com/", show))
print(pcall(wisp.http.request, {url = "https://api.example.com/", timeout_ms = 999}, show))
print(pcall(wisp.http.request, {url = "https://api.example.com/", timeout_ms = 120001}, show))
print(pcall(wisp.http.request, {url = "https://api.example.com/", follow = 1}, show))
print(pcall(wisp.http.request, {url = "https://api.example.com/", body = 5}, show))
print(pcall(wisp.http.request, {url = "https://api.example.com/"}))
)");
    ASSERT_TRUE(rt->open("h"));
    auto lines = wait_log("h", 17);
    ASSERT_EQ(lines.size(), 17u);
    for (size_t i = 0; i < 8; ++i) EXPECT_EQ(lines[i].rfind("false\t", 0), 0u) << lines[i];
    EXPECT_NE(lines[0].find("method"), std::string::npos) << lines[0];
    EXPECT_NE(lines[7].find("function expected"), std::string::npos) << lines[7];
    std::vector<std::string> answers(lines.begin() + 8, lines.end());
    std::sort(answers.begin(), answers.end());
    EXPECT_EQ(answers, (std::vector<std::string>{"nil\t200", "nil\t200", "nil\t200", "nil\t200",
                                                 "nil\t200", "nil\t200", "nil\t200",
                                                 "not_https\tnil", "permission_denied\tnil"}))
        << ::testing::PrintToString(lines);
    std::lock_guard<std::mutex> lock(mu);
    using M = wisp::net::Method;
    EXPECT_EQ(seen, (std::map<std::string, M>{{"https://api.example.com/GET", M::Get},
                                              {"https://api.example.com/POST", M::Post},
                                              {"https://api.example.com/PUT", M::Put},
                                              {"https://api.example.com/PATCH", M::Patch},
                                              {"https://api.example.com/DELETE", M::Delete},
                                              {"https://api.example.com/HEAD", M::Head},
                                              {"https://api.example.com/default", M::Get}}));
}

TEST_F(HttpBinding, UnavailableWithoutAProxy) {
    configure_deps = {};
    start();
    install_http("h", R"(
print(pcall(wisp.http.get, "https://api.example.com/", nil, function() end))
)");
    ASSERT_TRUE(rt->open("h"));
    auto lines = wait_log("h", 1);
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_NE(lines[0].find("wisp.http is not available"), std::string::npos) << lines[0];
}

}  // namespace
