// The agent's turn internals (components/wisp_agent/src): Calibration, the
// token estimate's multiplier kept per (profile id, model) pair (agent-core
// spec 3.3).

#include "turn_runner.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "calibration.hpp"
#include "reasoning_keep.hpp"
#include "support/agent_fixture.hpp"
#include "support/fake_provider.hpp"
#include "wisp/agent/agent_service.hpp"
#include "wisp/agent/tool_registry.hpp"
#include "wisp/base/value.hpp"
#include "wisp/net/request_watch.hpp"
#include "wisp/ui/strings.hpp"

using wisp::agent::Calibration;
using wisp::agent::ReasoningKeep;

TEST(ReasoningKeep, DropsTheOldestPastTheCap) {
    ReasoningKeep keep(10);
    const auto seqs = [&] {
        std::vector<uint64_t> v;
        for (const wisp::agent::ReasoningRef& r : keep.refs()) v.push_back(r.seq);
        return v;
    };
    keep.add(1, "aaaa");
    keep.add(2, "bbbb");
    keep.add(3, "cccc");
    EXPECT_EQ(seqs(), (std::vector<uint64_t>{2, 3}));
    EXPECT_EQ(keep.bytes(), 8u);
    ASSERT_EQ(keep.refs().size(), 2u);
    EXPECT_EQ(keep.refs()[0].text, "bbbb");
    EXPECT_EQ(keep.refs()[1].text, "cccc");
    // Larger than the cap alone: not kept, and nothing else goes.
    keep.add(4, "01234567890");
    keep.add(0, "x");
    keep.add(5, "");
    EXPECT_EQ(seqs(), (std::vector<uint64_t>{2, 3}));
    EXPECT_EQ(keep.bytes(), 8u);
}

TEST(Calibration, StartsAt100) {
    Calibration cal;
    EXPECT_EQ(cal.pct(), 100u);
    EXPECT_EQ(cal.scaled(300), 300u);
}

TEST(Calibration, ObserveSetsTheClampedRatio) {
    Calibration cal;
    cal.observe("p", "m", 2002, 1001);
    EXPECT_EQ(cal.pct(), 200u);
    EXPECT_EQ(cal.scaled(300), 600u);
    cal.observe("p", "m", 10, 1000);
    EXPECT_EQ(cal.pct(), Calibration::kMinPct);
    cal.observe("p", "m", 10000, 1000);
    EXPECT_EQ(cal.pct(), Calibration::kMaxPct);
    // An estimate of 0 counts as 1.
    cal.observe("p", "m", 5, 0);
    EXPECT_EQ(cal.pct(), Calibration::kMaxPct);
}

TEST(Calibration, OverflowRaisesToAtLeast150) {
    Calibration cal;
    cal.overflow("p", "m");
    EXPECT_EQ(cal.pct(), Calibration::kOverflowPct);
    cal.observe("p", "m", 2002, 1001);
    cal.overflow("p", "m");
    EXPECT_EQ(cal.pct(), 200u);
}

TEST(Calibration, SamePairKeepsItAcrossTurns) {
    Calibration cal;
    cal.observe("p", "m", 2002, 1001);
    cal.begin_turn("p", "m");
    EXPECT_EQ(cal.pct(), 200u);
}

TEST(Calibration, AnotherModelStartsAgain) {
    Calibration cal;
    cal.observe("p", "m", 2002, 1001);
    cal.begin_turn("p", "m2");
    EXPECT_EQ(cal.pct(), 100u);
}

TEST(Calibration, AnotherProfileWithTheSameModelStartsAgain) {
    Calibration cal;
    cal.observe("p", "m", 2002, 1001);
    cal.begin_turn("q", "m");
    EXPECT_EQ(cal.pct(), 100u);
}

// A turn with no active model has the key ("", ""), and it holds like any
// other pair.
TEST(Calibration, NoModelIsAPairToo) {
    Calibration cal;
    cal.observe("", "", 2002, 1001);
    cal.begin_turn("", "");
    cal.begin_turn("", "");
    EXPECT_EQ(cal.pct(), 200u);
}

TEST(Calibration, UnscaledWindow) {
    Calibration cal;
    EXPECT_EQ(cal.unscaled_window(4096), 4096u);
    cal.observe("p", "m", 2002, 1001);
    EXPECT_EQ(cal.unscaled_window(4096), 2048u);
    // The intermediate does not overflow 32 bits.
    cal.observe("p", "m", 10000, 1000);
    EXPECT_EQ(cal.unscaled_window(4000000000u), 1333333333u);
}

// TurnRunner: one turn against a TurnEnv (spec 3.10), with a scripted
// provider. The fixture lives outside namespace wisp::agent, whose
// TurnRunner class has the same name.

namespace {

namespace agent = wisp::agent;
namespace net = wisp::net;
using agent::Abort;
using agent::AgentEvent;
using agent::AgentService;
using agent::AgentState;
using agent::Message;
using agent::Outcome;
using agent::Role;
using agent::TurnInput;
using wisp::Map;
using wisp::Value;
using wisp::test::AgentHarness;
using wisp::test::FakeProvider;
using wisp::test::FakeStep;
using wisp::ui::StrId;
using Lines = std::vector<std::string>;

std::string str(const wisp::os::LargeString& s) { return {s.data(), s.size()}; }

std::string zh(StrId id) { return wisp::ui::tr(id, wisp::ui::Lang::Zh); }

// Records what the runner asks of the service, one trace line per call.
class FakeTurnEnv final : public agent::TurnEnv {
public:
    explicit FakeTurnEnv(AgentHarness& h) : h_(h) {}

