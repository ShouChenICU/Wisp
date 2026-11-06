// AgentService (wisp/agent/agent_service.hpp): the conversation loop
// against the scripted FakeLlmServer, a real storage worker and stand-in
// tools.

#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "support/agent_fixture.hpp"
#include "support/wait.hpp"
#include "wisp/agent/agent_service.hpp"
#include "wisp/agent/limits.hpp"
#include "wisp/base/json.hpp"
#include "wisp/core/log.hpp"
#include "wisp/ui/strings.hpp"

using wisp::ErrorCode;
using wisp::List;
using wisp::Map;
using wisp::Value;
using wisp::agent::AgentConfig;
using wisp::agent::AgentEvent;
using wisp::agent::AgentService;
using wisp::agent::AgentState;
using wisp::agent::ConversationStore;
using wisp::agent::History;
using wisp::agent::Message;
using wisp::agent::Role;
using wisp::agent::Source;
using wisp::agent::ToolCtx;
using wisp::agent::ToolDef;
using wisp::agent::ToolOutput;
using wisp::test::AgentHarness;
using wisp::test::FakeLlmServer;
using wisp::test::FakeReply;
using wisp::test::wait_until;
using Kind = AgentEvent::Kind;

namespace {

std::string str(const wisp::os::LargeString& s) { return std::string(s.data(), s.size()); }

FakeReply text_reply(const std::string& text) {
    return {.status = 200,
            .chunks = {FakeLlmServer::sse_text(text), FakeLlmServer::sse_finish("stop"),
                       FakeLlmServer::sse_done()}};
}

FakeReply tool_reply(const std::string& id, const std::string& wire, const std::string& args,
                     const std::string& finish = "tool_calls") {
    return {.status = 200,
            .chunks = {FakeLlmServer::sse_tool(0, id, wire, args),
                       FakeLlmServer::sse_finish(finish), FakeLlmServer::sse_done()}};
}

class AgentLoop : public ::testing::Test {
protected:
    void SetUp() override {
        wisp::log::reset_for_test();
        wisp::log::init();
    }

    void TearDown() override {
        h->server.release();
        svc.reset();
        h.reset();
        wisp::log::reset_for_test();
    }

    void make_harness(bool with_model = true) { h = std::make_unique<AgentHarness>(with_model); }

    AgentHarness& harness() {
        if (!h) make_harness();
        return *h;
    }

    // Records every tool call by dotted name.
    void add_tool(const std::string& name,
                  std::function<ToolOutput(const Value&, ToolCtx&)> run = nullptr) {
        ToolDef def;
        def.name = name;
        def.description = "test tool";
        def.run = [this, name, run](const Value& args, ToolCtx& ctx) {
            {
                std::lock_guard<std::mutex> lock(mu);
                calls.push_back(name);
            }
            if (run) return run(args, ctx);
            return ToolOutput{.ok = true, .result = Map{{"ok", true}}};
        };
        ASSERT_TRUE(harness().tools->add(std::move(def)));
    }

    void start(AgentConfig cfg = {}) {
        harness();
        svc = std::make_unique<AgentService>(h->deps(), cfg);
        listener = svc->add_listener([this](const AgentEvent& e) {
            std::lock_guard<std::mutex> lock(mu);
            events.push_back(e);
        });
        ASSERT_TRUE(svc->start());
    }

    size_t count(Kind k) {
        std::lock_guard<std::mutex> lock(mu);
        size_t n = 0;
        for (const auto& e : events) n += e.kind == k ? 1 : 0;
        return n;
    }

    bool wait_turns(size_t n, uint32_t ms = 5000) {
        return wait_until([&] { return count(Kind::TurnEnd) >= n; }, ms);
    }

    std::vector<AgentEvent> snapshot() {
        std::lock_guard<std::mutex> lock(mu);
        return events;
    }

    std::vector<std::string> tool_calls() {
        std::lock_guard<std::mutex> lock(mu);
        return calls;
    }

    std::vector<AgentState> states() {
        std::vector<AgentState> out;
        for (const auto& e : snapshot())
            if (e.kind == Kind::State) out.push_back(e.state);
        return out;
    }

    std::string last_status() {
        auto ev = snapshot();
        for (auto it = ev.rbegin(); it != ev.rend(); ++it)
            if (it->kind == Kind::TurnEnd) return it->status;
        return {};
    }

    History history() { return *h->store->history(); }

    std::vector<std::string> notes() {
        std::vector<std::string> out;
        for (const auto& m : history())
            if (m.role == Role::Note) out.push_back(str(m.text));
        return out;
    }

    // A copy: the history passed in is often a temporary.
    std::optional<Message> last_of(const History& all, Role r) {
        for (auto it = all.rbegin(); it != all.rend(); ++it)
            if (it->role == r) return *it;
        return std::nullopt;
    }

