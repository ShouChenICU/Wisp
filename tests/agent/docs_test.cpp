// docs.read (agent-core spec 4.4): the scene API by topic, the widgets
// from the schema and the rest from docs/scene-api.md and the examples,
// compiled in.

#include "wisp/agent/docs.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <string>
#include <vector>

#include "wisp/agent/tool_registry.hpp"
#include "wisp/agent/tools.hpp"
#include "wisp/base/json.hpp"
#include "wisp/scene/schema.hpp"

namespace {

using wisp::Value;

bool has(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

}  // namespace

TEST(Docs, TopicsCoverEveryModuleAndWidget) {
    const std::vector<std::string> topics = wisp::agent::doc_topics();
    for (const char* t : {"widgets", "lua", "screen", "examples"}) {
        EXPECT_TRUE(has(topics, t)) << t;
        auto doc = wisp::agent::read_doc(t);
        ASSERT_TRUE(doc.has_value()) << t;
        EXPECT_FALSE(doc->empty()) << t;
    }
    for (size_t i = 0; i < wisp::scene::kWidgetCount; ++i) {
        const auto& w = wisp::scene::widget_spec(static_cast<wisp::scene::Widget>(i));
        const std::string topic = "widget/" + std::string(w.name);
        EXPECT_TRUE(has(topics, topic)) << topic;
        auto doc = wisp::agent::read_doc(topic);
        ASSERT_TRUE(doc.has_value()) << topic;
        EXPECT_NE(doc->find("ui." + std::string(w.name)), std::string::npos) << topic;
    }
    for (const char* m : {"store", "http", "timer", "device", "scene", "log", "notify", "power",
                          "sensor", "files"}) {
        const std::string topic = std::string("lua/") + m;
        EXPECT_TRUE(has(topics, topic)) << topic;
        auto doc = wisp::agent::read_doc(topic);
        ASSERT_TRUE(doc.has_value()) << topic;
        EXPECT_NE(doc->find(std::string("wisp.") + m), std::string::npos) << topic;
    }
    // wisp.http.download is a section of its own, under the same module.
    EXPECT_NE(wisp::agent::read_doc("lua/http")->find("wisp.http.download"), std::string::npos);
    EXPECT_NE(wisp::agent::read_doc("screen")->find("row_span"), std::string::npos);
    EXPECT_TRUE(has(topics, "example/counter"));
    auto counter = wisp::agent::read_doc("example/counter");
    ASSERT_TRUE(counter.has_value());
    EXPECT_NE(counter->find("scene.json"), std::string::npos);
    EXPECT_NE(counter->find("main.lua"), std::string::npos);
    EXPECT_NE(wisp::agent::read_doc("examples")->find("counter"), std::string::npos);
    // The order the tool lists them in.
    EXPECT_EQ(topics.front(), "widgets");
    EXPECT_FALSE(wisp::agent::read_doc("nope").has_value());
    EXPECT_FALSE(wisp::agent::read_doc("widget/nope").has_value());
}

TEST(Docs, UnknownTopicListsTopics) {
    wisp::agent::ToolRegistry reg(wisp::agent::ToolExecutors{});
    ASSERT_TRUE(wisp::agent::add_docs_tools(reg));
    const wisp::agent::ToolDef* def = reg.find("docs.read");
    ASSERT_NE(def, nullptr);
    EXPECT_EQ(def->thread, wisp::agent::ToolThread::Agent);
    wisp::agent::ToolCtx ctx;
    ctx.cancelled = [] { return false; };
    wisp::agent::ToolOutput out = reg.call("docs_read", R"({"topic":"nope"})", ctx);
    ASSERT_FALSE(out.ok);
    EXPECT_EQ(out.result.find("error")->as_string(), "not_found");
    const Value* topics = out.result.find("topics");
    ASSERT_NE(topics, nullptr);
    EXPECT_EQ(topics->as_list().size(), wisp::agent::doc_topics().size());

    wisp::agent::ToolOutput ok = reg.call("docs_read", R"({"topic":"widget/button"})", ctx);
    ASSERT_TRUE(ok.ok) << wisp::to_json(ok.result);
    EXPECT_NE(ok.result.find("text")->as_string().find("on_click"), std::string::npos);
}
