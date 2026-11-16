// AgentTurnGolden: today's turn, pinned as goldens before the model client
// moves behind net::LlmProvider (spec 3.1, 3.10). Each scenario compares the
// listener's event sequence, the stored conversation and, where the bytes
// matter, the raw request bodies against literals. Only AgentHarness,
// FakeLlmServer and the public AgentService/context.hpp API are used, so a
// refactor of the service's insides cannot reach this file.

#include <gtest/gtest.h>

#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "support/agent_fixture.hpp"
#include "support/wait.hpp"
#include "wisp/agent/agent_service.hpp"
#include "wisp/agent/context.hpp"
#include "wisp/base/value.hpp"
#include "wisp/core/log.hpp"
#include "wisp/net/llm_client.hpp"
#include "wisp/ui/strings.hpp"

using wisp::Map;
using wisp::Value;
using wisp::agent::AgentConfig;
using wisp::agent::AgentEvent;
using wisp::agent::AgentService;
using wisp::agent::ContextSource;
using wisp::agent::DeviceSnapshot;
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
using wisp::ui::StrId;
using Kind = AgentEvent::Kind;
using Lines = std::vector<std::string>;

namespace {

std::string str(const wisp::os::LargeString& s) { return {s.data(), s.size()}; }

std::string zh(StrId id) { return wisp::ui::tr(id, wisp::ui::Lang::Zh); }

FakeReply text_reply(const std::string& text) {
    return {.status = 200,
            .chunks = {FakeLlmServer::sse_text(text), FakeLlmServer::sse_finish("stop"),
                       FakeLlmServer::sse_done()}};
}

FakeReply tool_reply(const std::string& id, const std::string& wire, const std::string& args) {
    return {.status = 200,
            .chunks = {FakeLlmServer::sse_tool(0, id, wire, args),
                       FakeLlmServer::sse_finish("tool_calls"), FakeLlmServer::sse_done()}};
}

FakeReply context_length_reply() {
    return {.status = 400,
            .chunks = {R"({"error":{"code":"context_length_exceeded","message":"too long"}})"}};
}

class AgentTurnGolden : public ::testing::Test {
protected:
    void SetUp() override {
        wisp::log::reset_for_test();
        wisp::log::init();
    }

    void TearDown() override {
        if (h) h->server.release();
        svc.reset();
        h.reset();
        wisp::log::reset_for_test();
    }

    AgentHarness& harness(bool with_model = true) {
        if (!h) h = std::make_unique<AgentHarness>(with_model);
        return *h;
    }

    void add_echo(const std::string& name) {
        ToolDef def;
        def.name = name;
        def.description = "test tool";
        def.run = [](const Value&, ToolCtx&) {
            return ToolOutput{.ok = true, .result = Map{{"ok", true}}};
        };
        ASSERT_TRUE(harness().tools->add(std::move(def)));
    }

    void start(AgentConfig cfg = {}, ContextSource context = {}) {
        harness();
        wisp::agent::AgentDeps deps = h->deps();
        deps.context = std::move(context);
        svc = std::make_unique<AgentService>(std::move(deps), cfg);
        svc->add_listener([this](const AgentEvent& e) {
            std::lock_guard<std::mutex> lock(mu);
            events.push_back(e);
        });
        ASSERT_TRUE(svc->start());
    }

    size_t ends() {
        std::lock_guard<std::mutex> lock(mu);
        size_t n = 0;
        for (const auto& e : events) n += e.kind == Kind::TurnEnd ? 1 : 0;
        return n;
    }

    bool wait_turns(size_t n) {
        return wait_until([&] { return ends() >= n; }, 10000);
    }

    // As AgentLoop's set_window: on the flash thread, which runs by now.
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

    void run_turn(const std::string& user, const std::string& reply) {
        const size_t before = ends();
        h->server.push(text_reply(reply));
        ASSERT_TRUE(svc->submit(user, Source::Web));
        ASSERT_TRUE(wait_turns(before + 1));
    }

