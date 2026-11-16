// The mcp.* agent tools (llm-tool-providers spec 4.3, decision 7) over a
// real McpServerService and an McpToolProvider behind FakeMcpServer.

#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <string_view>

#include "support/fake_devices.hpp"
#include "support/fake_io_queue.hpp"
#include "support/fake_mcp_server.hpp"
#include "support/secret_pieces.hpp"
#include "support/temp_dir.hpp"
#include "wisp/agent/mcp_tool_provider.hpp"
#include "wisp/agent/tool_registry.hpp"
#include "wisp/agent/tools.hpp"
#include "wisp/base/json.hpp"
#include "wisp/core/device_registry.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/core/log.hpp"
#include "wisp/core/mcp_servers.hpp"
#include "wisp/core/storage_manager.hpp"
#include "wisp/net/mcp_client.hpp"
#include "wisp/net/scene_http.hpp"

namespace {

using wisp::Map;
using wisp::Value;
using wisp::agent::McpToolProvider;
using wisp::agent::ToolCtx;
using wisp::agent::ToolOutput;
using wisp::agent::ToolRegistry;
using wisp::core::McpServerInput;
using wisp::core::McpServerService;
using wisp::net::McpClient;
using wisp::net::SceneHttp;
using wisp::net::TransportError;
using wisp::test::FakeMcpServer;

constexpr std::string_view kSecret = "Bearer tok-SECRET-123456";

std::string raw(const ToolOutput& output) {
    if (!output.result_json.empty())
        return std::string(output.result_json.data(), output.result_json.size());
    return wisp::to_json(output.result);
}

Value parsed(const ToolOutput& output) {
    auto v = wisp::from_json(raw(output));
    EXPECT_TRUE(v) << raw(output);
    return v ? std::move(v.value()) : Value{};
}

std::string error_of(const ToolOutput& out) {
    const Value v = parsed(out);
    const Value* e = v.find("error");
    return e == nullptr ? "" : e->as_string();
}

const Value* server_entry(const Value& result, std::string_view id) {
    for (const Value& s : result.find("servers")->as_list())
        if (s.find("id")->as_string() == id) return &s;
    return nullptr;
}

class McpTools : public ::testing::Test {
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
        in.url = "http://files.local/mcp";
        in.headers.push_back({.name = "Authorization", .value = std::string(kSecret)});
        auto made = servers->create(in);
        ASSERT_TRUE(made);
        ASSERT_EQ(made.value().id, "files");
        http = std::make_unique<SceneHttp>(fake);
        ASSERT_TRUE(http->start());
        client = std::make_unique<McpClient>(*http, *servers,
                                             wisp::net::McpClientConfig{.client_version = "1"});
        provider = std::make_unique<McpToolProvider>(*client, *servers);
        tools = std::make_unique<ToolRegistry>(wisp::agent::ToolExecutors{});
        tools->add_provider(*provider);
        ASSERT_TRUE(wisp::agent::add_find_tools(*tools));
        ASSERT_TRUE(
            wisp::agent::add_mcp_tools(*tools, {.servers = *servers, .provider = *provider}));
        fake.tools = {FakeMcpServer::tool("read", "Read a file"),
                      FakeMcpServer::tool("write", "Write a file")};
        ctx.cancelled = [] { return false; };
        ctx.lang = "zh";
    }

    void TearDown() override {
        tools.reset();
        provider.reset();
        client.reset();
        http->stop();
        http.reset();
        servers.reset();
        storage.reset();
        bus.stop();
    }

    ToolOutput run(std::string_view wire, Map args) {
        return tools->call(wire, wisp::to_json(Value(std::move(args))), ctx);
    }

    wisp::core::DeviceRegistry registry;
    wisp::core::EventBus bus;
    wisp::test::FakeIoQueue io;
    std::unique_ptr<wisp::core::StorageManager> storage;
    std::unique_ptr<McpServerService> servers;
    FakeMcpServer fake;
    std::unique_ptr<SceneHttp> http;
    std::unique_ptr<McpClient> client;
    std::unique_ptr<McpToolProvider> provider;
    std::unique_ptr<ToolRegistry> tools;
    ToolCtx ctx;
};