    uint64_t append(Message m) override {
        trace.push_back(std::string("append ") + agent::to_string(m.role));
        wisp::Result<uint64_t> r = wisp::Result<uint64_t>::err(wisp::ErrorCode::Busy, "refused");
        h_.worker.run_sync([&] { r = h_.store->append(std::move(m)); }, wisp::os::kForever,
                           wisp::core::IoLane::Interactive);
        return r ? r.value() : 0;
    }
    void note(StrId id) override { note_text(wisp::ui::tr(id, lang())); }
    void note_text(std::string_view t) override {
        trace.push_back("note " + std::string(t));
        Message m;
        m.role = Role::Note;
        m.turn = turn;
        m.text.assign(t);
        append(std::move(m));
    }
    void set_state(AgentState s, std::string_view tool = {}) override {
        trace.push_back(std::string("state ") + agent::to_string(s) +
                        (tool.empty() ? "" : " " + std::string(tool)));
    }
    void emit(const AgentEvent& e) override {
        events.push_back(e);
        switch (e.kind) {
            case AgentEvent::Kind::Delta:
                trace.push_back("Delta " + e.text);
                break;
            case AgentEvent::Kind::ToolStart:
                trace.push_back("ToolStart " + e.tool);
                break;
            case AgentEvent::Kind::ToolEnd:
                trace.push_back("ToolEnd " + e.tool + (e.ok ? " ok" : " failed"));
                break;
            case AgentEvent::Kind::ToolPrep:
                trace.push_back("ToolPrep " + e.tool);
                break;
            case AgentEvent::Kind::Message:
                trace.push_back("Message");
                break;
            default:
                trace.push_back("event");
                break;
        }
    }
    bool aborted() override { return aborted_flag; }
    Abort abort_reason() const override { return reason; }
    bool network_up() const override { return net; }
    wisp::ui::Lang lang() const override { return wisp::ui::Lang::Zh; }
    agent::TurnControls& controls() override { return ctl; }
    uint64_t now_ms() const override { return clock; }

    // The index of the first trace line equal to `line`, or -1.
    int index(const std::string& line) const {
        for (size_t i = 0; i < trace.size(); ++i)
            if (trace[i] == line) return static_cast<int>(i);
        return -1;
    }

    Lines trace;
    std::vector<AgentEvent> events;
    Abort reason = Abort::None;
    bool aborted_flag = false;
    bool net = true;
    uint64_t clock = 1000000;
    uint32_t turn = 1;
    agent::TurnControls ctl;

private:
    AgentHarness& h_;
};

// Everything a runner needs: the store (loaded), a scripted provider, the
// calibration and the env.
struct Rig {
    Rig() {
        EXPECT_TRUE(h.worker.run_sync([&] { EXPECT_TRUE(h.store->load()); }, wisp::os::kForever));
    }

    TurnInput input() const {
        return {.turn = 1,
                .serial = 1,
                .max_steps = 8,
                .start_ms = env.clock,
                .turn_ms = 600000,
                .source = agent::Source::Web};
    }

    void add_tool(const std::string& name, bool ok = true) {
        agent::ToolDef def;
        def.name = name;
        def.description = "test tool";
        def.run = [ok](const Value&, agent::ToolCtx&) {
            if (!ok) return agent::tool_error("boom", "it failed");
            return agent::ToolOutput{.ok = true, .result = Map{{"ok", true}}};
        };
        ASSERT_TRUE(h.tools->add(std::move(def)));
    }

    // Appends the user's message, then runs turn in.turn.
    Outcome run(TurnInput in, const std::string& user = "hi") {
        env.turn = in.turn;
        Message m;
        m.role = Role::User;
        m.turn = in.turn;
        m.text.assign(user);
        env.append(std::move(m));
        return agent::TurnRunner(fake, *h.tools, *h.store, context, cal, env).run(in);
    }
    Outcome run() { return run(input()); }

    std::optional<Message> last(Role r) {
        const auto hist = h.store->history();
        for (auto it = hist->rbegin(); it != hist->rend(); ++it)
            if (it->role == r) return *it;
        return std::nullopt;
    }

    AgentHarness h;
    FakeProvider fake;
    agent::Calibration cal;
    FakeTurnEnv env{h};
    agent::ContextSource context;
};

FakeStep text_step(const std::string& text) {
    FakeStep s;
    s.deltas = {text};
    s.result.finish = net::LlmFinish::Stop;
    return s;
}

FakeStep error_step(net::LlmError e) {
    FakeStep s;
    s.result.error = e;
    return s;
}

class TurnRunner : public ::testing::Test {
protected:
    // A new rig in place of the old one: two harnesses cannot run at once
    // (each starts its own flash thread).
    Rig& fresh() {
        r.reset();
        r = std::make_unique<Rig>();
        return *r;
    }

