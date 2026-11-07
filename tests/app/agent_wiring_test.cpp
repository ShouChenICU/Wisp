// The agent's WebSocket frames (agent-core spec 7.2): AgentWsBridge turns
// AgentService events into {"t":"agent",...} frames, coalescing reply
// text.

#include "agent_wiring.hpp"

#include <gtest/gtest.h>

#include <mutex>
#include <optional>
#include <string>
#include <vector>

#include "support/wait.hpp"
#include "wisp/agent/agent_service.hpp"
#include "wisp/agent/conversation.hpp"
#include "wisp/base/json.hpp"
#include "wisp/os/os.hpp"

using wisp::Value;
using wisp::agent::AgentEvent;
using wisp::agent::AgentState;
using wisp::agent::Message;
using wisp::agent::Role;
using wisp::app::AgentWsBridge;
using wisp::test::wait_until;
using Kind = AgentEvent::Kind;

namespace {

struct Sink {
    std::mutex mu;
    std::vector<Value> frames;

    AgentWsBridge::Send send() {
        return [this](std::string json) {
            auto v = wisp::from_json(json);
            EXPECT_TRUE(v) << json;
            std::lock_guard<std::mutex> lock(mu);
            frames.push_back(v ? v.value() : Value());
        };
    }
    std::vector<Value> snapshot() {
        std::lock_guard<std::mutex> lock(mu);
        return frames;
    }
};

std::string str(const Value& v, const char* key) {
    const Value* f = v.find(key);
    return f != nullptr && f->type() == Value::Type::String ? f->as_string() : std::string();
}

AgentEvent event(Kind k) {
    AgentEvent e;
    e.kind = k;
    return e;
}

std::optional<Message> find_none(uint64_t) { return std::nullopt; }

size_t code_points(const std::string& s) {
    size_t n = 0;
    for (char c : s) n += (static_cast<unsigned char>(c) & 0xC0) != 0x80 ? 1 : 0;
    return n;
}

std::string repeat(const std::string& s, int n) {
    std::string out;
    for (int i = 0; i < n; ++i) out += s;
    return out;
}

}  // namespace

TEST(AgentWiring, WsFramesFollowSpecShapes) {
    Sink sink;
    const std::string long_args(300, 'a');
    Message stored;
    stored.seq = 7;
    stored.role = Role::Summary;
    stored.covers_first = 1;
    stored.covers_last = 4;
    AgentWsBridge bridge(
        sink.send(),
        [&](uint64_t seq) -> std::optional<Message> {
            if (seq == 7) return stored;
            return std::nullopt;
        },
        [] { return uint64_t{3}; });

    AgentEvent delta = event(Kind::Delta);
    delta.text = "hi";
    bridge.on_event(delta);

    AgentEvent start = event(Kind::ToolStart);
    start.tool = "scene.install";
    start.args = repeat("汉", 200);
    bridge.on_event(start);

    AgentEvent end = event(Kind::ToolEnd);
    end.tool = "scene.install";
    end.result = std::string(200, 'r');
    end.ok = false;
    bridge.on_event(end);

    AgentEvent state = event(Kind::State);
    state.state = AgentState::Tool;
    state.tool = "scene.install";
    bridge.on_event(state);

    AgentEvent asking = event(Kind::Confirm);
    asking.text = "Delete?";
    bridge.on_event(asking);
    bridge.on_event(event(Kind::Confirm));  // text "": over

    AgentEvent msg = event(Kind::Message);
    msg.seq = 7;
    bridge.on_event(msg);
    AgentEvent unknown = event(Kind::Message);
    unknown.seq = 99;  // gone already: no frame
    bridge.on_event(unknown);

    AgentEvent cleared = event(Kind::Cleared);
    cleared.ok = true;
    bridge.on_event(cleared);

    bridge.on_event(event(Kind::TurnBegin));  // no frame
    bridge.on_event(event(Kind::TurnEnd));    // no frame

    const std::vector<Value> f = sink.snapshot();
    ASSERT_EQ(f.size(), 8u);
    for (const Value& v : f) EXPECT_EQ(str(v, "t"), "agent");

    EXPECT_EQ(str(f[0], "k"), "delta");
    EXPECT_EQ(str(f[0], "text"), "hi");

    EXPECT_EQ(str(f[1], "k"), "tool");
    EXPECT_EQ(str(f[1], "phase"), "start");
    EXPECT_EQ(str(f[1], "name"), "scene.install");
    EXPECT_LE(code_points(str(f[1], "args")), 200u);

    EXPECT_EQ(str(f[2], "phase"), "end");
    EXPECT_LE(code_points(str(f[2], "result")), 200u);
    ASSERT_NE(f[2].find("ok"), nullptr);
    EXPECT_FALSE(f[2].find("ok")->as_bool());

    EXPECT_EQ(str(f[3], "k"), "state");
    EXPECT_EQ(str(f[3], "state"), "tool");
    EXPECT_EQ(str(f[3], "tool"), "scene.install");

    EXPECT_EQ(str(f[4], "k"), "confirm");
    EXPECT_EQ(str(f[4], "text"), "Delete?");
    ASSERT_NE(f[5].find("text"), nullptr);
    EXPECT_EQ(f[5].find("text")->type(), Value::Type::Null);

    EXPECT_EQ(str(f[6], "k"), "msg");
    const Value* m = f[6].find("msg");
    ASSERT_NE(m, nullptr);
    EXPECT_EQ(m->find("seq")->as_int(), 7);
    EXPECT_EQ(str(*m, "role"), "summary");
    EXPECT_TRUE(m->find("hidden")->as_bool());
    EXPECT_TRUE(m->find("dropped")->as_bool());

    EXPECT_EQ(str(f[7], "k"), "cleared");
    EXPECT_TRUE(f[7].find("ok")->as_bool());
    EXPECT_EQ(f[7].find("epoch")->as_int(), 3);
}

