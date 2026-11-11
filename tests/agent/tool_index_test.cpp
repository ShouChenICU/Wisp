#include "wisp/agent/tool_index.hpp"

#include <gtest/gtest.h>

#include <string>
#include <thread>
#include <vector>

namespace {

wisp::core::SceneToolDecl tool(std::string name, std::string description) {
    return {std::move(name), std::move(description), R"({"type":"object"})", 30000};
}

}  // namespace

TEST(ToolIndex, ReplaceRemoveSearch) {
    wisp::agent::ToolIndex index;
    index.replace_scene("zeta", "Zulu", {tool("weather", "Current temperature for a saved city")});
    index.replace_scene(
        "alpha", "Alpha",
        {tool("today", "Saved city weather forecast"), tool("notes", "A personal notebook")});

    const auto matches = index.search("SAVED   CITY", "", 10);
    ASSERT_EQ(matches.size(), 2u);
    EXPECT_EQ(matches[0].full_name, "alpha.today");
    EXPECT_EQ(matches[1].full_name, "zeta.weather");

    const auto filtered = index.search("", "zeta", 10);
    ASSERT_EQ(filtered.size(), 1u);
    EXPECT_EQ(filtered[0].full_name, "zeta.weather");
    EXPECT_EQ(index.search("forecast missing", "", 10).size(), 0u);
    EXPECT_EQ(index.search("weather", "", 1).size(), 1u);
    EXPECT_EQ(index.size(), 3u);
    ASSERT_TRUE(index.find("zeta.weather"));

    index.replace_scene("zeta", "Zulu", {tool("forecast", "Tomorrow's weather")});
    EXPECT_FALSE(index.find("zeta.weather"));
    EXPECT_TRUE(index.find("zeta.forecast"));
    index.remove_scene("alpha");
    EXPECT_EQ(index.size(), 1u);
    EXPECT_FALSE(index.find("alpha.today"));
}

TEST(ToolIndex, ConcurrentSearchAndReplacement) {
    wisp::agent::ToolIndex index;
    const std::vector<wisp::core::SceneToolDecl> tools{tool("lookup", "A city lookup")};
    index.replace_scene("weather", "Weather", tools);

    std::thread writer([&] {
        for (int i = 0; i < 1000; ++i) {
            index.replace_scene("weather", "Weather", tools);
            if (i % 2 == 0) index.remove_scene("other");
        }
    });
    std::thread reader([&] {
        for (int i = 0; i < 1000; ++i) {
            const auto found = index.find("weather.lookup");
            const auto results = index.search("city", "weather", 20);
            EXPECT_LE(results.size(), 20u);
            if (found) EXPECT_EQ(found->scene_id, "weather");
        }
    });
    writer.join();
    reader.join();
    ASSERT_TRUE(index.find("weather.lookup"));
}
