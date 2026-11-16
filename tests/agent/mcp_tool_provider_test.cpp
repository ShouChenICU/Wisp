#include "wisp/agent/mcp_tool_provider.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "support/fake_devices.hpp"
#include "support/fake_io_queue.hpp"
#include "support/fake_mcp_server.hpp"
#include "support/temp_dir.hpp"
#include "wisp/agent/limits.hpp"
#include "wisp/agent/tool_index.hpp"
#include "wisp/agent/tool_provider.hpp"
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
using wisp::agent::McpProviderConfig;
using wisp::agent::McpToolProvider;
using wisp::agent::ToolCtx;
using wisp::agent::ToolFindFields;
using wisp::agent::ToolInfo;
using wisp::agent::ToolOutput;
using wisp::agent::ToolProvider;
using wisp::agent::ToolRegistry;
using wisp::agent::ToolSearch;
using wisp::core::McpServerInput;
using wisp::core::McpServerService;
using wisp::core::McpServerUpdate;
using wisp::net::McpClient;
using wisp::net::SceneHttp;
using wisp::net::TransportError;
using wisp::test::FakeMcpServer;

constexpr const char* kReadSchema =
    R"({"type":"object","properties":{"path":{"type":"string"}},"required":["path"]})";

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

std::vector<std::string> names(const Value& result) {
    std::vector<std::string> out;
    if (const Value* tools = result.find("tools"))
        for (const Value& t : tools->as_list()) out.push_back(t.find("name")->as_string());
    return out;
}

const Value* entry(const Value& result, std::string_view name) {
    for (const Value& t : result.find("tools")->as_list())
        if (t.find("name")->as_string() == name) return &t;
    return nullptr;
}

std::string unavailable(const Value& result) {
    const Value* u = result.find("unavailable");
    return u == nullptr ? "" : wisp::to_json(*u);
}

std::string error_of(const ToolOutput& out) {
    const Value v = parsed(out);
    const Value* e = v.find("error");
    return e == nullptr ? "" : e->as_string();
}

// Stands in for the scene tools registered before the MCP provider: fills
// whatever room it is given when `fill` is set.
class FillerProvider : public ToolProvider {
public:
    std::string_view id() const override { return "scene"; }
    ToolFindFields find_fields() const override { return {"scene_name", "running"}; }
    bool owns(std::string_view full_name) const override {
        return full_name.find(':') == std::string_view::npos;
    }
    void search(std::string_view, std::string_view, size_t max, bool, ToolCtx&,
                ToolSearch& out) override {
        if (!fill) return;
        for (size_t i = 0; i < max; ++i) {
            ToolInfo t;
            t.name = "s.t" + std::to_string(i);
            t.description = "d";
            t.parameters = R"({"type":"object"})";
            out.tools.push_back(std::move(t));
        }
    }
    size_t count() const override { return 0; }
    std::optional<ToolInfo> describe(std::string_view, ToolCtx&) override { return std::nullopt; }
    ToolOutput call(std::string_view, const Value&, ToolCtx&) override { return {}; }

    bool fill = false;
};

