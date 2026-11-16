#include <gtest/gtest.h>

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "wisp/agent/tool_provider.hpp"
#include "wisp/agent/tool_registry.hpp"
#include "wisp/agent/tools.hpp"
#include "wisp/base/json.hpp"

namespace {

using wisp::Value;
using wisp::agent::ToolCtx;
using wisp::agent::ToolFindFields;
using wisp::agent::ToolInfo;
using wisp::agent::ToolOutput;
using wisp::agent::ToolProvider;
using wisp::agent::ToolRegistry;
using wisp::agent::ToolSearch;
using wisp::agent::ToolSourceIssue;

struct SearchCall {
    std::string query;
    std::string scope;
    size_t max = 0;
    bool details = false;
};

struct CallRecord {
    std::string name;
    std::string args_json;
};

ToolInfo info(std::string_view name, std::string_view origin = "o", bool available = true) {
    ToolInfo t;
    t.name.assign(name.data(), name.size());
    t.description = "d";
    t.parameters = R"({"type":"object"})";
    t.origin.assign(origin.data(), origin.size());
    t.available = available;
    return t;
}

class FakeProvider : public ToolProvider {
public:
    FakeProvider(std::string id, ToolFindFields fields, char separator)
        : id_(std::move(id)), fields_(fields), separator_(separator) {}

    std::string_view id() const override { return id_; }
    ToolFindFields find_fields() const override { return fields_; }
    bool owns(std::string_view full_name) const override {
        const bool has_colon = full_name.find(':') != std::string_view::npos;
        return separator_ == ':' ? has_colon : !has_colon;
    }
    void search(std::string_view query, std::string_view scope, size_t max, bool details, ToolCtx&,
                ToolSearch& out) override {
        searches.push_back({std::string(query), std::string(scope), max, details});
        const size_t n = overfill ? max + 5 : std::min(max, tools.size());
        for (size_t i = 0; i < n; ++i) out.tools.push_back(tools[i % tools.size()]);
        for (const ToolSourceIssue& issue : unavailable) out.unavailable.push_back(issue);
    }
    size_t count() const override { return total; }
    void report_unavailable(ToolSearch& out) const override {
        ++reports;
        for (const ToolSourceIssue& issue : unavailable) out.unavailable.push_back(issue);
    }
    std::optional<ToolInfo> describe(std::string_view, ToolCtx&) override { return std::nullopt; }
    ToolOutput call(std::string_view full_name, const Value& args, ToolCtx&) override {
        calls.push_back({std::string(full_name), wisp::to_json(args)});
        ToolOutput out;
        out.result = Value(wisp::Map{{"ok", Value(true)}, {"from", Value(id_)}});
        return out;
    }

    std::vector<ToolInfo> tools;
    size_t total = 0;
    bool overfill = false;
    std::vector<ToolSourceIssue> unavailable;
    std::vector<SearchCall> searches;
    std::vector<CallRecord> calls;
    mutable size_t reports = 0;

private:
    std::string id_;
    ToolFindFields fields_;
    char separator_;
};

std::string raw(const ToolOutput& output) {
    if (!output.result_json.empty()) {
        return std::string(output.result_json.data(), output.result_json.size());
    }
    return wisp::to_json(output.result);
}

Value parsed(const ToolOutput& output) {
    auto v = wisp::from_json(raw(output));
    EXPECT_TRUE(v) << raw(output);
    return v ? std::move(v.value()) : Value{};
}

std::vector<std::string> names(const Value& result) {
    std::vector<std::string> out;
    for (const Value& t : result.find("tools")->as_list()) {
        out.push_back(t.find("name")->as_string());
    }
    return out;
}

class ToolFindTest : public ::testing::Test {
protected:
    void SetUp() override {
        ctx.cancelled = [] { return false; };
        ctx.lang = "en";
    }

    void add_both() {
        reg.add_provider(a);
        reg.add_provider(b);
        ASSERT_TRUE(wisp::agent::add_find_tools(reg));
    }

    ToolOutput call(const char* wire, const Value& args = Value(wisp::Map{})) {
        return reg.call(wire, wisp::to_json(args), ctx);
    }

    FakeProvider a{"a", {"scene_name", "running"}, '.'};
    FakeProvider b{"b", {"server_name", "available"}, ':'};
    ToolRegistry reg{wisp::agent::ToolExecutors{}};
    ToolCtx ctx;
};

using ToolFind = ToolFindTest;
using ToolCall = ToolFindTest;

}  // namespace

