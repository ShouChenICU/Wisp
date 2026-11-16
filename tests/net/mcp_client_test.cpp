#include "wisp/net/mcp_client.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "support/fake_devices.hpp"
#include "support/fake_io_queue.hpp"
#include "support/fake_mcp_server.hpp"
#include "support/secret_pieces.hpp"
#include "support/temp_dir.hpp"
#include "support/utf8.hpp"
#include "wisp/core/device_registry.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/core/log.hpp"
#include "wisp/core/mcp_servers.hpp"
#include "wisp/core/storage_manager.hpp"
#include "wisp/net/scene_http.hpp"

using wisp::core::McpHeaderInput;
using wisp::core::McpServerInput;
using wisp::core::McpServerService;
using wisp::core::McpServerUpdate;
using wisp::net::McpClient;
using wisp::net::McpClientConfig;
using wisp::net::McpError;
using wisp::net::McpListResult;
using wisp::net::McpWait;
using wisp::net::SceneHttp;
using wisp::net::TransportError;
using wisp::test::FakeMcpServer;

namespace {

constexpr const char* kSecret = "tok-SECRET-123456";

std::string str(const wisp::os::LargeString& s) { return {s.data(), s.size()}; }

uint64_t real_ms() { return wisp::os::now_ms(); }

class McpClientTest : public ::testing::Test {
protected:
    void SetUp() override {
        wisp::log::reset_for_test();
        wisp::log::init();
        registry.add(std::make_shared<wisp::test::FakeStorage>("storage.int",
                                                               wisp::test::temp_dir().string(),
                                                               /*removable=*/false));
        bus.start();
        storage = std::make_unique<wisp::core::StorageManager>(registry, bus);
        ASSERT_TRUE(storage->init());
        servers = std::make_unique<McpServerService>(*storage, bus, io.submit_io());
        ASSERT_TRUE(servers->init());
        McpServerInput in;
        in.name = "Files";
        in.url = "http://mcp.local/mcp";
        in.headers = {McpHeaderInput{"Authorization", std::string("Bearer ") + kSecret}};
        auto made = servers->create(in);
        ASSERT_TRUE(made);
        ASSERT_EQ(made.value().id, "files");
        http = std::make_unique<SceneHttp>(fake);
        ASSERT_TRUE(http->start());
        client = std::make_unique<McpClient>(*http, *servers, config());
        fake.tools = {FakeMcpServer::tool("read", "Read a file"),
                      FakeMcpServer::tool("write", "Write a file")};
    }

    void TearDown() override {
        fake.hold = false;
        client.reset();
        http->stop();
        http.reset();
        servers.reset();
        storage.reset();
        bus.stop();
    }

    // R10: one clock for everything compared with a wait deadline -- real
    // time plus a skew a test bumps to jump a minute or ten.
    McpClientConfig config() {
        return {.client_version = "1.2.3",
                .now_ms = [this] { return wisp::os::now_ms() + skew.load(); }};
    }
    uint64_t now() const { return wisp::os::now_ms() + skew.load(); }

    McpListResult list(bool refresh = false, const McpWait& wait = {}) {
        return client->list_tools("files", wait, refresh);
    }

    static std::vector<std::string> names(const McpListResult& r) {
        std::vector<std::string> out;
        for (const auto& t : r.tools) out.push_back(str(t.name));
        return out;
    }

    // R13: no log line holds the header value or any 8-byte piece of it.
    static void expect_no_secret_in_logs() {
        for (const auto& e : wisp::log::ring_entries())
            EXPECT_FALSE(wisp::test::holds_secret_piece(e.msg, kSecret)) << e.msg;
    }

    static size_t log_lines_with(std::string_view needle) {
        size_t n = 0;
        for (const auto& e : wisp::log::ring_entries())
            if (e.msg.find(needle) != std::string::npos) ++n;
        return n;
    }