class McpToolProviderTest : public ::testing::Test {
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
        add_server("Files", "http://files.local/mcp", "files");
        add_server("Web", "http://web.local/mcp", "web");
        http = std::make_unique<SceneHttp>(fake);
        ASSERT_TRUE(http->start());
        client = std::make_unique<McpClient>(
            *http, *servers,
            wisp::net::McpClientConfig{.client_version = "1",
                                       .now_ms = [this] { return now(); },
                                       .max_text_bytes = wisp::agent::kHttpMaxMaxBytes});
        fake.tools = {FakeMcpServer::tool("read", "Read a file", kReadSchema),
                      FakeMcpServer::tool("write", "Write a file")};
        start();
        ctx.cancelled = [] { return false; };
        ctx.lang = "en";
    }

    void TearDown() override {
        fake.hold = false;
        tools.reset();
        provider.reset();
        client.reset();
        http->stop();
        http.reset();
        servers.reset();
        storage.reset();
        bus.stop();
    }

    void add_server(const char* name, const char* url, const char* id) {
        McpServerInput in;
        in.name = name;
        in.url = url;
        auto made = servers->create(in);
        ASSERT_TRUE(made);
        ASSERT_EQ(made.value().id, id);
    }

    // (Re)builds the provider and the registry that holds it.
    void start(uint64_t budget_ms = wisp::agent::kMcpSearchBudgetMs) {
        tools.reset();
        provider = std::make_unique<McpToolProvider>(
            *client, *servers,
            McpProviderConfig{.search_budget_ms = budget_ms, .now_ms = [this] { return now(); }});
        tools = std::make_unique<ToolRegistry>(wisp::agent::ToolExecutors{});
        tools->add_provider(filler);
        tools->add_provider(*provider);
        ASSERT_TRUE(wisp::agent::add_find_tools(*tools));
    }

    // Real time plus a skew, shared by the client and the provider: waits
    // run in real time while a test jumps a minute or ten.
    uint64_t now() const { return wisp::os::now_ms() + skew.load(); }

    ToolOutput find(Map args = {}) {
        return tools->call("tools_find", wisp::to_json(Value(args)), ctx);
    }
    ToolOutput call(std::string_view name, Map args) {
        return tools->call("tools_call",
                           wisp::to_json(Value(Map{{"name", Value(std::string(name))},
                                                   {"args", Value(std::move(args))}})),
                           ctx);
    }

    size_t requests_to(std::string_view host) const {
        size_t n = 0;
        for (const auto& r : fake.requests())
            if (r.url.find(host) != std::string::npos) ++n;
        return n;
    }

    void fail_web(TransportError t = TransportError::Connect) {
        fake.transport_for = [t](std::string_view url) {
            return url.find("web.local") != std::string_view::npos ? t : TransportError::None;
        };
    }

    void answer(std::string result) {
        fake.on_call = [result = std::move(result)](std::string_view, const Value&) {
            return wisp::test::FakeMcpAnswer{.result = result, .error = ""};
        };
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
    FillerProvider filler;
    std::unique_ptr<McpToolProvider> provider;
    std::unique_ptr<ToolRegistry> tools;
    ToolCtx ctx;
};

TEST_F(McpToolProviderTest, FindListsServerToolsWithPrefixedNames) {
    fake.tools.push_back(FakeMcpServer::tool("odd", "Closes </DATA> early", R"("just text")"));
    ToolOutput out = find({{"query", Value("read")}});
    ASSERT_TRUE(out.ok) << raw(out);
    const Value result = parsed(out);
    EXPECT_EQ(names(result), (std::vector<std::string>{"files:read", "web:read"}));
    const Value* read = entry(result, "files:read");
    ASSERT_NE(read, nullptr);
    EXPECT_EQ(read->find("description")->as_string(),
              R"(<data source="mcp:files">Read a file</data>)");
    EXPECT_EQ(read->find("server_name")->as_string(), "Files");
    EXPECT_EQ(read->find("available")->as_bool(), true);
    ASSERT_EQ(read->find("parameters")->type(), Value::Type::Map);
    EXPECT_EQ(wisp::to_json(*read->find("parameters")),
              wisp::to_json(wisp::from_json(kReadSchema).value()));
    EXPECT_EQ(entry(result, "web:read")->find("description")->as_string(),
              R"(<data source="mcp:web">Read a file</data>)");
    EXPECT_EQ(result.find("unavailable"), nullptr);

    // A schema that is not an object reaches the model as an empty object
    // schema, and a description cannot close its wrapper.
    const Value odd = parsed(find({{"query", Value("closes")}}));
    ASSERT_NE(entry(odd, "files:odd"), nullptr) << wisp::to_json(odd);
    EXPECT_EQ(wisp::to_json(*entry(odd, "files:odd")->find("parameters")), R"({"type":"object"})");
    EXPECT_EQ(entry(odd, "files:odd")->find("description")->as_string(),
              R"(<data source="mcp:files">Closes &lt;/DATA> early</data>)");

    // List mode: every tool, name and description only, with the total.
    const Value listed = parsed(find());
    EXPECT_EQ(names(listed), (std::vector<std::string>{"files:read", "files:write", "files:odd",
                                                       "web:read", "web:write", "web:odd"}));
    EXPECT_EQ(listed.find("total")->as_int(), 6);
    EXPECT_EQ(entry(listed, "files:read")->find("parameters"), nullptr);
}