TEST_F(ToolFind, AsksProvidersInRegistrationOrderAndMerges) {
    a.tools = {info("a1"), info("a2")};
    b.tools = {info("b1"), info("b2"), info("b3")};
    add_both();
    ToolOutput out = call("tools_find", Value(wisp::Map{{"query", Value("x")}}));
    ASSERT_TRUE(out.ok) << raw(out);
    EXPECT_EQ(names(parsed(out)), (std::vector<std::string>{"a1", "a2", "b1", "b2", "b3"}));
    ASSERT_EQ(a.searches.size(), 1u);
    ASSERT_EQ(b.searches.size(), 1u);
    EXPECT_EQ(a.searches[0].max, 20u);
    EXPECT_EQ(b.searches[0].max, 18u);
    EXPECT_TRUE(a.searches[0].details);
    EXPECT_TRUE(b.searches[0].details);
    EXPECT_EQ(a.searches[0].query, "x");
    EXPECT_EQ(a.searches[0].scope, "");
    EXPECT_EQ(b.searches[0].scope, "");
}

TEST_F(ToolFind, ListModeTotalIsTheSumOfCounts) {
    for (int i = 0; i < 250; ++i) a.tools.push_back(info("a" + std::to_string(i)));
    a.total = 250;
    b.tools = {info("b1")};
    b.total = 7;
    add_both();
    ToolOutput out = call("tools_find");
    ASSERT_TRUE(out.ok) << raw(out);
    const Value result = parsed(out);
    EXPECT_EQ(result.find("tools")->as_list().size(), 200u);
    EXPECT_EQ(result.find("total")->as_int(), 257);
    ASSERT_EQ(a.searches.size(), 1u);
    EXPECT_FALSE(a.searches[0].details);
    EXPECT_EQ(a.searches[0].max, 200u);
    // A list-mode entry is name and description only.
    const Value& first = result.find("tools")->as_list().front();
    EXPECT_EQ(first.find("parameters"), nullptr);
    EXPECT_EQ(first.find("scene_name"), nullptr);
}

TEST_F(ToolFind, MergedListKeepsTheLimitsWhenAProviderOverfills) {
    a.tools = {info("a1")};
    a.overfill = true;
    b.tools = {info("b1")};
    add_both();
    ToolOutput out = call("tools_find", Value(wisp::Map{{"query", Value("a")}}));
    ASSERT_TRUE(out.ok) << raw(out);
    EXPECT_EQ(parsed(out).find("tools")->as_list().size(), 20u);
    EXPECT_TRUE(b.searches.empty());
}

TEST_F(ToolFind, DetailsUseEachProvidersFieldNames) {
    a.tools = {info("a.one", "Weather", true)};
    b.tools = {info("srv:two", "Server", false)};
    add_both();
    ToolOutput out = call("tools_find", Value(wisp::Map{{"query", Value("")}}));
    ASSERT_TRUE(out.ok) << raw(out);
    EXPECT_EQ(
        raw(out),
        R"({"ok":true,"tools":[{"name":"a.one","description":"d","parameters":{"type":"object"},"scene_name":"Weather","running":true},{"name":"srv:two","description":"d","parameters":{"type":"object"},"server_name":"Server","available":false}]})");
}

TEST_F(ToolFind, UnavailableAppearsOnlyWhenSomeProviderReportsIt) {
    a.tools = {info("a1")};
    a.total = 1;
    add_both();
    ToolOutput clean = call("tools_find", Value(wisp::Map{{"query", Value("")}}));
    EXPECT_EQ(parsed(clean).find("unavailable"), nullptr) << raw(clean);

    ToolSourceIssue issue;
    issue.source = "srv";
    issue.error = "timeout";
    b.unavailable.push_back(issue);
    ToolOutput details = call("tools_find", Value(wisp::Map{{"query", Value("")}}));
    EXPECT_EQ(
        raw(details),
        R"({"ok":true,"tools":[{"name":"a1","description":"d","parameters":{"type":"object"},"scene_name":"o","running":true}],"unavailable":[{"server":"srv","error":"timeout"}]})");

    ToolOutput listed = call("tools_find");
    EXPECT_EQ(
        raw(listed),
        R"({"ok":true,"tools":[{"name":"a1","description":"d"}],"total":1,"unavailable":[{"server":"srv","error":"timeout"}]})");
}