    // One line per event; consecutive identical ToolPrep lines collapse into
    // one, since their count depends on timing (kToolProgressMs).
    Lines trace() {
        std::lock_guard<std::mutex> lock(mu);
        Lines out;
        for (const AgentEvent& e : events) {
            std::string line;
            switch (e.kind) {
                case Kind::TurnBegin:
                    line = "TurnBegin turn=" + std::to_string(e.turn) +
                           " source=" + wisp::agent::to_string(e.source) + " text=" + e.text;
                    break;
                case Kind::TurnEnd:
                    line = "TurnEnd turn=" + std::to_string(e.turn) + " status=" + e.status +
                           " reason=" + e.reason + " note=" + e.note;
                    break;
                case Kind::Delta:
                    line = "Delta " + e.text;
                    break;
                case Kind::ToolPrep:
                    line = "ToolPrep " + e.tool;
                    break;
                case Kind::ToolStart:
                    line = "ToolStart " + e.tool + " " + e.args;
                    break;
                case Kind::ToolEnd:
                    line = "ToolEnd " + e.tool + " ok=" + (e.ok ? "1" : "0") + " " + e.result;
                    break;
                case Kind::State:
                    line = std::string("State ") + wisp::agent::to_string(e.state);
                    if (e.state == wisp::agent::AgentState::Tool) line += " " + e.tool;
                    break;
                case Kind::Confirm:
                    line = "Confirm " + e.text;
                    break;
                case Kind::Message:
                    line = "Message " + std::to_string(e.seq);
                    break;
                case Kind::Cleared:
                    line = std::string("Cleared ok=") + (e.ok ? "1" : "0");
                    break;
            }
            if (e.kind == Kind::ToolPrep && !out.empty() && out.back() == line) continue;
            out.push_back(std::move(line));
        }
        return out;
    }

    History history() { return *h->store->history(); }

    // One line per stored message.
    Lines transcript() {
        Lines out;
        for (const Message& m : history()) {
            std::string line = std::to_string(m.seq) + " " + wisp::agent::to_string(m.role) + " t" +
                               std::to_string(m.turn) + " " + str(m.text);
            switch (m.role) {
                case Role::Marker:
                    line += " marker=" + str(m.marker) + " status=" + str(m.status);
                    break;
                case Role::Assistant:
                    if (!m.calls.empty()) {
                        line += " calls=";
                        for (size_t i = 0; i < m.calls.size(); ++i) {
                            if (i > 0) line += ",";
                            line += str(m.calls[i].id) + ":" + str(m.calls[i].name);
                        }
                    }
                    break;
                case Role::Tool:
                    line += " call=" + str(m.call_id) + " tool=" + str(m.tool) +
                            " ok=" + (m.ok ? "1" : "0");
                    break;
                case Role::Summary:
                    line += " covers=" + std::to_string(m.covers_first) + "-" +
                            std::to_string(m.covers_last);
                    break;
                default:
                    break;
            }
            out.push_back(std::move(line));
        }
        return out;
    }

    std::string body(size_t i) { return h->server.requests().at(i).body; }

