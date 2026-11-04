// PatchQueue: scene batches waiting for the UI thread, applied at most
// 64 ops a frame (scene-runtime-core spec 3.2, 10).

#include "wisp/scene/patch_queue.hpp"

#include <gtest/gtest.h>

#include <string>
#include <thread>

#include "support/scene_view_fixture.hpp"

namespace {

using wisp::scene::Focus;
using wisp::scene::NodeDesc;
using wisp::scene::PatchQueue;
using wisp::scene::Prop;
using wisp::scene::ScrollTo;
using wisp::scene::UiBatch;
using wisp::scene::UiOp;
using wisp::scene::Widget;
using wisp::test::node;
using wisp::test::set;
using wisp::test::str;

constexpr uint32_t kInstance = 1;

class PatchQueueTest : public wisp::test::SceneViewTest {
protected:
    // A screen with `labels` labels, ids 2 .. labels + 1.
    static UiOp tree(size_t labels) {
        wisp::scene::Build b;
        b.nodes.push_back(node(1, 0, Widget::Screen));
        for (size_t i = 0; i < labels; ++i)
            b.nodes.push_back(node(static_cast<uint32_t>(i + 2), 1, Widget::Label));
        return b;
    }

    static UiBatch batch(std::initializer_list<UiOp> ops, uint32_t instance = kInstance) {
        UiBatch b;
        b.instance = instance;
        for (const auto& op : ops) b.ops.push_back(op);
        return b;
    }

    size_t frame() {
        size_t n = 0;
        on_ui([&] { n = queue.apply_frame(*view, kInstance); });
        return n;
    }

    std::string text(wisp::scene::NodeId id) {
        std::string s;
        on_ui([&] {
            lv_obj_t* o = view->object(id);
            s = o ? lv_label_get_text(o) : "<none>";
        });
        return s;
    }