    std::unique_ptr<Rig> r = std::make_unique<Rig>();
};

TEST_F(TurnRunner, PlainReplyEndsDone) {
    r->fake.push(text_step("hi"));
    const Outcome out = r->run();
    EXPECT_STREQ(out.status, "done");
    EXPECT_STREQ(out.reason, "");
    EXPECT_EQ(str(out.last_text), "hi");
    const int thinking = r->env.index("state thinking");
    const int output = r->env.index("state output");
    const int delta = r->env.index("Delta hi");
    int stored = -1;
    for (size_t i = 0; i < r->env.trace.size(); ++i)
        if (r->env.trace[i] == "append assistant") stored = static_cast<int>(i);
    ASSERT_GE(thinking, 0);
    EXPECT_LT(thinking, output);
    EXPECT_LT(output, delta);
    EXPECT_LT(delta, stored);
    const auto calls = r->fake.calls();
    ASSERT_EQ(calls.size(), 1u);
    EXPECT_EQ(calls[0].tools_json, str(r->h.tools->tools_json()));
}

TEST_F(TurnRunner, ToolCallFeedsTheResultBack) {
    r->add_tool("test.echo");
    FakeStep call;
    call.calls.push_back({.id = "c1", .name = "test_echo", .arguments = "{}"});
    call.result.finish = net::LlmFinish::ToolCalls;
    r->fake.push(std::move(call));
    r->fake.push(text_step("ok"));
    const Outcome out = r->run();
    EXPECT_STREQ(out.status, "done");
    EXPECT_EQ(out.steps, 1u);
    const auto calls = r->fake.calls();
    ASSERT_EQ(calls.size(), 2u);
    EXPECT_NE(calls[1].body.find("\"role\":\"tool\",\"tool_call_id\":\"c1\""), std::string::npos)
        << calls[1].body;
}

// A tool that fails and a name no tool has both answer with an error
// result the next request carries, and count as steps.
TEST_F(TurnRunner, FailingAndUnknownToolsAnswerWithErrors) {
    r->add_tool("test.fail", /*ok=*/false);
    FakeStep call;
    call.calls.push_back({.id = "c1", .name = "test_fail", .arguments = "{}"});
    call.calls.push_back({.id = "c2", .name = "no_such_tool", .arguments = "{}"});
    call.result.finish = net::LlmFinish::ToolCalls;
    r->fake.push(std::move(call));
    r->fake.push(text_step("sorry"));
    const Outcome out = r->run();
    EXPECT_STREQ(out.status, "done");
    EXPECT_EQ(out.steps, 2u);
    EXPECT_GE(r->env.index("ToolEnd test.fail failed"), 0);
    EXPECT_GE(r->env.index("ToolEnd no_such_tool failed"), 0);

    std::vector<Message> results;
    for (const Message& m : *r->h.store->history())
        if (m.role == Role::Tool) results.push_back(m);
    ASSERT_EQ(results.size(), 2u);
    EXPECT_FALSE(results[0].ok);
    EXPECT_EQ(str(results[0].tool), "test.fail");
    EXPECT_NE(str(results[0].text).find("boom"), std::string::npos);
    EXPECT_FALSE(results[1].ok);
    EXPECT_EQ(str(results[1].tool), "no_such_tool");
    EXPECT_NE(str(results[1].text).find("unknown_tool"), std::string::npos);

    const auto calls = r->fake.calls();
    ASSERT_EQ(calls.size(), 2u);
    EXPECT_NE(calls[1].body.find("\"tool_call_id\":\"c1\""), std::string::npos);
    EXPECT_NE(calls[1].body.find("\"tool_call_id\":\"c2\""), std::string::npos);
}

TEST_F(TurnRunner, ErrorsMapToReasons) {
    struct Row {
        net::LlmError error;
        bool body;
        const char* reason;
        StrId note;
    };
    const Row rows[] = {
        {net::LlmError::NoModel, false, "no_model", StrId::AgentErrNoModel},
        {net::LlmError::Unauthorized, false, "unauthorized", StrId::AgentErrUnauthorized},
        {net::LlmError::Transport, false, "network", StrId::AgentErrNetwork},
        {net::LlmError::Transport, true, "network_lost", StrId::AgentErrNetworkLost},
        {net::LlmError::Server, false, "server", StrId::AgentErrServer},
        {net::LlmError::Http, false, "server", StrId::AgentErrServer},
        {net::LlmError::BadStream, false, "server", StrId::AgentErrServer},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.reason);
        Rig& rig = fresh();
        FakeStep s = error_step(row.error);
        if (row.body) s.deltas = {"x"};
        rig.fake.push(std::move(s));
        const Outcome out = rig.run();
        EXPECT_STREQ(out.status, "error");
        EXPECT_STREQ(out.reason, row.reason);
        EXPECT_EQ(str(out.last_text), zh(row.note));
        const int error = rig.env.index("state error");
        const int note = rig.env.index("note " + zh(row.note));
        ASSERT_GE(error, 0);
        EXPECT_LT(error, note);
        const auto reply = rig.last(Role::Assistant);
        if (row.body) {
            ASSERT_TRUE(reply);
            EXPECT_EQ(str(reply->text), "x");
        } else {
            EXPECT_FALSE(reply);
        }
    }
}