    std::unique_ptr<AgentHarness> h;
    std::unique_ptr<AgentService> svc;
    std::mutex mu;
    std::vector<AgentEvent> events;
};

TEST_F(AgentTurnGolden, PlainReply) {
    start();
    h->server.push(text_reply("hi"));
    ASSERT_TRUE(svc->submit("hello", Source::Web));
    ASSERT_TRUE(wait_turns(1));

    EXPECT_EQ(trace(), (Lines{
                           "TurnBegin turn=1 source=web text=hello",
                           "Message 1",
                           "Message 2",
                           "State thinking",
                           "State output",
                           "Delta hi",
                           "Message 3",
                           "Message 4",
                           "State idle",
                           "TurnEnd turn=1 status=done reason= note=",
                       }));
    EXPECT_EQ(transcript(), (Lines{
                                "1 marker t1  marker=turn_begin status=",
                                "2 user t1 hello",
                                "3 assistant t1 hi",
                                "4 marker t1  marker=turn_end status=done",
                            }));
}

TEST_F(AgentTurnGolden, ToolCallThenReply) {
    add_echo("test.echo");
    start();
    h->server.push(tool_reply("c1", "test_echo", R"({"a":1})"));
    h->server.push(text_reply("done"));
    ASSERT_TRUE(svc->submit("go", Source::Web));
    ASSERT_TRUE(wait_turns(1));

    EXPECT_EQ(trace(), (Lines{
                           "TurnBegin turn=1 source=web text=go",
                           "Message 1",
                           "Message 2",
                           "State thinking",
                           "ToolPrep test.echo",
                           "Message 3",
                           "State tool test.echo",
                           R"(ToolStart test.echo {"a":1})",
                           "Message 4",
                           R"(ToolEnd test.echo ok=1 {"ok":true})",
                           "State thinking",
                           "State output",
                           "Delta done",
                           "Message 5",
                           "Message 6",
                           "State idle",
                           "TurnEnd turn=1 status=done reason= note=",
                       }));
    EXPECT_EQ(transcript(), (Lines{
                                "1 marker t1  marker=turn_begin status=",
                                "2 user t1 go",
                                "3 assistant t1  calls=c1:test.echo",
                                R"(4 tool t1 {"ok":true} call=c1 tool=test.echo ok=1)",
                                "5 assistant t1 done",
                                "6 marker t1  marker=turn_end status=done",
                            }));
}

TEST_F(AgentTurnGolden, ToolTurnBodies) {
    add_echo("test.echo");
    start();
    h->server.push(tool_reply("c1", "test_echo", R"({"a":1})"));
    h->server.push(text_reply("done"));
    ASSERT_TRUE(svc->submit("go", Source::Web));
    ASSERT_TRUE(wait_turns(1));

    ASSERT_EQ(h->server.requests().size(), 2u);
    EXPECT_EQ(
        body(0),
        R"({"model":"m","stream":true,"stream_options":{"include_usage":true},"messages":[)"
        R"({"role":"user","content":"go"}],)"
        R"("tools":[{"type":"function","function":{"name":"test_echo","description":"test tool",)"
        R"("parameters":{"properties":{},"type":"object"}}}]})");
    EXPECT_EQ(
        body(1),
        R"({"model":"m","stream":true,"stream_options":{"include_usage":true},"messages":[)"
        R"({"role":"user","content":"go"},)"
        R"({"role":"assistant","content":null,"tool_calls":[{"id":"c1","type":"function",)"
        R"("function":{"name":"test_echo","arguments":"{\"a\":1}"}}]},)"
        R"({"role":"tool","tool_call_id":"c1","content":"{\"ok\":true}"}],)"
        R"("tools":[{"type":"function","function":{"name":"test_echo","description":"test tool",)"
        R"("parameters":{"properties":{},"type":"object"}}}]})");
}

TEST_F(AgentTurnGolden, StepLimitSkipsTheRest) {
    add_echo("test.echo");
    start({.max_steps = 1});
    h->server.push(
        {.status = 200,
         .chunks = {FakeLlmServer::sse_tool(0, "c1", "test_echo", "{}"),
                    FakeLlmServer::sse_tool(1, "c2", "test_echo", "{}"),
                    FakeLlmServer::sse_finish("tool_calls"), FakeLlmServer::sse_done()}});
    ASSERT_TRUE(svc->submit("go", Source::Web));
    ASSERT_TRUE(wait_turns(1));

    EXPECT_EQ(trace(), (Lines{
                           "TurnBegin turn=1 source=web text=go",
                           "Message 1",
                           "Message 2",
                           "State thinking",
                           "ToolPrep test.echo",
                           "Message 3",
                           "State tool test.echo",
                           "ToolStart test.echo {}",
                           "Message 4",
                           R"(ToolEnd test.echo ok=1 {"ok":true})",
                           "Message 5",
                           "Message 6",
                           "Message 7",
                           "State idle",
                           "TurnEnd turn=1 status=step_limit reason=step_limit note=" +
                               zh(StrId::AgentStepLimit),
                       }));
    EXPECT_EQ(
        transcript(),
        (Lines{
            "1 marker t1  marker=turn_begin status=",
            "2 user t1 go",
            "3 assistant t1  calls=c1:test.echo,c2:test.echo",
            R"(4 tool t1 {"ok":true} call=c1 tool=test.echo ok=1)",
            R"(5 tool t1 {"error":"cancelled","message":"the turn reached its step limit","ok":false})"
            R"( call=c2 tool=test.echo ok=0)",
            "6 note t1 " + zh(StrId::AgentStepLimit),
            "7 marker t1  marker=turn_end status=step_limit",
        }));
}

