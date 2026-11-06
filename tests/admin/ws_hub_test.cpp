// WebSocket fan-out of logs and events (spec 6.2) and the /api/v1/ws
// upgrade's authentication (spec 6.3).

#include "wisp/admin/ws_hub.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <vector>

#include "admin/admin_fixture.hpp"
#include "support/fake_http.hpp"
#include "support/utf8.hpp"
#include "support/wait.hpp"
#include "wisp/base/json.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/core/log.hpp"
#include "wisp/core/wakelock.hpp"
#include "wisp/net/http.hpp"
#include "wisp/os/os.hpp"

using wisp::LogLevel;
using wisp::Map;
using wisp::Value;
using wisp::admin::WsHub;
using wisp::core::Event;
using wisp::core::EventBus;
using wisp::net::Method;
using wisp::test::AdminFixture;
using wisp::test::make_req;
using wisp::test::wait_until;
using wisp::test::with_csrf;
using wisp::test::with_token;

namespace {

// Records what the hub sends. `accept` = false makes send_text() report a
// full queue; `block_ms` makes it stall like a slow socket. The hello every
// connection starts with is kept apart (hellos()), so sent() and the
// counts cover the log and event traffic only; frames() has everything in
// order.
class FakeWsConnection : public wisp::net::WsConnection {
public:
    explicit FakeWsConnection(int id) : id_(id) {}

    int id() const override { return id_; }

    // Like the real connections, a closed one refuses every send.
    bool send_text(std::string_view text) override {
        if (closed) return false;
        ++send_calls;
        const uint32_t delay = block_ms.load();
        if (delay > 0) wisp::os::sleep_ms(delay);
        if (!accept) return false;
        std::lock_guard<std::mutex> lock(mutex_);
        frames_.emplace_back(text);
        if (text.find(R"("t":"hello")") != std::string_view::npos) {
            hellos_.emplace_back(text);
        } else {
            sent_.emplace_back(text);
        }
        return true;
    }

    void close() override { closed = true; }

    std::vector<Value> sent() const {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<Value> out;
        for (const std::string& s : sent_) {
            auto v = wisp::from_json(s);
            EXPECT_TRUE(v) << "not JSON: " << s;
            out.push_back(v ? v.value() : Value());
        }
        return out;
    }

    std::vector<std::string> sent_raw() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return sent_;
    }

    size_t sent_count() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return sent_.size();
    }

    std::vector<std::string> hellos() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return hellos_;
    }

    std::vector<std::string> frames() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return frames_;
    }

    std::atomic<bool> accept{true};
    std::atomic<uint32_t> block_ms{0};
    std::atomic<int> send_calls{0};
    std::atomic<bool> closed{false};

private:
    const int id_;
    mutable std::mutex mutex_;
    std::vector<std::string> sent_;
    std::vector<std::string> hellos_;
    std::vector<std::string> frames_;
};

std::string str(const Value& v, std::string_view key) {
    const Value* f = v.find(key);
    return f != nullptr && f->type() == Value::Type::String ? f->as_string() : std::string();
}

// A ring entry as the replay sends it.
Value replay_line(const wisp::LogEntry& e) {
    static const char* const kLevels[] = {"D", "I", "W", "E"};
    return Value(Map{{"t", Value("log")},
                     {"seq", Value(static_cast<int64_t>(e.seq))},
                     {"ts", Value(static_cast<int64_t>(e.ts_ms))},
                     {"level", Value(kLevels[static_cast<int>(e.level)])},
                     {"tag", Value(e.tag)},
                     {"msg", Value(e.msg)},
                     {"replay", Value(true)}});
}

// A line handed to the hub directly, as the log module would.
wisp::LogRecord rec(LogLevel level, const char* tag, const char* msg, uint32_t seq = 1,
                    uint64_t ts_ms = 0) {
    wisp::LogRecord r;
    r.level = level;
    r.tag = tag;
    r.msg = msg;
    r.seq = seq;
    r.ts_ms = ts_ms;
    return r;
}