    wisp::core::DeviceRegistry registry;
    wisp::core::EventBus bus;
    wisp::test::FakeIoQueue io;
    std::unique_ptr<wisp::core::StorageManager> storage;
    std::unique_ptr<McpServerService> servers;
    FakeMcpServer fake;
    std::unique_ptr<SceneHttp> http;
    std::atomic<uint64_t> skew{0};
    std::unique_ptr<McpClient> client;
};

TEST_F(McpClientTest, InitializeSendsTheProtocolAndClientInfo) {
    McpListResult r = list();
    ASSERT_EQ(r.failure.error, McpError::None);
    EXPECT_EQ(names(r), (std::vector<std::string>{"read", "write"}));

    const auto reqs = fake.requests();
    ASSERT_EQ(reqs.size(), 3u);
    EXPECT_EQ(reqs[0].method, "initialize");
    EXPECT_EQ(reqs[1].method, "notifications/initialized");
    EXPECT_FALSE(reqs[1].id.has_value());
    EXPECT_EQ(reqs[2].method, "tools/list");

    const auto& init = reqs[0];
    EXPECT_EQ(init.url, "http://mcp.local/mcp");
    ASSERT_NE(init.params.find("protocolVersion"), nullptr);
    EXPECT_EQ(init.params.find("protocolVersion")->as_string(), "2025-06-18");
    ASSERT_NE(init.params.find("capabilities"), nullptr);
    EXPECT_EQ(init.params.find("capabilities")->type(), wisp::Value::Type::Map);
    const wisp::Value* info = init.params.find("clientInfo");
    ASSERT_NE(info, nullptr);
    EXPECT_EQ(info->find("name")->as_string(), "wisp");
    EXPECT_EQ(info->find("version")->as_string(), "1.2.3");
    EXPECT_FALSE(init.header("Mcp-Session-Id").has_value());
    EXPECT_FALSE(init.header("MCP-Protocol-Version").has_value());

    for (const auto& req : reqs) {
        EXPECT_EQ(req.header("Accept"), "application/json, text/event-stream");
        EXPECT_EQ(req.header("Content-Type"), "application/json");
        EXPECT_EQ(req.header("Authorization"), std::string("Bearer ") + kSecret);
    }
    for (size_t i = 1; i < reqs.size(); ++i) {
        EXPECT_EQ(reqs[i].header("Mcp-Session-Id"), "s-1");
        EXPECT_EQ(reqs[i].header("MCP-Protocol-Version"), "2025-06-18");
    }
    // One log line per request, never a header value or the URL's path.
    EXPECT_EQ(log_lines_with("files initialize ok"), 1u);
    EXPECT_EQ(log_lines_with("files tools/list ok"), 1u);
    expect_no_secret_in_logs();
    for (const auto& e : wisp::log::ring_entries())
        EXPECT_EQ(e.msg.find("/mcp"), std::string::npos) << e.msg;
}

TEST_F(McpClientTest, AcceptsTheOlderRevision) {
    fake.protocol = "2025-03-26";
    McpListResult r = list();
    ASSERT_EQ(r.failure.error, McpError::None);
    const auto reqs = fake.requests();
    ASSERT_EQ(reqs.size(), 3u);
    EXPECT_EQ(reqs[2].header("MCP-Protocol-Version"), "2025-03-26");
}

TEST_F(McpClientTest, RefusesAnyOtherRevision) {
    fake.protocol = "2024-11-05";
    McpListResult r = list();
    EXPECT_EQ(r.failure.error, McpError::UnsupportedProtocol);
    EXPECT_EQ(fake.methods(), (std::vector<std::string>{"initialize"}));
}

TEST_F(McpClientTest, NoSessionIdSendsNone) {
    fake.session_id = "";
    ASSERT_EQ(list().failure.error, McpError::None);
    const auto reqs = fake.requests();
    ASSERT_EQ(reqs.size(), 3u);
    for (const auto& req : reqs) EXPECT_FALSE(req.header("Mcp-Session-Id").has_value());
    EXPECT_EQ(reqs[2].header("MCP-Protocol-Version"), "2025-06-18");
}

TEST_F(McpClientTest, SessionIdOver256BytesIsBadResponse) {
    fake.session_id = std::string(257, 'a');
    EXPECT_EQ(list().failure.error, McpError::BadResponse);
    EXPECT_EQ(fake.methods(), (std::vector<std::string>{"initialize"}));
}

TEST_F(McpClientTest, ExpiredSessionReinitializesOnceAndRetries) {
    ASSERT_EQ(list().failure.error, McpError::None);
    fake.expire_sessions = 1;
    McpListResult r = list(/*refresh=*/true);
    ASSERT_EQ(r.failure.error, McpError::None);
    EXPECT_EQ(r.tools.size(), 2u);
    EXPECT_EQ(fake.count("initialize"), 2u);
    EXPECT_EQ(fake.count("tools/list"), 3u);
}

TEST_F(McpClientTest, ExpiredTwiceFailsWithoutALoop) {
    fake.expire_sessions = 2;
    McpListResult r = list();
    EXPECT_EQ(r.failure.error, McpError::Unavailable);
    EXPECT_EQ(r.failure.category, "http");
    EXPECT_EQ(r.failure.http_status, 404);
    EXPECT_EQ(fake.count("initialize"), 2u);
    EXPECT_EQ(fake.count("tools/list"), 2u);
}

TEST_F(McpClientTest, ReadsSseAndSkipsNotifications) {
    fake.sse = true;
    McpListResult r = list();
    ASSERT_EQ(r.failure.error, McpError::None);
    EXPECT_EQ(names(r), (std::vector<std::string>{"read", "write"}));
}

TEST_F(McpClientTest, SseLeftOpenEndsAtTheResponse) {
    McpServerUpdate u;
    u.timeout_ms = 60000;
    ASSERT_TRUE(servers->update("files", u));
    fake.sse = true;
    fake.keep_open = true;
    const uint64_t start = real_ms();
    McpListResult r = list();
    EXPECT_LT(real_ms() - start, 2000u);
    ASSERT_EQ(r.failure.error, McpError::None);
    EXPECT_EQ(r.tools.size(), 2u);
}

TEST_F(McpClientTest, FollowsNextCursor) {
    fake.tools.clear();
    for (int i = 0; i < 5; ++i) fake.tools.push_back(FakeMcpServer::tool("t" + std::to_string(i)));
    fake.page_size = 2;
    McpListResult r = list();
    ASSERT_EQ(r.failure.error, McpError::None);
    EXPECT_EQ(names(r), (std::vector<std::string>{"t0", "t1", "t2", "t3", "t4"}));
    EXPECT_EQ(fake.count("tools/list"), 3u);
    const auto reqs = fake.requests();
    EXPECT_EQ(reqs[2].params.find("cursor"), nullptr);
    EXPECT_EQ(reqs[3].params.find("cursor")->as_string(), "2");
    EXPECT_EQ(reqs[4].params.find("cursor")->as_string(), "4");
}

TEST_F(McpClientTest, ListStopsAfterSixteenPages) {
    fake.tools.clear();
    for (int i = 0; i < 20; ++i) fake.tools.push_back(FakeMcpServer::tool("t" + std::to_string(i)));
    fake.page_size = 1;
    McpListResult r = list();
    ASSERT_EQ(r.failure.error, McpError::None);
    EXPECT_EQ(r.tools.size(), 16u);
    EXPECT_EQ(fake.count("tools/list"), 16u);
}

TEST_F(McpClientTest, ListLimits) {
    std::string cjk;
    for (int i = 0; i < 667; ++i) cjk += "\xe4\xb8\xad";  // 2001 bytes
    const std::string long_name(65, 'n');
    const std::string big_schema =
        R"({"type":"object","description":")" + std::string(17 * 1024, 'x') + "\"}";
    fake.tools = {
        FakeMcpServer::tool("long", cjk),
        FakeMcpServer::tool("big", "", big_schema),
        FakeMcpServer::tool("a b"),
        FakeMcpServer::tool(long_name),
        FakeMcpServer::tool("x:y"),
        FakeMcpServer::tool("str", "", R"("just text")"),
        FakeMcpServer::tool("arr", "", "[1,2]"),
        FakeMcpServer::tool("none", "", ""),
        FakeMcpServer::tool("obj", "",
                            R"({ "type" : "object", "properties" : {"a": {"type":"string"}} })"),
        // Raw bytes that are not UTF-8 (tool() would repair them).
        "{\"name\":\"bad8\",\"description\":\"ok\xff\"}",
        "{\"name\":\"bad8s\",\"inputSchema\":{\"type\":\"object\",\"description\":\"\xff\"}}",
    };
    const size_t specials_kept = 7;  // long str arr none obj bad8 bad8s
    for (int i = 0; i < 130; ++i) {
        char name[8];
        std::snprintf(name, sizeof(name), "t%03d", i);
        fake.tools.push_back(FakeMcpServer::tool(name));
    }

