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

namespace {

PropValue big(size_t n) { return PropValue{LargeString(n, 'x')}; }

}  // namespace

TEST(PatchBatch, RefusesAStringOverThePropLimit) {
    using wisp::scene::kMaxPropStringBytes;
    PatchBatch b;
    EXPECT_TRUE(b.set_prop(1, kText, big(kMaxPropStringBytes)));
    auto r = b.set_prop(2, kText, big(kMaxPropStringBytes + 1));
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().message, "value too large");
    EXPECT_EQ(b.size(), 1u);
}

TEST(PatchBatch, CapsTheBatchFootprint) {
    using wisp::scene::kMaxBatchBytes;
    using wisp::scene::kMaxPropStringBytes;
    using wisp::scene::kOpOverheadBytes;
    PatchBatch b;
    const size_t per = kMaxPropStringBytes + kOpOverheadBytes;
    const size_t fit = kMaxBatchBytes / per;  // 15
    for (size_t i = 0; i < fit; ++i)
        ASSERT_TRUE(b.set_prop(static_cast<uint32_t>(i + 1), kText, big(kMaxPropStringBytes)));
    EXPECT_EQ(b.bytes(), fit * per);
    auto r = b.set_prop(100, kText, big(kMaxPropStringBytes));
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().message, "ui update too large");
    EXPECT_EQ(b.size(), fit);
    EXPECT_EQ(b.bytes(), fit * per);
    // Small ops still fit; other kinds of op are refused the same way.
    EXPECT_TRUE(b.push(ListReset{9, 1}));
    wisp::scene::Build build;
    build.nodes.resize(1);
    build.nodes[0].props.emplace_back(kText, big(kMaxPropStringBytes));
    EXPECT_FALSE(b.push(std::move(build)));
    // take() and clear() reset the count.
    b.take(1);
    EXPECT_EQ(b.bytes(), 0u);
    EXPECT_TRUE(b.set_prop(1, kText, big(10)));
    b.clear();
    EXPECT_EQ(b.bytes(), 0u);
}

TEST(PatchBatch, CoalescedValueReplacesItsOldSize) {
    using wisp::scene::kMaxBatchBytes;
    using wisp::scene::kMaxPropStringBytes;
    using wisp::scene::kOpOverheadBytes;
    PatchBatch b;
    const size_t per = kMaxPropStringBytes + kOpOverheadBytes;
    const size_t fit = kMaxBatchBytes / per;
    for (size_t i = 0; i < fit; ++i)
        ASSERT_TRUE(b.set_prop(static_cast<uint32_t>(i + 1), kText, big(kMaxPropStringBytes)));
    // Replacing a value many times never grows the batch.
    for (int i = 0; i < 100; ++i) ASSERT_TRUE(b.set_prop(1, kText, big(kMaxPropStringBytes)));
    EXPECT_EQ(b.bytes(), fit * per);
    ASSERT_TRUE(b.set_prop(1, kText, PropValue{true}));
    EXPECT_EQ(b.bytes(), fit * per - kMaxPropStringBytes);
}

TEST(PatchBatch, CountsNodesPropsAndCommands) {
    using wisp::scene::kOpOverheadBytes;
    PatchBatch b;
    wisp::scene::Build build;
    build.nodes.resize(2);
    build.nodes[1].props.emplace_back(kText, str("abc"));
    build.nodes[1].props.emplace_back(kHidden, PropValue{true});
    ASSERT_TRUE(b.push(std::move(build)));
    // The op, two nodes and two props, plus the string's bytes.
    EXPECT_EQ(b.bytes(), 5 * kOpOverheadBytes + 3);
    wisp::scene::CanvasDraw draw;
    draw.commands.resize(100);
    ASSERT_TRUE(b.push(std::move(draw)));
    EXPECT_EQ(b.bytes(), 6 * kOpOverheadBytes + 3 + 100);
}