TEST(WsHub, ThirdClientRejected) {
    wisp::log::reset_for_test();
    wisp::log::init(4096);
    EventBus bus;
    WsHub hub(bus);
    auto c1 = std::make_shared<FakeWsConnection>(1);
    auto c2 = std::make_shared<FakeWsConnection>(2);
    auto c3 = std::make_shared<FakeWsConnection>(3);

    EXPECT_TRUE(hub.open(c1));
    EXPECT_TRUE(hub.open(c2));
    EXPECT_FALSE(hub.open(c3));
    EXPECT_NE(wisp::log::ring_snapshot().find("client 3 refused: at most 2 connections"),
              std::string::npos);

    // A slot frees up once a client leaves.
    hub.close(1);
    EXPECT_TRUE(hub.open(c3));

    // A reused id is refused for what it is, not as "too many".
    hub.close(2);
    EXPECT_FALSE(hub.open(std::make_shared<FakeWsConnection>(3)));
    EXPECT_NE(wisp::log::ring_snapshot().find("client 3 refused: id already open"),
              std::string::npos)
        << wisp::log::ring_snapshot();
}

// An open admin page keeps Wi-Fi on (spec 4.3): one network lock while at
// least one connection is open.
// A peer that vanished without closing (out of Wi-Fi range, a laptop lid
// shut) never reports a close. keepalive() pings every client; one that
// has sent nothing for three rounds is closed, and with the last one the
// admin lock goes.
TEST(WsHub, KeepaliveClosesASilentClient) {
    wisp::log::reset_for_test();
    wisp::log::init(16 * 1024);
    EventBus bus;
    wisp::core::WakeLockRegistry locks(bus);
    auto admin_held = [&locks] { return locks.held(wisp::core::WakeLockKind::Network); };
    auto pings = [](const FakeWsConnection& c) {
        size_t n = 0;
        for (const Value& v : c.sent())
            if (v == Value(Map{{"t", Value("ping")}})) ++n;
        return n;
    };
    WsHub hub(bus);
    hub.set_wake_locks(&locks);
    auto quiet = std::make_shared<FakeWsConnection>(1);
    auto chatty = std::make_shared<FakeWsConnection>(2);
    ASSERT_TRUE(hub.open(quiet, "a"));
    ASSERT_TRUE(hub.open(chatty, "b"));

    for (int round = 0; round < 2; ++round) {
        hub.keepalive();
        hub.pump();
        hub.on_message(2, R"({"t":"pong"})");
    }
    EXPECT_FALSE(quiet->closed);
    EXPECT_EQ(pings(*quiet), 2u);
    EXPECT_EQ(pings(*chatty), 2u);

    hub.keepalive();  // quiet's third silent round
    EXPECT_TRUE(quiet->closed);
    EXPECT_FALSE(chatty->closed);
    EXPECT_NE(wisp::log::ring_snapshot().find("client 1 closed: silent"), std::string::npos);
    EXPECT_TRUE(admin_held());

    // Anything the client sends counts, even a message the hub ignores.
    // The round after it resets the count; three silent ones follow.
    hub.on_message(2, "not json");
    hub.keepalive();
    hub.keepalive();
    hub.keepalive();
    EXPECT_FALSE(chatty->closed);
    hub.keepalive();
    EXPECT_TRUE(chatty->closed);
    EXPECT_FALSE(admin_held());
    wisp::log::reset_for_test();
}

TEST(WsHub, HoldsAdminLockWhileConnected) {
    wisp::log::reset_for_test();
    wisp::log::init(4096);
    EventBus bus;
    wisp::core::WakeLockRegistry locks(bus);
    auto admin_locks = [&locks] {
        int n = 0;
        for (const auto& l : locks.list()) {
            if (l.holder == "system.admin" && l.kind == wisp::core::WakeLockKind::Network &&
                l.reason == "admin session")
                ++n;
        }
        return n;
    };
    {
        WsHub hub(bus);
        hub.set_wake_locks(&locks);
        EXPECT_EQ(admin_locks(), 0);
        ASSERT_TRUE(hub.open(std::make_shared<FakeWsConnection>(1), "a"));
        EXPECT_EQ(admin_locks(), 1);
        ASSERT_TRUE(hub.open(std::make_shared<FakeWsConnection>(2), "b"));
        EXPECT_FALSE(hub.open(std::make_shared<FakeWsConnection>(3), "c"));
        EXPECT_EQ(admin_locks(), 1);
        hub.close(1);
        EXPECT_EQ(admin_locks(), 1);
        hub.close(2);
        EXPECT_EQ(admin_locks(), 0);

        ASSERT_TRUE(hub.open(std::make_shared<FakeWsConnection>(4), "a"));
        hub.close_client("a");
        EXPECT_EQ(admin_locks(), 0);
        ASSERT_TRUE(hub.open(std::make_shared<FakeWsConnection>(5), "a"));
        hub.close_all();
        EXPECT_EQ(admin_locks(), 0);

        // Destroyed with a connection open: the lock goes with it.
        ASSERT_TRUE(hub.open(std::make_shared<FakeWsConnection>(6), "a"));
        EXPECT_EQ(admin_locks(), 1);
    }
    EXPECT_EQ(admin_locks(), 0);
}