    McpListResult r = list();
    ASSERT_EQ(r.failure.error, McpError::None);
    ASSERT_EQ(r.tools.size(), 128u);
    char last[8];
    std::snprintf(last, sizeof(last), "t%03zu", 128 - specials_kept - 1);
    EXPECT_EQ(str(r.tools.back().name), last);

    auto find = [&](std::string_view name) -> const wisp::net::McpTool* {
        for (const auto& t : r.tools)
            if (str(t.name) == name) return &t;
        return nullptr;
    };
    const auto* lng = find("long");
    ASSERT_NE(lng, nullptr);
    EXPECT_LE(lng->description.size(), 1024u);
    EXPECT_GE(lng->description.size(), 1021u);
    EXPECT_TRUE(wisp::test::is_valid_utf8(str(lng->description)));
    for (const char* gone : {"big", "a b", "x:y"}) EXPECT_EQ(find(gone), nullptr) << gone;
    EXPECT_EQ(find(long_name), nullptr);
    for (const char* plain : {"str", "arr", "none", "bad8s"}) {
        ASSERT_NE(find(plain), nullptr) << plain;
        EXPECT_EQ(str(find(plain)->input_schema), R"({"type":"object"})") << plain;
    }
    ASSERT_NE(find("obj"), nullptr);
    EXPECT_EQ(str(find("obj")->input_schema),
              R"({"type":"object","properties":{"a":{"type":"string"}}})");
    ASSERT_NE(find("bad8"), nullptr);
    EXPECT_EQ(str(find("bad8")->description), "ok\xef\xbf\xbd");