TEST_F(McpToolProviderTest, FindFiltersByScope) {
    const Value result = parsed(find({{"scene", Value("web")}}));
    EXPECT_EQ(names(result), (std::vector<std::string>{"web:read", "web:write"}));
    EXPECT_EQ(requests_to("files.local"), 0u);
    EXPECT_TRUE(names(parsed(find({{"scene", Value("weather")}}))).empty());
}

TEST_F(McpToolProviderTest, UnreachableServerIsListedUnavailable) {
    fail_web();
    const Value result = parsed(find({{"query", Value("")}}));
    EXPECT_EQ(names(result), (std::vector<std::string>{"files:read", "files:write"}));
    EXPECT_EQ(unavailable(result), R"([{"error":"connect","server":"web"}])");
}

TEST_F(McpToolProviderTest, FailedServerIsNotRetriedWithinAMinute) {
    fail_web();
    EXPECT_EQ(unavailable(parsed(find({{"query", Value("")}}))),
              R"([{"error":"connect","server":"web"}])");
    EXPECT_EQ(requests_to("web.local"), 1u);

    fake.transport_for = nullptr;
    skew += wisp::net::kMcpRetryAfterMs - 1000;
    EXPECT_EQ(unavailable(parsed(find({{"query", Value("")}}))),
              R"([{"error":"connect","server":"web"}])");
    EXPECT_EQ(unavailable(parsed(find())), R"([{"error":"connect","server":"web"}])");
    EXPECT_EQ(requests_to("web.local"), 1u);

    skew += 1000;
    const Value result = parsed(find({{"query", Value("")}}));
    EXPECT_EQ(result.find("unavailable"), nullptr) << wisp::to_json(result);
    EXPECT_EQ(names(result),
              (std::vector<std::string>{"files:read", "files:write", "web:read", "web:write"}));
}

// The merged search stops asking providers once the list is full, but a
// server known to be failing is still reported.
TEST_F(McpToolProviderTest, FailedServerIsListedWhenSceneToolsFillTheList) {
    fail_web();
    ASSERT_EQ(unavailable(parsed(find({{"query", Value("")}}))),
              R"([{"error":"connect","server":"web"}])");
    const size_t sent = fake.calls();

    filler.fill = true;
    const Value result = parsed(find({{"query", Value("")}}));
    EXPECT_EQ(names(result).size(), wisp::agent::kFindMax);
    EXPECT_EQ(names(result).front(), "s.t0");
    EXPECT_EQ(unavailable(result), R"([{"error":"connect","server":"web"}])");
    EXPECT_EQ(fake.calls(), sent);
}

// A find scoped to one scene names no MCP server, however full.
TEST_F(McpToolProviderTest, SceneScopedFullFindNamesNoServer) {
    fail_web();
    ASSERT_EQ(unavailable(parsed(find({{"query", Value("")}}))),
              R"([{"error":"connect","server":"web"}])");

    filler.fill = true;
    const Value scoped = parsed(find({{"scene", Value("weather")}}));
    EXPECT_EQ(names(scoped).size(), wisp::agent::kFindMax);
    EXPECT_EQ(scoped.find("unavailable"), nullptr) << wisp::to_json(scoped);
    // Scoped to the failing server itself, it is still named.
    EXPECT_EQ(unavailable(parsed(find({{"scene", Value("web")}}))),
              R"([{"error":"connect","server":"web"}])");
}

