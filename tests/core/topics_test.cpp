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

TEST(Topics, TheRegistryHas28Rows) {
    // Four button patterns and 24 topics.
    EXPECT_EQ(std::size(topics::kAll), 28u);
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
