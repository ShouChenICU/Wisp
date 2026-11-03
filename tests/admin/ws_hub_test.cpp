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
using wisp::test::with_token;

namespace {

// Records what the hub sends. `accept` = false makes send_text() report a
// full queue; `block_ms` makes it stall like a slow socket.
class FakeWsConnection : public wisp::net::WsConnection {
public:
    explicit FakeWsConnection(int id) : id_(id) {}

    int id() const override { return id_; }

    bool send_text(std::string_view text) override {
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
    EventBus bus;
    WsHub hub(bus);
    auto c1 = std::make_shared<FakeWsConnection>(1);
    auto c2 = std::make_shared<FakeWsConnection>(2);
    auto c3 = std::make_shared<FakeWsConnection>(3);

    EXPECT_TRUE(hub.open(c1));
    EXPECT_TRUE(hub.open(c2));
    EXPECT_FALSE(hub.open(c3));

    // A slot frees up once a client leaves.
    hub.close(1);
    EXPECT_TRUE(hub.open(c3));
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
    hub.write(LogLevel::Error, "t\"ag", "live \"one\"\n\\done");
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
    EXPECT_GT(live.find("ts")->as_int(), 0);
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
    EXPECT_LT(wisp::os::now_ms() - t0, 10u);
    t0 = wisp::os::now_ms();
    WLOG_I("t", "through the log module");
    EXPECT_LT(wisp::os::now_ms() - t0, 10u);

    // Everything queued is delivered once the socket recovers.
    c->block_ms = 0;
    EXPECT_TRUE(wait_until([&] { return c->sent_count() >= 3; }, 3000));
    hub.stop();
}

TEST(WsHub, DestroyedHubIsNoLongerALogSink) {
    wisp::log::reset_for_test();
    wisp::log::init(4096);
    EventBus bus;
    auto c = std::make_shared<FakeWsConnection>(1);
    {
        WsHub hub(bus);
        hub.start();
        ASSERT_TRUE(hub.open(c));
        hub.on_message(1, R"({"sub":["log"]})");
    }
    // Would call into the destroyed hub if it were still registered.
    WLOG_I("t", "after the hub is gone");
    SUCCEED();
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

}  // namespace