// A provider the full list leaves unsearched still reports the sources it
// already knows to be failing.
TEST_F(ToolFind, SkippedProvidersStillReportUnavailableSources) {
    a.tools = {info("a1")};
    a.overfill = true;
    ToolSourceIssue issue;
    issue.source = "srv";
    issue.error = "connect";
    b.unavailable.push_back(issue);
    add_both();
    ToolOutput out = call("tools_find", Value(wisp::Map{{"query", Value("")}}));
    ASSERT_TRUE(out.ok) << raw(out);
    EXPECT_TRUE(b.searches.empty());
    EXPECT_EQ(b.reports, 1u);
    EXPECT_EQ(wisp::to_json(*parsed(out).find("unavailable")),
              R"([{"error":"connect","server":"srv"}])");
    EXPECT_EQ(a.reports, 0u);
}

TEST_F(ToolFind, CancelledBetweenProvidersIsCancelled) {
    a.tools = {info("a1")};
    b.tools = {info("b1")};
    add_both();
    ctx.cancelled = [this] { return !a.searches.empty(); };
    ToolOutput out = call("tools_find", Value(wisp::Map{{"query", Value("")}}));
    EXPECT_FALSE(out.ok);
    EXPECT_EQ(parsed(out).find("error")->as_string(), "cancelled");
    EXPECT_EQ(a.searches.size(), 1u);
    EXPECT_TRUE(b.searches.empty());
}

TEST_F(ToolFind, ScopeIsPassedToEveryProvider) {
    add_both();
    ToolOutput out = call("tools_find", Value(wisp::Map{{"scene", Value("srv")}}));
    ASSERT_TRUE(out.ok) << raw(out);
    ASSERT_EQ(a.searches.size(), 1u);
    ASSERT_EQ(b.searches.size(), 1u);
    EXPECT_EQ(a.searches[0].scope, "srv");
    EXPECT_EQ(b.searches[0].scope, "srv");
    EXPECT_TRUE(a.searches[0].details);
    EXPECT_TRUE(b.searches[0].details);
}

TEST_F(ToolCall, RoutesByOwnerAndPassesArgs) {
    add_both();
    ToolOutput out = call(
        "tools_call",
        Value(wisp::Map{{"name", Value("srv:echo")}, {"args", Value(wisp::Map{{"k", Value(1)}})}}));
    ASSERT_EQ(b.calls.size(), 1u);
    EXPECT_EQ(b.calls[0].name, "srv:echo");
    EXPECT_EQ(b.calls[0].args_json, R"({"k":1})");
    EXPECT_TRUE(a.calls.empty());
    EXPECT_EQ(raw(out), R"({"from":"b","ok":true})");
}

// ToolRegistry::call() rejects a missing "args" against kCallSchema before
// the handler runs, so the handler is called directly to see that it never
// dereferences a missing value.
TEST_F(ToolCall, MissingArgsReachTheProviderAsNull) {
    add_both();
    const wisp::agent::ToolDef* def = reg.find("tools.call");
    ASSERT_NE(def, nullptr);
    def->run(Value(wisp::Map{{"name", Value("x.y")}}), ctx);
    ASSERT_EQ(a.calls.size(), 1u);
    EXPECT_EQ(a.calls[0].name, "x.y");
    EXPECT_EQ(a.calls[0].args_json, "null");
}

TEST_F(ToolCall, NoOwnerIsNotFound) {
    reg.add_provider(b);
    ASSERT_TRUE(wisp::agent::add_find_tools(reg));
    ToolOutput out =
        call("tools_call", Value(wisp::Map{{"name", Value("x.y")}, {"args", Value(wisp::Map{})}}));
    EXPECT_FALSE(out.ok);
    EXPECT_EQ(raw(out), R"({"error":"not_found","message":"no tool named x.y","ok":false})");
    EXPECT_TRUE(b.calls.empty());
}

TEST(ToolRegistry, ProvidersKeepRegistrationOrder) {
    FakeProvider a{"a", {"scene_name", "running"}, '.'};
    FakeProvider b{"b", {"server_name", "available"}, ':'};
    ToolRegistry reg{wisp::agent::ToolExecutors{}};
    reg.add_provider(a);
    reg.add_provider(b);
    const auto providers = reg.providers();
    ASSERT_EQ(providers.size(), 2u);
    EXPECT_EQ(providers[0], &a);
    EXPECT_EQ(providers[1], &b);
}