TEST_F(TurnRunner, AbortReasonsMapToStatuses) {
    struct Row {
        Abort why;
        const char* status;
        const char* reason;
        std::string note;
        bool error_state;
    };
    const Row rows[] = {
        {Abort::Time, "time_limit", "time_limit",
         wisp::ui::tr_fill(StrId::AgentTimeLimit, wisp::ui::Lang::Zh, "10"), false},
        {Abort::Network, "error", "network_lost", zh(StrId::AgentErrNetworkLost), true},
        {Abort::Stall, "error", "network_lost", zh(StrId::AgentErrNetworkLost), true},
        {Abort::Stop, "stopped", "stopped", zh(StrId::AgentStopped), false},
    };
    for (const Row& row : rows) {
        SCOPED_TRACE(row.reason);
        Rig& rig = fresh();
        rig.env.reason = row.why;
        rig.env.aborted_flag = true;
        const Outcome out = rig.run();
        EXPECT_STREQ(out.status, row.status);
        EXPECT_STREQ(out.reason, row.reason);
        EXPECT_EQ(str(out.last_text), row.note);
        EXPECT_GE(rig.env.index("note " + row.note), 0);
        EXPECT_EQ(rig.env.index("state error") >= 0, row.error_state);
        EXPECT_TRUE(rig.fake.calls().empty());
    }
}

TEST_F(TurnRunner, NetworkDownBeforeARequest) {
    r->env.net = false;
    const Outcome out = r->run();
    EXPECT_STREQ(out.status, "error");
    EXPECT_STREQ(out.reason, "network");
    EXPECT_GE(r->env.index("note " + zh(StrId::AgentErrNetwork)), 0);
    EXPECT_TRUE(r->fake.calls().empty());
}

TEST_F(TurnRunner, ReadsTheModelOncePerTurn) {
    r->add_tool("test.echo");
    FakeStep call;
    call.calls.push_back({.id = "c1", .name = "test_echo", .arguments = "{}"});
    call.result.finish = net::LlmFinish::ToolCalls;
    r->fake.push(std::move(call));
    r->fake.push(text_step("ok"));
    r->run();
    EXPECT_EQ(r->fake.calls().size(), 2u);
    EXPECT_EQ(r->fake.active_model_calls(), 1);
}

// The reply's usage calibrates under the profile and model that answered.
TEST_F(TurnRunner, UsageCalibratesUnderTheAnsweringProfile) {
    r->fake.set_model(net::LlmModelInfo{
        .profile_id = "p9", .model = "m9", .context_window = 65536, .vision = false});
    FakeStep s = text_step("hi");
    s.result.usage = net::LlmUsage{.prompt = 100000000, .completion = 1, .total = 100000001};
    r->fake.push(std::move(s));
    const Outcome out = r->run();
    EXPECT_STREQ(out.status, "done");
    EXPECT_EQ(out.tokens, 100000001u);
    EXPECT_EQ(r->cal.pct(), agent::Calibration::kMaxPct);
    r->cal.begin_turn("p9", "m9");
    EXPECT_EQ(r->cal.pct(), agent::Calibration::kMaxPct);
    r->cal.begin_turn("p1", "m9");
    EXPECT_EQ(r->cal.pct(), 100u);
}

// A context_length refusal raises the calibration under the answering
// profile, compresses the earlier turns and tries once more.
TEST_F(TurnRunner, ContextRefusalCompressesAndRetriesOnce) {
    r->fake.set_model(net::LlmModelInfo{.profile_id = "p1", .model = "m", .context_window = 4096});
    const std::string big(3000, 'a');
    for (uint32_t t = 1; t <= 2; ++t) {
        r->fake.push(text_step(big));
        TurnInput in = r->input();
        in.turn = t;
        ASSERT_STREQ(r->run(in, "q" + std::to_string(t)).status, "done");
    }
    r->fake.push(error_step(net::LlmError::ContextLength));
    r->fake.push(text_step("S"));
    r->fake.push(text_step("ok"));
    TurnInput in = r->input();
    in.turn = 3;
    const Outcome out = r->run(in, "q3");
    EXPECT_STREQ(out.status, "done");
    EXPECT_EQ(str(out.last_text), "ok");
    const auto calls = r->fake.calls();
    ASSERT_EQ(calls.size(), 5u);
    EXPECT_EQ(calls[3].tools_json, "");  // the summary request
    const auto summary = r->last(Role::Summary);
    ASSERT_TRUE(summary);
    EXPECT_EQ(str(summary->text), "S");
    EXPECT_EQ(r->cal.pct(), agent::Calibration::kOverflowPct);
    r->cal.begin_turn("p1", "m");
    EXPECT_EQ(r->cal.pct(), agent::Calibration::kOverflowPct);
}

