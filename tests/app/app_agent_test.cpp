// The agent wired into the App (agent-core spec 2, 7.1): built and started
// with a screen and a scene runtime, reachable over the admin routes, and
// absent (503) without a screen.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <functional>
#include <memory>
#include <span>
#include <string>

#include "app/ui_net_harness.hpp"
#include "support/fake_http.hpp"
#include "support/wait.hpp"
#include "wisp/agent/agent_service.hpp"
#include "wisp/agent/tool_registry.hpp"
#include "wisp/app/power_manager.hpp"
#include "wisp/base/json.hpp"
#include "wisp/core/log.hpp"
#include "wisp/net/pairing_service.hpp"
#include "wisp/os/os.hpp"
#include "wisp/port/sim/sim_haptic.hpp"
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

// The clock the haptic and the device service read; the test moves it.
struct ManualClock {
    std::atomic<uint64_t> ms{1000};
    std::function<uint64_t()> fn() {
        return [this] { return ms.load(); };
    }
};

// An App with a screen and the simulator's haptic motor registered as a
// board would register it (one line, no other wiring), both on `clock`.
UiNetOptions with_haptic(ManualClock& clock) {
    UiNetOptions o = with_server(true);
    o.extra_devices = {
        std::make_shared<wisp::port::sim::SimHaptic>("haptic.main", "main", clock.fn())};
    o.device_clock = clock.fn();
    return o;
}

// Calls tool `wire` of the App's agent the way a turn does, with the
// result parsed whether the tool built it as a Value or as JSON text.
wisp::agent::ToolOutput call_tool(UiNetHarness& h, const char* wire, const Value& args) {
    wisp::agent::ToolCtx ctx;
    ctx.cancelled = [] { return false; };
    ctx.lang = "zh";
    wisp::agent::ToolOutput out = h.app->agent_tools()->call(wire, wisp::to_json(args), ctx);
    if (!out.result_json.empty()) {
        auto parsed =
            wisp::from_json(std::string_view(out.result_json.data(), out.result_json.size()));
        EXPECT_TRUE(parsed) << std::string(out.result_json.data(), out.result_json.size());
        if (parsed) out.result = std::move(parsed.value());
    }
    return out;
}

bool ok_of(const wisp::agent::ToolOutput& out) {
    const Value* f = out.result.find("ok");
    return f != nullptr && f->type() == Value::Type::Bool && f->as_bool();
}

std::string str_of(const Value& v, std::string_view key) {
    const Value* f = v.find(key);
    return f != nullptr && f->type() == Value::Type::String ? f->as_string() : std::string();
}

Value pulse_args() {
    return Value(wisp::Map{{"id", Value("haptic.main")},
                           {"op", Value("invoke")},
                           {"name", Value("pulse")},
                           {"args", Value(wisp::Map{{"ms", Value(int64_t{150})}})}});
}