    // One log line for each tool left out, naming it.
    EXPECT_EQ(log_lines_with("\"big\""), 1u);
    EXPECT_EQ(log_lines_with("\"a b\""), 1u);
    EXPECT_EQ(log_lines_with("\"" + long_name + "\""), 1u);
    EXPECT_EQ(log_lines_with("\"x:y\""), 1u);
}

TEST_F(McpClientTest, ListIsCachedForTenMinutes) {
    ASSERT_EQ(list().failure.error, McpError::None);
    const size_t calls = fake.calls();
    McpListResult again = list();
    ASSERT_EQ(again.failure.error, McpError::None);
    EXPECT_EQ(again.tools.size(), 2u);
    EXPECT_EQ(fake.calls(), calls);

    skew += wisp::net::kMcpListTtlMs;
    ASSERT_EQ(list().failure.error, McpError::None);
    EXPECT_EQ(fake.count("tools/list"), 2u);
    EXPECT_EQ(fake.count("initialize"), 1u);
}

TEST_F(McpClientTest, ConfigChangeDropsSessionAndCache) {
    ASSERT_EQ(list().failure.error, McpError::None);
    ASSERT_EQ(client->status().at(0).tools, std::optional<size_t>(2));
    McpServerUpdate u;
    u.url = "http://mcp.local/v2";
    ASSERT_TRUE(servers->update("files", u));
    EXPECT_EQ(client->status().at(0).tools, std::nullopt);
    ASSERT_EQ(list().failure.error, McpError::None);
    EXPECT_EQ(fake.count("initialize"), 2u);
    const auto reqs = fake.requests();
    EXPECT_EQ(reqs.back().url, "http://mcp.local/v2");
    EXPECT_FALSE(reqs[3].header("Mcp-Session-Id").has_value());
}

TEST_F(McpClientTest, DisabledServerIsNotContacted) {
    McpServerUpdate u;
    u.enabled = false;
    ASSERT_TRUE(servers->update("files", u));
    EXPECT_EQ(list().failure.error, McpError::NotFound);
    EXPECT_EQ(client->list_tools("nope", {}).failure.error, McpError::NotFound);
    EXPECT_EQ(fake.calls(), 0u);
}

TEST_F(McpClientTest, TransportFailuresAreUnavailable) {
    const std::pair<TransportError, const char*> cases[] = {
        {TransportError::Dns, "dns"},     {TransportError::Connect, "connect"},
        {TransportError::Tls, "tls"},     {TransportError::Timeout, "timeout"},
        {TransportError::Other, "other"},
    };
    for (const auto& [transport, category] : cases) {
        fake.transport = transport;
        McpListResult r = list();
        EXPECT_EQ(r.failure.error, McpError::Unavailable) << category;
        EXPECT_EQ(r.failure.category, category);
        skew += wisp::net::kMcpRetryAfterMs;
    }
    fake.transport = TransportError::None;
    fake.status = 500;
    McpListResult r = list();
    EXPECT_EQ(r.failure.error, McpError::Unavailable);
    EXPECT_EQ(r.failure.category, "http");
    EXPECT_EQ(r.failure.http_status, 500);
    skew += wisp::net::kMcpRetryAfterMs;

    fake.status = 0;
    const size_t calls = fake.calls();
    http->set_deferring(true);
    r = list();
    EXPECT_EQ(r.failure.error, McpError::Unavailable);
    EXPECT_EQ(r.failure.category, "offline");
    EXPECT_EQ(fake.calls(), calls);
    http->set_deferring(false);
}