// A second refusal in the same turn is not retried.
TEST_F(TurnRunner, SecondContextRefusalEndsTheTurn) {
    r->fake.set_model(net::LlmModelInfo{.profile_id = "p1", .model = "m", .context_window = 4096});
    const std::string big(3000, 'a');
    for (uint32_t t = 1; t <= 2; ++t) {
        r->fake.push(text_step(big));
        TurnInput in = r->input();
        in.turn = t;
        ASSERT_STREQ(r->run(in, "q" + std::to_string(t)).status, "done");
    }
    r->fake.push(error_step(net::LlmError::ContextLength));
    r->fake.push(text_step("S"));
    r->fake.push(error_step(net::LlmError::ContextLength));
    TurnInput in = r->input();
    in.turn = 3;
    const Outcome out = r->run(in, "q3");
    EXPECT_STREQ(out.status, "error");
    EXPECT_STREQ(out.reason, "context");
    EXPECT_EQ(str(out.last_text), zh(StrId::AgentErrContext));
    EXPECT_EQ(r->fake.calls().size(), 5u);
}

TEST_F(TurnRunner, RequestTimeoutLeavesTheMarginPastTheTurn) {
    {
        r->fake.push(text_step("a"));
        TurnInput in = r->input();
        in.start_ms = r->env.clock - 1000;
        in.turn_ms = 60000;
        r->run(in);
    }
    {
        r->fake.push(text_step("b"));
        TurnInput in = r->input();
        in.turn = 2;
        r->run(in);
    }
    const auto calls = r->fake.calls();
    ASSERT_EQ(calls.size(), 2u);
    EXPECT_EQ(calls[0].timeout_ms, 59000u + AgentService::kDeadlineMarginMs);
    EXPECT_EQ(calls[1].timeout_ms, AgentService::kRequestMs);
}

TEST_F(TurnRunner, ControlsFollowTheRequest) {
    std::optional<bool> in_request;
    std::optional<uint64_t> last_byte;
    r->env.clock += 5000;
    FakeStep s = text_step("hi");
    s.during = [&](const net::LlmCall&) {
        in_request = r->env.ctl.in_request.load();
        last_byte = r->env.ctl.last_byte.load();
    };
    r->fake.push(std::move(s));
    r->run();
    EXPECT_EQ(in_request, true);
    EXPECT_EQ(last_byte, r->env.clock);
    EXPECT_FALSE(r->env.ctl.in_request.load());
}

// A stop that came before the request was armed aborts it as soon as it is.
TEST_F(TurnRunner, ArmedAfterAStopAbortsAtOnce) {
    wisp::os::LargeUnique<net::RequestWatch> watch = net::RequestWatch::create();
    ASSERT_TRUE(watch);
    r->env.ctl.watch = watch.get();
    r->env.reason = Abort::Stop;
    std::optional<net::RequestWatch::Reason> seen;
    FakeStep s = text_step("hi");
    s.during = [&](const net::LlmCall& call) {
        ASSERT_EQ(call.watch, watch.get());
        const uint32_t t = call.watch->arm(1000);
        call.on_armed(t);
        seen = call.watch->reason();
        EXPECT_EQ(r->env.ctl.ticket.load(), t);
        call.watch->disarm();
    };
    r->fake.push(std::move(s));
    r->run();
    EXPECT_EQ(seen, net::RequestWatch::Reason::Aborted);
}

net::LlmModelInfo with_dialect(wisp::core::LlmDialect d) {
    return {.profile_id = "p1", .model = "m", .context_window = 65536, .dialect = d};
}

// Turn 1: a tool call with reasoning "r1", then a reply.
void push_reasoned_call(Rig& rig) {
    rig.add_tool("test.echo");
    FakeStep call;
    call.calls.push_back({.id = "c1", .name = "test_echo", .arguments = "{}"});
    call.result.finish = net::LlmFinish::ToolCalls;
    call.result.reasoning.assign("r1");
    rig.fake.push(std::move(call));
    rig.fake.push(text_step("ok"));
}

TEST_F(TurnRunner, ReasoningEchoedOnlyWithinTheTurn) {
    r->fake.set_model(with_dialect({.reasoning = wisp::core::LlmReasoning::Echo}));
    push_reasoned_call(*r);
    EXPECT_STREQ(r->run().status, "done");
    auto calls = r->fake.calls();
    ASSERT_EQ(calls.size(), 2u);
    EXPECT_NE(calls[1].body.find("\"reasoning_content\":\"r1\""), std::string::npos)
        << calls[1].body;

    // The next turn (a new runner) sends none of it.
    r->fake.push(text_step("x"));
    TurnInput in = r->input();
    in.turn = 2;
    EXPECT_STREQ(r->run(in).status, "done");
    calls = r->fake.calls();
    ASSERT_EQ(calls.size(), 3u);
    EXPECT_EQ(calls[2].body.find("reasoning_content"), std::string::npos) << calls[2].body;

    // Reasoning is never stored.
    for (const Message& m : *r->h.store->history()) {
        EXPECT_EQ(str(m.text).find("r1"), std::string::npos) << str(m.text);
        for (const agent::ToolCallRec& c : m.calls)
            EXPECT_EQ(str(c.args).find("r1"), std::string::npos);
    }
}

TEST_F(TurnRunner, ReasoningNotKeptUnderDrop) {
    push_reasoned_call(*r);
    EXPECT_STREQ(r->run().status, "done");
    const auto calls = r->fake.calls();
    ASSERT_EQ(calls.size(), 2u);
    EXPECT_EQ(calls[1].body.find("reasoning_content"), std::string::npos) << calls[1].body;
}