TEST(WsHub, BackfillsRingThenStreamsLive) {
    wisp::log::reset_for_test();
    wisp::log::init(4096);
    EventBus bus;
    WsHub hub(bus, "0123456789abcdef");
    WLOG_I("boot", "first line");
    WLOG_W("wifi", "second \"quoted\" line\nwith a second row");

    auto c = std::make_shared<FakeWsConnection>(1);
    ASSERT_TRUE(hub.open(c));
    const std::vector<wisp::LogEntry> ring = wisp::log::ring_entries();
    ASSERT_GE(ring.size(), 3u);  // the two above and "client 1 connected"
    hub.on_message(1, R"({"sub":["log"]})");
    hub.pump();

    // One replay message per ring entry, with the ring's own seq, ts, level
    // and tag; a message holding '\n' stays one message.
    std::vector<Value> sent = c->sent();
    ASSERT_EQ(sent.size(), ring.size());
    for (size_t i = 0; i < ring.size(); ++i) {
        EXPECT_EQ(sent[i], replay_line(ring[i])) << i;
    }
    EXPECT_EQ(str(sent[1], "tag"), "wifi");
    EXPECT_EQ(str(sent[1], "level"), "W");
    EXPECT_EQ(str(sent[1], "msg"), "second \"quoted\" line\nwith a second row");
    EXPECT_EQ(sent[1].find("seq")->as_int(), sent[0].find("seq")->as_int() + 1);

    // Live lines follow, with their own level, tag, seq and timestamp and
    // no "replay"; tag and message are JSON-escaped.
    hub.write(rec(LogLevel::Error, "t\"ag", "live \"one\"\n\\done", 77, 12345));
    WLOG_D("net", "via the log module");
    hub.pump();
    sent = c->sent();
    ASSERT_EQ(sent.size(), ring.size() + 2);
    const Value& live = sent[ring.size()];
    EXPECT_EQ(live, Value(Map{{"t", Value("log")},
                              {"seq", Value(int64_t{77})},
                              {"ts", Value(int64_t{12345})},
                              {"level", Value("E")},
                              {"tag", Value("t\"ag")},
                              {"msg", Value("live \"one\"\n\\done")}}));
    // Through the log module, the frame carries exactly the ring's seq and
    // ts (spec 6.2: ms since boot, the same clock as /device's uptime_ms).
    const wisp::LogEntry newest = wisp::log::ring_entries().back();
    ASSERT_EQ(newest.msg, "via the log module");
    EXPECT_EQ(sent.back().find("seq")->as_int(), static_cast<int64_t>(newest.seq));
    EXPECT_EQ(sent.back().find("ts")->as_int(), static_cast<int64_t>(newest.ts_ms));
    EXPECT_EQ(sent.back().find("replay"), nullptr);
    EXPECT_EQ(str(sent.back(), "level"), "D");
    EXPECT_EQ(str(sent.back(), "tag"), "net");

    // Re-sending the same subscription does not replay the ring.
    hub.on_message(1, R"({"sub":["log"]})");
    hub.pump();
    EXPECT_EQ(c->sent_count(), ring.size() + 2);
}

// The hello naming this boot is every connection's first frame, ahead of a
// replay, a dropped notice or anything queued, and is sent once.
TEST(WsHub, HelloComesFirst) {
    wisp::log::reset_for_test();
    wisp::log::init(4096);
    EventBus bus;
    WsHub hub(bus, "00ff00ff00ff00ff");
    WLOG_I("t", "already in the ring");

    auto c = std::make_shared<FakeWsConnection>(1);
    c->accept = false;
    ASSERT_TRUE(hub.open(c));
    hub.on_message(1, R"({"sub":["log"]})");
    for (int i = 0; i < 300; ++i) hub.write(rec(LogLevel::Info, "t", "flood", 1000 + i));
    hub.pump();  // refused: the hello stays owed
    EXPECT_TRUE(c->frames().empty());

    c->accept = true;
    hub.pump();
    hub.pump();
    const std::vector<std::string> frames = c->frames();
    ASSERT_GE(frames.size(), 3u);
    EXPECT_EQ(wisp::from_json(frames[0]).value(),
              Value(Map{{"t", Value("hello")}, {"boot_id", Value("00ff00ff00ff00ff")}}));
    EXPECT_EQ(c->hellos().size(), 1u);
    EXPECT_EQ(str(wisp::from_json(frames[1]).value(), "t"), "dropped");

    // A second connection gets its own hello, even before subscribing.
    auto d = std::make_shared<FakeWsConnection>(2);
    ASSERT_TRUE(hub.open(d));
    hub.pump();
    ASSERT_EQ(d->frames().size(), 1u);
    EXPECT_EQ(d->hellos().size(), 1u);
}