TEST_F(McpClientTest, FailureIsRememberedForAMinute) {
    fake.transport = TransportError::Connect;
    EXPECT_EQ(list().failure.error, McpError::Unavailable);
    EXPECT_EQ(fake.calls(), 1u);
    ASSERT_EQ(client->status().size(), 1u);
    EXPECT_EQ(client->status()[0].error, "connect");

    fake.transport = TransportError::None;
    McpListResult r = list(/*refresh=*/true);
    EXPECT_EQ(r.failure.error, McpError::Unavailable);
    EXPECT_EQ(r.failure.category, "connect");
    EXPECT_EQ(fake.calls(), 1u);

    skew += wisp::net::kMcpRetryAfterMs;
    EXPECT_EQ(client->status()[0].error, "");
    EXPECT_EQ(list().failure.error, McpError::None);
    EXPECT_EQ(client->status()[0].tools, std::optional<size_t>(2));
}

TEST_F(McpClientTest, CancelStopsTheRequest) {
    fake.hold = true;
    std::atomic<bool> stop{false};
    std::thread flip([&] {
        while (fake.held.load() == 0) std::this_thread::sleep_for(std::chrono::milliseconds(1));
        stop = true;
    });
    McpListResult r =
        list(false, McpWait{.deadline_ms = 0, .cancelled = [&] { return stop.load(); }});
    flip.join();
    EXPECT_EQ(r.failure.error, McpError::Cancelled);
    for (int i = 0; i < 200 && !fake.saw_cancel.load(); ++i)
        std::this_thread::sleep_for(std::chrono::milliseconds(5));
    EXPECT_TRUE(fake.saw_cancel.load());
    // Not the server's failure: nothing is remembered.
    EXPECT_EQ(client->status()[0].error, "");
}

TEST_F(McpClientTest, DeadlineEndsTheWait) {
    fake.hold = true;
    const uint64_t start = real_ms();
    McpListResult r = list(false, McpWait{.deadline_ms = now() + 300, .cancelled = {}});
    const uint64_t took = real_ms() - start;
    EXPECT_EQ(r.failure.error, McpError::Timeout);
    EXPECT_GE(took, 250u);
    EXPECT_LT(took, 1500u);
    EXPECT_EQ(client->status()[0].error, "");
    fake.hold = false;
    // The next call is not held back by a remembered failure.
    EXPECT_EQ(list().failure.error, McpError::None);
}

TEST_F(McpClientTest, OversizeListPageIsBadResponse) {
    fake.tools = {FakeMcpServer::tool("huge", std::string(600 * 1024, 'd'))};
    McpListResult r = list();
    EXPECT_EQ(r.failure.error, McpError::BadResponse);
    EXPECT_EQ(fake.count("tools/list"), 1u);

    // The same over SSE.
    skew += wisp::net::kMcpRetryAfterMs;
    fake.sse = true;
    EXPECT_EQ(list().failure.error, McpError::BadResponse);
}

// A page past SseParser's default event limit but under kMcpMaxResponse
// is read whole when it comes as an event stream, as it is as JSON.
TEST_F(McpClientTest, LargeSsePageIsAccepted) {
    fake.sse = true;
    fake.tools = {FakeMcpServer::tool("big", std::string(300 * 1024, 'd')),
                  FakeMcpServer::tool("small")};
    McpListResult r = list();
    ASSERT_EQ(r.failure.error, McpError::None);
    EXPECT_EQ(names(r), (std::vector<std::string>{"big", "small"}));
}

TEST_F(McpClientTest, FailureLogsNeverCarryAHeaderValue) {
    fake.status = 401;
    EXPECT_EQ(list().failure.error, McpError::Unavailable);
    EXPECT_EQ(log_lines_with("files initialize error:http"), 1u);
    skew += wisp::net::kMcpRetryAfterMs;

    fake.status = 0;
    fake.raw_body = std::string(R"({"jsonrpc":"2.0","id":2,"error":{"code":-32001,"message":")") +
                    "bad token Bearer " + kSecret + R"("}})";
    EXPECT_EQ(list().failure.error, McpError::RpcError);
    EXPECT_EQ(log_lines_with("files initialize error:mcp_error"), 1u);
    expect_no_secret_in_logs();
}