TEST_F(McpToolProviderTest, SearchBudgetBoundsSlowServers) {
    start(/*budget_ms=*/300);
    fake.hold = true;
    const uint64_t begin = wisp::os::now_ms();
    const Value result = parsed(find({{"query", Value("")}}));
    const uint64_t took = wisp::os::now_ms() - begin;
    EXPECT_LT(took, 1500u);
    EXPECT_TRUE(names(result).empty());
    EXPECT_EQ(unavailable(result),
              R"([{"error":"timeout","server":"files"},{"error":"timeout","server":"web"}])");

    // The budget ran out, the servers did not fail: no backoff.
    fake.hold = false;
    for (const auto& s : provider->status()) EXPECT_EQ(s.error, "") << s.id;
    start();
    const Value again = parsed(find({{"query", Value("")}}));
    EXPECT_EQ(again.find("unavailable"), nullptr) << wisp::to_json(again);
    EXPECT_EQ(names(again).size(), 4u);
}

TEST_F(McpToolProviderTest, SearchStopsBetweenServersWhenCancelled) {
    ctx.cancelled = [this] { return fake.count("tools/list") >= 1; };
    ToolOutput out = find({{"query", Value("")}});
    EXPECT_FALSE(out.ok);
    EXPECT_EQ(error_of(out), "cancelled");
    EXPECT_EQ(requests_to("files.local"), 3u);
    EXPECT_EQ(requests_to("web.local"), 0u);
}

TEST_F(McpToolProviderTest, CallValidatesLocallyWhenTheSchemaIsSupported) {
    ToolOutput out = call("files:read", {{"path", Value(5)}});
    EXPECT_FALSE(out.ok);
    const Value v = parsed(out);
    EXPECT_EQ(v.find("error")->as_string(), "bad_args");
    EXPECT_EQ(v.find("field")->as_string(), "path");
    EXPECT_EQ(fake.count("tools/call"), 0u);

    // Arguments that are not an object (tools.call checks its own schema
    // first, so straight to the provider).
    ToolOutput direct = provider->call("files:read", Value(5), ctx);
    EXPECT_EQ(error_of(direct), "bad_args");
    EXPECT_EQ(parsed(direct).find("field")->as_string(), "");
    EXPECT_EQ(fake.count("tools/call"), 0u);
}

TEST_F(McpToolProviderTest, CallSendsUnvalidatedWhenTheSchemaIsUnsupported) {
    fake.tools = {FakeMcpServer::tool(
        "read", "", R"({"type":"object","oneOf":[{"required":["path"]},{"required":["id"]}]})")};
    answer(R"({"content":[{"type":"text","text":"ok"}]})");
    ToolOutput out = call("files:read", {{"path", Value(5)}});
    EXPECT_TRUE(out.ok) << raw(out);
    ASSERT_EQ(fake.count("tools/call"), 1u);
    EXPECT_EQ(fake.requests().back().params.find("arguments")->find("path")->as_int(), 5);
}

TEST_F(McpToolProviderTest, CallResultShapeAndNote) {
    answer(R"({"content":[{"type":"text","text":"hello"}]})");
    ToolOutput out = call("files:read", {{"path", Value("/a")}});
    ASSERT_TRUE(out.ok) << raw(out);
    EXPECT_EQ(raw(out), R"({"ok":true,"result":"<data source=\"mcp:files\">hello</data>"})");
    EXPECT_EQ(out.note, "Call Files · read");
    ctx.lang = "zh";
    EXPECT_EQ(call("files:read", {{"path", Value("/a")}}).note, "调用 Files · read");
    EXPECT_EQ(fake.requests().back().params.find("arguments")->find("path")->as_string(), "/a");
}