TEST(WsHub, LogsOnlyReachLogSubscribers) {
    wisp::log::reset_for_test();
    EventBus bus;
    WsHub hub(bus);
    auto c1 = std::make_shared<FakeWsConnection>(1);
    auto c2 = std::make_shared<FakeWsConnection>(2);
    ASSERT_TRUE(hub.open(c1));
    ASSERT_TRUE(hub.open(c2));
    hub.on_message(1, R"({"sub":["log"]})");
    hub.on_message(2, R"({"sub":["events"]})");

    hub.write(rec(LogLevel::Info, "t", "hello"));
    hub.pump();
    EXPECT_EQ(c1->sent_count(), 1u);
    EXPECT_EQ(c2->sent_count(), 0u);

    // Dropping "log" stops the stream; a closed client gets nothing.
    hub.on_message(1, R"({"sub":[]})");
    hub.write(rec(LogLevel::Info, "t", "unsubscribed"));
    hub.pump();
    EXPECT_EQ(c1->sent_count(), 1u);
    hub.on_message(1, R"({"sub":["log"]})");
    hub.close(1);
    hub.write(rec(LogLevel::Info, "t", "closed"));
    hub.pump();
    EXPECT_EQ(c1->sent_count(), 1u);

    // Malformed messages are ignored.
    hub.on_message(2, "not json");
    hub.on_message(2, R"({"sub":"log"})");
    hub.on_message(7, R"({"sub":["log"]})");
    hub.write(rec(LogLevel::Info, "t", "still events only"));
    hub.pump();
    EXPECT_EQ(c2->sent_count(), 0u);
}

// Agent frames (agent-core spec 7.2) go only to the clients whose last
// {"sub":[...]} named "agent", in the order they were sent.
TEST(WsHub, AgentFramesOnlyToAgentSubscribers) {
    wisp::log::reset_for_test();
    EventBus bus;
    WsHub hub(bus);
    auto agent = std::make_shared<FakeWsConnection>(1);
    auto other = std::make_shared<FakeWsConnection>(2);
    ASSERT_TRUE(hub.open(agent));
    ASSERT_TRUE(hub.open(other));
    hub.on_message(1, R"({"sub":["agent"]})");
    hub.on_message(2, R"({"sub":["log","events"]})");

    hub.send_agent(R"({"t":"agent","k":"delta","text":"a"})");
    hub.send_agent(R"({"t":"agent","k":"delta","text":"b"})");
    hub.pump();
    ASSERT_EQ(agent->sent_count(), 2u);
    EXPECT_EQ(str(agent->sent()[0], "text"), "a");
    EXPECT_EQ(str(agent->sent()[1], "text"), "b");
    for (const Value& v : other->sent()) EXPECT_NE(str(v, "t"), "agent");

    // Dropping "agent" stops them.
    hub.on_message(1, R"({"sub":["log"]})");
    hub.send_agent(R"({"t":"agent","k":"delta","text":"c"})");
    hub.pump();
    size_t agent_frames = 0;
    for (const Value& v : agent->sent()) agent_frames += str(v, "t") == "agent" ? 1 : 0;
    EXPECT_EQ(agent_frames, 2u);
}