TEST_F(McpClientTest, NotJsonRpcIsBadResponse) {
    fake.raw_body = "[]";
    EXPECT_EQ(list().failure.error, McpError::BadResponse);
    skew += wisp::net::kMcpRetryAfterMs;

    fake.raw_body.clear();
    fake.id_offset = 1;
    EXPECT_EQ(list().failure.error, McpError::BadResponse);
    skew += wisp::net::kMcpRetryAfterMs;

    // An SSE stream that ends without the response.
    fake.sse = true;
    fake.id_offset = 7;
    EXPECT_EQ(list().failure.error, McpError::BadResponse);
}

TEST_F(McpClientTest, RpcErrorCarriesTheCode) {
    fake.raw_body = R"({"jsonrpc":"2.0","id":1,"error":{"code":-32603,"message":"boom"}})";
    McpListResult r = list();
    EXPECT_EQ(r.failure.error, McpError::RpcError);
    EXPECT_EQ(r.failure.rpc_code, -32603);
    EXPECT_EQ(str(r.failure.message), "boom");
}

TEST_F(McpClientTest, ProbeReportsToolsAndLeavesNoState) {
    auto target = servers->resolve_for_request("files");
    ASSERT_TRUE(target.has_value());
    wisp::core::McpProbe p = McpClient::probe(*http, *target, "1.2.3");
    EXPECT_TRUE(p.ok);
    EXPECT_EQ(p.error, "");
    EXPECT_EQ(p.protocol, "2025-06-18");
    ASSERT_EQ(p.tools.size(), 2u);
    EXPECT_EQ(str(p.tools[0]), "read");
    EXPECT_EQ(str(p.tools[1]), "write");
    EXPECT_LT(p.latency_ms, 2000u);
    ASSERT_EQ(client->status().size(), 1u);
    EXPECT_EQ(client->status()[0].tools, std::nullopt);
    EXPECT_EQ(fake.count("initialize"), 1u);

    // The prober App injects is the same thing.
    servers->set_prober(wisp::net::make_mcp_prober(*http, "1.2.3"));
    auto tested = servers->test("files");
    ASSERT_TRUE(tested);
    EXPECT_TRUE(tested.value().ok);
}

// The admin test route waits for probe(): it has one overall deadline,
// however many requests and pages it would take, and abandons rather
// than cancels at it (the agent's requests share kAgentHttpId).
TEST_F(McpClientTest, ProbeHasAnOverallDeadline) {
    EXPECT_EQ(wisp::net::kMcpProbeTimeoutMs,
              wisp::net::kMcpRequestTimeoutMs + wisp::net::kMcpWaitSlackMs);
    auto target = *servers->resolve_for_request("files");
    fake.hold = true;
    const uint64_t start = real_ms();
    wisp::core::McpProbe p = McpClient::probe(*http, target, "1", /*timeout_ms=*/300);
    const uint64_t took = real_ms() - start;
    EXPECT_FALSE(p.ok);
    EXPECT_EQ(p.error, "timeout");
    EXPECT_GE(took, 250u);
    EXPECT_LT(took, 1500u);
    EXPECT_FALSE(fake.saw_cancel.load());
    fake.hold = false;
}

TEST_F(McpClientTest, ProbeMapsCategories) {
    auto target = *servers->resolve_for_request("files");
    http->set_deferring(true);
    EXPECT_EQ(McpClient::probe(*http, target, "1").error, "connect");
    http->set_deferring(false);

    fake.transport = TransportError::Other;
    EXPECT_EQ(McpClient::probe(*http, target, "1").error, "connect");
    fake.transport = TransportError::Dns;
    EXPECT_EQ(McpClient::probe(*http, target, "1").error, "dns");
    fake.transport = TransportError::None;

    fake.status = 401;
    auto p = McpClient::probe(*http, target, "1");
    EXPECT_EQ(p.error, "http");
    EXPECT_EQ(p.http_status, 401);
    fake.status = 0;

    fake.tools = {FakeMcpServer::tool("huge", std::string(600 * 1024, 'd'))};
    EXPECT_EQ(McpClient::probe(*http, target, "1").error, "bad_response");

    fake.protocol = "2024-11-05";
    p = McpClient::probe(*http, target, "1");
    EXPECT_FALSE(p.ok);
    EXPECT_EQ(p.error, "unsupported_protocol");
}

}  // namespace