TEST_F(AgentTurnGolden, UnauthorizedEndsWithANote) {
    start();
    h->server.push({.status = 401});
    ASSERT_TRUE(svc->submit("hello", Source::Web));
    ASSERT_TRUE(wait_turns(1));

    EXPECT_EQ(trace(), (Lines{
                           "TurnBegin turn=1 source=web text=hello",
                           "Message 1",
                           "Message 2",
                           "State thinking",
                           "State error",
                           "Message 3",
                           "Message 4",
                           "State idle",
                           "TurnEnd turn=1 status=error reason=unauthorized note=" +
                               zh(StrId::AgentErrUnauthorized),
                       }));
    EXPECT_EQ(transcript(), (Lines{
                                "1 marker t1  marker=turn_begin status=",
                                "2 user t1 hello",
                                "3 note t1 " + zh(StrId::AgentErrUnauthorized),
                                "4 marker t1  marker=turn_end status=error",
                            }));
}

TEST_F(AgentTurnGolden, StopMidStreamKeepsTheText) {
    start();
    h->server.push({.status = 200, .chunks = {FakeLlmServer::sse_text("par")}, .hold_after = 1});
    ASSERT_TRUE(svc->submit("go", Source::Web));
    ASSERT_TRUE(wait_until([&] { return h->server.held() == 1; }, 2000));
    svc->stop_turn();
    ASSERT_TRUE(wait_turns(1));

    EXPECT_EQ(trace(),
              (Lines{
                  "TurnBegin turn=1 source=web text=go",
                  "Message 1",
                  "Message 2",
                  "State thinking",
                  "State output",
                  "Delta par",
                  "Message 3",
                  "Message 4",
                  "Message 5",
                  "State idle",
                  "TurnEnd turn=1 status=stopped reason=stopped note=" + zh(StrId::AgentStopped),
              }));
    EXPECT_EQ(transcript(), (Lines{
                                "1 marker t1  marker=turn_begin status=",
                                "2 user t1 go",
                                "3 assistant t1 par",
                                "4 note t1 " + zh(StrId::AgentStopped),
                                "5 marker t1  marker=turn_end status=stopped",
                            }));
}