TEST(WsHub, ForwardsOnlyAllowedTopics) {
    wisp::log::reset_for_test();
    EventBus bus;
    WsHub hub(bus);
    ASSERT_TRUE(bus.start());
    auto events = std::make_shared<FakeWsConnection>(1);
    auto logs = std::make_shared<FakeWsConnection>(2);
    ASSERT_TRUE(hub.open(events));
    ASSERT_TRUE(hub.open(logs));
    hub.on_message(1, R"({"sub":["events"]})");
    hub.on_message(2, R"({"sub":["log"]})");

    bus.publish(Event{"net.wifi.state", "test", 0, {{"state", Value("connected")}}});
    bus.publish(Event{"button.power.short", "test", 0, {}});
    bus.publish(Event{"power.state", "test", 0, {{"state", Value("active")}}});
    bus.publish(Event{"storage.sd.inserted", "test", 0, {}});
    bus.publish(Event{"system.boot.completed", "test", 0, {}});
    bus.publish(
        Event{"scene.state", "scene", 0, {{"id", Value("todo")}, {"state", Value("running")}}});
    bus.publish(Event{"networking", "test", 0, {}});
    ASSERT_TRUE(bus.wait_idle(2000));
    hub.pump();

    std::vector<Value> sent = events->sent();
    ASSERT_EQ(sent.size(), 5u);
    EXPECT_EQ(sent[0], Value(Map{{"t", Value("event")},
                                 {"topic", Value("net.wifi.state")},
                                 {"payload", Value(Map{{"state", Value("connected")}})}}));
    EXPECT_EQ(str(sent[1], "topic"), "power.state");
    EXPECT_EQ(str(sent[2], "topic"), "storage.sd.inserted");
    EXPECT_EQ(str(sent[3], "topic"), "system.boot.completed");
    EXPECT_EQ(str(sent[4], "topic"), "scene.state");
    EXPECT_EQ(logs->sent_count(), 0u);
    bus.stop();
}

// A factory reset waits (boundedly) for its "done" event to leave before
// it revokes the tokens that close these sockets.
TEST(WsHub, FlushWaitsUntilQueuesAreSent) {
    wisp::log::reset_for_test();
    EventBus bus;
    WsHub hub(bus);
    ASSERT_TRUE(bus.start());
    EXPECT_TRUE(hub.flush(0));  // nobody connected: nothing owed

    auto c = std::make_shared<FakeWsConnection>(1);
    c->accept = false;
    ASSERT_TRUE(hub.open(c));
    hub.on_message(1, R"({"sub":["events"]})");
    hub.start();
    bus.publish(Event{"system.factory_reset", "test", 0, {{"state", Value("done")}}});
    ASSERT_TRUE(bus.wait_idle(2000));

    const uint64_t t0 = wisp::os::now_ms();
    EXPECT_FALSE(hub.flush(150));  // the socket keeps refusing
    EXPECT_GE(wisp::os::now_ms() - t0, 150u);
    EXPECT_EQ(c->sent_count(), 0u);

    c->accept = true;
    EXPECT_TRUE(hub.flush(2000));
    std::vector<Value> sent = c->sent();
    ASSERT_EQ(sent.size(), 1u);
    EXPECT_EQ(str(sent[0], "topic"), "system.factory_reset");
    hub.stop();
    bus.stop();
}

TEST(WsHub, SlowClientDropsOldestAndReportsCount) {
    wisp::log::reset_for_test();
    EventBus bus;
    WsHub hub(bus);
    auto c = std::make_shared<FakeWsConnection>(1);
    c->accept = false;
    ASSERT_TRUE(hub.open(c));
    hub.on_message(1, R"({"sub":["log"]})");

    for (int i = 0; i < 300; ++i) {
        hub.write(rec(LogLevel::Info, "t", ("m" + std::to_string(i)).c_str()));
        // A refused send keeps the message for the next pump.
        if (i % 50 == 0) hub.pump();
    }
    EXPECT_GT(c->send_calls.load(), 0);
    EXPECT_EQ(c->sent_count(), 0u);

    c->accept = true;
    hub.pump();
    std::vector<Value> sent = c->sent();
    ASSERT_EQ(sent.size(), 1u + 256u);
    EXPECT_EQ(sent[0], Value(Map{{"t", Value("dropped")}, {"count", Value(int64_t{44})}}));
    for (int i = 0; i < 256; ++i) {
        EXPECT_EQ(str(sent[1 + i], "msg"), "m" + std::to_string(44 + i)) << i;
    }

    // The count is reported once.
    hub.write(rec(LogLevel::Info, "t", "after"));
    hub.pump();
    sent = c->sent();
    ASSERT_EQ(sent.size(), 258u);
    EXPECT_EQ(str(sent.back(), "msg"), "after");
}

TEST(WsHub, LogWriteNeverBlocksOnSend) {
    wisp::log::reset_for_test();
    wisp::log::init(4096);
    EventBus bus;
    WsHub hub(bus);
    hub.start();
    auto c = std::make_shared<FakeWsConnection>(1);
    c->block_ms = 1000;
    ASSERT_TRUE(hub.open(c));
    hub.on_message(1, R"({"sub":["log"]})");
    hub.write(rec(LogLevel::Info, "t", "wake the sender"));
    ASSERT_TRUE(wait_until([&] { return c->send_calls.load() > 0; }, 1000));

    // The sender task is now stuck inside send_text() for a second.
    uint64_t t0 = wisp::os::now_ms();
    hub.write(rec(LogLevel::Info, "t", "direct"));
    EXPECT_LT(wisp::os::now_ms() - t0, 50u);
    t0 = wisp::os::now_ms();
    WLOG_I("t", "through the log module");
    EXPECT_LT(wisp::os::now_ms() - t0, 50u);

    // Everything queued is delivered once the socket recovers.
    c->block_ms = 0;
    EXPECT_TRUE(wait_until([&] { return c->sent_count() >= 3; }, 3000));
    hub.stop();
}

