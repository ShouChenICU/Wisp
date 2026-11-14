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
#include "wisp/agent/tool_registry.hpp"
#include "wisp/base/json.hpp"
#include "wisp/core/log.hpp"
#include "wisp/net/pairing_service.hpp"
#include "wisp/os/os.hpp"
#include "wisp/shell/activity.hpp"
#include "wisp/ui/strings.hpp"

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

// device.list and device.call sit right after the notification tools,
// before docs.read: the order the model sees (and the provider caches).
TEST(AppAgent, DeviceToolsFollowTheNotificationTools) {
    UiNetHarness h(with_server(true));
    ASSERT_TRUE(h.start());
    const wisp::agent::ToolRegistry* tools = h.app->agent_tools();
    ASSERT_NE(tools, nullptr);
    const std::string json(tools->tools_json().data(), tools->tools_json().size());
    const size_t dismiss = json.find("\"notify_dismiss\"");
    const size_t list = json.find("\"device_list\"");
    const size_t call = json.find("\"device_call\"");
    const size_t docs = json.find("\"docs_read\"");
    ASSERT_NE(dismiss, std::string::npos);
    ASSERT_NE(list, std::string::npos);
    ASSERT_NE(call, std::string::npos);
    ASSERT_NE(docs, std::string::npos);
    EXPECT_LT(dismiss, list);
    EXPECT_LT(list, call);
    EXPECT_LT(call, docs);
}

// Every tool the wired agent registers has an activity label of its own in
// every language, so a new tool cannot show "Call <tool>" to the user.
TEST(AppAgent, EveryRegisteredToolHasAnActivityLabel) {
    UiNetHarness h(with_server(true));
    ASSERT_TRUE(h.start());
    const wisp::agent::ToolRegistry* tools = h.app->agent_tools();
    ASSERT_NE(tools, nullptr);
    auto parsed =
        wisp::from_json(std::string_view(tools->tools_json().data(), tools->tools_json().size()));
    ASSERT_TRUE(parsed);
    size_t checked = 0;
    for (const Value& t : parsed.value().as_list()) {
        const std::string& wire = t.find("function")->find("name")->as_string();
        const wisp::agent::ToolDef* def = tools->find_wire(wire);
        ASSERT_NE(def, nullptr) << wire;
        const std::string dotted(def->name.data(), def->name.size());
        for (wisp::ui::Lang lang : wisp::ui::kAllLangs) {
            const auto label = wisp::shell::activity_label(dotted, "", lang);
            EXPECT_EQ(std::string_view(label.data(), label.size()).find(dotted),
                      std::string_view::npos)
                << dotted << " has no label";
        }
        ++checked;
    }
    EXPECT_GT(checked, 30u);
}

TEST(AppAgent, HeadlessHasNoAgent) {
    UiNetHarness h(with_server(false));
    ASSERT_TRUE(h.start());
    EXPECT_EQ(h.app->agent(), nullptr);
    EXPECT_EQ(h.app->agent_tools(), nullptr);
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
