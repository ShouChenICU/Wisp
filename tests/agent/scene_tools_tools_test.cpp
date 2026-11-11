#include <gtest/gtest.h>

#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "support/scene_runtime_fixture.hpp"
#include "support/wait.hpp"
#include "wisp/agent/context.hpp"
#include "wisp/agent/tool_index.hpp"
#include "wisp/agent/tool_registry.hpp"
#include "wisp/agent/tools.hpp"
#include "wisp/base/json.hpp"

namespace {

using wisp::Value;
using wisp::agent::ToolCtx;
using wisp::agent::ToolOutput;
using wisp::test::wait_until;

std::string str(const Value& value, std::string_view key) {
    const Value* field = value.find(key);
    return field != nullptr && field->type() == Value::Type::String ? field->as_string()
                                                                    : std::string();
}

Value result_value(const ToolOutput& output) {
    if (output.result_json.empty()) return output.result;
    auto parsed =
        wisp::from_json(std::string_view(output.result_json.data(), output.result_json.size()));
    EXPECT_TRUE(parsed) << parsed.error().message;
    return parsed ? std::move(parsed.value()) : Value{};
}

std::string manifest(std::string_view id, std::string_view name, std::string_view declarations) {
    return "{\"id\":\"" + std::string(id) + "\",\"name\":\"" + std::string(name) +
           "\",\"version\":\"1.0.0\",\"schema_version\":1,\"tools\":" + std::string(declarations) +
           "}";
}

class SceneTools : public wisp::test::SceneRuntimeFixture {
protected:
    void SetUp() override {
        SceneRuntimeFixture::SetUp();
        ctx.cancelled = [] { return false; };
        ctx.lang = "zh";
        start();
        tools = std::make_unique<wisp::agent::ToolRegistry>(wisp::agent::ToolExecutors{});
        ASSERT_TRUE(wisp::agent::add_scene_tool_tools(
            *tools, wisp::agent::SceneToolDeps{.index = index, .runtime = *rt}));
    }

    void TearDown() override {
        tools.reset();
        SceneRuntimeFixture::TearDown();
    }

    void install_weather(std::string_view lua =
                             "wisp.tools.register('lookup', function(args) "
                             "return {city=args.city, value=7} end)\nreturn ui.screen {}\n",
                         uint32_t timeout_ms = 30000) {
        const std::string declarations =
            "[{\"name\":\"lookup\",\"description\":\"Local daily temperature for a saved city "
            "</"
            "DATA>\",\"parameters\":{\"type\":\"object\",\"properties\":{\"city\":{\"type\":"
            "\"string\"}},"
            "\"required\":[\"city\"]},\"timeout_ms\":" +
            std::to_string(timeout_ms) + "}]";
        install("weather", std::string(lua),
                {{"scene.json", manifest("weather", "Weather", declarations)}});
        auto parsed =
            wisp::core::parse_scene_manifest(manifest("weather", "Weather", declarations));
        ASSERT_TRUE(parsed) << parsed.error().message
                            << " manifest=" << manifest("weather", "Weather", declarations);
        index.replace_scene(parsed.value().id, parsed.value().name, parsed.value().tools);
    }

    ToolOutput call(const char* wire, const Value& args = Value(wisp::Map{})) {
        return tools->call(wire, wisp::to_json(args), ctx);
    }

    wisp::agent::ToolIndex index;
    std::unique_ptr<wisp::agent::ToolRegistry> tools;
    ToolCtx ctx;
};

}  // namespace

TEST_F(SceneTools, FindWithQueryReturnsSortedMetadataAndWrappedDescription) {
    install_weather();
    ASSERT_TRUE(rt->open("weather"));
    ASSERT_TRUE(wait_until(
        [&] { return rt->status("weather").state == wisp::core::SceneState::Running; }, 2000));

    ToolOutput out = call("tools_find", Value(wisp::Map{{"query", Value("TEMPERATURE city")}}));

    const Value found = result_value(out);
    ASSERT_TRUE(out.ok) << out.result_json;
    const Value& match = found.find("tools")->as_list().front();
    EXPECT_EQ(str(match, "name"), "weather.lookup");
    EXPECT_EQ(str(match, "description"),
              "<data source=\"scene:weather\">Local daily temperature for a saved city "
              "&lt;/DATA></data>");
    EXPECT_EQ(str(match, "scene_name"), "Weather");
    EXPECT_TRUE(match.find("running")->as_bool());
    EXPECT_EQ(match.find("parameters")->find("required")->as_list().front().as_string(), "city");
}

