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
// full queue; `block_ms` makes it stall like a slow socket.
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
        sent_.emplace_back(text);
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

    std::atomic<bool> accept{true};
    std::atomic<uint32_t> block_ms{0};
    std::atomic<int> send_calls{0};
    std::atomic<bool> closed{false};

private:
    const int id_;
    mutable std::mutex mutex_;
    std::vector<std::string> sent_;
};

std::string str(const Value& v, std::string_view key) {
    const Value* f = v.find(key);
    return f != nullptr && f->type() == Value::Type::String ? f->as_string() : std::string();
}

Value log_line(int64_t ts, std::string level, std::string tag, std::string msg) {
    return Value(Map{{"t", Value("log")},
                     {"ts", Value(ts)},
                     {"level", Value(std::move(level))},
                     {"tag", Value(std::move(tag))},
                     {"msg", Value(std::move(msg))}});
}

// Splits the ring snapshot into its lines, without the trailing '\n'.
std::vector<std::string> ring_lines() {
    std::vector<std::string> out;
    std::string snap = wisp::log::ring_snapshot();
    size_t start = 0;
    while (start < snap.size()) {
        size_t nl = snap.find('\n', start);
        if (nl == std::string::npos) nl = snap.size();
        out.push_back(snap.substr(start, nl - start));
        start = nl + 1;
    }
    return out;
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

TEST(WsHub, BackfillsRingThenStreamsLive) {
    wisp::log::reset_for_test();
    wisp::log::init(4096);
    EventBus bus;
    WsHub hub(bus);
    WLOG_I("boot", "first line");
    WLOG_W("wifi", "second \"quoted\" line");

    auto c = std::make_shared<FakeWsConnection>(1);
    ASSERT_TRUE(hub.open(c));
    const std::vector<std::string> ring = ring_lines();
    ASSERT_GE(ring.size(), 2u);
    hub.on_message(1, R"({"sub":["log"]})");
    hub.pump();

    std::vector<Value> sent = c->sent();
    ASSERT_EQ(sent.size(), ring.size());
    for (size_t i = 0; i < ring.size(); ++i) {
        EXPECT_EQ(sent[i], log_line(0, "I", "", ring[i])) << i;
    }

    // Live lines follow, with their real level, tag and timestamp; tag and
    // message are JSON-escaped.
    const auto before = static_cast<int64_t>(wisp::os::now_ms());
    hub.write(LogLevel::Error, "t\"ag", "live \"one\"\n\\done");
    const auto after = static_cast<int64_t>(wisp::os::now_ms());
    WLOG_D("net", "via the log module");
    hub.pump();
    sent = c->sent();
    ASSERT_EQ(sent.size(), ring.size() + 2);
    const Value& live = sent[ring.size()];
    EXPECT_EQ(str(live, "t"), "log");
    EXPECT_EQ(str(live, "level"), "E");
    EXPECT_EQ(str(live, "tag"), "t\"ag");
    EXPECT_EQ(str(live, "msg"), "live \"one\"\n\\done");
    ASSERT_NE(live.find("ts"), nullptr);
    // ms since boot, the same clock as /device's uptime_ms (spec 6.2).
    EXPECT_GE(live.find("ts")->as_int(), before);
    EXPECT_LE(live.find("ts")->as_int(), after);
    EXPECT_EQ(str(sent.back(), "level"), "D");
    EXPECT_EQ(str(sent.back(), "tag"), "net");
    EXPECT_EQ(str(sent.back(), "msg"), "via the log module");

    // Re-sending the same subscription does not replay the ring.
    hub.on_message(1, R"({"sub":["log"]})");
    hub.pump();
    EXPECT_EQ(c->sent_count(), ring.size() + 2);
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

    hub.write(LogLevel::Info, "t", "hello");
    hub.pump();
    EXPECT_EQ(c1->sent_count(), 1u);
    EXPECT_EQ(c2->sent_count(), 0u);

    // Dropping "log" stops the stream; a closed client gets nothing.
    hub.on_message(1, R"({"sub":[]})");
    hub.write(LogLevel::Info, "t", "unsubscribed");
    hub.pump();
    EXPECT_EQ(c1->sent_count(), 1u);
    hub.on_message(1, R"({"sub":["log"]})");
    hub.close(1);
    hub.write(LogLevel::Info, "t", "closed");
    hub.pump();
    EXPECT_EQ(c1->sent_count(), 1u);

    // Malformed messages are ignored.
    hub.on_message(2, "not json");
    hub.on_message(2, R"({"sub":"log"})");
    hub.on_message(7, R"({"sub":["log"]})");
    hub.write(LogLevel::Info, "t", "still events only");
    hub.pump();
    EXPECT_EQ(c2->sent_count(), 0u);
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
    bus.publish(Event{"networking", "test", 0, {}});
    ASSERT_TRUE(bus.wait_idle(2000));
    hub.pump();

    std::vector<Value> sent = events->sent();
    ASSERT_EQ(sent.size(), 4u);
    EXPECT_EQ(sent[0], Value(Map{{"t", Value("event")},
                                 {"topic", Value("net.wifi.state")},
                                 {"payload", Value(Map{{"state", Value("connected")}})}}));
    EXPECT_EQ(str(sent[1], "topic"), "power.state");
    EXPECT_EQ(str(sent[2], "topic"), "storage.sd.inserted");
    EXPECT_EQ(str(sent[3], "topic"), "system.boot.completed");
    EXPECT_EQ(logs->sent_count(), 0u);
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
        hub.write(LogLevel::Info, "t", ("m" + std::to_string(i)).c_str());
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
    hub.write(LogLevel::Info, "t", "after");
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
    hub.write(LogLevel::Info, "t", "wake the sender");
    ASSERT_TRUE(wait_until([&] { return c->send_calls.load() > 0; }, 1000));

    // The sender task is now stuck inside send_text() for a second.
    uint64_t t0 = wisp::os::now_ms();
    hub.write(LogLevel::Info, "t", "direct");
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
    hub.write(LogLevel::Warn, "t\x01", "\x1b[31mcut \xe4\xb8");
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
        hub.write(LogLevel::Info, "t", (std::to_string(i) + ":" + std::string(1000, 'x')).c_str());
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
    hub.write(LogLevel::Info, "t", "small");
    hub.write(LogLevel::Info, "t", std::string(40000, 'y').c_str());
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
    hub.write(LogLevel::Info, "t", "queued for both");

    hub.close_client("");
    hub.close_client("nobody");
    EXPECT_FALSE(a->closed);
    hub.close_client("client-a");
    EXPECT_TRUE(a->closed);
    EXPECT_FALSE(b->closed);

    hub.write(LogLevel::Info, "t", "after");
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
    ASSERT_TRUE(f.bus.wait_idle(2000));

    auto a = std::make_shared<FakeWsConnection>(21);
    auto b = std::make_shared<FakeWsConnection>(22);
    ASSERT_TRUE(ws.on_open(a, with_token(req, token_a)));
    ASSERT_TRUE(ws.on_open(b, with_token(req, token_b)));
    ws.on_message(21, R"({"sub":["events"]})");
    ws.on_message(22, R"({"sub":["events"]})");

    auto res = f.srv->handle(with_csrf(
        with_token(make_req(Method::Delete, "/api/v1/clients/" + a_id.value()), token_b)));
    ASSERT_EQ(res.status, 204);
    EXPECT_TRUE(a->closed);
    EXPECT_FALSE(b->closed);
    f.bus.publish(Event{"net.wifi.state", "test", 0, {{"state", Value("connected")}}});
    EXPECT_TRUE(wait_until([&] { return b->sent_count() >= 1; }, 2000));
    ASSERT_TRUE(f.bus.wait_idle(2000));
    wisp::os::sleep_ms(20);
    EXPECT_EQ(a->send_calls.load(), 0);
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