// The connection dropped after finish_reason: the calls run, but the
// attempt was cut, so its reasoning is not echoed (plan decision 7).
TEST_F(TurnRunner, ReasoningOfACutAttemptIsNotKept) {
    r->fake.set_model(with_dialect({.reasoning = wisp::core::LlmReasoning::Echo}));
    r->add_tool("test.echo");
    FakeStep call;
    call.deltas = {"t"};
    call.calls.push_back({.id = "c1", .name = "test_echo", .arguments = "{}"});
    call.result.finish = net::LlmFinish::ToolCalls;
    call.result.error = net::LlmError::Transport;
    call.result.reasoning.assign("r1");
    r->fake.push(std::move(call));
    r->fake.push(text_step("ok"));
    const Outcome out = r->run();
    EXPECT_STREQ(out.status, "done");
    EXPECT_EQ(out.steps, 1u);
    const auto calls = r->fake.calls();
    ASSERT_EQ(calls.size(), 2u);
    EXPECT_NE(calls[1].body.find("\"tool_call_id\":\"c1\""), std::string::npos) << calls[1].body;
    EXPECT_EQ(calls[1].body.find("reasoning_content"), std::string::npos) << calls[1].body;
}

TEST_F(TurnRunner, PromptFollowsTheDialect) {
    r->context = [](agent::Source) { return agent::DeviceSnapshot{}; };
    r->fake.set_model(with_dialect({.system_msgs = wisp::core::LlmSystemMsgs::First}));
    r->fake.push(text_step("hi"));
    EXPECT_STREQ(r->run().status, "done");
    const auto calls = r->fake.calls();
    ASSERT_EQ(calls.size(), 1u);
    const std::string& body = calls[0].body;
    EXPECT_NE(body.find("The <context> block at the start of the latest user message"),
              std::string::npos)
        << body;
    const std::string system = "\"role\":\"system\"";
    const size_t at = body.find(system);
    ASSERT_NE(at, std::string::npos);
    EXPECT_EQ(body.find(system, at + 1), std::string::npos) << body;
}

// Two steps' reasoning is echoed in order, each on its own reply.
TEST_F(TurnRunner, TwoStepsReasoningEchoedInOrder) {
    r->fake.set_model(with_dialect({.reasoning = wisp::core::LlmReasoning::Echo}));
    r->add_tool("test.echo");
    for (const char* id : {"c1", "c2"}) {
        FakeStep call;
        call.calls.push_back({.id = id, .name = "test_echo", .arguments = "{}"});
        call.result.finish = net::LlmFinish::ToolCalls;
        call.result.reasoning.assign(std::string(id) == "c1" ? "r1" : "r2");
        r->fake.push(std::move(call));
    }
    r->fake.push(text_step("ok"));
    EXPECT_STREQ(r->run().status, "done");
    const auto calls = r->fake.calls();
    ASSERT_EQ(calls.size(), 3u);
    const std::string& body = calls[2].body;
    const size_t r1 = body.find("\"reasoning_content\":\"r1\"");
    const size_t r2 = body.find("\"reasoning_content\":\"r2\"");
    ASSERT_NE(r1, std::string::npos) << body;
    ASSERT_NE(r2, std::string::npos) << body;
    EXPECT_LT(r1, r2);
    const std::string assistant = "\"role\":\"assistant\"";
    const size_t a1 = body.find(assistant);
    const size_t a2 = body.find(assistant, a1 + 1);
    ASSERT_NE(a2, std::string::npos) << body;
    EXPECT_LT(a1, r1);
    EXPECT_LT(r1, a2);
    EXPECT_LT(a2, r2);
}

// Spec 3.7: a reply cut by the length limit is continued once.

const std::string kContinue =
    "Your previous reply was cut off by the length limit. Continue exactly where it stopped. "
    "Do not repeat anything.";

FakeStep cut_step(const std::string& text) {
    FakeStep s;
    if (!text.empty()) s.deltas = {text};
    s.result.finish = net::LlmFinish::Length;
    return s;
}

std::vector<Message> stored(Rig& rig, Role role) {
    std::vector<Message> out;
    for (const Message& m : *rig.h.store->history())
        if (m.role == role) out.push_back(m);
    return out;
}

TEST_F(TurnRunner, LengthReplyIsContinuedOnce) {
    FakeStep a = cut_step("Hel");
    a.result.usage = net::LlmUsage{.prompt = 0, .completion = 0, .total = 10};
    r->fake.push(std::move(a));
    FakeStep b = text_step("lo");
    b.result.usage = net::LlmUsage{.prompt = 0, .completion = 0, .total = 5};
    r->fake.push(std::move(b));
    const Outcome out = r->run();
    EXPECT_STREQ(out.status, "done");
    EXPECT_EQ(out.steps, 0u);
    EXPECT_EQ(out.tokens, 15u);
    EXPECT_EQ(str(out.last_text), "Hello");
    const auto replies = stored(*r, Role::Assistant);
    ASSERT_EQ(replies.size(), 1u);
    EXPECT_EQ(str(replies[0].text), "Hello");
    EXPECT_TRUE(stored(*r, Role::Note).empty());
    int outputs = 0;
    for (const std::string& line : r->env.trace) outputs += line == "state output" ? 1 : 0;
    EXPECT_EQ(outputs, 1);
    const int hel = r->env.index("Delta Hel");
    const int lo = r->env.index("Delta lo");
    ASSERT_GE(hel, 0);
    EXPECT_LT(hel, lo);
    const auto calls = r->fake.calls();
    ASSERT_EQ(calls.size(), 2u);
    EXPECT_NE(
        calls[1].body.find(R"({"role":"assistant","content":"Hel"},{"role":"user","content":")" +
                           kContinue + R"("}])"),
        std::string::npos)
        << calls[1].body;
}