TEST(WsHub, DestroyedHubIsNoLongerALogSink) {
    wisp::log::reset_for_test();
    wisp::log::init(4096);
    EventBus bus;
    // The connection outlives the hub.
    auto c = std::make_shared<FakeWsConnection>(1);
    {
        WsHub hub(bus);
        hub.start();
        ASSERT_TRUE(hub.open(c));
        hub.on_message(1, R"({"sub":["log"]})");
        WLOG_I("t", "while the hub is up");
        ASSERT_TRUE(wait_until([&] { return c->sent_count() > 0; }, 2000));
    }
    const int calls = c->send_calls.load();
    WLOG_I("t", "after the hub is gone");
    wisp::os::sleep_ms(20);
    EXPECT_EQ(c->send_calls.load(), calls);
}

// Whatever a log line or event carries (terminal colour codes, a message
// cut mid-character), the frames stay valid UTF-8 without raw control
// bytes, so the browser neither drops the socket nor fails JSON.parse().
TEST(WsHub, FramesAreValidUtf8WithoutControlBytes) {
    wisp::log::reset_for_test();
    wisp::log::init(4096);
    EventBus bus;
    WsHub hub(bus);
    WLOG_I("t", "\x1b[31mred\x1b[0m");
    auto c = std::make_shared<FakeWsConnection>(1);
    ASSERT_TRUE(hub.open(c));
    hub.on_message(1, R"({"sub":["log"]})");
    hub.write(rec(LogLevel::Warn, "t\x01", "\x1b[31mcut \xe4\xb8"));
    hub.pump();

    const std::vector<std::string> raw = c->sent_raw();
    ASSERT_GE(raw.size(), 2u);
    for (const std::string& frame : raw) {
        EXPECT_TRUE(wisp::test::is_valid_utf8(frame)) << frame;
        EXPECT_FALSE(wisp::test::has_control_bytes(frame)) << frame;
    }
    const std::vector<Value> sent = c->sent();
    EXPECT_NE(str(sent.front(), "msg").find("\x1b[31mred"), std::string::npos);
    EXPECT_EQ(str(sent.back(), "tag"), "t\x01");
    EXPECT_EQ(str(sent.back(), "msg"), "\x1b[31mcut \xEF\xBF\xBD");
}

// Besides the 256-message cap, each client's queue holds at most 32 KiB:
// on ESP these strings land in internal RAM.
TEST(WsHub, QueueIsBoundedByBytes) {
    wisp::log::reset_for_test();
    EventBus bus;
    WsHub hub(bus);
    auto c = std::make_shared<FakeWsConnection>(1);
    c->accept = false;
    ASSERT_TRUE(hub.open(c));
    hub.on_message(1, R"({"sub":["log"]})");

    constexpr int kWritten = 100;
    for (int i = 0; i < kWritten; ++i) {
        hub.write(
            rec(LogLevel::Info, "t", (std::to_string(i) + ":" + std::string(1000, 'x')).c_str()));
    }
    c->accept = true;
    hub.pump();

    const std::vector<std::string> raw = c->sent_raw();
    ASSERT_GE(raw.size(), 2u);
    const std::vector<Value> sent = c->sent();
    ASSERT_EQ(str(sent[0], "t"), "dropped");
    const int64_t dropped = sent[0].find("count")->as_int();
    const size_t kept = raw.size() - 1;
    EXPECT_EQ(static_cast<int64_t>(kept) + dropped, kWritten);
    size_t bytes = 0;
    for (size_t i = 1; i < raw.size(); ++i) bytes += raw[i].size();
    EXPECT_LE(bytes, 32768u);
    // Only as many as fit were dropped: one more would not have.
    EXPECT_GT(bytes + raw[1].size(), 32768u);
    // The newest ones were kept.
    for (size_t i = 1; i < raw.size(); ++i) {
        const int n = kWritten - static_cast<int>(kept) + static_cast<int>(i) - 1;
        EXPECT_EQ(str(sent[i], "msg").substr(0, std::to_string(n).size() + 1),
                  std::to_string(n) + ":")
            << i;
    }

    // A single message over the budget is dropped on its own, without
    // pushing out what is already queued.
    c->accept = false;
    hub.write(rec(LogLevel::Info, "t", "small"));
    hub.write(rec(LogLevel::Info, "t", std::string(40000, 'y').c_str()));
    c->accept = true;
    hub.pump();
    const std::vector<Value> after = c->sent();
    ASSERT_EQ(after.size(), sent.size() + 2);
    EXPECT_EQ(after[sent.size()],
              Value(Map{{"t", Value("dropped")}, {"count", Value(int64_t{1})}}));
    EXPECT_EQ(str(after.back(), "msg"), "small");
}