TEST_F(AgentTurnGolden, ContextRefusalCompressesAndRetries) {
    set_window(4096);
    add_echo("scene.list");
    start();
    const std::string big(3000, 'a');
    run_turn("q1", big);
    run_turn("q2", big);
    ASSERT_EQ(h->server.requests().size(), 2u);

    h->server.push(context_length_reply());
    h->server.push(text_reply("S"));
    run_turn("q3", "ok");

    EXPECT_EQ(trace(), (Lines{
                           "TurnBegin turn=1 source=web text=q1",
                           "Message 1",
                           "Message 2",
                           "State thinking",
                           "State output",
                           "Delta " + big,
                           "Message 3",
                           "Message 4",
                           "State idle",
                           "TurnEnd turn=1 status=done reason= note=",
                           "TurnBegin turn=2 source=web text=q2",
                           "Message 5",
                           "Message 6",
                           "State thinking",
                           "State output",
                           "Delta " + big,
                           "Message 7",
                           "Message 8",
                           "State idle",
                           "TurnEnd turn=2 status=done reason= note=",
                           "TurnBegin turn=3 source=web text=q3",
                           "Message 9",
                           "Message 10",
                           "State thinking",
                           "Message 11",
                           "State output",
                           "Delta ok",
                           "Message 12",
                           "Message 13",
                           "State idle",
                           "TurnEnd turn=3 status=done reason= note=",
                       }));
    EXPECT_EQ(transcript(), (Lines{
                                "1 marker t1  marker=turn_begin status=",
                                "2 user t1 q1",
                                "3 assistant t1 " + big,
                                "4 marker t1  marker=turn_end status=done",
                                "5 marker t2  marker=turn_begin status=",
                                "6 user t2 q2",
                                "7 assistant t2 " + big,
                                "8 marker t2  marker=turn_end status=done",
                                "9 marker t3  marker=turn_begin status=",
                                "10 user t3 q3",
                                "11 summary t3 S covers=1-4",
                                "12 assistant t3 ok",
                                "13 marker t3  marker=turn_end status=done",
                            }));
    // The refused request, the summary request (no tools), the retry.
    ASSERT_EQ(h->server.requests().size(), 5u);
    EXPECT_NE(h->server.body_json(2).find("tools"), nullptr);
    EXPECT_EQ(h->server.body_json(3).find("tools"), nullptr);
    EXPECT_NE(h->server.body_json(4).find("tools"), nullptr);

    const History all = history();
    std::optional<Message> s;
    History before;
    for (const Message& m : all) {
        if (m.role == Role::Summary) {
            s = m;
            break;
        }
        before.push_back(m);
    }
    ASSERT_TRUE(s.has_value());
    EXPECT_EQ(
        body(3),
        str(wisp::net::build_chat_body(
            "m",
            wisp::agent::build_summary_request(before, s->covers_first, s->covers_last).messages,
            "")));
}

TEST_F(AgentTurnGolden, ContextSourceBody) {
    DeviceSnapshot snap;
    snap.lang = "en";
    snap.wall_s = 1793923200;
    snap.utc_offset_min = 480;
    snap.weekday = 3;
    snap.memory = {"[1] likes tea"};
    snap.battery = 80;
    start({}, [snap](Source) { return snap; });
    h->server.push(text_reply("hi"));
    ASSERT_TRUE(svc->submit("hello", Source::Web));
    ASSERT_TRUE(wait_turns(1));

    EXPECT_EQ(trace(), (Lines{
                           "TurnBegin turn=1 source=web text=hello",
                           "Message 1",
                           "Message 2",
                           "State thinking",
                           "State output",
                           "Delta hi",
                           "Message 3",
                           "Message 4",
                           "State idle",
                           "TurnEnd turn=1 status=done reason= note=",
                       }));
    EXPECT_EQ(transcript(), (Lines{
                                "1 marker t1  marker=turn_begin status=",
                                "2 user t1 hello",
                                "3 assistant t1 hi",
                                "4 marker t1  marker=turn_end status=done",
                            }));

    History upto;
    for (const Message& m : history()) {
        upto.push_back(m);
        if (m.role == Role::User) break;
    }
    DeviceSnapshot with_source = snap;
    with_source.source = Source::Web;
    EXPECT_EQ(body(0), str(wisp::net::build_chat_body(
                           "m", wisp::agent::build_request(upto, with_source).messages,
                           str(h->tools->tools_json()))));
}

TEST_F(AgentTurnGolden, NoModelEndsWithANote) {
    harness(false);
    start();
    ASSERT_TRUE(svc->submit("hello", Source::Web));
    ASSERT_TRUE(wait_turns(1));

    EXPECT_TRUE(h->server.requests().empty());
    EXPECT_EQ(trace(),
              (Lines{
                  "TurnBegin turn=1 source=web text=hello",
                  "Message 1",
                  "Message 2",
                  "State thinking",
                  "State error",
                  "Message 3",
                  "Message 4",
                  "State idle",
                  "TurnEnd turn=1 status=error reason=no_model note=" + zh(StrId::AgentErrNoModel),
              }));
    EXPECT_EQ(transcript(), (Lines{
                                "1 marker t1  marker=turn_begin status=",
                                "2 user t1 hello",
                                "3 note t1 " + zh(StrId::AgentErrNoModel),
                                "4 marker t1  marker=turn_end status=error",
                            }));
}

}  // namespace