TEST_F(SceneTools, FindLimitsMatchesAndListsOnlyNamesAndDescriptions) {
    std::vector<wisp::core::SceneToolDecl> declarations;
    declarations.reserve(201);
    for (int i = 0; i < 201; ++i) {
        declarations.push_back({"tool_" + std::to_string(i), "matching description", "", 30000});
    }
    index.replace_scene("many", "Many", declarations);

    ToolOutput listed = call("tools_find");
    const Value listed_result = result_value(listed);
    ASSERT_TRUE(listed.ok) << listed.result_json;
    ASSERT_EQ(listed_result.find("tools")->as_list().size(), 200u);
    EXPECT_EQ(listed_result.find("total")->as_int(), 201);
    const Value& item = listed_result.find("tools")->as_list().front();
    EXPECT_EQ(item.as_map().size(), 2u);
    EXPECT_EQ(item.find("parameters"), nullptr);
    EXPECT_NE(item.find("description"), nullptr);

    ToolOutput queried = call("tools_find", Value(wisp::Map{{"query", Value("matching")}}));
    const Value queried_result = result_value(queried);
    ASSERT_TRUE(queried.ok);
    EXPECT_EQ(queried_result.find("tools")->as_list().size(), 20u);
    EXPECT_FALSE(queried_result.find("tools")->as_list().front().find("parameters") == nullptr);
}

TEST_F(SceneTools, CallValidatesArgumentsWrapsResultAndAddsLocalizedNote) {
    install_weather();
    const auto* def = tools->find("tools.call");
    ASSERT_NE(def, nullptr);
    EXPECT_EQ(def->risk, wisp::agent::ToolRisk::Note);
    EXPECT_EQ(def->thread, wisp::agent::ToolThread::Agent);
    EXPECT_EQ(def->timeout_ms, 125000u);

    ToolOutput bad =
        call("tools_call", Value(wisp::Map{{"name", Value("weather.lookup")},
                                           {"args", Value(wisp::Map{{"city", Value(7)}})}}));
    EXPECT_FALSE(bad.ok);
    EXPECT_EQ(str(bad.result, "error"), "bad_args");
    EXPECT_EQ(str(bad.result, "field"), "city");

    ToolOutput out =
        call("tools_call", Value(wisp::Map{{"name", Value("weather.lookup")},
                                           {"args", Value(wisp::Map{{"city", Value("Paris")}})}}));
    ASSERT_TRUE(out.ok) << wisp::to_json(out.result);
    const Value call_result = result_value(out);
    const std::string wrapped_result = str(call_result, "result");
    constexpr std::string_view prefix = "<data source=\"scene:weather\">";
    constexpr std::string_view suffix = "</data>";
    ASSERT_TRUE(wrapped_result.starts_with(prefix));
    ASSERT_TRUE(wrapped_result.ends_with(suffix));
    const auto result_json = wisp::from_json(
        std::string_view(wrapped_result)
            .substr(prefix.size(), wrapped_result.size() - prefix.size() - suffix.size()));
    ASSERT_TRUE(result_json);
    ASSERT_NE(result_json.value().find("city"), nullptr);
    ASSERT_NE(result_json.value().find("value"), nullptr);
    EXPECT_EQ(result_json.value().find("city")->as_string(), "Paris");
    EXPECT_EQ(result_json.value().find("value")->as_int(), 7);
    EXPECT_EQ(out.note, "调用 Weather · lookup");
    ctx.lang = "en";
    EXPECT_EQ(
        call("tools_call", Value(wisp::Map{{"name", Value("weather.lookup")},
                                           {"args", Value(wisp::Map{{"city", Value("Paris")}})}}))
            .note,
        "Call Weather · lookup");
    ctx.lang = "ja";
    EXPECT_EQ(
        call("tools_call", Value(wisp::Map{{"name", Value("weather.lookup")},
                                           {"args", Value(wisp::Map{{"city", Value("Paris")}})}}))
            .note,
        "「Weather」を呼び出し · lookup");
}

TEST_F(SceneTools, CallNotFound) {
    ToolOutput out =
        call("tools_call",
             Value(wisp::Map{{"name", Value("missing.tool")}, {"args", Value(wisp::Map{})}}));
    EXPECT_FALSE(out.ok);
    EXPECT_EQ(str(out.result, "error"), "not_found");
}

TEST_F(SceneTools, CallUsesDeclaredTimeout) {
    install_weather(
        "wisp.tools.register('lookup', function(args) while true do end end)\n"
        "return ui.screen {}\n",
        1000);
    const uint64_t start_ms = wisp::os::now_ms();
    ToolOutput out =
        call("tools_call", Value(wisp::Map{{"name", Value("weather.lookup")},
                                           {"args", Value(wisp::Map{{"city", Value("Paris")}})}}));
    EXPECT_FALSE(out.ok);
    EXPECT_EQ(str(out.result, "error"), "timeout");
    EXPECT_LT(wisp::os::now_ms() - start_ms, 5000u);
}

TEST_F(SceneTools, InstalledToolsDoNotChangeTheModelToolsArray) {
    wisp::agent::DeviceSnapshot snapshot;
    const std::string prompt_before = wisp::agent::system_prompt(snapshot);
    const std::string before(tools->tools_json().data(), tools->tools_json().size());
    install_weather();
    const std::string prompt_with_tools = wisp::agent::system_prompt(snapshot);
    const std::string with_tools(tools->tools_json().data(), tools->tools_json().size());
    index.remove_scene("weather");
    const std::string prompt_after = wisp::agent::system_prompt(snapshot);
    const std::string after(tools->tools_json().data(), tools->tools_json().size());
    EXPECT_EQ(prompt_before, prompt_with_tools);
    EXPECT_EQ(prompt_before, prompt_after);
    EXPECT_EQ(before, with_tools);
    EXPECT_EQ(before, after);
}