// A revoked browser must lose its live stream too (spec 6.4).
TEST(WsHub, CloseClientClosesThatClientsConnections) {
    wisp::log::reset_for_test();
    EventBus bus;
    WsHub hub(bus);
    auto a = std::make_shared<FakeWsConnection>(1);
    auto b = std::make_shared<FakeWsConnection>(2);
    ASSERT_TRUE(hub.open(a, "client-a"));
    ASSERT_TRUE(hub.open(b, "client-b"));
    hub.on_message(1, R"({"sub":["log"]})");
    hub.on_message(2, R"({"sub":["log"]})");
    hub.write(rec(LogLevel::Info, "t", "queued for both"));

    hub.close_client("");
    hub.close_client("nobody");
    EXPECT_FALSE(a->closed);
    hub.close_client("client-a");
    EXPECT_TRUE(a->closed);
    EXPECT_FALSE(b->closed);

    hub.write(rec(LogLevel::Info, "t", "after"));
    hub.pump();
    EXPECT_EQ(a->send_calls.load(), 0);
    // b also streams the hub's own "closed" log line.
    const std::vector<Value> to_b = b->sent();
    ASSERT_EQ(to_b.size(), 3u);
    EXPECT_EQ(str(to_b[0], "msg"), "queued for both");
    EXPECT_NE(str(to_b[1], "msg").find("revoked"), std::string::npos);
    EXPECT_EQ(str(to_b[2], "msg"), "after");
    // The server's on_close for the closed socket is harmless, and the
    // slot is free again.
    hub.close(1);
    EXPECT_TRUE(hub.open(std::make_shared<FakeWsConnection>(3), "client-c"));

    hub.close_all();
    EXPECT_TRUE(b->closed);
    EXPECT_TRUE(hub.open(std::make_shared<FakeWsConnection>(4)));
    EXPECT_TRUE(hub.open(std::make_shared<FakeWsConnection>(5)));
}

TEST(AdminServer, WsRequiresToken) {
    AdminFixture f;
    wisp::net::WsHandlers ws = f.srv->ws_handlers();
    ASSERT_TRUE(ws.on_open && ws.on_message && ws.on_close);
    auto c1 = std::make_shared<FakeWsConnection>(11);
    const auto req = make_req(Method::Get, "/api/v1/ws");

    EXPECT_FALSE(ws.on_open(c1, req));
    EXPECT_FALSE(ws.on_open(c1, with_token(req, "bogus")));
    const std::string token = f.token();
    // Same Host check as plain HTTP requests (DNS-rebinding defence).
    EXPECT_FALSE(ws.on_open(c1, wisp::test::host(with_token(req, token), "evil.example")));
    ASSERT_TRUE(ws.on_open(c1, with_token(req, token)));

    // Accepted connections are served by the hub: subscribe and receive.
    // Pairing's own events are flushed first so only ours is expected.
    ASSERT_TRUE(f.bus.wait_idle(2000));
    ws.on_message(11, R"({"sub":["events"]})");
    f.bus.publish(Event{"net.wifi.state", "test", 0, {{"state", Value("connected")}}});
    EXPECT_TRUE(wait_until([&] { return c1->sent_count() >= 1; }, 2000));
    ASSERT_EQ(c1->sent_count(), 1u);
    EXPECT_EQ(str(c1->sent()[0], "topic"), "net.wifi.state");

    // At most two connections, and a closed one frees its slot.
    auto c2 = std::make_shared<FakeWsConnection>(12);
    auto c3 = std::make_shared<FakeWsConnection>(13);
    EXPECT_TRUE(ws.on_open(c2, with_token(req, token)));
    EXPECT_FALSE(ws.on_open(c3, with_token(req, token)));
    ws.on_close(11);
    EXPECT_TRUE(ws.on_open(c3, with_token(req, token)));
}

