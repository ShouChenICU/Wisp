// docs.read (agent-core spec 4.4): the scene API by topic, widget/<name>
// from the schema and the rest from docs/scene-api.md and the examples,
// compiled in compressed.

#include "wisp/agent/docs.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <limits>
#include <map>
#include <string>
#include <vector>

#include "docs_data.hpp"
#include "wisp/agent/limits.hpp"
#include "wisp/agent/tool_registry.hpp"
#include "wisp/agent/tools.hpp"
#include "wisp/base/json.hpp"
#include "wisp/os/os.hpp"
#include "wisp/port/sim/sim_os.hpp"
#include "wisp/scene/schema.hpp"

namespace {

using wisp::Value;

bool has(const std::vector<std::string>& v, const std::string& s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

Value run_docs(const std::string& topic, int64_t offset) {
    wisp::agent::ToolRegistry reg(wisp::agent::ToolExecutors{});
    EXPECT_TRUE(wisp::agent::add_docs_tools(reg));
    wisp::agent::ToolCtx ctx;
    ctx.cancelled = [] { return false; };
    return reg
        .call("docs_read",
              R"({"topic":")" + topic + R"(","offset":)" + std::to_string(offset) + "}", ctx)
        .result;
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

// The lua, lua/<module> and screen topics are docs/scene-api.md, which is
// schema_markdown(), cut at its "## " headings: "## wisp.<m>..." goes to
// lua/<m>, "## Screen shape" to screen, the rest (the intro included) to
// lua; the HTML comment is left out and each topic is trimmed and ends in
// one newline. Each is exactly that text, and the topics come in the
// order widgets, widget/<name>..., lua, lua/<module>... (by name), screen,
// examples, example/<id>....
TEST(Docs, SceneApiTopicsAreTheSchemaDocCutAtItsHeadings) {
    std::string md = wisp::scene::schema_markdown();
    const size_t c0 = md.find("<!--");
    const size_t c1 = md.find("-->");
    ASSERT_NE(c0, std::string::npos);
    ASSERT_NE(c1, std::string::npos);
    size_t after = c1 + 3;
    while (after < md.size() && md[after] == '\n') ++after;
    md.erase(c0, after - c0);

    std::map<std::string, std::string> want{{"lua", ""}};
    std::string topic = "lua";
    size_t pos = 0;
    while (pos < md.size()) {
        size_t nl = md.find('\n', pos);
        nl = nl == std::string::npos ? md.size() : nl + 1;
        const std::string line = md.substr(pos, nl - pos);
        pos = nl;
        if (line.rfind("## ", 0) == 0) {
            const std::string title = line.substr(3);
            if (title.rfind("wisp.", 0) == 0) {
                size_t end = 5;
                while (end < title.size() &&
                       ((title[end] >= 'a' && title[end] <= 'z') || title[end] == '_')) {
                    ++end;
                }
                topic = "lua/" + title.substr(5, end - 5);
            } else if (title == "Screen shape\n") {
                topic = "screen";
            } else {
                topic = "lua";
            }
        }
        want[topic] += line;
    }
    ASSERT_GT(want.size(), 10u);
    ASSERT_TRUE(want.count("screen"));

    std::vector<std::string> order;
    for (size_t i = 0; i < wisp::scene::kWidgetCount; ++i) {
        order.push_back(
            "widget/" +
            std::string(wisp::scene::widget_spec(static_cast<wisp::scene::Widget>(i)).name));
    }
    order.insert(order.begin(), "widgets");
    order.push_back("lua");
    for (auto& [t, text] : want) {
        size_t b = text.find_first_not_of(" \t\r\n");
        size_t e = text.find_last_not_of(" \t\r\n");
        text = b == std::string::npos ? "\n" : text.substr(b, e - b + 1) + "\n";
        if (t.rfind("lua/", 0) == 0) order.push_back(t);
    }
    order.push_back("screen");

    for (const auto& [t, text] : want) {
        const auto doc = wisp::agent::read_doc(t);
        ASSERT_TRUE(doc.has_value()) << t;
        EXPECT_EQ(*doc, text) << t;
    }
    const std::vector<std::string> topics = wisp::agent::doc_topics();
    ASSERT_GT(topics.size(), order.size());
    EXPECT_EQ(std::vector<std::string>(topics.begin(), topics.begin() + order.size()), order);
    EXPECT_EQ(topics[order.size()], "examples");
    for (size_t i = order.size() + 1; i < topics.size(); ++i) {
        EXPECT_EQ(topics[i].rfind("example/", 0), 0u) << topics[i];
        if (i > order.size() + 1) EXPECT_LT(topics[i - 1], topics[i]);
    }
}

// widgets is the schema doc from "## Value types" up to "## Screen shape",
// and widget/<name> the schema's own text for that widget.
TEST(Docs, WidgetTopicsAreTheSchemaText) {
    const std::string md = wisp::scene::schema_markdown();
    const size_t from = md.find("## Value types");
    const size_t to = md.find("## Screen shape");
    ASSERT_NE(from, std::string::npos);
    ASSERT_NE(to, std::string::npos);
    EXPECT_EQ(*wisp::agent::read_doc("widgets"), md.substr(from, to - from));
    for (size_t i = 0; i < wisp::scene::kWidgetCount; ++i) {
        const std::string name(wisp::scene::widget_spec(static_cast<wisp::scene::Widget>(i)).name);
        EXPECT_EQ(*wisp::agent::read_doc("widget/" + name), wisp::scene::widget_markdown(name))
            << name;
    }
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

TEST(Docs, LongTopicComesInPagesThatJoinToTheWhole) {
    const std::string whole = *wisp::agent::read_doc("lua");
    ASSERT_GT(whole.size(), wisp::agent::kDocsPageBytes);
    std::string joined;
    int64_t offset = 0;
    for (int page = 0; page < 8; ++page) {
        const Value r = run_docs("lua", offset);
        const std::string text = r.find("text")->as_string();
        EXPECT_LE(text.size(), wisp::agent::kDocsPageBytes);
        EXPECT_EQ(r.find("total_bytes")->as_int(), static_cast<int64_t>(whole.size()));
        joined += text;
        const Value* next = r.find("next_offset");
        if (next == nullptr) break;
        EXPECT_EQ(text.back(), '\n');
        EXPECT_EQ(next->as_int(), static_cast<int64_t>(joined.size()));
        offset = next->as_int();
    }
    EXPECT_EQ(joined, whole);
}

TEST(Docs, ShortTopicHasNoNextOffset) {
    const Value r = run_docs("widget/label", 0);
    EXPECT_EQ(r.find("next_offset"), nullptr);
}

TEST(Docs, OffsetPastTheEndIsAnError) {
    const Value r = run_docs("widget/label", 1 << 20);
    EXPECT_EQ(r.find("ok")->as_bool(), false);
    EXPECT_EQ(r.find("error")->as_string(), "bad_offset");
}

// An offset that is there but not an integer is refused, never read as 0:
// the registry's schema check refuses it first, and the tool's own body
// (reached without that check, as a direct run is) says bad_offset.
TEST(Docs, OffsetThatIsNotAnIntegerIsAnError) {
    wisp::agent::ToolRegistry reg(wisp::agent::ToolExecutors{});
    ASSERT_TRUE(wisp::agent::add_docs_tools(reg));
    const wisp::agent::ToolDef* def = reg.find("docs.read");
    ASSERT_NE(def, nullptr);
    wisp::agent::ToolCtx ctx;
    ctx.cancelled = [] { return false; };
    for (const char* offset : {R"("12")", "1.5", "true", "null"}) {
        SCOPED_TRACE(offset);
        const std::string args = std::string(R"({"topic":"lua","offset":)") + offset + "}";
        const wisp::agent::ToolOutput via_registry = reg.call("docs_read", args, ctx);
        EXPECT_FALSE(via_registry.ok);
        auto parsed = wisp::from_json(args);
        ASSERT_TRUE(parsed);
        const wisp::agent::ToolOutput out = def->run(parsed.value(), ctx);
        EXPECT_FALSE(out.ok);
        const Value* err = out.result.find("error");
        ASSERT_NE(err, nullptr) << wisp::to_json(out.result);
        EXPECT_EQ(err->as_string(), "bad_offset") << wisp::to_json(out.result);
    }
}

// The compiled-in text inflates; a stream that is corrupt, truncated or
// of another size than the table says is bad data, not missing memory.
TEST(Docs, UnpackTellsBadDataFromNoMemory) {
    using wisp::ErrorCode;
    using wisp::agent::PackedText;
    using wisp::agent::unpack_text;
    const PackedText good = wisp::agent::examples_text();
    auto ok = unpack_text(good);
    ASSERT_TRUE(ok) << ok.error().message;
    EXPECT_EQ(ok.value().size(), good.size);

    // BTYPE 11 is reserved.
    const uint8_t reserved[] = {0x07, 0x00};
    auto bad = unpack_text(PackedText{reserved, 16});
    ASSERT_FALSE(bad);
    EXPECT_EQ(bad.error().code, ErrorCode::InvalidArgument);

    auto cut = unpack_text(PackedText{good.deflated.first(good.deflated.size() / 2), good.size});
    ASSERT_FALSE(cut);
    EXPECT_EQ(cut.error().code, ErrorCode::InvalidArgument);

    auto longer = unpack_text(PackedText{good.deflated, good.size + 1});
    ASSERT_FALSE(longer);
    EXPECT_EQ(longer.error().code, ErrorCode::InvalidArgument);

    wisp::port::sim::set_alloc_failure(wisp::os::Mem::Large, true);
    auto none = unpack_text(good);
    wisp::port::sim::set_alloc_failure(wisp::os::Mem::Large, false);
    ASSERT_FALSE(none);
    EXPECT_EQ(none.error().code, ErrorCode::NoMemory);
}

// Without the memory to inflate it, a topic reads as missing rather than
// crashing (docs.read itself says no_memory).
TEST(Docs, ReadWithoutMemoryFindsNothing) {
    wisp::port::sim::set_alloc_failure(wisp::os::Mem::Large, true);
    const bool found = wisp::agent::read_doc("example/counter").has_value();
    wisp::port::sim::set_alloc_failure(wisp::os::Mem::Large, false);
    EXPECT_FALSE(found);
}
