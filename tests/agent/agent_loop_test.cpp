// AgentService (wisp/agent/agent_service.hpp): the conversation loop
// against the scripted FakeLlmServer, a real storage worker and stand-in
// tools.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <fstream>
#include <iterator>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "support/agent_fixture.hpp"
#include "support/bus_flood.hpp"
#include "support/confirm_harness.hpp"
#include "support/fake_provider.hpp"
#include "support/wait.hpp"
#include "wisp/agent/agent_service.hpp"
#include "wisp/agent/limits.hpp"
#include "wisp/agent/tools.hpp"
#include "wisp/base/json.hpp"
#include "wisp/core/log.hpp"
#include "wisp/core/memory_store.hpp"
#include "wisp/ui/strings.hpp"

using wisp::ErrorCode;
using wisp::List;
using wisp::Map;
using wisp::Value;
using wisp::agent::AgentConfig;
using wisp::agent::AgentEvent;
using wisp::agent::AgentService;
using wisp::agent::AgentState;
using wisp::agent::AgentStatus;
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
using wisp::test::FakeProvider;
using wisp::test::FakeReply;
using wisp::test::FakeStep;
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
        ch.reset();
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
        wisp::agent::AgentDeps deps = h->deps();
        deps.context = context;
        svc = std::make_unique<AgentService>(std::move(deps), cfg);
        if (ch) svc->set_confirm(ch->confirm.get());
        if (follow_wifi) svc->set_wifi(&h->wifi);
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

    std::string last_status() { return last_end().status; }
    std::string last_reason() { return last_end().reason; }

    AgentEvent last_end() {
        auto ev = snapshot();
        for (auto it = ev.rbegin(); it != ev.rend(); ++it)
            if (it->kind == Kind::TurnEnd) return *it;
        return {};
    }

    // Records every agent.state payload in order; call before start().
    void record_bus_states() {
        harness().bus.subscribe("agent.state", [this](const wisp::core::Event& e) {
            EXPECT_EQ(e.source, "agent");
            std::lock_guard<std::mutex> lock(mu);
            bus_states.push_back(e.payload);
        });
    }

    // The agent.state payloads so far, once the bus has delivered them.
    std::vector<Map> bus_snapshot() {
        EXPECT_TRUE(h->bus.wait_idle(1000));
        std::lock_guard<std::mutex> lock(mu);
        return bus_states;
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

    // Sets the active model's context window (on the flash thread, which
    // is running by now).
    void set_window(uint32_t tokens) {
        harness();
        ASSERT_TRUE(h->worker.run_sync(
            [&] {
                const std::string id = h->models->get().active_for(wisp::core::ModelKind::Llm);
                wisp::core::ProfileUpdate u;
                u.options = wisp::core::ModelOptions{{"context_window", std::to_string(tokens)}};
                ASSERT_TRUE(h->models->update(id, u));
            },
            wisp::os::kForever));
    }

    // Runs one whole turn: `user` in, `reply` out.
    void run_turn(const std::string& user, const std::string& reply) {
        const size_t before = count(Kind::TurnEnd);
        h->server.push(text_reply(reply));
        ASSERT_TRUE(svc->submit(user, Source::Web));
        ASSERT_TRUE(wait_turns(before + 1));
    }

    std::unique_ptr<AgentHarness> h;
    std::unique_ptr<AgentService> svc;
    // The device confirmation, for the tests that need one (before start()).
    std::unique_ptr<wisp::test::ConfirmHarness> ch;
    wisp::agent::ContextSource context;
    // Hands the harness's Wi-Fi cell to the service (before start()).
    bool follow_wifi = false;
    uint32_t listener = 0;
    std::mutex mu;
    std::vector<AgentEvent> events;
    std::vector<std::string> calls;
    std::vector<Map> bus_states;
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

TEST_F(AgentLoop, DoneTurnHasNoReason) {
    start();
    h->server.push(text_reply("all fine"));
    ASSERT_TRUE(svc->submit("hi", Source::Web));
    ASSERT_TRUE(wait_turns(1));
    const AgentEvent end = last_end();
    EXPECT_EQ(end.status, "done");
    EXPECT_TRUE(end.reason.empty()) << end.reason;
    EXPECT_TRUE(end.note.empty()) << end.note;
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

TEST_F(AgentLoop, StopsAfterMaxToolCalls) {
    add_tool("scene.list");
    start();
    for (uint32_t i = 0; i <= wisp::agent::kMaxSteps; ++i)
        h->server.push(tool_reply("call_" + std::to_string(i), "scene_list", "{}"));
    ASSERT_TRUE(svc->submit("loop", Source::Web));
    ASSERT_TRUE(wait_turns(1, 10000));
    EXPECT_EQ(tool_calls().size(), wisp::agent::kMaxSteps);
    EXPECT_EQ(last_status(), "step_limit");
    EXPECT_EQ(last_reason(), "step_limit");
    EXPECT_EQ(last_end().note, "步骤太多，已停止");
    auto n = notes();
    ASSERT_FALSE(n.empty());
    EXPECT_EQ(n.back(), "步骤太多，已停止");
    // The call not run still has its answer, so the next request is valid.
    std::optional<Message> t = last_of(history(), Role::Tool);
    ASSERT_TRUE(t.has_value());
    EXPECT_EQ(str(t->call_id), "call_" + std::to_string(wisp::agent::kMaxSteps));
    EXPECT_FALSE(t->ok);
}

TEST_F(AgentLoop, ChangedStepLimitAppliesFromTheNextTurn) {
    add_tool("scene.list");
    start();
    svc->set_turn_limits(3, wisp::agent::kTurnMs);
    for (uint32_t i = 0; i <= 3; ++i)
        h->server.push(tool_reply("call_" + std::to_string(i), "scene_list", "{}"));
    ASSERT_TRUE(svc->submit("loop", Source::Web));
    ASSERT_TRUE(wait_turns(1, 10000));
    EXPECT_EQ(tool_calls().size(), 3u);
    EXPECT_EQ(last_status(), "step_limit");
}

TEST_F(AgentLoop, LimitChangeDuringATurnWaitsForTheNextTurn) {
    // The first call lowers the limit to one call; the running turn still
    // makes its three calls and answers.
    add_tool("scene.list", [this](const Value&, ToolCtx&) {
        svc->set_turn_limits(1, wisp::agent::kTurnMs);
        return ToolOutput{.ok = true, .result = Map{{"ok", true}}};
    });
    start();
    for (int i = 0; i < 3; ++i)
        h->server.push(tool_reply("c" + std::to_string(i), "scene_list", "{}"));
    h->server.push(text_reply("done"));
    ASSERT_TRUE(svc->submit("first", Source::Web));
    ASSERT_TRUE(wait_turns(1, 10000));
    EXPECT_EQ(tool_calls().size(), 3u);
    EXPECT_EQ(last_status(), "done");

    // The next turn stops after one call.
    for (int i = 0; i < 2; ++i)
        h->server.push(tool_reply("d" + std::to_string(i), "scene_list", "{}"));
    ASSERT_TRUE(svc->submit("second", Source::Web));
    ASSERT_TRUE(wait_turns(2, 10000));
    EXPECT_EQ(tool_calls().size(), 4u);
    EXPECT_EQ(last_status(), "step_limit");
}

TEST_F(AgentLoop, TimeLimitNoteNamesTheConfiguredMinutes) {
    add_tool("scene.list", [this](const Value&, ToolCtx&) {
        h->advance(7 * 60000 + 1);
        return ToolOutput{.ok = true, .result = Map{{"ok", true}}};
    });
    start();
    svc->set_turn_limits(wisp::agent::kMaxSteps, 7 * 60000);
    h->server.push(tool_reply("c1", "scene_list", "{}"));
    h->server.push(text_reply("never"));
    ASSERT_TRUE(svc->submit("slow", Source::Web));
    ASSERT_TRUE(wait_turns(1));
    EXPECT_EQ(last_status(), "time_limit");
    EXPECT_EQ(last_reason(), "time_limit");
    EXPECT_EQ(last_end().note, "超过 7 分钟，已停止");
    auto n = notes();
    ASSERT_FALSE(n.empty());
    EXPECT_EQ(n.back(), "超过 7 分钟，已停止");
}

TEST_F(AgentLoop, StopsAfterTurnTimeLimit) {
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
    EXPECT_NE(n.back().find(std::to_string(wisp::agent::kTurnMs / 60000)), std::string::npos);
}

TEST_F(AgentLoop, TimeLimitDuringRequestEndsAsTimeLimit) {
    start({.turn_ms = 300});
    h->server.push({.status = 200, .chunks = {FakeLlmServer::sse_text("Hel")}, .hold_after = 1});
    ASSERT_TRUE(svc->submit("slow", Source::Web));
    ASSERT_TRUE(wait_turns(1, 3000));
    EXPECT_EQ(h->server.held(), 0);
    EXPECT_EQ(last_status(), "time_limit");
    EXPECT_EQ(last_reason(), "time_limit");
    auto n = notes();
    ASSERT_EQ(n.size(), 1u);
    // Under a minute still reads as one.
    EXPECT_EQ(n[0], "超过 1 分钟，已停止");
}

TEST_F(AgentLoop, ToolCallsCarryTheTurn) {
    std::vector<uint32_t> turns;
    add_tool("scene.list", [&](const Value&, ToolCtx& ctx) {
        {
            std::lock_guard<std::mutex> lock(mu);
            turns.push_back(ctx.turn);
        }
        return ToolOutput{.ok = true, .result = Map{{"ok", true}}};
    });
    start();
    for (int turn = 0; turn < 2; ++turn) {
        h->server.push(tool_reply("c" + std::to_string(turn), "scene_list", "{}"));
        h->server.push(text_reply("ok"));
    }
    ASSERT_TRUE(svc->submit("one", Source::Web));
    ASSERT_TRUE(wait_turns(1));
    ASSERT_TRUE(svc->submit("two", Source::Web));
    ASSERT_TRUE(wait_turns(2));
    std::lock_guard<std::mutex> lock(mu);
    EXPECT_EQ(turns, (std::vector<uint32_t>{1, 2}));
}

// A new conversation numbers its turns from 1 again; ToolCtx::turn does
// not repeat.
TEST_F(AgentLoop, ToolCallTurnIsUniqueAcrossConversations) {
    std::vector<uint32_t> turns;
    add_tool("scene.list", [&](const Value&, ToolCtx& ctx) {
        {
            std::lock_guard<std::mutex> lock(mu);
            turns.push_back(ctx.turn);
        }
        return ToolOutput{.ok = true, .result = Map{{"ok", true}}};
    });
    start();
    for (int turn = 0; turn < 2; ++turn) {
        h->server.push(tool_reply("c" + std::to_string(turn), "scene_list", "{}"));
        h->server.push(text_reply("ok"));
    }
    ASSERT_TRUE(svc->submit("one", Source::Web));
    ASSERT_TRUE(wait_turns(1));
    ASSERT_TRUE(svc->new_conversation());
    ASSERT_TRUE(svc->submit("two", Source::Web));
    ASSERT_TRUE(wait_turns(2));
    ASSERT_EQ(history().front().turn, 1u);
    std::lock_guard<std::mutex> lock(mu);
    ASSERT_EQ(turns.size(), 2u);
    EXPECT_NE(turns[0], 0u);
    EXPECT_NE(turns[0], turns[1]);
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

TEST_F(AgentLoop, StoppedTurnReasonIsStopped) {
    start();
    h->server.push({.status = 200, .chunks = {FakeLlmServer::sse_text("Hel")}, .hold_after = 1});
    ASSERT_TRUE(svc->submit("go", Source::Web));
    ASSERT_TRUE(wait_until([&] { return h->server.held() == 1; }, 2000));
    svc->stop_turn();
    ASSERT_TRUE(wait_turns(1));
    const AgentEvent end = last_end();
    EXPECT_EQ(end.status, "stopped");
    EXPECT_EQ(end.reason, "stopped");
    EXPECT_EQ(end.note, wisp::ui::tr(wisp::ui::StrId::AgentStopped, wisp::ui::Lang::Zh));
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
    EXPECT_EQ(last_reason(), "no_model");
    EXPECT_EQ(last_end().note, n[0]);
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
    EXPECT_EQ(last_reason(), "network");
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

TEST_F(AgentLoop, TurnEndCarriesReasonAndNote) {
    start();
    h->server.push({.status = 401, .chunks = {"{\"error\":\"bad key sk-test\"}"}});
    ASSERT_TRUE(svc->submit("hi", Source::Web));
    ASSERT_TRUE(wait_turns(1));
    AgentEvent end = last_end();
    EXPECT_EQ(end.status, "error");
    EXPECT_EQ(end.reason, "unauthorized");
    EXPECT_EQ(end.note, wisp::ui::tr(wisp::ui::StrId::AgentErrUnauthorized, wisp::ui::Lang::Zh));
    EXPECT_EQ(end.note.find("sk-test"), std::string::npos);

    // A server error (500, retried twice) is "server".
    for (int i = 0; i < 3; ++i) h->server.push({.status = 500, .chunks = {}});
    ASSERT_TRUE(svc->submit("again", Source::Web));
    ASSERT_TRUE(wait_turns(2));
    end = last_end();
    EXPECT_EQ(end.status, "error");
    EXPECT_EQ(end.reason, "server");
    EXPECT_EQ(end.note, wisp::ui::tr(wisp::ui::StrId::AgentErrServer, wisp::ui::Lang::Zh));
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
    follow_wifi = true;
    start();
    h->server.push({.status = 200, .chunks = {FakeLlmServer::sse_text("Hel")}, .hold_after = 1});
    ASSERT_TRUE(svc->submit("go", Source::Web));
    ASSERT_TRUE(wait_until([&] { return h->server.held() == 1; }, 2000));
    const uint64_t t0 = wisp::os::now_ms();
    h->set_wifi(wisp::net::WifiState::Backoff);
    ASSERT_TRUE(wait_turns(1, 2000));
    EXPECT_LE(wisp::os::now_ms() - t0, 2000u);
    EXPECT_EQ(last_status(), "error");
    auto n = notes();
    ASSERT_FALSE(n.empty());
    EXPECT_EQ(n.back(), "网络中断");
    EXPECT_EQ(last_reason(), "network_lost");
    std::optional<Message> a = last_of(history(), Role::Assistant);
    ASSERT_TRUE(a.has_value());
    EXPECT_EQ(str(a->text), "Hel");
    EXPECT_TRUE(h->locks->list().empty());
    EXPECT_EQ(svc->state(), AgentState::Idle);

    // While the network is down no request is made.
    ASSERT_TRUE(svc->submit("again", Source::Web));
    ASSERT_TRUE(wait_turns(2));
    EXPECT_EQ(h->server.requests().size(), 1u);
    EXPECT_EQ(last_reason(), "network");

    h->set_wifi(wisp::net::WifiState::Connected);
    ASSERT_TRUE(h->bus.wait_idle(1000));
    h->server.push(text_reply("back"));
    ASSERT_TRUE(svc->submit("third", Source::Web));
    ASSERT_TRUE(wait_turns(3));
    EXPECT_EQ(last_status(), "done");
}

// The cell decides alone: deps.network_up (still true here) is not asked,
// and the request ends long before the stall check would end it.
TEST_F(AgentLoop, LostWifiAbortsTheRequest) {
    follow_wifi = true;
    start({.stall_ms = 60000});
    h->server.push({.status = 200, .chunks = {FakeLlmServer::sse_text("Hel")}, .hold_after = 1});
    ASSERT_TRUE(svc->submit("go", Source::Web));
    ASSERT_TRUE(wait_until([&] { return h->server.held() == 1; }, 2000));
    h->set_wifi(wisp::net::WifiState::Off);
    ASSERT_TRUE(wait_turns(1, 2000));
    EXPECT_EQ(h->server.held(), 0);
    EXPECT_EQ(last_status(), "error");
    EXPECT_EQ(last_reason(), "network_lost");
    EXPECT_TRUE(h->network_up.load());
}

// The cell's value at start() counts before it ever changes, and its
// first delivery (not connected, nothing in flight) leaves no abort
// behind for a later turn.
TEST_F(AgentLoop, KnowsTheNetworkAtStart) {
    harness().set_wifi(wisp::net::WifiState::Backoff);
    follow_wifi = true;
    start();
    ASSERT_TRUE(h->bus.wait_idle(1000));
    ASSERT_TRUE(svc->submit("hi", Source::Web));
    ASSERT_TRUE(wait_turns(1));
    EXPECT_EQ(h->server.requests().size(), 0u);
    EXPECT_EQ(last_status(), "error");
    EXPECT_EQ(last_reason(), "network");

    h->set_wifi(wisp::net::WifiState::Connected);
    h->server.push(text_reply("up"));
    ASSERT_TRUE(svc->submit("again", Source::Web));
    ASSERT_TRUE(wait_turns(2));
    EXPECT_EQ(h->server.requests().size(), 1u);
    EXPECT_EQ(last_status(), "done");
}

// connected -> connecting -> connected while wisp.bus is held reaches the
// service as one update, Connected: the request in flight goes on. The
// stall check is out of reach (60 s), so only the subscription could end
// it.
TEST_F(AgentLoop, ReconnectBetweenRunsDoesNotAbort) {
    follow_wifi = true;
    start({.stall_ms = 60000});
    h->server.push({.status = 200,
                    .chunks = {FakeLlmServer::sse_text("Hel"), FakeLlmServer::sse_text("lo"),
                               FakeLlmServer::sse_finish("stop"), FakeLlmServer::sse_done()},
                    .hold_after = 1});
    ASSERT_TRUE(svc->submit("go", Source::Web));
    ASSERT_TRUE(wait_until([&] { return h->server.held() == 1; }, 2000));
    {
        wisp::test::BusFlood flood(h->bus);
        flood.hold();
        h->set_wifi(wisp::net::WifiState::Connecting);
        h->set_wifi(wisp::net::WifiState::Connected);
        flood.release();
    }
    ASSERT_TRUE(h->bus.wait_idle(1000));
    EXPECT_EQ(count(Kind::TurnEnd), 0u);
    EXPECT_EQ(h->server.held(), 1);
    h->server.release();
    ASSERT_TRUE(wait_turns(1));
    EXPECT_EQ(last_status(), "done");
    std::optional<Message> a = last_of(history(), Role::Assistant);
    ASSERT_TRUE(a.has_value());
    EXPECT_EQ(str(a->text), "Hello");
}

// A link that died while the cell never left Connected (the drop and the
// reconnect coalesced) still ends the stream, through the stall check.
// Both a stall and a network abort end as network_lost, so the stall is
// told apart by time: it can fire only stall_ms after the last byte, which
// came after t0, while a network abort would end the turn as soon as the
// flood is released.
TEST_F(AgentLoop, DeadLinkStillEndsByStall) {
    constexpr uint32_t kStallMs = 200;
    follow_wifi = true;
    start({.stall_ms = kStallMs});
    h->server.push({.status = 200, .chunks = {FakeLlmServer::sse_text("Hel")}, .hold_after = 1});
    const uint64_t t0 = wisp::os::now_ms();
    ASSERT_TRUE(svc->submit("go", Source::Web));
    ASSERT_TRUE(wait_until([&] { return h->server.held() == 1; }, 2000));
    {
        wisp::test::BusFlood flood(h->bus);
        flood.hold();
        h->set_wifi(wisp::net::WifiState::Connecting);
        h->set_wifi(wisp::net::WifiState::Connected);
        flood.release();
    }
    ASSERT_TRUE(wait_turns(1, 3000));
    EXPECT_GE(wisp::os::now_ms() - t0, kStallMs);
    EXPECT_EQ(h->server.held(), 0);
    EXPECT_EQ(last_status(), "error");
    EXPECT_EQ(last_reason(), "network_lost");
    std::optional<Message> a = last_of(history(), Role::Assistant);
    ASSERT_TRUE(a.has_value());
    EXPECT_EQ(str(a->text), "Hel");
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
    EXPECT_EQ(last_reason(), "network_lost");
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
    EXPECT_EQ(last_reason(), "network_lost");
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

// The error a turn passes through stays counted once it is idle again.
TEST_F(AgentLoop, StatusCellCountsErrorsThatEndInIdle) {
    record_bus_states();
    start();
    EXPECT_EQ(svc->status_cell().get(), AgentStatus{});
    h->server.push({.status = 401, .chunks = {"{\"error\":\"bad key\"}"}});
    ASSERT_TRUE(svc->submit("hi", Source::Web));
    ASSERT_TRUE(wait_turns(1));
    EXPECT_EQ(last_reason(), "unauthorized");
    AgentStatus want;
    want.errors = 1;
    EXPECT_EQ(svc->status_cell().get(), want);
    EXPECT_EQ(states(),
              (std::vector<AgentState>{AgentState::Thinking, AgentState::Error, AgentState::Idle}));
    EXPECT_EQ(bus_snapshot(), (std::vector<Map>{{{"state", Value("thinking")}},
                                                {{"state", Value("error")}},
                                                {{"state", Value("idle")}}}));

    // A good turn leaves the count alone; the next error moves it on.
    run_turn("ok?", "ok");
    EXPECT_EQ(svc->status_cell().get(), want);
    h->server.push({.status = 401, .chunks = {"{\"error\":\"bad key\"}"}});
    ASSERT_TRUE(svc->submit("hi", Source::Web));
    ASSERT_TRUE(wait_turns(3));
    want.errors = 2;
    EXPECT_EQ(svc->status_cell().get(), want);
}

// A tool name the model made up, longer than the status holds: the status
// (and so agent.state) carries its first 72 bytes, cut at a code-point
// boundary; the listener and the model see the whole name.
TEST_F(AgentLoop, UnknownLongToolNameIsCutInTheStatus) {
    const std::string name(100, 'n');
    // Byte 72 falls inside the two-byte "é": the cut moves back to 71.
    const std::string utf8 = std::string(71, 'u') + "\xC3\xA9" + std::string(27, 'v');
    // Shared with the listener, which a failed assertion leaves behind.
    struct Seen {
        std::mutex mu;
        std::vector<AgentStatus> during;
    };
    auto seen = std::make_shared<Seen>();
    record_bus_states();
    start();
    AgentService* agent = svc.get();
    const uint32_t watcher = svc->add_listener([seen, agent](const AgentEvent& e) {
        if (e.kind != Kind::State || e.state != AgentState::Tool) return;
        std::lock_guard<std::mutex> lock(seen->mu);
        seen->during.push_back(agent->status_cell().get());
    });
    h->server.push(tool_reply("c1", name, "{}"));
    h->server.push(text_reply("sorry"));
    ASSERT_TRUE(svc->submit("go", Source::Web));
    ASSERT_TRUE(wait_turns(1));
    h->server.push(tool_reply("c2", utf8, "{}"));
    h->server.push(text_reply("sorry"));
    ASSERT_TRUE(svc->submit("again", Source::Web));
    ASSERT_TRUE(wait_turns(2));
    svc->remove_listener(watcher);

    {
        std::lock_guard<std::mutex> lock(seen->mu);
        ASSERT_EQ(seen->during.size(), 2u);
        EXPECT_EQ(seen->during[0].state, AgentState::Tool);
        EXPECT_EQ(seen->during[0].tool.view(), name.substr(0, 72));
        EXPECT_EQ(seen->during[1].tool.view(), utf8.substr(0, 71));
    }
    std::vector<std::string> bus_tools;
    for (const Map& m : bus_snapshot())
        if (m.at("state").as_string() == "tool") bus_tools.push_back(m.at("tool").as_string());
    EXPECT_EQ(bus_tools, (std::vector<std::string>{name.substr(0, 72), utf8.substr(0, 71)}));

    // Unchanged: the listener's ToolStart and the tool error the model gets.
    std::vector<std::string> started;
    for (const auto& e : snapshot())
        if (e.kind == Kind::ToolStart) started.push_back(e.tool);
    EXPECT_EQ(started, (std::vector<std::string>{name, utf8}));
    std::vector<Message> results;
    for (const auto& m : history())
        if (m.role == Role::Tool) results.push_back(m);
    ASSERT_EQ(results.size(), 2u);
    EXPECT_EQ(str(results[0].tool), name);
    EXPECT_FALSE(results[0].ok);
    auto r = wisp::from_json(str(results[0].text));
    ASSERT_TRUE(r);
    EXPECT_EQ(r.value().find("error")->as_string(), "unknown_tool");
    EXPECT_NE(str(results[0].text).find("no tool named " + name), std::string::npos);
    EXPECT_EQ(svc->status_cell().get().state, AgentState::Idle);
    EXPECT_TRUE(svc->status_cell().get().tool.empty());
}

// Two unknown names that agree in their first 72 bytes, the first one
// called twice in a row: the listener sees a State for each change of
// name and none for the repeat. agent.state is built from the status,
// which cannot tell the two names apart, so it goes out once, with the
// cut name (plan deviation 8).
TEST_F(AgentLoop, AlikeLongToolNamesInARow) {
    const std::string a = std::string(72, 'a') + std::string(28, '1');
    const std::string b = std::string(72, 'a') + std::string(28, '2');
    record_bus_states();
    start();
    h->server.push(
        {.status = 200,
         .chunks = {FakeLlmServer::sse_tool(0, "c1", a, "{}"),
                    FakeLlmServer::sse_tool(1, "c2", a, "{}"),
                    FakeLlmServer::sse_tool(2, "c3", b, "{}"),
                    FakeLlmServer::sse_finish("tool_calls"), FakeLlmServer::sse_done()}});
    h->server.push(text_reply("ok"));
    ASSERT_TRUE(svc->submit("go", Source::Web));
    ASSERT_TRUE(wait_turns(1));
    std::vector<std::string> tools;
    for (const auto& e : snapshot())
        if (e.kind == Kind::State && e.state == AgentState::Tool) tools.push_back(e.tool);
    EXPECT_EQ(tools, (std::vector<std::string>{a, b}));
    EXPECT_EQ(bus_snapshot(),
              (std::vector<Map>{{{"state", Value("thinking")}},
                                {{"state", Value("tool")}, {"tool", Value(a.substr(0, 72))}},
                                {{"state", Value("thinking")}},
                                {{"state", Value("output")}},
                                {{"state", Value("idle")}}}));
}

// The exact agent.state payloads of a tool turn and of a failed turn: the
// status cell's mirror must publish these and nothing else.
TEST_F(AgentLoop, AgentStatePayloadsArePinned) {
    add_tool("scene.list");
    record_bus_states();
    start();
    h->server.push(tool_reply("c1", "scene_list", "{}"));
    h->server.push(text_reply("ok"));
    ASSERT_TRUE(svc->submit("go", Source::Web));
    ASSERT_TRUE(wait_turns(1));
    const std::vector<Map> tool_turn{{{"state", Value("thinking")}},
                                     {{"state", Value("tool")}, {"tool", Value("scene.list")}},
                                     {{"state", Value("thinking")}},
                                     {{"state", Value("output")}},
                                     {{"state", Value("idle")}}};
    EXPECT_EQ(bus_snapshot(), tool_turn);

    h->server.push({.status = 401, .chunks = {"{\"error\":\"bad key\"}"}});
    ASSERT_TRUE(svc->submit("again", Source::Web));
    ASSERT_TRUE(wait_turns(2));
    std::vector<Map> both = tool_turn;
    both.push_back({{"state", Value("thinking")}});
    both.push_back({{"state", Value("error")}});
    both.push_back({{"state", Value("idle")}});
    EXPECT_EQ(bus_snapshot(), both);
}

TEST_F(AgentLoop, InterruptedTurnReportedAfterRestart) {
    harness();
    // Written on the harness's worker, the flash thread.
    ASSERT_TRUE(h->worker.run_sync(
        [&] {
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
        },
        wisp::os::kForever));
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

// A call the model is still writing shows as ToolPrep (the dotted name
// and the bytes so far) before its ToolStart; a text answer has none.
TEST_F(AgentLoop, ToolPrepBeforeToolStart) {
    add_tool("scene.write_file");
    start();
    h->server.push(
        {.status = 200,
         .chunks = {FakeLlmServer::sse_tool(0, "c1", "scene_write_file", ""),
                    FakeLlmServer::sse_tool(0, "", "", "{\"id\":\"a\"}"),
                    FakeLlmServer::sse_finish("tool_calls"), FakeLlmServer::sse_done()}});
    h->server.push({.status = 200,
                    .chunks = {FakeLlmServer::sse_text("done"), FakeLlmServer::sse_finish("stop"),
                               FakeLlmServer::sse_done()}});
    ASSERT_TRUE(svc->submit("go", Source::Web));
    ASSERT_TRUE(wait_turns(1));
    auto ev = snapshot();
    int prep_at = -1, start_at = -1;
    for (size_t i = 0; i < ev.size(); ++i) {
        if (ev[i].kind == Kind::ToolPrep && prep_at < 0) {
            prep_at = static_cast<int>(i);
            EXPECT_EQ(ev[i].tool, "scene.write_file");
            EXPECT_EQ(ev[i].bytes, 0u);
            EXPECT_TRUE(ev[i].args.empty());
        }
        if (ev[i].kind == Kind::ToolStart && start_at < 0) start_at = static_cast<int>(i);
    }
    ASSERT_GE(prep_at, 0);
    ASSERT_GE(start_at, 0);
    EXPECT_LT(prep_at, start_at);
    // The second request answered in text: no ToolPrep after the tool.
    for (size_t i = static_cast<size_t>(start_at); i < ev.size(); ++i)
        EXPECT_NE(ev[i].kind, Kind::ToolPrep);
}

TEST_F(AgentLoop, NoToolPrepForATextAnswer) {
    start();
    h->server.push({.status = 200,
                    .chunks = {FakeLlmServer::sse_text("hi"), FakeLlmServer::sse_finish("stop"),
                               FakeLlmServer::sse_done()}});
    ASSERT_TRUE(svc->submit("go", Source::Web));
    ASSERT_TRUE(wait_turns(1));
    EXPECT_EQ(count(Kind::ToolPrep), 0u);
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
    // Written on the harness's worker, the flash thread.
    ASSERT_TRUE(h->worker.run_sync(
        [&] {
            ConversationStore before(*h->storage, h->dir);
            ASSERT_TRUE(before.load());
            Message big;
            big.turn = 1;
            big.role = Role::Note;
            big.text.assign(wisp::agent::kRolloverBytes + 10, 'x');
            ASSERT_TRUE(before.append(big));
        },
        wisp::os::kForever));
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

TEST_F(AgentLoop, ContextFramesTheRequest) {
    std::atomic<int> asked{0};
    std::atomic<int> source{-1};
    context = [&](Source s) {
        ++asked;
        source = static_cast<int>(s);
        wisp::agent::DeviceSnapshot d;
        d.shape = {.kind = wisp::hal::ScreenShapeKind::Rect, .width = 320, .height = 240};
        d.memory = {"[1] 称呼：小陈"};
        return d;
    };
    start();
    h->server.push(text_reply("hi"));
    ASSERT_TRUE(svc->submit("hello", Source::Voice));
    ASSERT_TRUE(wait_turns(1));
    EXPECT_EQ(asked.load(), 1);
    EXPECT_EQ(source.load(), static_cast<int>(Source::Voice));
    const Value body_msgs = h->server.body_json(0);
    const List& msgs = body_msgs.find("messages")->as_list();
    ASSERT_EQ(msgs.size(), 3u);
    EXPECT_EQ(msgs[0].find("role")->as_string(), "system");
    EXPECT_NE(msgs[0].find("content")->as_string().find("320x240"), std::string::npos);
    EXPECT_EQ(msgs[1].find("content")->as_string(), "hello");
    EXPECT_EQ(msgs[2].find("role")->as_string(), "system");
    const std::string& info = msgs[2].find("content")->as_string();
    EXPECT_EQ(info.rfind("[1] ", 0), 0u) << info;
    // The snapshot's source is the turn's, whatever the context source says.
    EXPECT_NE(info.find("Source: voice"), std::string::npos) << info;
}

TEST_F(AgentLoop, MemoryNotesInTheRequest) {
    const std::string path =
        harness().storage->write_root(wisp::core::Area::System).value() + "/memory/notes.json";
    auto memory = std::make_unique<wisp::core::MemoryStore>(
        *harness().storage, path, &harness().bus, [] { return int64_t{1700000000}; });
    ASSERT_TRUE(
        harness().worker.run_sync([&] { ASSERT_TRUE(memory->load()); }, wisp::os::kForever));
    ASSERT_TRUE(wisp::agent::add_memory_tools(*harness().tools,
                                              wisp::agent::MemoryToolDeps{.store = *memory}));

    wisp::core::MemoryStore* mem = memory.get();
    context = [mem](Source) {
        wisp::agent::DeviceSnapshot d;
        for (const wisp::core::MemoryNote& n : mem->list()) {
            d.memory.push_back("[" + std::to_string(n.id) + "] " + str(n.text));
        }
        return d;
    };
    start();

    h->server.push(tool_reply("c1", "memory_add", R"({"text":"称呼：小陈"})"));
    h->server.push(text_reply("ok"));
    ASSERT_TRUE(svc->submit("remember me", Source::Web));
    ASSERT_TRUE(wait_turns(1));

    ASSERT_EQ(h->server.requests().size(), 2u);
    const Value body = h->server.body_json(1);
    const List& msgs = body.find("messages")->as_list();
    const std::string& info = msgs.back().find("content")->as_string();
    EXPECT_EQ(info.rfind("[1] ", 0), 0u) << info;
}

TEST_F(AgentLoop, CompressionRequestReplacesOldTurns) {
    // 4096 tokens: compress past 3072 (9216 bytes), down to 1638.
    set_window(4096);
    add_tool("scene.list");
    start();
    const std::string big(4000, 'a');
    run_turn("q1", big);
    run_turn("q2", big);
    run_turn("q3", big);
    ASSERT_EQ(h->server.requests().size(), 3u);
    History before = history();
    uint64_t turn2_end = 0;
    for (const auto& m : before)
        if (m.turn == 2) turn2_end = m.seq;

    h->server.push(text_reply("they asked q1 and q2"));
    run_turn("q4", "done");
    ASSERT_EQ(h->server.requests().size(), 5u);

    // The summary request: no tools, a fixed prompt, the old turns.
    Value sum = h->server.body_json(3);
    EXPECT_EQ(sum.find("tools"), nullptr);
    const List& sm = sum.find("messages")->as_list();
    ASSERT_EQ(sm.size(), 2u);
    EXPECT_EQ(sm[0].find("role")->as_string(), "system");
    EXPECT_NE(sm[1].find("content")->as_string().find("User: q1"), std::string::npos);
    EXPECT_EQ(sm[1].find("content")->as_string().find("q3"), std::string::npos);

    // The normal request: the summary in place of turns 1-2.
    Value next = h->server.body_json(4);
    EXPECT_NE(next.find("tools"), nullptr);
    const List& nm = next.find("messages")->as_list();
    ASSERT_EQ(nm.size(), 4u);
    EXPECT_EQ(nm[0].find("role")->as_string(), "system");
    EXPECT_NE(nm[0].find("content")->as_string().find("they asked q1 and q2"), std::string::npos);
    EXPECT_EQ(nm[1].find("content")->as_string(), "q3");
    EXPECT_EQ(nm[3].find("content")->as_string(), "q4");

    auto stored = last_of(history(), Role::Summary);
    ASSERT_TRUE(stored);
    EXPECT_EQ(str(stored->text), "they asked q1 and q2");
    EXPECT_EQ(stored->covers_first, 1u);
    EXPECT_EQ(stored->covers_last, turn2_end);
    EXPECT_EQ(stored->turn, 4u);
    // The originals stay in the file.
    std::ifstream f(h->dir + "/current.jsonl", std::ios::binary);
    std::string file((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    EXPECT_NE(file.find("\"q1\""), std::string::npos);
    EXPECT_NE(file.find("\"summary\""), std::string::npos);
    EXPECT_EQ(last_status(), "done");

    // The next turn reuses the summary instead of compressing again.
    run_turn("q5", "ok");
    ASSERT_EQ(h->server.requests().size(), 6u);
    const Value body_later = h->server.body_json(5);
    const List& later = body_later.find("messages")->as_list();
    EXPECT_NE(later[0].find("content")->as_string().find("they asked q1 and q2"),
              std::string::npos);
}

TEST_F(AgentLoop, SummaryFailureFallsBackToDropping) {
    set_window(4096);
    start();
    const std::string big(4000, 'a');
    run_turn("q1", big);
    run_turn("q2", big);
    run_turn("q3", big);
    // The summary request fails three times (500, retried twice).
    for (int i = 0; i < 3; ++i) h->server.push({.status = 500, .chunks = {}});
    run_turn("q4", "done");
    ASSERT_EQ(h->server.requests().size(), 3u + 3u + 1u);
    EXPECT_EQ(last_status(), "done");

    auto n = notes();
    ASSERT_EQ(n.size(), 1u);
    EXPECT_EQ(n[0], wisp::ui::tr(wisp::ui::StrId::AgentContextDropped, wisp::ui::Lang::Zh));
    const Value body_msgs = h->server.body_json(6);
    const List& msgs = body_msgs.find("messages")->as_list();
    ASSERT_EQ(msgs.size(), 3u);
    EXPECT_EQ(msgs[0].find("content")->as_string(), "q3");
    EXPECT_EQ(msgs[2].find("content")->as_string(), "q4");
    for (const auto& m : msgs) EXPECT_NE(m.find("content")->as_string(), "q1");
}

TEST_F(AgentLoop, SummaryWithoutConnectionKeepsTheTurns) {
    set_window(4096);
    start();
    const std::string big(4000, 'a');
    run_turn("q1", big);
    run_turn("q2", big);
    run_turn("q3", big);
    // The client retries a connect failure twice before it gives up.
    for (int i = 0; i < 3; ++i)
        h->server.push({.status = 0, .chunks = {}, .fail = wisp::net::TransportError::Connect});
    run_turn("q4", "done");
    ASSERT_EQ(h->server.requests().size(), 7u);
    EXPECT_EQ(last_status(), "done");
    EXPECT_FALSE(last_of(history(), Role::Summary));
    EXPECT_TRUE(notes().empty());
    // Sent whole: the turns are kept for a later attempt.
    const Value body_msgs = h->server.body_json(6);
    const List& msgs = body_msgs.find("messages")->as_list();
    EXPECT_EQ(msgs[0].find("content")->as_string(), "q1");
}

namespace {

FakeReply context_length_reply() {
    return {.status = 400,
            .chunks = {R"({"error":{"code":"context_length_exceeded","message":"too long"}})"}};
}

FakeReply usage_reply(const std::string& text, int prompt) {
    return {.status = 200,
            .chunks = {FakeLlmServer::sse_text(text), FakeLlmServer::sse_finish("stop"),
                       FakeLlmServer::sse_usage(prompt, 1), FakeLlmServer::sse_done()}};
}

}  // namespace

TEST_F(AgentLoop, ContextLengthErrorCompressesAndRetriesOnce) {
    // 4096 tokens: turns 1-2 (about 2700 tokens) fit under 75% by the
    // estimate, so turn 3 is sent whole and refused.
    set_window(4096);
    add_tool("scene.list");
    start();
    const std::string big(4000, 'a');
    run_turn("q1", big);
    run_turn("q2", big);
    ASSERT_EQ(h->server.requests().size(), 2u);

    h->server.push(context_length_reply());
    h->server.push(text_reply("they asked q1"));
    run_turn("q3", "done");

    EXPECT_EQ(last_status(), "done");
    EXPECT_TRUE(last_of(history(), Role::Summary));
    // The refused request, the summary request (no tools), one retry.
    ASSERT_EQ(h->server.requests().size(), 5u);
    EXPECT_EQ(h->server.body_json(3).find("tools"), nullptr);
    const Value retry = h->server.body_json(4);
    EXPECT_NE(retry.find("tools"), nullptr);
    const List& msgs = retry.find("messages")->as_list();
    EXPECT_NE(msgs[0].find("content")->as_string().find("they asked q1"), std::string::npos);
    EXPECT_EQ(msgs.back().find("content")->as_string(), "q3");
    EXPECT_TRUE(notes().empty());
}

TEST_F(AgentLoop, SecondContextLengthErrorEndsTheTurnWithANote) {
    set_window(4096);
    start();
    const std::string big(4000, 'a');
    run_turn("q1", big);
    run_turn("q2", big);
    h->server.push(context_length_reply());
    h->server.push(text_reply("they asked q1"));
    h->server.push(context_length_reply());
    ASSERT_TRUE(svc->submit("q3", Source::Web));
    ASSERT_TRUE(wait_turns(3));
    // The refused request, the summary, the refused retry.
    EXPECT_EQ(h->server.requests().size(), 5u);
    EXPECT_EQ(last_status(), "error");
    auto n = notes();
    ASSERT_EQ(n.size(), 1u);
    EXPECT_EQ(n[0], wisp::ui::tr(wisp::ui::StrId::AgentErrContext, wisp::ui::Lang::Zh));
    EXPECT_EQ(last_reason(), "context");
}

// Nothing to summarize and no result to cut: the retry would be the same
// request, refused the same way.
TEST_F(AgentLoop, ContextLengthWithNothingToShrinkIsNotRetried) {
    start();
    h->server.push(context_length_reply());
    h->server.push(text_reply("never"));
    ASSERT_TRUE(svc->submit("hi", Source::Web));
    ASSERT_TRUE(wait_turns(1));
    EXPECT_EQ(h->server.requests().size(), 1u);
    EXPECT_EQ(last_status(), "error");
    auto n = notes();
    ASSERT_EQ(n.size(), 1u);
    EXPECT_EQ(n[0], wisp::ui::tr(wisp::ui::StrId::AgentErrContext, wisp::ui::Lang::Zh));
    EXPECT_EQ(last_reason(), "context");
}

TEST_F(AgentLoop, OversizedLiveResultIsCutInsteadOfOverflowing) {
    // 8192 tokens; one 60 KB result is about 20000 tokens on its own.
    set_window(8192);
    add_tool("scene.read_file", [](const Value&, ToolCtx&) {
        return ToolOutput{.ok = true, .result = Map{{"content", std::string(60000, 'x')}}};
    });
    start();
    h->server.push(tool_reply("c1", "scene_read_file", "{}"));
    h->server.push(text_reply("read it"));
    ASSERT_TRUE(svc->submit("read the file", Source::Web));
    ASSERT_TRUE(wait_turns(1));

    EXPECT_EQ(last_status(), "done");
    ASSERT_EQ(h->server.requests().size(), 2u);
    const Value next = h->server.body_json(1);
    const List& msgs = next.find("messages")->as_list();
    ASSERT_EQ(msgs.size(), 3u);
    EXPECT_EQ(msgs[2].find("role")->as_string(), "tool");
    EXPECT_LT(msgs[2].find("content")->as_string().size(), wisp::agent::kToolResultKeepBytes + 64);
    // The stored result stays whole.
    std::optional<Message> stored = last_of(history(), Role::Tool);
    ASSERT_TRUE(stored);
    EXPECT_GT(stored->text.size(), 60000u);
}

TEST_F(AgentLoop, UsageCalibratesTheEstimate) {
    // Turn 2's request is estimated at (2 + 3000 + 2) / 3 = 1001 tokens and
    // the model counts twice that. Turn 3's history, 6006 bytes = 2002
    // tokens, fits under 75% of 4096 (3072) as estimated, not when doubled.
    set_window(4096);
    start();
    run_turn("q1", std::string(3000, 'a'));
    h->server.push(usage_reply("ok", 2002));
    ASSERT_TRUE(svc->submit("q2", Source::Web));
    ASSERT_TRUE(wait_turns(2));
    ASSERT_EQ(h->server.requests().size(), 2u);

    h->server.push(text_reply("they said q1"));
    run_turn(std::string(3000, 'q'), "done");
    EXPECT_EQ(last_status(), "done");
    ASSERT_EQ(h->server.requests().size(), 4u);
    // The summary request: a fixed prompt and the transcript.
    const Value sum = h->server.body_json(2);
    ASSERT_EQ(sum.find("messages")->as_list().size(), 2u);
    EXPECT_NE(sum.find("messages")->as_list()[1].find("content")->as_string().find("User: q1"),
              std::string::npos);
    EXPECT_TRUE(last_of(history(), Role::Summary));
}

// The calibration belongs to the model that reported it: after a switch
// the plain estimate counts again, and turn 3 fits without a summary.
TEST_F(AgentLoop, ModelSwitchResetsTheCalibration) {
    set_window(4096);
    start();
    run_turn("q1", std::string(3000, 'a'));
    h->server.push(usage_reply("ok", 2002));
    ASSERT_TRUE(svc->submit("q2", Source::Web));
    ASSERT_TRUE(wait_turns(2));
    ASSERT_EQ(h->server.requests().size(), 2u);

    ASSERT_TRUE(h->worker.run_sync(
        [&] {
            const std::string id = h->models->get().active_for(wisp::core::ModelKind::Llm);
            wisp::core::ProfileUpdate u;
            u.model = "m2";
            ASSERT_TRUE(h->models->update(id, u));
        },
        wisp::os::kForever));
    run_turn(std::string(3000, 'q'), "done");
    EXPECT_EQ(last_status(), "done");
    ASSERT_EQ(h->server.requests().size(), 3u);
    EXPECT_EQ(*h->server.body_json(2).find("model"), Value("m2"));
    EXPECT_FALSE(last_of(history(), Role::Summary));
}

// Two profiles on different providers with the same model name do not
// share it: as above, but switching to another profile whose model is also
// "m", and turn 3 again fits without a summary.
TEST_F(AgentLoop, SameModelOnAnotherProfileStartsUncalibrated) {
    set_window(4096);
    start();
    run_turn("q1", std::string(3000, 'a'));
    h->server.push(usage_reply("ok", 2002));
    ASSERT_TRUE(svc->submit("q2", Source::Web));
    ASSERT_TRUE(wait_turns(2));
    ASSERT_EQ(h->server.requests().size(), 2u);

    ASSERT_TRUE(h->worker.run_sync(
        [&] {
            auto p =
                h->models->create({.kind = wisp::core::ModelKind::Llm,
                                   .name = "other",
                                   .preset = "custom",
                                   .base_url = "https://other.example.com/v1",
                                   .model = "m",
                                   .options = wisp::core::ModelOptions{{"context_window", "4096"}},
                                   .api_key = "sk-2"});
            ASSERT_TRUE(p);
            ASSERT_TRUE(h->models->activate(p.value().id));
        },
        wisp::os::kForever));
    run_turn(std::string(3000, 'q'), "done");
    EXPECT_EQ(last_status(), "done");
    ASSERT_EQ(h->server.requests().size(), 3u);
    EXPECT_FALSE(last_of(history(), Role::Summary));
}

namespace {

ToolDef risky_tool(const std::string& name, wisp::agent::ToolRisk risk,
                   std::function<ToolOutput(const Value&, ToolCtx&)> run) {
    ToolDef def;
    def.name = name;
    def.description = "test tool";
    def.risk = risk;
    def.timeout_ms = 60000;
    def.run = std::move(run);
    return def;
}

}  // namespace

// While a tool waits for the device, the agent is waiting_confirm and its
// listeners (the web page) get the question, then "" once it is answered.
TEST_F(AgentLoop, WaitingConfirmStateAndWebSeesText) {
    ch = std::make_unique<wisp::test::ConfirmHarness>();
    ASSERT_TRUE(ch->ok);
    harness();
    wisp::agent::ConfirmFn fn;
    std::atomic<bool> allowed{false};
    ASSERT_TRUE(h->tools->add(
        risky_tool("scene.delete", wisp::agent::ToolRisk::Confirm, [&](const Value&, ToolCtx& c) {
            allowed = fn("Agent 要删除场景「番茄钟」", c);
            return ToolOutput{.ok = true, .result = Map{{"ok", true}}};
        })));
    start();
    fn = svc->confirm_fn();
    h->server.push(tool_reply("c1", "scene_delete", "{}"));
    h->server.push(text_reply("done"));
    ASSERT_TRUE(svc->submit("go", Source::Web));

    ASSERT_TRUE(ch->wait_showing("番茄钟"));
    EXPECT_EQ(svc->state(), AgentState::WaitingConfirm);
    EXPECT_EQ(svc->view().confirm, std::optional<std::string>("Agent 要删除场景「番茄钟」"));
    ASSERT_TRUE(ch->press(true));
    ASSERT_TRUE(wait_turns(1));
    EXPECT_TRUE(allowed.load());
    EXPECT_FALSE(svc->view().confirm.has_value());

    std::vector<std::string> confirms;
    for (const auto& e : snapshot())
        if (e.kind == Kind::Confirm) confirms.push_back(e.text);
    EXPECT_EQ(confirms, (std::vector<std::string>{"Agent 要删除场景「番茄钟」", ""}));
    const std::vector<AgentState> s = states();
    auto at = std::find(s.begin(), s.end(), AgentState::WaitingConfirm);
    ASSERT_NE(at, s.end());
    // Back to the tool once answered.
    ASSERT_NE(at + 1, s.end());
    EXPECT_EQ(*(at + 1), AgentState::Tool);
}

// A confirm asked off wisp.agent (a tool on the storage worker) is refused
// without a dialog: the wait would hold that thread.
TEST_F(AgentLoop, ConfirmOffTheAgentThreadIsRefused) {
    ch = std::make_unique<wisp::test::ConfirmHarness>();
    ASSERT_TRUE(ch->ok);
    harness();
    wisp::agent::ConfirmFn fn;
    std::atomic<int> answer{-1};
    ToolDef def = risky_tool("scene.files_remove", wisp::agent::ToolRisk::Confirm,
                             [&](const Value&, ToolCtx& c) {
                                 answer = fn("remove?", c) ? 1 : 0;
                                 return ToolOutput{.ok = true, .result = Map{{"ok", true}}};
                             });
    def.thread = wisp::agent::ToolThread::Fs;
    ASSERT_TRUE(h->tools->add(std::move(def)));
    start();
    fn = svc->confirm_fn();
    h->server.push(tool_reply("c1", "scene_files_remove", "{}"));
    h->server.push(text_reply("done"));
    ASSERT_TRUE(svc->submit("go", Source::Web));
    ASSERT_TRUE(wait_turns(1));
    EXPECT_EQ(answer.load(), 0);
    EXPECT_EQ(ch->wakes.load(), 0);
    EXPECT_EQ(count(Kind::Confirm), 0u);
}

// A note tool's success adds the system's note after the tool's result,
// and the result the model sees next carries it as "system_note".
TEST_F(AgentLoop, NoteToolsInsertSystemNote) {
    harness();
    ASSERT_TRUE(h->tools->add(
        risky_tool("settings.set", wisp::agent::ToolRisk::Note, [](const Value&, ToolCtx&) {
            return ToolOutput{.ok = true, .result = Map{{"ok", true}}, .note = "亮度 200 → 120"};
        })));
    start();
    h->server.push(tool_reply("c1", "settings_set", R"({"key":"brightness","value":120})"));
    h->server.push(text_reply("done"));
    ASSERT_TRUE(svc->submit("dim it", Source::Web));
    ASSERT_TRUE(wait_turns(1));

    History all = history();
    size_t tool_at = all.size();
    for (size_t i = 0; i < all.size(); ++i)
        if (all[i].role == Role::Tool) tool_at = i;
    ASSERT_LT(tool_at + 1, all.size());
    EXPECT_EQ(all[tool_at + 1].role, Role::Note);
    EXPECT_EQ(str(all[tool_at + 1].text), "亮度 200 → 120");
    EXPECT_EQ(all[tool_at + 1].turn, all[tool_at].turn);

    ASSERT_EQ(h->server.requests().size(), 2u);
    const Value body = h->server.body_json(1);
    const List& msgs = body.find("messages")->as_list();
    const Value* tool_msg = nullptr;
    for (const Value& m : msgs)
        if (m.find("role")->as_string() == "tool") tool_msg = &m;
    ASSERT_NE(tool_msg, nullptr);
    auto content = wisp::from_json(tool_msg->find("content")->as_string());
    ASSERT_TRUE(content);
    ASSERT_NE(content.value().find("system_note"), nullptr);
    EXPECT_EQ(content.value().find("system_note")->as_string(), "亮度 200 → 120");
    // The note itself is not sent as a message of its own.
    for (const Value& m : msgs) {
        const Value* text = m.find("content");
        if (text != nullptr && text->type() == Value::Type::String)
            EXPECT_NE(text->as_string(), "亮度 200 → 120");
    }
}

TEST_F(AgentLoop, NoNoteOnFailure) {
    harness();
    ASSERT_TRUE(h->tools->add(
        risky_tool("settings.set", wisp::agent::ToolRisk::Note, [](const Value&, ToolCtx&) {
            ToolOutput o = wisp::agent::tool_error("bad_value", "no such level");
            o.note = "亮度 200 → 999";
            return o;
        })));
    start();
    h->server.push(tool_reply("c1", "settings_set", "{}"));
    h->server.push(text_reply("done"));
    ASSERT_TRUE(svc->submit("dim it", Source::Web));
    ASSERT_TRUE(wait_turns(1));
    EXPECT_TRUE(notes().empty());
    const Value body = h->server.body_json(1);
    for (const Value& m : body.find("messages")->as_list()) {
        if (m.find("role")->as_string() != "tool") continue;
        auto content = wisp::from_json(m.find("content")->as_string());
        ASSERT_TRUE(content);
        EXPECT_EQ(content.value().find("system_note"), nullptr);
    }
}

// Clearing every conversation asks on the device first: denied, nothing
// changes; allowed, the current conversation and the archives go.
TEST_F(AgentLoop, ClearAllNeedsConfirm) {
    ch = std::make_unique<wisp::test::ConfirmHarness>();
    ASSERT_TRUE(ch->ok);
    start();
    run_turn("one", "first");
    ASSERT_TRUE(svc->new_conversation());
    run_turn("two", "second");
    auto archives = [&] {
        size_t n = 0;
        EXPECT_TRUE(h->worker.run_sync(
            [&] {
                auto a = h->store->archives();
                ASSERT_TRUE(a);
                n = a.value().size();
            },
            wisp::os::kForever));
        return n;
    };
    ASSERT_EQ(archives(), 1u);
    const size_t before = history().size();
    ASSERT_GT(before, 0u);
    const std::string question = wisp::ui::tr(wisp::ui::StrId::AgentConfirmClear);

    auto clear = [&](bool allow) {
        std::atomic<int> result{-1};
        const size_t cleared_before = count(Kind::Cleared);
        EXPECT_TRUE(svc->request_clear([&](bool ok) { result = ok ? 1 : 0; }));
        EXPECT_TRUE(ch->wait_showing(question));
        EXPECT_EQ(svc->state(), AgentState::WaitingConfirm);
        EXPECT_TRUE(ch->press(allow));
        EXPECT_TRUE(wait_until([&] { return result.load() >= 0; }, 5000));
        EXPECT_TRUE(wait_until([&] { return count(Kind::Cleared) > cleared_before; }, 5000));
        EXPECT_EQ(svc->state(), AgentState::Idle);
        return result.load();
    };

    EXPECT_EQ(clear(false), 0);
    EXPECT_EQ(history().size(), before);
    EXPECT_EQ(archives(), 1u);
    auto last_cleared = [&] {
        std::optional<AgentEvent> e;
        for (const auto& ev : snapshot())
            if (ev.kind == Kind::Cleared) e = ev;
        return e;
    };
    ASSERT_TRUE(last_cleared());
    EXPECT_FALSE(last_cleared()->ok);

    EXPECT_EQ(clear(true), 1);
    EXPECT_TRUE(history().empty());
    EXPECT_EQ(archives(), 0u);
    ASSERT_TRUE(last_cleared());
    EXPECT_TRUE(last_cleared()->ok);

    // The next turn starts the numbering over.
    run_turn("three", "third");
    EXPECT_EQ(history().front().turn, 1u);
}

TEST_F(AgentLoop, ClearAllWithoutConfirmIsRefused) {
    start();
    run_turn("one", "first");
    const size_t before = history().size();
    std::atomic<int> result{-1};
    ASSERT_TRUE(svc->request_clear([&](bool ok) { result = ok ? 1 : 0; }));
    ASSERT_TRUE(wait_until([&] { return result.load() >= 0; }, 5000));
    EXPECT_EQ(result.load(), 0);
    EXPECT_EQ(history().size(), before);
}

// A stop while the clear waits for the device closes the dialog and
// leaves the history.
TEST_F(AgentLoop, StopCancelsClear) {
    ch = std::make_unique<wisp::test::ConfirmHarness>();
    ASSERT_TRUE(ch->ok);
    start();
    run_turn("one", "first");
    const size_t before = history().size();
    std::atomic<int> result{-1};
    ASSERT_TRUE(svc->request_clear([&](bool ok) { result = ok ? 1 : 0; }));
    ASSERT_TRUE(ch->wait_showing(wisp::ui::tr(wisp::ui::StrId::AgentConfirmClear)));
    svc->stop_turn();
    ASSERT_TRUE(wait_until([&] { return result.load() >= 0; }, 5000));
    EXPECT_EQ(result.load(), 0);
    EXPECT_EQ(history().size(), before);
    EXPECT_TRUE(wait_until([&] { return !ch->dialog_open(); }, 2000));
}

TEST_F(AgentLoop, ClearIsBusyDuringATurn) {
    start();
    h->server.push({.status = 200, .chunks = {FakeLlmServer::sse_text("slow")}, .hold_after = 1});
    ASSERT_TRUE(svc->submit("hi", Source::Web));
    ASSERT_TRUE(wait_until([&] { return svc->state() == AgentState::Output; }, 5000));
    auto r = svc->request_clear([](bool) { ADD_FAILURE() << "done called"; });
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().message, "busy");
    svc->stop_turn();
    ASSERT_TRUE(wait_turns(1));
}

// Appends are short and someone waits for them: they take the Interactive
// lane. Archiving (a rename and a new file) keeps the Normal one.
TEST_F(AgentLoop, AppendsUseTheInteractiveIo) {
    start();
    const int io_before = h->io_calls.load();
    const int interactive_before = h->interactive_io_calls.load();
    run_turn("hello", "hi");
    // turn_begin, the user message, the reply, turn_end.
    EXPECT_EQ(h->interactive_io_calls.load() - interactive_before, 4);
    EXPECT_EQ(h->io_calls.load(), io_before);

    ASSERT_TRUE(svc->new_conversation());
    EXPECT_EQ(h->io_calls.load(), io_before + 1);
    EXPECT_EQ(h->interactive_io_calls.load() - interactive_before, 4);
}

// Without an Interactive executor the appends still reach the store,
// through run_io.
TEST_F(AgentLoop, AppendsFallBackToRunIoWithoutAnInteractiveExecutor) {
    harness();
    auto deps = h->deps();
    deps.run_io_interactive = nullptr;
    svc = std::make_unique<AgentService>(std::move(deps));
    listener = svc->add_listener([this](const AgentEvent& e) {
        std::lock_guard<std::mutex> lock(mu);
        events.push_back(e);
    });
    ASSERT_TRUE(svc->start());
    const int io_before = h->io_calls.load();
    const size_t stored = history().size();
    run_turn("hello", "hi");
    // turn_begin, the user message, the reply, turn_end.
    EXPECT_EQ(h->io_calls.load() - io_before, 4);
    EXPECT_EQ(h->interactive_io_calls.load(), 0);
    EXPECT_GT(history().size(), stored);
}

// The agent reaches the model only through net::LlmProvider: a scripted
// provider runs a whole tool turn, and the HTTP client is never used.
TEST_F(AgentLoop, RunsOverAnyProvider) {
    FakeProvider fake;
    harness().provider = &fake;
    add_tool("scene.list");
    FakeStep call;
    call.calls.push_back({.id = "c1", .name = "scene_list", .arguments = "{}"});
    call.result.finish = wisp::net::LlmFinish::ToolCalls;
    fake.push(std::move(call));
    FakeStep answer;
    answer.deltas = {"hi"};
    answer.result.finish = wisp::net::LlmFinish::Stop;
    fake.push(std::move(answer));
    start();
    ASSERT_TRUE(svc->submit("list", Source::Web));
    ASSERT_TRUE(wait_turns(1));

    EXPECT_EQ(last_status(), "done");
    EXPECT_EQ(tool_calls(), std::vector<std::string>{"scene.list"});
    std::vector<Role> roles;
    for (const auto& m : history()) roles.push_back(m.role);
    EXPECT_EQ(roles, (std::vector<Role>{Role::Marker, Role::User, Role::Assistant, Role::Tool,
                                        Role::Assistant, Role::Marker}));
    const auto recorded = fake.calls();
    ASSERT_EQ(recorded.size(), 2u);
    EXPECT_NE(recorded[1].body.find("\"tool_call_id\":\"c1\""), std::string::npos)
        << recorded[1].body;
    EXPECT_TRUE(h->server.requests().empty());
}

// The provider's own "no model" ends the turn as no_model, with the note.
TEST_F(AgentLoop, NoModelFromTheProvider) {
    FakeProvider fake;
    harness().provider = &fake;
    fake.set_model(std::nullopt);
    FakeStep step;
    step.result.error = wisp::net::LlmError::NoModel;
    fake.push(std::move(step));
    start();
    ASSERT_TRUE(svc->submit("hi", Source::Web));
    ASSERT_TRUE(wait_turns(1));

    EXPECT_EQ(last_reason(), "no_model");
    const auto n = notes();
    ASSERT_FALSE(n.empty());
    EXPECT_EQ(n.back(), wisp::ui::tr(wisp::ui::StrId::AgentErrNoModel, wisp::ui::Lang::Zh));
}

// The context window is the provider's active model's, read once per turn:
// at 4096 tokens the history of three long turns is compressed before the
// fourth turn's request.
TEST_F(AgentLoop, WindowComesFromTheProvider) {
    FakeProvider fake;
    harness().provider = &fake;
    fake.set_model(
        wisp::net::LlmModelInfo{.profile_id = "p1", .model = "m", .context_window = 4096});
    add_tool("scene.list");
    start();
    const std::string big(4000, 'a');
    for (int i = 1; i <= 3; ++i) {
        const int before = fake.active_model_calls();
        const size_t ends = count(Kind::TurnEnd);
        FakeStep step;
        step.deltas = {big};
        step.result.finish = wisp::net::LlmFinish::Stop;
        fake.push(std::move(step));
        ASSERT_TRUE(svc->submit("q" + std::to_string(i), Source::Web));
        ASSERT_TRUE(wait_turns(ends + 1));
        EXPECT_EQ(fake.active_model_calls(), before + 1);
    }
    ASSERT_EQ(fake.calls().size(), 3u);
    EXPECT_FALSE(last_of(history(), Role::Summary));

    const int before = fake.active_model_calls();
    FakeStep summary;
    summary.deltas = {"they asked q1 and q2"};
    summary.result.finish = wisp::net::LlmFinish::Stop;
    fake.push(std::move(summary));
    FakeStep reply;
    reply.deltas = {"done"};
    reply.result.finish = wisp::net::LlmFinish::Stop;
    fake.push(std::move(reply));
    ASSERT_TRUE(svc->submit("q4", Source::Web));
    ASSERT_TRUE(wait_turns(4));
    EXPECT_EQ(fake.active_model_calls(), before + 1);

    const auto recorded = fake.calls();
    ASSERT_EQ(recorded.size(), 5u);
    EXPECT_EQ(recorded[3].tools_json, "");
    EXPECT_NE(recorded[4].tools_json, "");
    const auto stored = last_of(history(), Role::Summary);
    ASSERT_TRUE(stored);
    EXPECT_EQ(str(stored->text), "they asked q1 and q2");
    EXPECT_EQ(last_status(), "done");
}

// Spec 3.7: a reply cut by the length limit and continued reaches the
// listeners as one stream of deltas and the history as one message.
TEST_F(AgentLoop, ContinuedReplyIsOneMessageOnTheWire) {
    start();
    h->server.push({.status = 200,
                    .chunks = {FakeLlmServer::sse_text("Hel"), FakeLlmServer::sse_finish("length"),
                               FakeLlmServer::sse_done()}});
    h->server.push(text_reply("lo"));
    ASSERT_TRUE(svc->submit("go", Source::Web));
    ASSERT_TRUE(wait_turns(1));
    EXPECT_EQ(last_status(), "done");

    std::vector<std::string> deltas;
    std::vector<uint64_t> seqs;
    for (const AgentEvent& e : snapshot()) {
        if (e.kind == Kind::Delta) deltas.push_back(e.text);
        if (e.kind == Kind::Message) seqs.push_back(e.seq);
    }
    EXPECT_EQ(deltas, (std::vector<std::string>{"Hel", "lo"}));
    const History all = history();
    size_t replies = 0;
    for (const Message& m : all) {
        EXPECT_NE(m.role, Role::Note);
        if (m.role != Role::Assistant) continue;
        ++replies;
        EXPECT_EQ(str(m.text), "Hello");
        EXPECT_EQ(std::count(seqs.begin(), seqs.end(), m.seq), 1);
    }
    EXPECT_EQ(replies, 1u);
    const std::vector<AgentState> seen = states();
    EXPECT_EQ(std::count(seen.begin(), seen.end(), AgentState::Output), 1);

    ASSERT_EQ(h->server.requests().size(), 2u);
    const Value body = h->server.body_json(1);
    const List& msgs = body.find("messages")->as_list();
    ASSERT_GE(msgs.size(), 3u);
    const Value& partial = msgs[msgs.size() - 2];
    EXPECT_EQ(partial.find("role")->as_string(), "assistant");
    EXPECT_EQ(partial.find("content")->as_string(), "Hel");
    const Value& prompt = msgs.back();
    EXPECT_EQ(prompt.find("role")->as_string(), "user");
    EXPECT_EQ(prompt.find("content")->as_string(),
              "Your previous reply was cut off by the length limit. Continue exactly where it "
              "stopped. Do not repeat anything.");
}

// The summary request's text is used as it is, even when cut by the length
// limit (spec 3.7): it is never continued.
TEST_F(AgentLoop, SummaryRequestsAreNotContinued) {
    FakeProvider fake;
    harness().provider = &fake;
    fake.set_model(
        wisp::net::LlmModelInfo{.profile_id = "p1", .model = "m", .context_window = 4096});
    add_tool("scene.list");
    start();
    const std::string big(4000, 'a');
    for (int i = 1; i <= 3; ++i) {
        const size_t ends = count(Kind::TurnEnd);
        FakeStep step;
        step.deltas = {big};
        step.result.finish = wisp::net::LlmFinish::Stop;
        fake.push(std::move(step));
        ASSERT_TRUE(svc->submit("q" + std::to_string(i), Source::Web));
        ASSERT_TRUE(wait_turns(ends + 1));
    }
    FakeStep summary;
    summary.deltas = {"S"};
    summary.result.finish = wisp::net::LlmFinish::Length;
    fake.push(std::move(summary));
    FakeStep reply;
    reply.deltas = {"done"};
    reply.result.finish = wisp::net::LlmFinish::Stop;
    fake.push(std::move(reply));
    ASSERT_TRUE(svc->submit("q4", Source::Web));
    ASSERT_TRUE(wait_turns(4));

    const auto recorded = fake.calls();
    ASSERT_EQ(recorded.size(), 5u);
    EXPECT_EQ(recorded[3].tools_json, "");
    for (const auto& c : recorded)
        EXPECT_EQ(c.body.find("Your previous reply was cut off"), std::string::npos) << c.body;
    const auto stored = last_of(history(), Role::Summary);
    ASSERT_TRUE(stored);
    EXPECT_EQ(str(stored->text), "S");
    EXPECT_EQ(last_status(), "done");
}