    PatchQueue queue;
};

TEST_F(PatchQueueTest, AppliesAtMost64PerFrame) {
    start();
    queue.submit(batch({tree(130)}));
    EXPECT_EQ(queue.pending(), 1u);
    EXPECT_EQ(frame(), 1u);

    UiBatch b;
    b.instance = kInstance;
    for (uint32_t i = 0; i < 130; ++i)
        b.ops.push_back(set(i + 2, Prop::Text, str(std::to_string(i).c_str())));
    queue.submit(std::move(b));
    EXPECT_EQ(queue.pending(), 130u);
    EXPECT_EQ(frame(), 64u);
    EXPECT_EQ(text(65), "63");
    EXPECT_NE(text(66), "64");
    EXPECT_EQ(frame(), 64u);
    EXPECT_EQ(frame(), 2u);
    EXPECT_EQ(frame(), 0u);
    EXPECT_EQ(queue.pending(), 0u);
    EXPECT_EQ(text(131), "129");
}

TEST_F(PatchQueueTest, BuildCountsOnceWithItsSetProps) {
    start();
    UiBatch b;
    b.instance = kInstance;
    b.ops.push_back(tree(100));
    for (uint32_t i = 0; i < 100; ++i) b.ops.push_back(set(i + 2, Prop::Text, str("x")));
    b.ops.push_back(ScrollTo{1, 0});
    queue.submit(std::move(b));
    EXPECT_EQ(queue.pending(), 2u);
    EXPECT_EQ(frame(), 2u);
    EXPECT_EQ(text(101), "x");
}

TEST_F(PatchQueueTest, CoalescesAcrossPendingBatches) {
    start();
    queue.submit(batch({tree(2)}));
    EXPECT_EQ(frame(), 1u);
    queue.submit(batch({set(2, Prop::Text, str("first"))}));
    queue.submit(batch({set(2, Prop::Text, str("second"))}));
    EXPECT_EQ(queue.pending(), 1u);
    EXPECT_EQ(frame(), 1u);
    EXPECT_EQ(text(2), "second");

    // Not across an op in between: the order stays as sent.
    queue.submit(batch({set(2, Prop::Text, str("a"))}));
    queue.submit(batch({ScrollTo{1, 0}}));
    queue.submit(batch({set(2, Prop::Text, str("b"))}));
    EXPECT_EQ(queue.pending(), 3u);
    EXPECT_EQ(frame(), 3u);
    EXPECT_EQ(text(2), "b");

    // A SetProp whose earlier value was already applied is queued anew.
    queue.submit(batch({set(3, Prop::Text, str("c"))}));
    EXPECT_EQ(frame(), 1u);
    queue.submit(batch({set(3, Prop::Text, str("d"))}));
    EXPECT_EQ(queue.pending(), 1u);
    EXPECT_EQ(frame(), 1u);
    EXPECT_EQ(text(3), "d");
}

TEST_F(PatchQueueTest, DropsOtherInstances) {
    start();
    queue.submit(batch({tree(1)}));
    EXPECT_EQ(frame(), 1u);
    queue.submit(batch({set(2, Prop::Text, str("old"))}, 7));
    queue.submit(batch({tree(3)}, 7));
    EXPECT_EQ(frame(), 0u);
    EXPECT_EQ(queue.pending(), 0u);
    EXPECT_EQ(text(2), "");
    EXPECT_EQ(text(4), "<none>");

    // Ops of another instance do not take this one's frame budget.
    UiBatch other;
    other.instance = 7;
    for (uint32_t i = 0; i < 100; ++i) other.ops.push_back(set(i + 2, Prop::Text, str("o")));
    queue.submit(std::move(other));
    queue.submit(batch({set(2, Prop::Text, str("mine"))}));
    EXPECT_EQ(frame(), 1u);
    EXPECT_EQ(text(2), "mine");
}

TEST_F(PatchQueueTest, OverflowCollapsesToFullRefresh) {
    start();
    queue.submit(batch({tree(1)}));
    EXPECT_EQ(frame(), 1u);
    EXPECT_FALSE(queue.take_refresh_request());

    UiBatch full;
    full.instance = kInstance;
    for (uint32_t i = 0; i < 1024; ++i) full.ops.push_back(set(i + 2, Prop::Text, str("v")));
    queue.submit(std::move(full));
    EXPECT_EQ(queue.pending(), 1024u);
    EXPECT_FALSE(queue.take_refresh_request());

    queue.submit(batch({set(5000, Prop::Text, str("w"))}));
    EXPECT_EQ(queue.pending(), 0u);
    EXPECT_TRUE(queue.take_refresh_request());
    EXPECT_FALSE(queue.take_refresh_request());
    EXPECT_EQ(frame(), 0u);
}

TEST_F(PatchQueueTest, OverflowKeepsAPendingBuild) {
    start();
    UiBatch b;
    b.instance = kInstance;
    b.ops.push_back(tree(2));
    b.ops.push_back(set(2, Prop::Text, str("built")));
    for (uint32_t i = 0; i < 1100; ++i) b.ops.push_back(Focus{i + 10});
    queue.submit(std::move(b));
    EXPECT_TRUE(queue.take_refresh_request());
    EXPECT_EQ(queue.pending(), 1u);
    EXPECT_EQ(frame(), 1u);
    EXPECT_EQ(text(2), "built");
}

TEST_F(PatchQueueTest, ClearDropsEverything) {
    start();
    queue.submit(batch({tree(1), set(2, Prop::Text, str("x"))}));
    queue.clear();
    EXPECT_EQ(queue.pending(), 0u);
    EXPECT_EQ(frame(), 0u);
    EXPECT_EQ(text(2), "<none>");
}

TEST_F(PatchQueueTest, SubmitsFromAnotherThread) {
    start();
    queue.submit(batch({tree(1)}));
    EXPECT_EQ(frame(), 1u);
    std::thread producer([&] {
        for (int i = 0; i < 200; ++i)
            queue.submit(batch({set(2, Prop::Text, str(std::to_string(i).c_str()))}));
    });
    for (int spins = 0; spins < 100000 && text(2) != "199"; ++spins) frame();
    producer.join();
    frame();
    EXPECT_EQ(text(2), "199");
    EXPECT_EQ(queue.pending(), 0u);
}

}  // namespace