size_t count_of(const std::string& text, const std::string& needle) {
    size_t n = 0;
    for (size_t at = text.find(needle); at != std::string::npos;
         at = text.find(needle, at + needle.size())) {
        ++n;
    }
    return n;
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
    ManualClock clock;
    UiNetHarness h(with_haptic(clock));
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

// On a board without a device that describes itself the two device tools
// are not offered (they could only ever list nothing), and every other
// tool is exactly what a board with such a device gets.
TEST(AppAgent, DeviceToolsAreAbsentWithoutADescribedDevice) {
    auto tool_list = [](const UiNetOptions& opts) {
        UiNetHarness h(opts);
        EXPECT_TRUE(h.start());
        const wisp::agent::ToolRegistry* tools = h.app->agent_tools();
        EXPECT_NE(tools, nullptr);
        if (tools == nullptr) return wisp::List{};
        auto parsed = wisp::from_json(
            std::string_view(tools->tools_json().data(), tools->tools_json().size()));
        EXPECT_TRUE(parsed);
        return parsed ? parsed.value().as_list() : wisp::List{};
    };
    ManualClock clock;
    wisp::List with_device = tool_list(with_haptic(clock));
    const wisp::List without = tool_list(with_server(true));
    auto wire_of = [](const Value& t) { return t.find("function")->find("name")->as_string(); };
    for (const Value& t : without) {
        EXPECT_NE(wire_of(t), "device_list");
        EXPECT_NE(wire_of(t), "device_call");
    }
    const size_t before = with_device.size();
    std::erase_if(with_device, [&](const Value& t) {
        return wire_of(t) == "device_list" || wire_of(t) == "device_call";
    });
    EXPECT_EQ(before - with_device.size(), 2u);
    EXPECT_TRUE(wisp::to_json(Value(with_device)) == wisp::to_json(Value(without)));
}

// Every tool the wired agent registers has an activity label of its own in
// every language, so a new tool cannot show "Call <tool>" to the user. The
// App has a described device, so the device tools are covered too.
TEST(AppAgent, EveryRegisteredToolHasAnActivityLabel) {
    ManualClock clock;
    UiNetHarness h(with_haptic(clock));
    ASSERT_TRUE(h.start());
    const wisp::agent::ToolRegistry* tools = h.app->agent_tools();
    ASSERT_NE(tools, nullptr);
    ASSERT_NE(tools->find_wire("device_list"), nullptr);
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

// Device model spec 5: a device no core service knows, added by one board
// line, reaches the agent's device tools and, through lua.run, the
// scenes' wisp.device -- one DeviceService behind both, so the command's
// interval holds across them and both calls leave the same log line.
TEST(AppAgent, HapticIsReachableFromTheAgentAndFromLua) {
    wisp::log::reset_for_test();
    wisp::log::init();
    ManualClock clock;
    UiNetHarness h(with_haptic(clock));
    ASSERT_TRUE(h.start());
    ASSERT_NE(h.app->agent_tools(), nullptr);

    wisp::agent::ToolOutput listed = call_tool(h, "device_list", Value(wisp::Map{}));
    ASSERT_TRUE(ok_of(listed)) << wisp::to_json(listed.result);
    const Value* devices = listed.result.find("devices");
    ASSERT_NE(devices, nullptr);
    ASSERT_EQ(devices->as_list().size(), 1u) << wisp::to_json(listed.result);
    EXPECT_EQ(str_of(devices->as_list()[0], "id"), "haptic.main");
    EXPECT_EQ(str_of(devices->as_list()[0], "class"), "other");
    EXPECT_EQ(str_of(devices->as_list()[0], "role"), "main");

    wisp::agent::ToolOutput pulsed = call_tool(h, "device_call", pulse_args());
    ASSERT_TRUE(ok_of(pulsed)) << wisp::to_json(pulsed.result);
    EXPECT_EQ(pulsed.note, "设备 haptic.main：pulse");
    wisp::agent::ToolOutput active = call_tool(
        h, "device_call",
        Value(wisp::Map{
            {"id", Value("haptic.main")}, {"op", Value("get")}, {"name", Value("active")}}));
    ASSERT_TRUE(ok_of(active)) << wisp::to_json(active.result);
    ASSERT_NE(active.result.find("value"), nullptr);
    EXPECT_TRUE(active.result.find("value")->as_bool());

    // pulse's min_interval_ms (100) counts every caller; past it, the
    // agent's Lua VM (which holds every device, as holder "agent") pulses.
    clock.ms += 150;
    wisp::agent::ToolOutput ran = call_tool(
        h, "lua_run",
        Value(wisp::Map{
            {"code", Value("return wisp.device.call('haptic.main', 'pulse', {ms = 150})")}}));
    ASSERT_TRUE(ok_of(ran)) << wisp::to_json(ran.result);
    EXPECT_EQ(str_of(ran.result, "result"), R"(<data source="lua">true</data>)");

    const std::string log = wisp::log::ring_snapshot();
    EXPECT_EQ(count_of(log, "device haptic.main.pulse by agent"), 2u) << log;
    EXPECT_EQ(count_of(log, "haptic pulse 150 ms 80%"), 2u) << log;
    wisp::log::reset_for_test();
}

// The haptic declares caps standby "off": with the screen off a command
// is refused (busy) without waking it, and works again once awake.
TEST(AppAgent, HapticIsOffWhileTheScreenIsOff) {
    ManualClock clock;
    UiNetHarness h(with_haptic(clock));
    ASSERT_TRUE(h.start());
    wisp::app::PowerManager* pm = h.app->power_manager();
    ASSERT_NE(pm, nullptr);

    h.app->ui()->run_blocking([&] { pm->enter_standby(); });
    ASSERT_TRUE(pm->standby());
    h.app->bus().wait_idle(2000);
    wisp::agent::ToolOutput off = call_tool(h, "device_call", pulse_args());
    EXPECT_FALSE(ok_of(off));
    EXPECT_EQ(str_of(off.result, "error"), "busy") << wisp::to_json(off.result);
    // The service's own refusal, not the motor's: the App fed it power.state.
    EXPECT_NE(str_of(off.result, "message").find("is off while the screen is off"),
              std::string::npos)
        << wisp::to_json(off.result);
    EXPECT_TRUE(off.note.empty());
    EXPECT_TRUE(pm->standby());

    h.app->ui()->run_blocking([&] { pm->exit_standby(); });
    h.app->bus().wait_idle(2000);
    wisp::agent::ToolOutput on = call_tool(h, "device_call", pulse_args());
    EXPECT_TRUE(ok_of(on)) << wisp::to_json(on.result);
}

// device.status lists each medium with whether it is the removable one, so
// the agent can tell the card from the internal flash without its id.
TEST(AppAgent, DeviceStatusSaysWhichMediumIsRemovable) {
    UiNetOptions opts = with_server(true);
    opts.extra_devices = {std::make_shared<wisp::test::FakeStorage>(
        "storage.card", wisp::test::temp_dir().string(), /*removable=*/true)};
    UiNetHarness h(opts);
    ASSERT_TRUE(h.start());
    // The readout is refreshed on the storage worker after each call.
    Value media;
    ASSERT_TRUE(wait_until(
        [&] {
            wisp::agent::ToolOutput out = call_tool(h, "device_status", Value(wisp::Map{}));
            const Value* storage = out.result.find("storage");
            if (storage == nullptr || storage->type() != Value::Type::List ||
                storage->as_list().size() < 2) {
                return false;
            }
            media = *storage;
            return true;
        },
        3000));
    ASSERT_EQ(media.as_list().size(), 2u) << wisp::to_json(media);
    for (const Value& m : media.as_list()) {
        const Value* removable = m.find("removable");
        ASSERT_NE(removable, nullptr) << wisp::to_json(m);
        ASSERT_EQ(removable->type(), Value::Type::Bool) << wisp::to_json(m);
        EXPECT_EQ(removable->as_bool(), str_of(m, "id") == "storage.card") << wisp::to_json(m);
    }
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