    std::unique_ptr<AgentHarness> h;
    std::unique_ptr<AgentService> svc;
    uint32_t listener = 0;
    std::mutex mu;
    std::vector<AgentEvent> events;
    std::vector<std::string> calls;
};

TEST_F(AgentLoop, PlainReplyEndsTurn) {
    start();
    h->server.push(text_reply("hi"));
    ASSERT_TRUE(svc->submit("hello", Source::Web));
    ASSERT_TRUE(wait_turns(1));

    History all = history();
    ASSERT_EQ(all.size(), 4u);
    EXPECT_EQ(all[0].role, Role::Marker);
    EXPECT_EQ(str(all[0].marker), "turn_begin");
    EXPECT_EQ(all[1].role, Role::User);
    EXPECT_EQ(str(all[1].text), "hello");
    EXPECT_EQ(all[2].role, Role::Assistant);
    EXPECT_EQ(str(all[2].text), "hi");
    EXPECT_EQ(all[3].role, Role::Marker);
    EXPECT_EQ(str(all[3].marker), "turn_end");
    EXPECT_EQ(str(all[3].status), "done");
    for (const auto& m : all) EXPECT_EQ(m.turn, 1u);
    EXPECT_EQ(states(), (std::vector<AgentState>{AgentState::Thinking, AgentState::Output,
                                                 AgentState::Idle}));
    EXPECT_EQ(svc->state(), AgentState::Idle);
    // The request carried the user message.
    Value body = h->server.body_json(0);
    const List& msgs = body.find("messages")->as_list();
    ASSERT_EQ(msgs.size(), 1u);
    EXPECT_EQ(msgs[0].find("role")->as_string(), "user");
    EXPECT_EQ(msgs[0].find("content")->as_string(), "hello");
}

TEST_F(AgentLoop, SubmitRecordsSource) {
    start();
    h->server.push(text_reply("ok"));
    ASSERT_TRUE(svc->submit("hi", Source::Voice));
    ASSERT_TRUE(wait_turns(1));
    std::optional<Message> u = last_of(history(), Role::User);
    ASSERT_TRUE(u.has_value());
    EXPECT_EQ(u->source, Source::Voice);
}

TEST_F(AgentLoop, TurnBeginAndEndFrameTheTurn) {
    start();
    h->server.push(text_reply("ok"));
    ASSERT_TRUE(svc->submit("hi", Source::Voice));
    ASSERT_TRUE(wait_turns(1));
    auto ev = snapshot();
    ASSERT_GE(ev.size(), 2u);
    EXPECT_EQ(ev.front().kind, Kind::TurnBegin);
    EXPECT_EQ(ev.front().turn, 1u);
    EXPECT_EQ(ev.front().source, Source::Voice);
    EXPECT_EQ(ev.front().text, "hi");
    EXPECT_EQ(ev.back().kind, Kind::TurnEnd);
    EXPECT_EQ(ev.back().turn, 1u);
    EXPECT_EQ(ev.back().status, "done");
}

TEST_F(AgentLoop, EveryListenerSeesEveryEvent) {
    start();
    std::mutex mu2;
    std::vector<AgentEvent> second;
    uint32_t id2 = svc->add_listener([&](const AgentEvent& e) {
        std::lock_guard<std::mutex> lock(mu2);
        second.push_back(e);
    });
    h->server.push(text_reply("one"));
    ASSERT_TRUE(svc->submit("a", Source::Web));
    ASSERT_TRUE(wait_turns(1));
    {
        auto first = snapshot();
        std::lock_guard<std::mutex> lock(mu2);
        ASSERT_EQ(first.size(), second.size());
        for (size_t i = 0; i < first.size(); ++i) {
            EXPECT_EQ(first[i].kind, second[i].kind) << i;
            EXPECT_EQ(first[i].seq, second[i].seq) << i;
            EXPECT_EQ(first[i].text, second[i].text) << i;
        }
    }
    svc->remove_listener(id2);
    size_t before = 0;
    {
        std::lock_guard<std::mutex> lock(mu2);
        before = second.size();
    }
    h->server.push(text_reply("two"));
    ASSERT_TRUE(svc->submit("b", Source::Web));
    ASSERT_TRUE(wait_turns(2));
    std::lock_guard<std::mutex> lock(mu2);
    EXPECT_EQ(second.size(), before);
}

TEST_F(AgentLoop, RunsToolCallsInOrderThenAnswers) {
    add_tool("scene.list");
    add_tool("scene.read");
    start();
    h->server.push(
        {.status = 200,
         .chunks = {FakeLlmServer::sse_tool(0, "c1", "scene_list", "{}"),
                    FakeLlmServer::sse_tool(1, "c2", "scene_read", "{\"id\":\"a\"}"),
                    FakeLlmServer::sse_finish("tool_calls"), FakeLlmServer::sse_done()}});
    h->server.push(text_reply("done"));
    ASSERT_TRUE(svc->submit("go", Source::Web));
    ASSERT_TRUE(wait_turns(1));

    EXPECT_EQ(tool_calls(), (std::vector<std::string>{"scene.list", "scene.read"}));
    ASSERT_EQ(h->server.requests().size(), 2u);
    Value body = h->server.body_json(1);
    const List& msgs = body.find("messages")->as_list();
    ASSERT_EQ(msgs.size(), 4u);
    EXPECT_EQ(msgs[1].find("role")->as_string(), "assistant");
    const List& tc = msgs[1].find("tool_calls")->as_list();
    ASSERT_EQ(tc.size(), 2u);
    EXPECT_EQ(tc[0].find("id")->as_string(), "c1");
    EXPECT_EQ(tc[0].find("function")->find("name")->as_string(), "scene_list");
    EXPECT_EQ(tc[1].find("id")->as_string(), "c2");
    EXPECT_EQ(msgs[2].find("role")->as_string(), "tool");
    EXPECT_EQ(msgs[2].find("tool_call_id")->as_string(), "c1");
    EXPECT_EQ(msgs[3].find("tool_call_id")->as_string(), "c2");
    EXPECT_EQ(last_status(), "done");

    // Stored with dotted names.
    History all = history();
    const Message* a = nullptr;
    for (const auto& m : all)
        if (m.role == Role::Assistant && !m.calls.empty()) a = &m;
    ASSERT_NE(a, nullptr);
    EXPECT_EQ(str(a->calls[1].name), "scene.read");
    std::optional<Message> t = last_of(all, Role::Tool);
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(str(t->tool), "scene.read");
    EXPECT_EQ(str(t->call_id), "c2");
    EXPECT_TRUE(t->ok);
}

TEST_F(AgentLoop, StopsAfter24ToolCalls) {
    add_tool("scene.list");
    start();
    for (uint32_t i = 0; i <= wisp::agent::kMaxSteps; ++i)
        h->server.push(tool_reply("call_" + std::to_string(i), "scene_list", "{}"));
    ASSERT_TRUE(svc->submit("loop", Source::Web));
    ASSERT_TRUE(wait_turns(1, 10000));
    EXPECT_EQ(tool_calls().size(), wisp::agent::kMaxSteps);
    EXPECT_EQ(last_status(), "step_limit");
    auto n = notes();
    ASSERT_FALSE(n.empty());
    EXPECT_EQ(n.back(), "步骤太多，已停止");
    // The call not run still has its answer, so the next request is valid.
    std::optional<Message> t = last_of(history(), Role::Tool);
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(str(t->call_id), "call_24");
    EXPECT_FALSE(t->ok);
}

TEST_F(AgentLoop, StopsAfterTenMinutes) {
    add_tool("scene.list", [this](const Value&, ToolCtx&) {
        h->advance(wisp::agent::kTurnMs + 1);
        return ToolOutput{.ok = true, .result = Map{{"ok", true}}};
    });
    start();
    h->server.push(tool_reply("c1", "scene_list", "{}"));
    h->server.push(text_reply("never"));
    ASSERT_TRUE(svc->submit("slow", Source::Web));
    ASSERT_TRUE(wait_turns(1));
    EXPECT_EQ(last_status(), "time_limit");
    EXPECT_EQ(h->server.requests().size(), 1u);
    auto n = notes();
    ASSERT_FALSE(n.empty());
    EXPECT_NE(n.back().find("10"), std::string::npos);
}

TEST_F(AgentLoop, TimeLimitDuringRequestEndsAsTimeLimit) {
    start({.turn_ms = 300});
    h->server.push({.status = 200, .chunks = {FakeLlmServer::sse_text("Hel")}, .hold_after = 1});
    ASSERT_TRUE(svc->submit("slow", Source::Web));
    ASSERT_TRUE(wait_turns(1, 3000));
    EXPECT_EQ(h->server.held(), 0);
    EXPECT_EQ(last_status(), "time_limit");
    auto n = notes();
    ASSERT_EQ(n.size(), 1u);
    EXPECT_EQ(n[0], wisp::ui::tr(wisp::ui::StrId::AgentTimeLimit, wisp::ui::Lang::Zh));
}

TEST_F(AgentLoop, GeneratedCallIdsAreUniqueAcrossTurns) {
    add_tool("scene.list");
    start();
    for (int turn = 0; turn < 2; ++turn) {
        // No id from the model: the service makes one up.
        h->server.push(tool_reply("", "scene_list", "{}"));
        h->server.push(text_reply("ok"));
    }
    ASSERT_TRUE(svc->submit("one", Source::Web));
    ASSERT_TRUE(wait_turns(1));
    ASSERT_TRUE(svc->submit("two", Source::Web));
    ASSERT_TRUE(wait_turns(2));
    ASSERT_EQ(h->server.requests().size(), 4u);
    Value body = h->server.body_json(3);
    std::vector<std::string> asked, answered;
    for (const Value& m : body.find("messages")->as_list()) {
        if (const Value* tc = m.find("tool_calls"))
            for (const Value& c : tc->as_list()) asked.push_back(c.find("id")->as_string());
        if (const Value* id = m.find("tool_call_id")) answered.push_back(id->as_string());
    }
    ASSERT_EQ(asked.size(), 2u);
    EXPECT_NE(asked[0], asked[1]);
    EXPECT_EQ(answered, asked);
}

TEST_F(AgentLoop, FailedStartCanBeRetried) {
    harness();
    std::atomic<bool> refuse{true};
    auto deps = h->deps();
    deps.run_io = [&](std::function<void()> fn) {
        if (refuse.load()) return wisp::Result<void>::err(wisp::ErrorCode::Busy, "refused");
        return h->worker.run_sync(std::move(fn), wisp::os::kForever);
    };
    svc = std::make_unique<AgentService>(std::move(deps));
    listener = svc->add_listener([this](const AgentEvent& e) {
        std::lock_guard<std::mutex> lock(mu);
        events.push_back(e);
    });
    EXPECT_FALSE(svc->start());
    refuse = false;
    ASSERT_TRUE(svc->start());
    h->server.push(text_reply("ok"));
    ASSERT_TRUE(svc->submit("hi", Source::Web));
    ASSERT_TRUE(wait_turns(1));
    EXPECT_EQ(last_status(), "done");
}

TEST_F(AgentLoop, StopDuringStreamAborts) {
    start();
    h->server.push({.status = 200, .chunks = {FakeLlmServer::sse_text("Hel")}, .hold_after = 1});
    ASSERT_TRUE(svc->submit("go", Source::Web));
    ASSERT_TRUE(wait_until([&] { return h->server.held() == 1; }, 2000));
    const uint64_t t0 = wisp::os::now_ms();
    svc->stop_turn();
    ASSERT_TRUE(wait_until([&] { return h->server.held() == 0; }, 1000));
    EXPECT_LT(wisp::os::now_ms() - t0, 200u);
    ASSERT_TRUE(wait_turns(1));
    EXPECT_EQ(last_status(), "stopped");
    History all = history();
    std::optional<Message> a = last_of(all, Role::Assistant);
    ASSERT_TRUE(a.has_value());
    EXPECT_EQ(str(a->text), "Hel");
    auto n = notes();
    ASSERT_FALSE(n.empty());
    EXPECT_EQ(n.back(), "已中止");
    EXPECT_EQ(h->server.requests().size(), 1u);
}

TEST_F(AgentLoop, StopDuringToolCancelsIt) {
    std::atomic<bool> running{false}, saw_cancel{false};
    add_tool("scene.list", [&](const Value&, ToolCtx& ctx) {
        running = true;
        const uint64_t until = wisp::os::now_ms() + 3000;
        while (wisp::os::now_ms() < until) {
            if (ctx.cancelled && ctx.cancelled()) {
                saw_cancel = true;
                break;
            }
            wisp::os::sleep_ms(5);
        }
        return ToolOutput{.ok = true, .result = Map{{"ok", true}}};
    });
    start();
    h->server.push(tool_reply("c1", "scene_list", "{}"));
    h->server.push(text_reply("never"));
    ASSERT_TRUE(svc->submit("go", Source::Web));
    ASSERT_TRUE(wait_until([&] { return running.load(); }, 2000));
    svc->stop_turn();
    ASSERT_TRUE(wait_turns(1));
    EXPECT_TRUE(saw_cancel.load());
    EXPECT_EQ(last_status(), "stopped");
    EXPECT_EQ(h->server.requests().size(), 1u);
}

TEST_F(AgentLoop, QueuesOneMessageAndRefusesMore) {
    start();
    FakeReply a = text_reply("A");
    a.hold_after = 0;
    h->server.push(a);
    h->server.push(text_reply("B"));
    ASSERT_TRUE(svc->submit("first", Source::Web));
    ASSERT_TRUE(wait_until([&] { return h->server.held() == 1; }, 2000));
    ASSERT_TRUE(svc->submit("second", Source::Web));
    EXPECT_TRUE(svc->view().queued);
    auto c = svc->submit("third", Source::Web);
    ASSERT_FALSE(c);
    EXPECT_EQ(c.error().code, ErrorCode::Busy);
    EXPECT_EQ(c.error().message, "busy");
    h->server.release();
    ASSERT_TRUE(wait_turns(2));
    auto ev = snapshot();
    std::vector<std::pair<Kind, std::string>> frame;
    for (const auto& e : ev) {
        if (e.kind == Kind::TurnBegin) frame.emplace_back(e.kind, e.text);
        if (e.kind == Kind::TurnEnd) frame.emplace_back(e.kind, e.status);
    }
    EXPECT_EQ(frame, (std::vector<std::pair<Kind, std::string>>{{Kind::TurnBegin, "first"},
                                                                {Kind::TurnEnd, "done"},
                                                                {Kind::TurnBegin, "second"},
                                                                {Kind::TurnEnd, "done"}}));
    EXPECT_EQ(h->server.max_concurrent(), 1);
}

TEST_F(AgentLoop, StopTurnDropsTheQueuedMessage) {
    start();
    h->server.push({.status = 200, .chunks = {FakeLlmServer::sse_text("x")}, .hold_after = 1});
    ASSERT_TRUE(svc->submit("first", Source::Web));
    ASSERT_TRUE(wait_until([&] { return h->server.held() == 1; }, 2000));
    ASSERT_TRUE(svc->submit("second", Source::Web));
    svc->stop_turn();
    ASSERT_TRUE(wait_turns(1));
    wisp::os::sleep_ms(100);
    EXPECT_EQ(count(Kind::TurnBegin), 1u);
    EXPECT_FALSE(svc->view().queued);
}

TEST_F(AgentLoop, RejectsEmptyAndOverlongMessages) {
    start();
    auto e = svc->submit("", Source::Web);
    ASSERT_FALSE(e);
    EXPECT_EQ(e.error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(e.error().message, "empty");
    std::string han;
    for (size_t i = 0; i < wisp::agent::kMaxUserChars; ++i) han += "汉";
    auto t = svc->submit(han + "汉", Source::Web);
    ASSERT_FALSE(t);
    EXPECT_EQ(t.error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(t.error().message, "too_long");
    h->server.push(text_reply("ok"));
    EXPECT_TRUE(svc->submit(han, Source::Web));
    ASSERT_TRUE(wait_turns(1));
}

TEST_F(AgentLoop, NoModelExplainsInConversation) {
    make_harness(/*with_model=*/false);
    start();
    ASSERT_TRUE(svc->submit("hi", Source::Web));
    ASSERT_TRUE(wait_turns(1));
    EXPECT_EQ(h->server.requests().size(), 0u);
    EXPECT_EQ(last_status(), "error");
    auto n = notes();
    ASSERT_EQ(n.size(), 1u);
    EXPECT_NE(n[0].find("“模型”"), std::string::npos) << n[0];
    auto s = states();
    ASSERT_GE(s.size(), 2u);
    EXPECT_EQ(s[s.size() - 2], AgentState::Error);
    EXPECT_EQ(s.back(), AgentState::Idle);
}

TEST_F(AgentLoop, NetworkDownExplainsInConversation) {
    harness().network_up = false;
    start();
    ASSERT_TRUE(svc->submit("hi", Source::Web));
    ASSERT_TRUE(wait_turns(1));
    EXPECT_EQ(h->server.requests().size(), 0u);
    EXPECT_EQ(last_status(), "error");
    auto n = notes();
    ASSERT_EQ(n.size(), 1u);
    EXPECT_NE(n[0].find("网络"), std::string::npos) << n[0];
}

TEST_F(AgentLoop, UnauthorizedTellsUserToCheckModel) {
    start();
    h->server.push({.status = 401, .chunks = {"{\"error\":\"bad key sk-test\"}"}});
    ASSERT_TRUE(svc->submit("hi", Source::Web));
    ASSERT_TRUE(wait_turns(1));
    EXPECT_EQ(h->server.requests().size(), 1u);
    EXPECT_EQ(last_status(), "error");
    auto n = notes();
    ASSERT_EQ(n.size(), 1u);
    EXPECT_NE(n[0].find("API Key"), std::string::npos) << n[0];
    EXPECT_NE(n[0].find("“模型”"), std::string::npos) << n[0];
}

TEST_F(AgentLoop, NotesFollowTheDeviceLanguage) {
    harness().set_lang("en");
    h->network_up = false;
    start();
    ASSERT_TRUE(svc->submit("hi", Source::Web));
    ASSERT_TRUE(wait_turns(1));
    auto n = notes();
    ASSERT_EQ(n.size(), 1u);
    EXPECT_NE(n[0].find("network"), std::string::npos) << n[0];
}

TEST_F(AgentLoop, MalformedToolArgumentsGoBackAsBadArgs) {
    add_tool("scene.read");
    start();
    h->server.push(tool_reply("c1", "scene_read", "{\"id\":"));
    h->server.push(text_reply("sorry"));
    ASSERT_TRUE(svc->submit("go", Source::Web));
    ASSERT_TRUE(wait_turns(1));
    EXPECT_TRUE(tool_calls().empty());
    std::optional<Message> t = last_of(history(), Role::Tool);
    ASSERT_TRUE(t.has_value());
    EXPECT_FALSE(t->ok);
    auto r = wisp::from_json(str(t->text));
    ASSERT_TRUE(r);
    EXPECT_EQ(r.value().find("error")->as_string(), "bad_args");
    EXPECT_EQ(h->server.requests().size(), 2u);
    EXPECT_EQ(last_status(), "done");
}

TEST_F(AgentLoop, LengthCutoffWithPartialToolCallIsBadArgs) {
    add_tool("scene.read");
    start();
    h->server.push(tool_reply("c1", "scene_read", "{\"path\":\"ma", "length"));
    h->server.push(text_reply("sorry"));
    ASSERT_TRUE(svc->submit("go", Source::Web));
    ASSERT_TRUE(wait_turns(1));
    EXPECT_TRUE(tool_calls().empty());
    std::optional<Message> t = last_of(history(), Role::Tool);
    ASSERT_TRUE(t.has_value());
    EXPECT_FALSE(t->ok);
    auto r = wisp::from_json(str(t->text));
    ASSERT_TRUE(r);
    EXPECT_EQ(r.value().find("error")->as_string(), "bad_args");
    EXPECT_EQ(h->server.requests().size(), 2u);
    EXPECT_EQ(last_status(), "done");
}

TEST_F(AgentLoop, WifiLossMidStreamEndsTurnCleanly) {
    start();
    h->server.push({.status = 200, .chunks = {FakeLlmServer::sse_text("Hel")}, .hold_after = 1});
    ASSERT_TRUE(svc->submit("go", Source::Web));
    ASSERT_TRUE(wait_until([&] { return h->server.held() == 1; }, 2000));
    const uint64_t t0 = wisp::os::now_ms();
    h->bus.publish({"net.wifi.state", "net.wifi", 0, Map{{"state", "backoff"}}});
    ASSERT_TRUE(wait_turns(1, 2000));
    EXPECT_LE(wisp::os::now_ms() - t0, 2000u);
    EXPECT_EQ(last_status(), "error");
    auto n = notes();
    ASSERT_FALSE(n.empty());
    EXPECT_EQ(n.back(), "网络中断");
    std::optional<Message> a = last_of(history(), Role::Assistant);
    ASSERT_TRUE(a.has_value());
    EXPECT_EQ(str(a->text), "Hel");
    EXPECT_TRUE(h->locks->list().empty());
    EXPECT_EQ(svc->state(), AgentState::Idle);

    // While the network is down no request is made.
    ASSERT_TRUE(svc->submit("again", Source::Web));
    ASSERT_TRUE(wait_turns(2));
    EXPECT_EQ(h->server.requests().size(), 1u);

    h->bus.publish({"net.wifi.state", "net.wifi", 0, Map{{"state", "connected"}}});
    ASSERT_TRUE(h->bus.wait_idle(1000));
    h->server.push(text_reply("back"));
    ASSERT_TRUE(svc->submit("third", Source::Web));
    ASSERT_TRUE(wait_turns(3));
    EXPECT_EQ(last_status(), "done");
}

TEST_F(AgentLoop, StalledStreamTimesOut) {
    start({.stall_ms = 200});
    h->server.push({.status = 200, .chunks = {FakeLlmServer::sse_text("Hel")}, .hold_after = 1});
    ASSERT_TRUE(svc->submit("go", Source::Web));
    ASSERT_TRUE(wait_turns(1, 3000));
    EXPECT_EQ(h->server.held(), 0);
    EXPECT_EQ(last_status(), "error");
    auto n = notes();
    ASSERT_FALSE(n.empty());
    EXPECT_EQ(n.back(), "网络中断");
}

TEST_F(AgentLoop, TransportErrorAfterFinishCountsAsDone) {
    start();
    // The connection drops after finish_reason, before [DONE]: the reply
    // is whole.
    h->server.push({.status = 200,
                    .chunks = {FakeLlmServer::sse_text("whole"), FakeLlmServer::sse_finish("stop")},
                    .fail = wisp::net::TransportError::Timeout});
    ASSERT_TRUE(svc->submit("go", Source::Web));
    ASSERT_TRUE(wait_turns(1));
    EXPECT_EQ(last_status(), "done");
    EXPECT_TRUE(notes().empty());
    std::optional<Message> a = last_of(history(), Role::Assistant);
    ASSERT_TRUE(a.has_value());
    EXPECT_EQ(str(a->text), "whole");
}

TEST_F(AgentLoop, TransportErrorMidStreamKeepsTextAndExplains) {
    start();
    h->server.push({.status = 200,
                    .chunks = {FakeLlmServer::sse_text("half")},
                    .fail = wisp::net::TransportError::Timeout});
    ASSERT_TRUE(svc->submit("go", Source::Web));
    ASSERT_TRUE(wait_turns(1));
    EXPECT_EQ(last_status(), "error");
    std::optional<Message> a = last_of(history(), Role::Assistant);
    ASSERT_TRUE(a.has_value());
    EXPECT_EQ(str(a->text), "half");
    auto n = notes();
    ASSERT_EQ(n.size(), 1u);
    EXPECT_EQ(n[0], "网络中断");
}

TEST_F(AgentLoop, HoldsCpuAndNetworkLocksDuringTurn) {
    start();
    h->server.push({.status = 200, .chunks = {FakeLlmServer::sse_text("x")}, .hold_after = 1});
    ASSERT_TRUE(svc->submit("go", Source::Web));
    ASSERT_TRUE(wait_until([&] { return h->server.held() == 1; }, 2000));
    auto held = h->locks->list();
    ASSERT_EQ(held.size(), 2u);
    bool cpu = false, net = false;
    for (const auto& l : held) {
        EXPECT_EQ(l.holder, "system.agent");
        EXPECT_EQ(l.reason, "agent_turn");
        cpu |= l.kind == wisp::core::WakeLockKind::Cpu;
        net |= l.kind == wisp::core::WakeLockKind::Network;
    }
    EXPECT_TRUE(cpu);
    EXPECT_TRUE(net);
    svc->stop_turn();
    ASSERT_TRUE(wait_turns(1));
    EXPECT_TRUE(h->locks->list().empty());
}

TEST_F(AgentLoop, PublishesAgentStateEvents) {
    add_tool("scene.list");
    std::mutex bmu;
    std::vector<wisp::core::Event> seen;
    harness().bus.subscribe("agent.state", [&](const wisp::core::Event& e) {
        std::lock_guard<std::mutex> lock(bmu);
        seen.push_back(e);
    });
    start();
    h->server.push(tool_reply("c1", "scene_list", "{}"));
    h->server.push(text_reply("ok"));
    ASSERT_TRUE(svc->submit("go", Source::Web));
    ASSERT_TRUE(wait_turns(1));
    ASSERT_TRUE(h->bus.wait_idle(1000));
    std::lock_guard<std::mutex> lock(bmu);
    bool tool = false;
    for (const auto& e : seen) {
        EXPECT_EQ(e.source, "agent");
        const Value* s =
            e.payload.find("state") == e.payload.end() ? nullptr : &e.payload.at("state");
        ASSERT_NE(s, nullptr);
        if (s->as_string() == "tool") {
            tool = true;
            ASSERT_NE(e.payload.find("tool"), e.payload.end());
            EXPECT_EQ(e.payload.at("tool").as_string(), "scene.list");
        } else {
            EXPECT_EQ(e.payload.find("tool"), e.payload.end());
        }
    }
    EXPECT_TRUE(tool);
    ASSERT_FALSE(seen.empty());
    EXPECT_EQ(seen.back().payload.at("state").as_string(), "idle");
}

TEST_F(AgentLoop, InterruptedTurnReportedAfterRestart) {
    harness();
    {
        ConversationStore before(*h->storage, h->dir);
        ASSERT_TRUE(before.load());
        Message begin;
        begin.turn = 1;
        begin.role = Role::Marker;
        begin.marker = "turn_begin";
        ASSERT_TRUE(before.append(begin));
        Message user;
        user.turn = 1;
        user.role = Role::User;
        user.text = "make a timer";
        ASSERT_TRUE(before.append(user));
    }
    start();
    History all = history();
    ASSERT_FALSE(all.empty());
    EXPECT_EQ(all.back().role, Role::Note);
    EXPECT_EQ(str(all.back().text), "已中断");
    EXPECT_EQ(all.back().turn, 1u);

    // The next turn numbers on, and its request leaves the note out.
    h->server.push(text_reply("ok"));
    ASSERT_TRUE(svc->submit("again", Source::Web));
    ASSERT_TRUE(wait_turns(1));
    EXPECT_EQ(snapshot().back().turn, 2u);
    Value body = h->server.body_json(0);
    const List& msgs = body.find("messages")->as_list();
    ASSERT_EQ(msgs.size(), 2u);
    EXPECT_EQ(msgs[0].find("content")->as_string(), "make a timer");
}

TEST_F(AgentLoop, ScreenOffTurnPostsDoneNotice) {
    harness().screen_off = true;
    start();
    std::string line;
    for (int i = 0; i < 30; ++i) line += "汉";
    h->server.push(text_reply(line + "\nsecond line"));
    ASSERT_TRUE(svc->submit("hi", Source::Web));
    ASSERT_TRUE(wait_turns(1));
    auto unread = h->notes->unread();
    ASSERT_EQ(unread.size(), 1u);
    EXPECT_EQ(str(unread[0].code), "agent_done");
    // 64 bytes cut back to a whole character: 21 x 3 bytes.
    EXPECT_EQ(str(unread[0].arg), line.substr(0, 63));
}

TEST_F(AgentLoop, ScreenOnTurnPostsNoNotice) {
    start();
    h->server.push(text_reply("hello"));
    ASSERT_TRUE(svc->submit("hi", Source::Web));
    ASSERT_TRUE(wait_turns(1));
    EXPECT_TRUE(h->notes->unread().empty());
}

TEST_F(AgentLoop, ListenerSeesDeltasToolsAndMessages) {
    add_tool("scene.read", [](const Value&, ToolCtx&) {
        return ToolOutput{.ok = true, .result = Map{{"ok", true}, {"text", std::string(500, 'r')}}};
    });
    start();
    const std::string args = "{\"id\":\"" + std::string(300, 'a') + "\"}";
    h->server.push(tool_reply("c1", "scene_read", args));
    h->server.push({.status = 200,
                    .chunks = {FakeLlmServer::sse_text("Hel"), FakeLlmServer::sse_text("lo"),
                               FakeLlmServer::sse_finish("stop"), FakeLlmServer::sse_done()}});
    ASSERT_TRUE(svc->submit("go", Source::Web));
    ASSERT_TRUE(wait_turns(1));
    auto ev = snapshot();
    std::string deltas;
    std::vector<uint64_t> seqs;
    int starts = 0, ends = 0;
    for (const auto& e : ev) {
        if (e.kind == Kind::Delta) deltas += e.text;
        if (e.kind == Kind::Message) seqs.push_back(e.seq);
        if (e.kind == Kind::ToolStart) {
            ++starts;
            EXPECT_EQ(e.tool, "scene.read");
            EXPECT_EQ(e.args.size(), wisp::agent::kSummaryChars);
            EXPECT_EQ(e.args, args.substr(0, wisp::agent::kSummaryChars));
        }
        if (e.kind == Kind::ToolEnd) {
            ++ends;
            EXPECT_EQ(e.tool, "scene.read");
            EXPECT_TRUE(e.ok);
            EXPECT_EQ(e.result.size(), wisp::agent::kSummaryChars);
        }
    }
    EXPECT_EQ(deltas, "Hello");
    EXPECT_EQ(starts, 1);
    EXPECT_EQ(ends, 1);
    History all = history();
    ASSERT_EQ(seqs.size(), all.size());
    for (size_t i = 0; i < all.size(); ++i) EXPECT_EQ(seqs[i], all[i].seq);
}

TEST_F(AgentLoop, NewConversationArchives) {
    start();
    h->server.push(text_reply("one"));
    ASSERT_TRUE(svc->submit("a", Source::Web));
    ASSERT_TRUE(wait_turns(1));
    ASSERT_TRUE(svc->new_conversation());
    EXPECT_EQ(count(Kind::Cleared), 1u);
    EXPECT_TRUE(history().empty());
    std::vector<wisp::agent::ArchiveInfo> list;
    std::atomic<bool> done{false};
    h->worker.submit([&] {
        auto r = h->store->archives();
        if (r) list = r.value();
        done = true;
    });
    ASSERT_TRUE(wait_until([&] { return done.load(); }, 2000));
    EXPECT_EQ(list.size(), 1u);

    // Busy while a turn runs.
    h->server.push({.status = 200, .chunks = {FakeLlmServer::sse_text("x")}, .hold_after = 1});
    ASSERT_TRUE(svc->submit("b", Source::Web));
    ASSERT_TRUE(wait_until([&] { return h->server.held() == 1; }, 2000));
    auto r = svc->new_conversation();
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, ErrorCode::Busy);
    svc->stop_turn();
    ASSERT_TRUE(wait_turns(2));
    // A new conversation numbers its turns from 1 again.
    EXPECT_EQ(snapshot().back().turn, 1u);
}

TEST_F(AgentLoop, RolloverArchivesBeforeTheNextTurn) {
    harness();
    {
        ConversationStore before(*h->storage, h->dir);
        ASSERT_TRUE(before.load());
        Message big;
        big.turn = 1;
        big.role = Role::Note;
        big.text.assign(wisp::agent::kRolloverBytes + 10, 'x');
        ASSERT_TRUE(before.append(big));
    }
    start();
    ASSERT_TRUE(h->store->needs_rollover());
    h->server.push(text_reply("ok"));
    ASSERT_TRUE(svc->submit("hi", Source::Web));
    ASSERT_TRUE(wait_turns(1));
    History all = history();
    ASSERT_GE(all.size(), 2u);
    EXPECT_EQ(all[0].role, Role::Note);
    EXPECT_EQ(str(all[0].text), "对话太长，已存档并开始新对话");
    EXPECT_FALSE(h->store->needs_rollover());
    auto ev = snapshot();
    ASSERT_GE(ev.size(), 2u);
    EXPECT_EQ(ev[0].kind, Kind::Cleared);
    EXPECT_EQ(ev[1].kind, Kind::TurnBegin);
    EXPECT_EQ(ev[1].turn, 1u);
    // The request carries only the new conversation.
    Value body = h->server.body_json(0);
    EXPECT_EQ(body.find("messages")->as_list().size(), 1u);
}

TEST_F(AgentLoop, LogsStateChanges) {
    start();
    h->server.push(text_reply("ok"));
    ASSERT_TRUE(svc->submit("hi", Source::Voice));
    ASSERT_TRUE(wait_turns(1));
    const std::string ring = wisp::log::ring_snapshot();
    EXPECT_NE(ring.find("agent: turn 1 start source=voice"), std::string::npos) << ring;
    EXPECT_NE(ring.find("agent: state thinking"), std::string::npos) << ring;
    EXPECT_NE(ring.find("agent: state idle"), std::string::npos) << ring;
}

TEST_F(AgentLoop, LogsMetadataOnly) {
    add_tool("scene.list");
    start();
    h->server.push(tool_reply("c1", "scene_list", "{\"q\":\"private-args\"}"));
    h->server.push(
        {.status = 200,
         .chunks = {FakeLlmServer::sse_text("secret-reply"), FakeLlmServer::sse_finish("stop"),
                    FakeLlmServer::sse_usage(12, 34), FakeLlmServer::sse_done()}});
    ASSERT_TRUE(svc->submit("secret-user-text", Source::Web));
    ASSERT_TRUE(wait_turns(1));
    const std::string ring = wisp::log::ring_snapshot();
    EXPECT_NE(ring.find("agent: turn 1 start source=web"), std::string::npos) << ring;
    EXPECT_NE(ring.find("agent: tool scene.list ok "), std::string::npos) << ring;
    EXPECT_NE(ring.find("agent: turn 1 end done steps=1 tokens=46"), std::string::npos) << ring;
    EXPECT_EQ(ring.find("secret-user-text"), std::string::npos);
    EXPECT_EQ(ring.find("secret-reply"), std::string::npos);
    EXPECT_EQ(ring.find("private-args"), std::string::npos);
}

TEST_F(AgentLoop, ViewShowsTheRunningTurn) {
    start();
    h->server.push({.status = 200, .chunks = {FakeLlmServer::sse_text("x")}, .hold_after = 1});
    ASSERT_TRUE(svc->submit("go", Source::Voice));
    ASSERT_TRUE(wait_until([&] { return h->server.held() == 1; }, 2000));
    auto v = svc->view();
    ASSERT_TRUE(v.running_turn.has_value());
    EXPECT_EQ(*v.running_turn, 1u);
    ASSERT_TRUE(v.running_source.has_value());
    EXPECT_EQ(*v.running_source, Source::Voice);
    EXPECT_EQ(v.state, AgentState::Output);
    ASSERT_NE(v.messages, nullptr);
    EXPECT_FALSE(v.queued);
    svc->stop_turn();
    ASSERT_TRUE(wait_turns(1));
    v = svc->view();
    EXPECT_FALSE(v.running_turn.has_value());
    EXPECT_FALSE(v.running_source.has_value());
    EXPECT_EQ(v.state, AgentState::Idle);
}

}  // namespace