TEST_F(TurnRunner, SecondCutStoresTheTextWithANote) {
    r->fake.push(cut_step("Hel"));
    r->fake.push(cut_step("lo"));
    const Outcome out = r->run();
    EXPECT_STREQ(out.status, "done");
    EXPECT_EQ(str(out.last_text), "Hello");
    const auto replies = stored(*r, Role::Assistant);
    ASSERT_EQ(replies.size(), 1u);
    EXPECT_EQ(str(replies[0].text), "Hello");
    const auto notes = stored(*r, Role::Note);
    ASSERT_EQ(notes.size(), 1u);
    EXPECT_EQ(str(notes[0].text), zh(StrId::AgentReplyCut));
    EXPECT_LT(replies[0].seq, notes[0].seq);
    EXPECT_EQ(r->fake.calls().size(), 2u);
}

TEST_F(TurnRunner, FailedContinuationStoresTheTextWithANote) {
    r->fake.push(cut_step("Hel"));
    FakeStep b = error_step(net::LlmError::Server);
    b.deltas = {"lo"};
    r->fake.push(std::move(b));
    const Outcome out = r->run();
    EXPECT_STREQ(out.status, "done");
    EXPECT_STREQ(out.reason, "");
    EXPECT_EQ(str(out.last_text), "Hello");
    const auto replies = stored(*r, Role::Assistant);
    ASSERT_EQ(replies.size(), 1u);
    EXPECT_EQ(str(replies[0].text), "Hello");
    const auto notes = stored(*r, Role::Note);
    ASSERT_EQ(notes.size(), 1u);
    EXPECT_EQ(str(notes[0].text), zh(StrId::AgentReplyCut));
    EXPECT_EQ(r->env.index("state error"), -1);
    EXPECT_EQ(r->fake.calls().size(), 2u);
}

// Steps of the continuation that makes a tool call: "Hel" cut, then call
// c1, then "ok".
void push_continued_call(Rig& rig, const char* r1 = "", const char* r2 = "") {
    rig.add_tool("test.echo");
    FakeStep a = cut_step("Hel");
    a.result.reasoning.assign(r1);
    rig.fake.push(std::move(a));
    FakeStep b;
    b.calls.push_back({.id = "c1", .name = "test_echo", .arguments = "{}"});
    b.result.finish = net::LlmFinish::ToolCalls;
    b.result.reasoning.assign(r2);
    rig.fake.push(std::move(b));
    rig.fake.push(text_step("ok"));
}

TEST_F(TurnRunner, ContinuationToolCallsRun) {
    push_continued_call(*r);
    const Outcome out = r->run();
    EXPECT_STREQ(out.status, "done");
    EXPECT_EQ(out.steps, 1u);
    const auto replies = stored(*r, Role::Assistant);
    ASSERT_EQ(replies.size(), 2u);
    EXPECT_EQ(str(replies[0].text), "Hel");
    ASSERT_EQ(replies[0].calls.size(), 1u);
    EXPECT_EQ(str(replies[0].calls[0].id), "c1");
    EXPECT_GE(r->env.index("ToolEnd test.echo ok"), 0);
    const auto tools = stored(*r, Role::Tool);
    ASSERT_EQ(tools.size(), 1u);
    EXPECT_TRUE(tools[0].ok);
    const auto calls = r->fake.calls();
    ASSERT_EQ(calls.size(), 3u);
    EXPECT_EQ(calls[2].body.find(kContinue), std::string::npos) << calls[2].body;
}

TEST_F(TurnRunner, ContinuationReasoningJoins) {
    r->fake.set_model(with_dialect({.reasoning = wisp::core::LlmReasoning::Echo}));
    push_continued_call(*r, "r1", "r2");
    EXPECT_STREQ(r->run().status, "done");
    const auto calls = r->fake.calls();
    ASSERT_EQ(calls.size(), 3u);
    EXPECT_NE(
        calls[1].body.find(
            R"({"role":"assistant","content":"Hel","reasoning_content":"r1"},{"role":"user")"),
        std::string::npos)
        << calls[1].body;
    EXPECT_NE(calls[2].body.find(R"("reasoning_content":"r1r2")"), std::string::npos)
        << calls[2].body;
}

