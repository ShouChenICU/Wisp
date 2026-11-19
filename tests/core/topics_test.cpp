#include "wisp/core/topics.hpp"

#include <gtest/gtest.h>

#include <iterator>
#include <set>
#include <string_view>

namespace topics = wisp::core::topics;

TEST(Topics, NamesAreUnique) {
    std::set<std::string_view> seen;
    for (const topics::Topic& t : topics::kAll) {
        EXPECT_FALSE(t.name.empty());
        EXPECT_TRUE(seen.insert(t.name).second) << "duplicate: " << t.name;
    }
}

// tools/check_topics.py reports a topic constant missing from kAll; this
// pins find()'s matching.
TEST(Topics, FindMatchesWholeRegisteredNamesOnly) {
    const topics::Topic* t = topics::find(topics::kPowerState);
    ASSERT_NE(t, nullptr);
    EXPECT_EQ(t->name, topics::kPowerState);
    // A button topic is "<device id>.<edge>" and a button's id is
    // "button.<name>": the pattern names the part after "button.".
    const topics::Topic* down = topics::find("button.<name>.down");
    ASSERT_NE(down, nullptr);
    EXPECT_EQ(down->kind, topics::Kind::InputEdge);
    EXPECT_EQ(topics::find("button.power.down"), nullptr);
    EXPECT_EQ(topics::find("power.stat"), nullptr);
    EXPECT_EQ(topics::find("power.state.x"), nullptr);
    EXPECT_EQ(topics::find(""), nullptr);
    // find() is usable at compile time, so a constant can be checked there.
    static_assert(topics::find(topics::kSceneState)->kind == topics::Kind::Notification);
}

TEST(Topics, TheRegistryHas31Rows) {
    // Four button patterns and 27 topics.
    EXPECT_EQ(std::size(topics::kAll), 31u);
    size_t edges = 0;
    for (const topics::Topic& t : topics::kAll) {
        if (t.kind == topics::Kind::InputEdge) {
            ++edges;
            EXPECT_TRUE(t.name.starts_with(topics::kButtonPrefix)) << t.name;
            EXPECT_FALSE(t.forwarded) << t.name;
        }
    }
    EXPECT_EQ(edges, 4u);
}

// The media topics (media spec 9): two cell mirrors and the error
// telemetry, all forwarded to the admin console.
TEST(Topics, MediaTopicsAreRegistered) {
    const topics::Topic* player = topics::find("media.player");
    const topics::Topic* library = topics::find("media.library");
    const topics::Topic* error = topics::find("media.error");
    ASSERT_NE(player, nullptr);
    ASSERT_NE(library, nullptr);
    ASSERT_NE(error, nullptr);
    EXPECT_EQ(player->name, topics::kMediaPlayer);
    EXPECT_EQ(library->name, topics::kMediaLibrary);
    EXPECT_EQ(error->name, topics::kMediaError);
    EXPECT_EQ(player->kind, topics::Kind::CellMirror);
    EXPECT_EQ(library->kind, topics::Kind::CellMirror);
    EXPECT_EQ(error->kind, topics::Kind::Telemetry);
    EXPECT_TRUE(player->forwarded);
    EXPECT_TRUE(library->forwarded);
    EXPECT_TRUE(error->forwarded);
}