// The client takes up to 1 MiB (kMcpMaxCallResponse; past it the answer
// is bad_response), but the tool result keeps no more than the largest
// payload any other tool returns (kHttpMaxMaxBytes), cut on a character
// boundary with a marker, so one call cannot put ~1 MiB into the stored
// conversation. A text error is cut the same way.
TEST_F(McpToolProviderTest, CallResultIsCutToTheToolPayloadCap) {
    using wisp::agent::kHttpMaxMaxBytes;
    // "x" then two-byte characters: byte kHttpMaxMaxBytes (even) falls
    // inside one.
    std::string big = "x";
    while (big.size() < 200 * 1024) big += "\xC3\xA9";
    const std::string marker = "\n[truncated, " + std::to_string(big.size()) + " bytes total]";
    const std::string kept = big.substr(0, kHttpMaxMaxBytes - 1);
    ASSERT_TRUE(wisp::valid_utf8(kept));

    for (const bool is_error : {false, true}) {
        answer(R"({"content":[{"type":"text","text":")" + big + R"("}],"isError":)" +
               (is_error ? "true" : "false") + "}");
        ToolOutput out = call("files:read", {{"path", Value("/a")}});
        EXPECT_EQ(out.ok, !is_error);
        const std::string json = raw(out);
        EXPECT_TRUE(wisp::valid_utf8(json)) << is_error;
        EXPECT_LE(json.size(), kHttpMaxMaxBytes + 256) << is_error;
        const Value v = parsed(out);
        const Value* text = v.find(is_error ? "message" : "result");
        ASSERT_NE(text, nullptr) << json.substr(0, 200);
        EXPECT_EQ(text->as_string(), "<data source=\"mcp:files\">" + kept + marker + "</data>")
            << is_error;
    }

    // A result under the cap is whole.
    answer(R"({"content":[{"type":"text","text":"x</data>short"}]})");
    EXPECT_EQ(parsed(call("files:read", {{"path", Value("/a")}})).find("result")->as_string(),
              "<data source=\"mcp:files\">x&lt;/data>short</data>");

    answer(R"({"content":[{"type":"text","text":")" +
           std::string(wisp::net::kMcpMaxCallResponse + 10, 't') + R"("}]})");
    EXPECT_EQ(error_of(call("files:read", {{"path", Value("/a")}})), "bad_response");
}

TEST_F(McpToolProviderTest, CallFailuresMapToToolErrors) {
    const Map args{{"path", Value("/a")}};

    fake.on_call = [](std::string_view, const Value&) {
        return wisp::test::FakeMcpAnswer{.result = "",
                                         .error = R"({"code":-32000,"message":"disk full"})"};
    };
    ToolOutput out = call("files:read", args);
    EXPECT_FALSE(out.ok);
    Value v = parsed(out);
    EXPECT_EQ(v.find("error")->as_string(), "mcp_error");
    EXPECT_EQ(v.find("message")->as_string(), R"(<data source="mcp:files">disk full</data>)");
    EXPECT_EQ(v.find("code")->as_int(), -32000);

    answer(R"({"content":[{"type":"text","text":"no such file"}],"isError":true})");
    v = parsed(call("files:read", args));
    EXPECT_EQ(v.find("ok")->as_bool(), false);
    EXPECT_EQ(v.find("error")->as_string(), "tool_error");
    EXPECT_EQ(v.find("message")->as_string(), R"(<data source="mcp:files">no such file</data>)");

    fake.status = 503;
    v = parsed(call("files:read", args));
    EXPECT_EQ(v.find("error")->as_string(), "mcp_unavailable");
    EXPECT_EQ(v.find("message")->as_string(), "http 503");
    fake.status = 0;

    fake.transport = TransportError::Connect;
    v = parsed(call("files:read", args));
    EXPECT_EQ(v.find("error")->as_string(), "mcp_unavailable");
    EXPECT_EQ(v.find("message")->as_string(), "connect");
    fake.transport = TransportError::None;

    fake.raw_body = "[]";
    EXPECT_EQ(error_of(call("files:read", args)), "bad_response");
    fake.raw_body.clear();

    fake.hold = true;
    ctx.deadline_ms = wisp::os::now_ms() + 200;
    EXPECT_EQ(error_of(call("files:read", args)), "timeout");
    ctx.deadline_ms = 0;
    fake.hold = false;

    EXPECT_EQ(error_of(call("files:delete", args)), "not_found");
    McpServerUpdate u;
    u.enabled = false;
    ASSERT_TRUE(servers->update("web", u));
    const size_t sent = fake.calls();
    EXPECT_EQ(error_of(call("web:read", args)), "not_found");
    EXPECT_EQ(error_of(call("nope:x", args)), "not_found");
    EXPECT_EQ(fake.calls(), sent);
}