// A browser always sends Origin on a WebSocket upgrade, and cross-site
// pages can open one: the cookie's SameSite=Strict should keep the token
// out of such a request, and an Origin that is not this device refuses it
// as well. Clients that send no Origin (scripts) are unaffected.
TEST(AdminServer, WsRefusesAForeignOrigin) {
    AdminFixture f;
    wisp::net::WsHandlers ws = f.srv->ws_handlers();
    const std::string token = f.token();
    const auto req = with_token(make_req(Method::Get, "/api/v1/ws"), token);
    auto origin = [&](wisp::net::HttpRequest r, const char* value) {
        r.headers["origin"] = value;
        return r;
    };
    auto c = std::make_shared<FakeWsConnection>(21);
    for (const char* bad :
         {"http://evil.example", "http://192.168.1.50:8080", "null",
          "http://192.168.1.50.evil.example", "ws://192.168.1.50", "http://192.168.1.5"}) {
        EXPECT_FALSE(ws.on_open(c, origin(req, bad))) << bad;
    }
    EXPECT_TRUE(ws.on_open(c, origin(req, "http://192.168.1.50")));
    auto c2 = std::make_shared<FakeWsConnection>(22);
    EXPECT_TRUE(
        ws.on_open(c2, origin(wisp::test::host(req, "WISP.local:8900"), "http://wisp.local:8900")));
}

// Logout, DELETE /clients/{id} and revoke_all() (factory reset) close the
// revoked browsers' WebSockets at once; other browsers keep streaming.
TEST(AdminServer, RevokedClientsLoseTheirWebSocket) {
    AdminFixture f;
    wisp::net::WsHandlers ws = f.srv->ws_handlers();
    const auto req = make_req(Method::Get, "/api/v1/ws");
    const std::string token_a = f.token();
    const std::string token_b = f.token();
    auto a_id = f.pairing->authenticate(token_a, f.now);
    ASSERT_TRUE(a_id);
    // The first authenticated request takes the "admin request" lock, a
    // power.wakelock event that would reach `a` too: take it now, before
    // the count below, so the revoking request only renews it.
    ASSERT_EQ(f.srv->handle(with_token(make_req(Method::Get, "/api/v1/settings"), token_b)).status,
              200);
    ASSERT_TRUE(f.bus.wait_idle(2000));

    auto a = std::make_shared<FakeWsConnection>(21);
    auto b = std::make_shared<FakeWsConnection>(22);
    ASSERT_TRUE(ws.on_open(a, with_token(req, token_a)));
    ASSERT_TRUE(ws.on_open(b, with_token(req, token_b)));
    ws.on_message(21, R"({"sub":["events"]})");
    ws.on_message(22, R"({"sub":["events"]})");
    // Both got their hello; nothing reaches `a` after the revocation.
    ASSERT_TRUE(
        wait_until([&] { return a->hellos().size() == 1 && b->hellos().size() == 1; }, 2000));
    // The opens took the "admin session" lock; its power.wakelock event may
    // still be on its way to the subscribers. Let it arrive first.
    ASSERT_TRUE(f.bus.wait_idle(2000));
    ASSERT_TRUE(f.srv->flush_ws(2000));
    const int a_calls = a->send_calls.load();

    auto res = f.srv->handle(with_csrf(
        with_token(make_req(Method::Delete, "/api/v1/clients/" + a_id.value()), token_b)));
    ASSERT_EQ(res.status, 204);
    EXPECT_TRUE(a->closed);
    EXPECT_FALSE(b->closed);
    f.bus.publish(Event{"net.wifi.state", "test", 0, {{"state", Value("connected")}}});
    EXPECT_TRUE(wait_until([&] { return b->sent_count() >= 1; }, 2000));
    ASSERT_TRUE(f.bus.wait_idle(2000));
    wisp::os::sleep_ms(20);
    EXPECT_EQ(a->send_calls.load(), a_calls) << ::testing::PrintToString(a->sent_raw());
    ws.on_close(21);

    res = f.srv->handle(with_csrf(with_token(make_req(Method::Post, "/api/v1/logout"), token_b)));
    ASSERT_EQ(res.status, 200);
    EXPECT_TRUE(b->closed);
    ws.on_close(22);

    const std::string token_c = f.token();
    auto c = std::make_shared<FakeWsConnection>(23);
    ASSERT_TRUE(ws.on_open(c, with_token(req, token_c)));
    f.pairing->revoke_all();
    EXPECT_TRUE(c->closed);
}

}  // namespace
