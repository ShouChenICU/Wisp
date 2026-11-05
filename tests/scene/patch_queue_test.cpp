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

// `count` SetProps of text `value` on labels first .. first + count - 1.
UiBatch texts(uint32_t count, const char* value, bool refresh = false, uint32_t first = 2) {
    UiBatch b;
    b.instance = kInstance;
    b.refresh = refresh;
    for (uint32_t i = 0; i < count; ++i) b.ops.push_back(set(first + i, Prop::Text, str(value)));
    return b;
}

TEST_F(PatchQueueTest, OverflowCollapsesToFullRefresh) {
    start();
    queue.submit(batch({tree(1)}));
    EXPECT_EQ(frame(), 1u);
    EXPECT_FALSE(queue.take_refresh_request(kInstance));

    queue.submit(texts(1024, "v"));
    EXPECT_EQ(queue.pending(), 1024u);
    EXPECT_FALSE(queue.take_refresh_request(kInstance));

    // The backlog goes; the batch that overflowed it stays whole.
    queue.submit(batch({set(5000, Prop::Text, str("w"))}));
    EXPECT_EQ(queue.pending(), 1u);
    EXPECT_TRUE(queue.take_refresh_request(kInstance));
    EXPECT_FALSE(queue.take_refresh_request(kInstance));
    EXPECT_EQ(frame(), 1u);
    EXPECT_EQ(text(2), "");
}

TEST_F(PatchQueueTest, LoneLargeBatchIsAppliedWhole) {
    start();
    queue.submit(batch({tree(1500)}));
    EXPECT_EQ(frame(), 1u);
    queue.submit(texts(1500, "all"));
    EXPECT_FALSE(queue.take_refresh_request(kInstance));
    EXPECT_EQ(queue.pending(), 1500u);
    size_t applied = 0;
    for (int i = 0; i < 30 && queue.pending() > 0; ++i) applied += frame();
    EXPECT_EQ(applied, 1500u);
    EXPECT_FALSE(queue.take_refresh_request(kInstance));
    EXPECT_EQ(text(2), "all");
    EXPECT_EQ(text(1501), "all");
}

TEST_F(PatchQueueTest, RefreshOf1500BindingsTerminates) {
    start();
    queue.submit(batch({tree(1500)}));
    EXPECT_EQ(frame(), 1u);
    queue.submit(texts(1000, "stale"));
    queue.submit(texts(100, "newer", false, 1002));
    ASSERT_TRUE(queue.take_refresh_request(kInstance));

    // The scene task answers with every bound value; neither that batch
    // nor the small ones that follow while it drains ask again.
    int refreshes = 0;
    queue.submit(texts(1500, "current", /*refresh=*/true));
    for (int i = 0; i < 100 && queue.pending() > 0; ++i) {
        if (i % 4 == 0) queue.submit(batch({set(2, Prop::Text, str("current"))}));
        if (queue.take_refresh_request(kInstance)) {
            ++refreshes;
            queue.submit(texts(1500, "current", true));
        }
        frame();
    }
    EXPECT_EQ(refreshes, 0);
    EXPECT_EQ(queue.pending(), 0u);
    EXPECT_FALSE(queue.take_refresh_request(kInstance));
    EXPECT_EQ(text(2), "current");
    EXPECT_EQ(text(1501), "current");
}

TEST_F(PatchQueueTest, OverflowKeepsAPendingBuild) {
    start();
    queue.submit(batch({tree(2), set(2, Prop::Text, str("built"))}));
    UiBatch backlog;
    backlog.instance = kInstance;
    for (uint32_t i = 0; i < 1000; ++i) backlog.ops.push_back(set(i + 10, Prop::Text, str("x")));
    queue.submit(std::move(backlog));
    EXPECT_EQ(queue.pending(), 1001u);
    queue.submit(texts(30, "y", false, 2000));
    EXPECT_TRUE(queue.take_refresh_request(kInstance));
    EXPECT_EQ(queue.pending(), 31u);
    EXPECT_EQ(frame(), 31u);
    EXPECT_EQ(text(2), "built");
}