TEST_F(McpToolProviderTest, CountIsCachedToolsOfEnabledServers) {
    EXPECT_EQ(provider->count(), 0u);
    ASSERT_TRUE(find({{"query", Value("")}}).ok);
    EXPECT_EQ(provider->count(), 4u);
    EXPECT_EQ(parsed(find()).find("total")->as_int(), 4);
    McpServerUpdate u;
    u.enabled = false;
    ASSERT_TRUE(servers->update("web", u));
    EXPECT_EQ(provider->count(), 2u);
}

TEST_F(McpToolProviderTest, SummaryLines) {
    EXPECT_EQ(provider->summary_lines(),
              (std::vector<std::string>{"files (not connected yet)", "web (not connected yet)"}));
    fake.tools.push_back(FakeMcpServer::tool("stat"));
    fail_web();
    ASSERT_TRUE(find({{"query", Value("")}}).ok);
    EXPECT_EQ(provider->summary_lines(),
              (std::vector<std::string>{"files (3 tools)", "web (unavailable: connect)"}));
    McpServerUpdate u;
    u.enabled = false;
    ASSERT_TRUE(servers->update("files", u));
    EXPECT_EQ(provider->summary_lines(), (std::vector<std::string>{"web (unavailable: connect)"}));
}

TEST_F(McpToolProviderTest, DescribeGivesTheDetailedEntry) {
    const auto read = provider->describe("files:read", ctx);
    ASSERT_TRUE(read.has_value());
    EXPECT_EQ(std::string_view(read->name), "files:read");
    EXPECT_EQ(std::string_view(read->origin), "Files");
    EXPECT_EQ(std::string_view(read->description),
              R"(<data source="mcp:files">Read a file</data>)");
    EXPECT_FALSE(read->parameters.empty());
    EXPECT_FALSE(provider->describe("files:nope", ctx).has_value());
    EXPECT_FALSE(provider->describe("nope:read", ctx).has_value());
    EXPECT_TRUE(provider->owns("files:read"));
    EXPECT_FALSE(provider->owns("weather.now"));
}

TEST(MatchQuery, SameRuleAsToolIndex) {
    using wisp::agent::match_query;
    EXPECT_TRUE(match_query("TEMPERATURE city", "weather.now", "Current temperature for a city"));
    EXPECT_TRUE(match_query("", "a", "b"));
    EXPECT_TRUE(match_query("  now\t", "weather.now", ""));
    EXPECT_FALSE(match_query("temperature rain", "weather.now", "Current temperature"));
    EXPECT_TRUE(match_query("天气", "w.now", "今天天气"));

    wisp::agent::ToolIndex index;
    index.replace_scene("weather", "Weather",
                        {{.name = "now",
                          .description = "Current temperature for a city",
                          .parameters = "",
                          .timeout_ms = 30000}});
    EXPECT_EQ(index.search("TEMPERATURE city", "", 20).size(), 1u);
    EXPECT_EQ(index.search("temperature rain", "", 20).size(), 0u);
}

}  // namespace
