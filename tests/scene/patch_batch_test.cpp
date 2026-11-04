#include "wisp/scene/patch_batch.hpp"

#include <gtest/gtest.h>

#include <variant>

using wisp::os::LargeString;
using wisp::scene::ListReset;
using wisp::scene::PatchBatch;
using wisp::scene::PropValue;
using wisp::scene::ScrollTo;
using wisp::scene::SetProp;

namespace {

constexpr wisp::scene::PropId kText = 15;
constexpr wisp::scene::PropId kHidden = 12;

PropValue str(const char* s) { return PropValue{LargeString(s)}; }

}  // namespace

TEST(PatchBatch, CoalescesSamePropKeepsOrder) {
    PatchBatch b;
    b.set_prop(1, kText, str("a"));
    b.push(ListReset{7, 3});
    b.set_prop(1, kText, str("b"));
    EXPECT_EQ(b.size(), 2u);

    auto batch = b.take(9);
    EXPECT_EQ(batch.instance, 9u);
    ASSERT_EQ(batch.ops.size(), 2u);
    const auto* set = std::get_if<SetProp>(&batch.ops[0]);
    ASSERT_NE(set, nullptr);
    EXPECT_EQ(set->node, 1u);
    EXPECT_EQ(set->prop, kText);
    ASSERT_TRUE(std::holds_alternative<LargeString>(set->value));
    EXPECT_EQ(std::get<LargeString>(set->value), "b");
    const auto* reset = std::get_if<ListReset>(&batch.ops[1]);
    ASSERT_NE(reset, nullptr);
    EXPECT_EQ(reset->list, 7u);
    EXPECT_EQ(reset->count, 3u);
}

TEST(PatchBatch, DistinctPropsAndNodesStaySeparate) {
    PatchBatch b;
    b.set_prop(1, kText, str("a"));
    b.set_prop(1, kHidden, PropValue{true});
    b.set_prop(2, kText, str("c"));
    b.push(SetProp{1, kHidden, PropValue{false}});  // pushed SetProps coalesce too
    b.push(ScrollTo{4, 0});
    b.push(ScrollTo{4, 1});  // other ops are never merged
    ASSERT_EQ(b.size(), 5u);
    auto batch = b.take(1);
    const auto* hidden = std::get_if<SetProp>(&batch.ops[1]);
    ASSERT_NE(hidden, nullptr);
    EXPECT_EQ(hidden->prop, kHidden);
    EXPECT_EQ(std::get<bool>(hidden->value), false);
}

TEST(PatchBatch, TakeAndClearEmptyTheBatch) {
    PatchBatch b;
    b.set_prop(1, kText, str("a"));
    auto first = b.take(1);
    EXPECT_EQ(first.ops.size(), 1u);
    EXPECT_EQ(b.size(), 0u);
    // The coalescing index went with the ops: a new value is a new op.
    b.set_prop(1, kText, str("b"));
    EXPECT_EQ(b.size(), 1u);
    b.clear();
    EXPECT_EQ(b.size(), 0u);
    b.set_prop(1, kText, str("c"));
    b.set_prop(1, kText, str("d"));
    auto second = b.take(2);
    ASSERT_EQ(second.ops.size(), 1u);
    EXPECT_EQ(std::get<LargeString>(std::get<SetProp>(second.ops[0]).value), "d");
}
