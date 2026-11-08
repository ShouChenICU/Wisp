// The agent wired into the App (agent-core spec 2, 7.1): built and started
// with a screen and a scene runtime, reachable over the admin routes, and
// absent (503) without a screen.

#include <gtest/gtest.h>

#include <algorithm>
#include <span>
#include <string>

#include "app/ui_net_harness.hpp"
#include "support/fake_http.hpp"
#include "support/wait.hpp"
#include "wisp/agent/agent_service.hpp"
#include "wisp/base/json.hpp"
#include "wisp/core/log.hpp"
#include "wisp/net/pairing_service.hpp"
#include "wisp/os/os.hpp"

using wisp::Value;
using wisp::net::HttpRequest;
using wisp::net::HttpResponse;
using wisp::net::Method;
using wisp::test::make_req;
using wisp::test::UiNetHarness;
using wisp::test::UiNetOptions;
using wisp::test::wait_until;
using wisp::test::with_csrf;
using wisp::test::with_token;

namespace {

std::string pair_browser(UiNetHarness& h) {
    wisp::net::PairingService* pairing = h.app->pairing();
    EXPECT_NE(pairing, nullptr);
    if (pairing == nullptr) return {};
    EXPECT_TRUE(pairing->request_code(wisp::os::now_ms()));
    auto code = pairing->active_code(wisp::os::now_ms()).value_or("");
    auto token = pairing->confirm(code, wisp::net::ClientInfo{}, wisp::os::now_ms());
    EXPECT_TRUE(token);
    return token ? token.value() : std::string();
}

HttpResponse get(UiNetHarness& h, const std::string& token, const std::string& path) {
    return h.server->handle(with_token(make_req(Method::Get, path), token));
}

HttpResponse post_message(UiNetHarness& h, const std::string& token, const std::string& text) {
    const std::string body = wisp::to_json(Value(wisp::Map{{"text", Value(text)}}));
    HttpRequest head =
        with_csrf(with_token(make_req(Method::Post, "/api/v1/agent/messages"), token));
    head.content_length = body.size();
    auto stream = h.server->stream_handler();
    if (!stream) return {};
    auto start = stream(head);
    if (!start) return {};
    if (!start->sink) return start->response;
    if (auto early = start->sink->on_data(
            std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(body.data()), body.size()))) {
        return *early;
    }
    return start->sink->on_end();
}

std::string body_text(const HttpResponse& res) {
    if (!res.static_body.empty()) {
        return std::string(reinterpret_cast<const char*>(res.static_body.data()),
                           res.static_body.size());
    }
    return res.body;
}

UiNetOptions with_server(bool display) {
    UiNetOptions o;
    o.with_server = true;
    o.with_display = display;
    return o;
}

}  // namespace

TEST(AppAgent, BootStartsAgentWithAScreen) {
    wisp::log::reset_for_test();
    wisp::log::init();
    UiNetHarness h(with_server(true));
    ASSERT_TRUE(h.start());
    ASSERT_NE(h.app->agent(), nullptr);
    const std::string token = pair_browser(h);

    HttpResponse conv = get(h, token, "/api/v1/agent/conversation");
    ASSERT_EQ(conv.status, 200) << conv.body;
    auto v = wisp::from_json(body_text(conv));
    ASSERT_TRUE(v);
    EXPECT_EQ(v.value().find("state")->as_string(), "idle");

    // wisp.agent runs: the message is taken (not "not running") and the
    // turn ends (no model is set up, so with a note saying so).
    HttpResponse sent = post_message(h, token, "hello");
    ASSERT_EQ(sent.status, 202) << sent.body;
    ASSERT_TRUE(wait_until(
        [&] {
            auto view = h.app->agent()->view();
            return !view.running_turn && view.messages->size() >= 3 &&
                   std::any_of(view.messages->begin(), view.messages->end(),
                               [](const wisp::agent::Message& m) {
                                   return m.role == wisp::agent::Role::Note;
                               });
        },
        10000));
    EXPECT_EQ(wisp::log::ring_snapshot().find("agent tools"), std::string::npos)
        << wisp::log::ring_snapshot();
    wisp::log::reset_for_test();
}

TEST(AppAgent, HeadlessHasNoAgent) {
    UiNetHarness h(with_server(false));
    ASSERT_TRUE(h.start());
    EXPECT_EQ(h.app->agent(), nullptr);
    const std::string token = pair_browser(h);
    HttpResponse conv = get(h, token, "/api/v1/agent/conversation");
    EXPECT_EQ(conv.status, 503);
    EXPECT_EQ(post_message(h, token, "hello").status, 503);
}

// Stopping the App while a message waits for the model ends the turn
// before the scenes and the storage worker go.
TEST(AppAgent, StopsWithATurnInFlight) {
    UiNetHarness h(with_server(true));
    ASSERT_TRUE(h.start());
    const std::string token = pair_browser(h);
    ASSERT_EQ(post_message(h, token, "hello").status, 202);
    h.app->stop();
    EXPECT_EQ(h.app->agent(), nullptr);
}

// The per-turn limits come from Settings (agent/max_steps, agent/turn_min)
// and follow PUT /settings without a restart.
TEST(AppAgent, TurnLimitsFollowTheSettings) {
    UiNetHarness h(with_server(true));
    ASSERT_TRUE(h.start());
    ASSERT_NE(h.app->agent(), nullptr);
    EXPECT_EQ(h.app->agent()->turn_limits().max_steps, wisp::agent::kMaxSteps);
    EXPECT_EQ(h.app->agent()->turn_limits().turn_ms, wisp::agent::kTurnMs);
    const std::string token = pair_browser(h);

    HttpResponse put = h.server->handle(with_csrf(with_token(
        make_req(Method::Put, "/api/v1/settings", R"({"agent_max_steps":8,"agent_turn_min":3})"),
        token)));
    ASSERT_EQ(put.status, 200) << put.body;
    EXPECT_TRUE(wait_until(
        [&] {
            auto l = h.app->agent()->turn_limits();
            return l.max_steps == 8 && l.turn_ms == 3u * 60000;
        },
        5000));

    HttpResponse bad = h.server->handle(with_csrf(
        with_token(make_req(Method::Put, "/api/v1/settings", R"({"agent_turn_min":0})"), token)));
    EXPECT_EQ(bad.status, 400);
    EXPECT_EQ(h.app->agent()->turn_limits().turn_ms, 3u * 60000);
}