TEST_F(McpTools, ServersNeverCarryHeaderValues) {
    McpServerInput web;
    web.name = "Web";
    web.url = "http://web.local/mcp";
    ASSERT_TRUE(servers->create(web));

    // Before any search: not listed yet, no failure.
    ToolOutput out = run("mcp_servers", {});
    ASSERT_TRUE(out.ok) << raw(out);
    EXPECT_FALSE(wisp::test::holds_secret_piece(raw(out), kSecret)) << raw(out);
    Value result = parsed(out);
    EXPECT_EQ(result.find("max")->as_int(), 8);
    ASSERT_EQ(result.find("servers")->as_list().size(), 2u);
    const Value* files = server_entry(result, "files");
    ASSERT_NE(files, nullptr);
    EXPECT_EQ(files->find("name")->as_string(), "Files");
    EXPECT_EQ(files->find("url")->as_string(), "http://files.local/mcp");
    EXPECT_TRUE(files->find("enabled")->as_bool());
    EXPECT_EQ(files->find("tools")->type(), Value::Type::Null);
    EXPECT_EQ(files->find("error")->type(), Value::Type::Null);
    EXPECT_EQ(wisp::to_json(*files->find("headers")), R"(["Authorization"])");
    EXPECT_EQ(wisp::to_json(*server_entry(result, "web")->find("headers")), "[]");

    // After a search: the listed server's tool count, the failed one's
    // category.
    fake.transport_for = [](std::string_view url) {
        return url.find("web.local") != std::string_view::npos ? TransportError::Connect
                                                               : TransportError::None;
    };
    ASSERT_TRUE(run("tools_find", {}).ok);
    out = run("mcp_servers", {});
    EXPECT_FALSE(wisp::test::holds_secret_piece(raw(out), kSecret)) << raw(out);
    result = parsed(out);
    EXPECT_EQ(server_entry(result, "files")->find("tools")->as_int(), 2);
    EXPECT_EQ(server_entry(result, "files")->find("error")->type(), Value::Type::Null);
    EXPECT_EQ(server_entry(result, "web")->find("tools")->type(), Value::Type::Null);
    EXPECT_EQ(server_entry(result, "web")->find("error")->as_string(), "connect");
}

TEST_F(McpTools, AddCreatesAnEnabledServerWithoutHeaders) {
    ToolOutput out = run(
        "mcp_add",
        {{"name", Value("Docs")},
         {"url", Value("https://docs.example/mcp")},
         {"headers", Value(wisp::List{Value(Map{{"name", Value("X")}, {"value", Value("v")}})})}});
    ASSERT_TRUE(out.ok) << raw(out);
    const Value result = parsed(out);
    const Value* server = result.find("server");
    ASSERT_NE(server, nullptr) << raw(out);
    EXPECT_EQ(server->find("id")->as_string(), "docs");
    EXPECT_EQ(server->find("name")->as_string(), "Docs");
    EXPECT_EQ(server->find("url")->as_string(), "https://docs.example/mcp");
    EXPECT_TRUE(server->find("enabled")->as_bool());
    EXPECT_EQ(out.note, "添加了 MCP 服务器「Docs」");

    // The extra "headers" argument is ignored: nothing of it is stored.
    const auto stored = servers->get("docs");
    ASSERT_TRUE(stored);
    EXPECT_TRUE(stored->enabled);
    EXPECT_TRUE(stored->headers.empty());
    const auto target = servers->resolve_for_request("docs");
    ASSERT_TRUE(target);
    EXPECT_TRUE(target->headers.empty());
}

TEST_F(McpTools, AddValidationIsBadArgs) {
    ToolOutput out = run("mcp_add", {{"name", Value("Ftp")}, {"url", Value("ftp://x")}});
    EXPECT_FALSE(out.ok);
    EXPECT_EQ(error_of(out), "bad_args") << raw(out);
    EXPECT_FALSE(parsed(out).find("message")->as_string().empty());
    EXPECT_TRUE(out.note.empty());
    EXPECT_EQ(servers->list().size(), 1u);

    // The name's limit is in the schema.
    out = run("mcp_add", {{"name", Value(std::string(33, 'n'))}, {"url", Value("http://a/mcp")}});
    EXPECT_EQ(error_of(out), "bad_args") << raw(out);
    EXPECT_EQ(servers->list().size(), 1u);
}

TEST_F(McpTools, AddPastEightIsLimitReached) {
    for (int i = 2; i <= 8; ++i) {
        McpServerInput in;
        in.name = "S" + std::to_string(i);
        in.url = "http://s" + std::to_string(i) + ".local/mcp";
        ASSERT_TRUE(servers->create(in));
    }
    const ToolOutput out =
        run("mcp_add", {{"name", Value("Ninth")}, {"url", Value("http://nine.local/mcp")}});
    EXPECT_FALSE(out.ok);
    EXPECT_EQ(error_of(out), "limit_reached") << raw(out);
    EXPECT_EQ(servers->list().size(), 8u);
}