// A call still being written: its name and its bytes so far, nothing of
// the arguments.
TEST(AgentWiring, WsToolPrepFrame) {
    Sink sink;
    AgentWsBridge bridge(sink.send(), find_none, [] { return uint64_t{0}; });
    AgentEvent prep = event(Kind::ToolPrep);
    prep.tool = "scene.write_file";
    prep.bytes = 3277;
    bridge.on_event(prep);
    const std::vector<Value> f = sink.snapshot();
    ASSERT_EQ(f.size(), 1u);
    EXPECT_EQ(str(f[0], "t"), "agent");
    EXPECT_EQ(str(f[0], "k"), "tool");
    EXPECT_EQ(str(f[0], "phase"), "prep");
    EXPECT_EQ(str(f[0], "name"), "scene.write_file");
    ASSERT_NE(f[0].find("bytes"), nullptr);
    EXPECT_EQ(f[0].find("bytes")->as_int(), 3277);
    EXPECT_EQ(f[0].find("args"), nullptr);
}

TEST(AgentWiring, DeltasCoalescedPer100ms) {
    Sink sink;
    AgentWsBridge bridge(sink.send(), find_none, [] { return uint64_t{1}; });
    std::string all;
    for (int i = 0; i < 50; ++i) {
        AgentEvent d = event(Kind::Delta);
        d.text = std::to_string(i) + ",";
        all += d.text;
        bridge.on_event(d);
        wisp::os::sleep_ms(1);
    }
    // The rest follows from the timer, without another event.
    auto joined = [&] {
        std::string text;
        for (const Value& v : sink.snapshot()) text += str(v, "text");
        return text;
    };
    ASSERT_TRUE(wait_until([&] { return joined() == all; }, 2000)) << joined();
    EXPECT_LE(sink.snapshot().size(), 2u);

    // Text still waiting leaves before the next frame of another kind.
    AgentEvent d = event(Kind::Delta);
    d.text = "tail";
    bridge.on_event(d);
    AgentEvent s = event(Kind::State);
    s.state = AgentState::Idle;
    bridge.on_event(s);
    const std::vector<Value> f = sink.snapshot();
    ASSERT_GE(f.size(), 2u);
    EXPECT_EQ(str(f[f.size() - 2], "k"), "delta");
    EXPECT_EQ(str(f.back(), "k"), "state");
    std::string text;
    for (const Value& v : f) text += str(v, "text");
    EXPECT_EQ(text, all + "tail");
}