TEST_F(PatchQueueTest, OverflowReplaysLastScrollAndFocusAfterTheRefresh) {
    start("rect");
    queue.submit(batch({UiOp{wisp::test::build({
        node(1, 0, Widget::Screen),
        node(3, 1, Widget::Scroll,
             {{Prop::H, wisp::test::num(100)}, {Prop::W, wisp::test::num(200)}}),
        node(4, 3, Widget::Label, {{Prop::H, wisp::test::num(80)}}),
        node(5, 3, Widget::Label, {{Prop::H, wisp::test::num(80)}}),
        node(6, 3, Widget::Label, {{Prop::H, wisp::test::num(80)}}),
        node(7, 3, Widget::Label, {{Prop::H, wisp::test::num(80)}}),
        node(8, 1, Widget::Input),
        node(9, 1, Widget::Input),
    })}}));
    EXPECT_EQ(frame(), 1u);

    UiBatch backlog;
    backlog.instance = kInstance;
    backlog.ops.push_back(ScrollTo{3, 1});
    backlog.ops.push_back(Focus{8});
    backlog.ops.push_back(ScrollTo{3, 3});
    backlog.ops.push_back(Focus{9});
    for (uint32_t i = 0; i < 1000; ++i) backlog.ops.push_back(set(i + 100, Prop::Text, str("x")));
    queue.submit(std::move(backlog));
    queue.submit(texts(30, "y"));
    ASSERT_TRUE(queue.take_refresh_request(kInstance));
    EXPECT_EQ(queue.pending(), 30u);

    // Replayed after the refresh, newest per node only.
    UiBatch refresh;
    refresh.instance = kInstance;
    refresh.refresh = true;
    refresh.ops.push_back(set(4, Prop::Text, str("r")));
    queue.submit(std::move(refresh));
    EXPECT_EQ(queue.pending(), 33u);
    while (queue.pending() > 0) frame();
    on_ui([&] {
        ASSERT_NE(layer->keyboard(), nullptr);
        EXPECT_EQ(lv_keyboard_get_textarea(layer->keyboard()), view->object(9));
        lv_obj_update_layout(view->screen());
        lv_area_t box, child;
        lv_obj_get_coords(view->object(3), &box);
        lv_obj_get_coords(view->object(7), &child);
        EXPECT_LE(child.y2, box.y2);
        EXPECT_GE(child.y1, box.y1);
    });
}

// Overflow is each instance's own: another instance's backlog neither
// counts towards it nor is collapsed by it, and its request stays its own.
TEST_F(PatchQueueTest, OverflowIsPerInstance) {
    start();
    queue.submit(batch({tree(1)}));
    EXPECT_EQ(frame(), 1u);
    queue.submit(batch({set(2, Prop::Text, str("a")), ScrollTo{1, 0}}, 7));
    UiBatch other;
    other.instance = 7;
    for (uint32_t i = 0; i < 1000; ++i) other.ops.push_back(set(i + 10, Prop::Text, str("o")));
    queue.submit(std::move(other));
    // 1002 of instance 7 queued: 100 more of this one do not overflow.
    queue.submit(texts(100, "v"));
    EXPECT_FALSE(queue.take_refresh_request(kInstance));
    EXPECT_FALSE(queue.take_refresh_request(7));
    EXPECT_EQ(queue.pending(), 1102u);

    // This one's backlog overflows: only its own ops are collapsed.
    queue.submit(texts(1000, "w", false, 2000));
    EXPECT_TRUE(queue.take_refresh_request(kInstance));
    EXPECT_FALSE(queue.take_refresh_request(7));
    EXPECT_EQ(queue.pending(), 2002u);
}

// The ScrollTos and Focuses a collapse kept aside go out with their own
// instance's refresh batch, not with another instance's.
TEST_F(PatchQueueTest, KeptAsideOpsWaitForTheirOwnRefresh) {
    start();
    UiBatch backlog;
    backlog.instance = 7;
    backlog.ops.push_back(ScrollTo{3, 1});
    for (uint32_t i = 0; i < 1000; ++i) backlog.ops.push_back(set(i + 100, Prop::Text, str("x")));
    queue.submit(std::move(backlog));
    UiBatch more;
    more.instance = 7;
    for (uint32_t i = 0; i < 30; ++i) more.ops.push_back(set(i + 2, Prop::Text, str("y")));
    queue.submit(std::move(more));
    ASSERT_TRUE(queue.take_refresh_request(7));
    EXPECT_EQ(queue.pending(), 30u);

    queue.submit(texts(1, "mine", /*refresh=*/true));
    EXPECT_EQ(queue.pending(), 31u);

    UiBatch refresh;
    refresh.instance = 7;
    refresh.refresh = true;
    refresh.ops.push_back(set(4, Prop::Text, str("r")));
    queue.submit(std::move(refresh));
    // Its refresh op, then the ScrollTo kept for it.
    EXPECT_EQ(queue.pending(), 33u);
}

TEST_F(PatchQueueTest, ABuildSupersedesWhatIsQueuedBeforeIt) {
    start();
    queue.submit(batch({tree(3)}));
    queue.submit(texts(3, "old"));
    queue.submit(batch({ScrollTo{1, 0}}));
    queue.submit(batch({tree(2), set(2, Prop::Text, str("new"))}));
    EXPECT_EQ(queue.pending(), 1u);
    // Another instance's ops are left for apply_frame() to drop.
    queue.submit(batch({set(2, Prop::Text, str("other"))}, 7));
    queue.submit(batch({tree(2)}));
    EXPECT_EQ(queue.pending(), 2u);
    EXPECT_EQ(frame(), 1u);
    EXPECT_EQ(text(2), "");
    EXPECT_EQ(text(4), "<none>");
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