TEST_F(McpTools, RemoveAndEnable) {
    ToolOutput out = run("mcp_enable", {{"id", Value("files")}, {"enabled", Value(false)}});
    ASSERT_TRUE(out.ok) << raw(out);
    EXPECT_FALSE(parsed(out).find("enabled")->as_bool());
    EXPECT_EQ(out.note, "停用了 MCP 服务器「Files」");
    EXPECT_FALSE(servers->get("files")->enabled);

    ctx.lang = "ja";
    out = run("mcp_enable", {{"id", Value("files")}, {"enabled", Value(true)}});
    ASSERT_TRUE(out.ok) << raw(out);
    EXPECT_TRUE(parsed(out).find("enabled")->as_bool());
    EXPECT_EQ(out.note, "MCP サーバー「Files」を有効化");
    EXPECT_TRUE(servers->get("files")->enabled);
    // The headers survive an enable switch.
    EXPECT_EQ(servers->get("files")->headers.size(), 1u);

    ctx.lang = "en";
    out = run("mcp_enable", {{"id", Value("files")}, {"enabled", Value(false)}});
    EXPECT_EQ(out.note, "Disabled MCP server \"Files\"");
    out = run("mcp_remove", {{"id", Value("files")}});
    ASSERT_TRUE(out.ok) << raw(out);
    EXPECT_EQ(out.note, "Removed MCP server \"Files\"");
    EXPECT_TRUE(servers->list().empty());

    out = run("mcp_remove", {{"id", Value("files")}});
    EXPECT_EQ(error_of(out), "not_found") << raw(out);
    EXPECT_TRUE(out.note.empty());
    out = run("mcp_enable", {{"id", Value("nope")}, {"enabled", Value(true)}});
    EXPECT_EQ(error_of(out), "not_found") << raw(out);
    EXPECT_TRUE(out.note.empty());

    // The remaining notes in every language.
    McpServerInput in;
    in.name = "Docs";
    in.url = "http://docs.local/mcp";
    ASSERT_TRUE(servers->create(in));
    ctx.lang = "zh";
    EXPECT_EQ(run("mcp_enable", {{"id", Value("docs")}, {"enabled", Value(true)}}).note,
              "启用了 MCP 服务器「Docs」");
    ctx.lang = "ja";
    EXPECT_EQ(run("mcp_enable", {{"id", Value("docs")}, {"enabled", Value(false)}}).note,
              "MCP サーバー「Docs」を無効化");
    ctx.lang = "en";
    EXPECT_EQ(run("mcp_enable", {{"id", Value("docs")}, {"enabled", Value(true)}}).note,
              "Enabled MCP server \"Docs\"");
    ctx.lang = "ja";
    EXPECT_EQ(run("mcp_remove", {{"id", Value("docs")}}).note, "MCP サーバー「Docs」を削除");
    ASSERT_TRUE(servers->create(in));
    ctx.lang = "zh";
    EXPECT_EQ(run("mcp_remove", {{"id", Value("docs")}}).note, "删除了 MCP 服务器「Docs」");
    ASSERT_TRUE(servers->create(in));
    ctx.lang = "ja";
    EXPECT_EQ(
        run("mcp_add", {{"name", Value("Wiki")}, {"url", Value("http://wiki.local/mcp")}}).note,
        "MCP サーバー「Wiki」を追加");
    ctx.lang = "en";
    EXPECT_EQ(
        run("mcp_add", {{"name", Value("Blog")}, {"url", Value("http://blog.local/mcp")}}).note,
        "Added MCP server \"Blog\"");
}

TEST_F(McpTools, RunOnTheAgentThreadWithoutConfirmation) {
    for (const char* name : {"mcp.servers", "mcp.add", "mcp.remove", "mcp.enable"}) {
        const wisp::agent::ToolDef* def = tools->find(name);
        ASSERT_NE(def, nullptr) << name;
        EXPECT_EQ(def->thread, wisp::agent::ToolThread::Agent) << name;
        EXPECT_NE(def->risk, wisp::agent::ToolRisk::Confirm) << name;
    }
    EXPECT_EQ(tools->find("mcp.servers")->risk, wisp::agent::ToolRisk::Normal);
    EXPECT_EQ(tools->find("mcp.add")->risk, wisp::agent::ToolRisk::Note);
}

}  // namespace