// The first reply counts as whole only because its connection dropped
// after finish_reason: its reasoning may be cut, so the joined reply echoes
// none, not even the continuation's (plan decision 7).
TEST_F(TurnRunner, ContinuationAfterACutAttemptEchoesNoReasoning) {
    r->fake.set_model(with_dialect({.reasoning = wisp::core::LlmReasoning::Echo}));
    r->add_tool("test.echo");
    FakeStep a = cut_step("Hel");
    a.result.error = net::LlmError::Transport;
    a.result.reasoning.assign("r1");
    r->fake.push(std::move(a));
    FakeStep b;
    b.calls.push_back({.id = "c1", .name = "test_echo", .arguments = "{}"});
    b.result.finish = net::LlmFinish::ToolCalls;
    b.result.reasoning.assign("r2");
    r->fake.push(std::move(b));
    r->fake.push(text_step("ok"));
    EXPECT_STREQ(r->run().status, "done");
    const auto calls = r->fake.calls();
    ASSERT_EQ(calls.size(), 3u);
    EXPECT_EQ(calls[1].body.find("reasoning_content"), std::string::npos) << calls[1].body;
    EXPECT_NE(calls[2].body.find("\"tool_call_id\":\"c1\""), std::string::npos) << calls[2].body;
    EXPECT_EQ(calls[2].body.find("reasoning_content"), std::string::npos) << calls[2].body;
}

// A stop already set when the cut reply returns: no continuation request,
// the text is stored and the turn ends stopped, without the cut note.
TEST_F(TurnRunner, AbortWhenTheCutReplyReturnsIsNotContinued) {
    FakeStep a = cut_step("Hel");
    a.during = [this](const net::LlmCall&) {
        r->env.reason = Abort::Stop;
        r->env.aborted_flag = true;
    };
    r->fake.push(std::move(a));
    const Outcome out = r->run();
    EXPECT_STREQ(out.status, "stopped");
    EXPECT_EQ(r->fake.calls().size(), 1u);
    const auto replies = stored(*r, Role::Assistant);
    ASSERT_EQ(replies.size(), 1u);
    EXPECT_EQ(str(replies[0].text), "Hel");
    const auto notes = stored(*r, Role::Note);
    ASSERT_EQ(notes.size(), 1u);
    EXPECT_EQ(str(notes[0].text), zh(StrId::AgentStopped));
}

// The continuation's request is not the one estimated: its prompt usage
// does not calibrate (plan decision 8).
TEST_F(TurnRunner, ContinuationUsageDoesNotCalibrate) {
    r->fake.push(cut_step("Hel"));
    FakeStep b = text_step("lo");
    b.result.usage = net::LlmUsage{.prompt = 100000000, .completion = 1, .total = 100000001};
    r->fake.push(std::move(b));
    const Outcome out = r->run();
    EXPECT_STREQ(out.status, "done");
    EXPECT_EQ(out.tokens, 100000001u);
    EXPECT_EQ(r->cal.pct(), 100u);
}

TEST_F(TurnRunner, StopDuringTheContinuationKeepsTheText) {
    r->fake.push(cut_step("Hel"));
    FakeStep b;
    b.deltas = {"l"};
    b.hold = true;
    b.during = [this](const net::LlmCall&) {
        r->env.reason = Abort::Stop;
        r->env.aborted_flag = true;
    };
    r->fake.push(std::move(b));
    const Outcome out = r->run();
    EXPECT_STREQ(out.status, "stopped");
    const auto replies = stored(*r, Role::Assistant);
    ASSERT_EQ(replies.size(), 1u);
    EXPECT_EQ(str(replies[0].text), "Hell");
    const auto notes = stored(*r, Role::Note);
    ASSERT_EQ(notes.size(), 1u);
    EXPECT_EQ(str(notes[0].text), zh(StrId::AgentStopped));
    EXPECT_EQ(r->fake.calls().size(), 2u);
}

TEST_F(TurnRunner, EmptyCutReplyIsNotContinued) {
    r->fake.push(cut_step(""));
    const Outcome out = r->run();
    EXPECT_STREQ(out.status, "done");
    EXPECT_EQ(r->fake.calls().size(), 1u);
    EXPECT_TRUE(stored(*r, Role::Assistant).empty());
    const auto notes = stored(*r, Role::Note);
    ASSERT_EQ(notes.size(), 1u);
    EXPECT_EQ(str(notes[0].text), zh(StrId::AgentReplyCut));
}

// A cut with tool calls keeps the bad_args rule; the model's next request
// is the continuation.
TEST_F(TurnRunner, LengthWithToolCallsIsNotContinued) {
    r->add_tool("test.echo");
    FakeStep a;
    a.calls.push_back({.id = "c1", .name = "test_echo", .arguments = "{\"x\":"});
    a.result.finish = net::LlmFinish::Length;
    r->fake.push(std::move(a));
    r->fake.push(text_step("ok"));
    const Outcome out = r->run();
    EXPECT_STREQ(out.status, "done");
    const auto calls = r->fake.calls();
    ASSERT_EQ(calls.size(), 2u);
    EXPECT_NE(calls[1].body.find("\"role\":\"tool\",\"tool_call_id\":\"c1\""), std::string::npos)
        << calls[1].body;
    EXPECT_NE(calls[1].body.find("bad_args"), std::string::npos) << calls[1].body;
    EXPECT_EQ(calls[1].body.find(kContinue), std::string::npos) << calls[1].body;
    EXPECT_TRUE(stored(*r, Role::Note).empty());
}

}  // namespace
