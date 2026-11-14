// The scene list page (wisp/shell/scene_list.hpp, mvp spec 6.4) in the
// shell: listed on the storage worker, sorted by SceneRecents, greyed and
// marked, opened with a tap and deleted from the long-press card.

#include "wisp/shell/scene_list.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <future>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "shell/page_fixture.hpp"
#include "support/agent_fixture.hpp"
#include "wisp/shell/scene_recents.hpp"

using wisp::shell::Page;
using wisp::shell::SceneList;
using wisp::shell::ShellScene;
using wisp::test::is_hidden;
using wisp::test::text_of;

namespace {

// 16x16, opaque.
const uint8_t kIconPng[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48, 0x44, 0x52,
    0x00, 0x00, 0x00, 0x10, 0x00, 0x00, 0x00, 0x10, 0x08, 0x02, 0x00, 0x00, 0x00, 0x90, 0x91, 0x68,
    0x36, 0x00, 0x00, 0x00, 0x16, 0x49, 0x44, 0x41, 0x54, 0x78, 0x9c, 0x63, 0x88, 0xda, 0xf2, 0x9f,
    0x24, 0xc4, 0x30, 0xaa, 0x61, 0x54, 0xc3, 0xf0, 0xd5, 0x00, 0x00, 0x75, 0x1d, 0x0d, 0x1f, 0xb9,
    0xfe, 0x2d, 0x83, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
};

std::shared_ptr<const wisp::os::LargeBuffer> icon() {
    auto b = wisp::os::LargeBuffer::allocate(sizeof(kIconPng));
    std::memcpy(b.data(), kIconPng, sizeof(kIconPng));
    return std::make_shared<const wisp::os::LargeBuffer>(std::move(b));
}

ShellScene scene(std::string id, std::string name) {
    ShellScene s;
    s.id = std::move(id);
    s.name = std::move(name);
    return s;
}

}  // namespace

// The fixture (also used by the shape checks in subpage_shapes_test.cpp).
class SceneListTest : public ::testing::Test {
protected:
    void boot(const std::string& shape = "rect", int32_t w = 480, int32_t h = 480) {
        recents = std::make_unique<wisp::shell::SceneRecents>(
            *agent.storage,
            agent.storage->write_root(wisp::core::Area::System).value() + "/shell/recents.json",
            [this](std::function<void()> job) { return agent.worker.submit(std::move(job)); });
        harness = std::make_unique<wisp::test::PageHarness>(shape, w, h);
        ASSERT_TRUE(harness->started());
        auto& d = harness->deps();
        d.submit_io = [this](std::function<void()> job) {
            return agent.worker.submit(std::move(job));
        };
        d.list_scenes = [this] {
            on_worker_listing = agent.worker.on_worker();
            std::lock_guard<std::mutex> lock(mu);
            return wisp::shell::ShellListing{scenes, false};
        };
        d.read_scene_icon = [this](const std::string& id) {
            std::lock_guard<std::mutex> lock(mu);
            icon_reads.push_back(id);
            return icon();
        };
        d.scene_has_data = [this](const std::string& id) {
            on_worker_has_data = agent.worker.on_worker();
            std::lock_guard<std::mutex> lock(mu);
            ++data_asks;
            return std::find(with_data.begin(), with_data.end(), id) != with_data.end();
        };
        d.recents = recents.get();
        d.open_scene = [this](const std::string& id) {
            std::lock_guard<std::mutex> lock(mu);
            opened.push_back(id);
        };
        d.delete_scene = [this](const std::string& id, bool with_data) {
            std::lock_guard<std::mutex> lock(mu);
            deleted.emplace_back(id, with_data);
            return wisp::Result<void>::ok();
        };
        harness->boot();
    }

    void TearDown() override {
        // A listing still queued posts to the harness's UI runtime.
        (void)agent.worker.drain(5000);
        harness.reset();
        recents.reset();
    }

    SceneList* page() { return harness->shell().scene_list(); }
    void open_page(size_t want) {
        harness->show(Page::Scenes);
        ASSERT_NE(page(), nullptr);
        ASSERT_TRUE(harness->ui_until([&] {
            return page()->cards() == want && (want > 0 || page()->empty_label() != nullptr);
        }));
        harness->on_ui([&] { lv_obj_update_layout(lv_screen_active()); });
    }

    wisp::test::AgentHarness agent;
    std::unique_ptr<wisp::shell::SceneRecents> recents;
    std::unique_ptr<wisp::test::PageHarness> harness;
    std::mutex mu;
    std::vector<ShellScene> scenes;
    std::vector<std::string> opened;
    std::vector<std::pair<std::string, bool>> deleted;
    // The scenes scene_has_data says have data.
    std::vector<std::string> with_data;
    int data_asks = 0;
    std::vector<std::string> icon_reads;
    std::atomic<bool> on_worker_listing{false};
    std::atomic<bool> on_worker_has_data{false};
};

TEST_F(SceneListTest, SortedMarkedAndGreyed) {
    boot();
    {
        ShellScene clock = scene("clock", "时钟");
        clock.icon_png = icon();
        ShellScene counter = scene("counter", "计数器");
        counter.background = true;
        ShellScene cam = scene("sdcam", "相册");
        cam.available = false;
        std::lock_guard<std::mutex> lock(mu);
        scenes = {std::move(clock), std::move(counter), std::move(cam), scene("todo", "待办")};
    }
    recents->opened("todo");
    recents->opened("counter");
    open_page(4);
    EXPECT_TRUE(on_worker_listing.load());
    harness->on_ui([&] {
        // Most recent first, then by id.
        EXPECT_EQ(page()->scene(0).id, "counter");
        EXPECT_EQ(page()->scene(1).id, "todo");
        EXPECT_EQ(page()->scene(2).id, "clock");
        EXPECT_EQ(page()->scene(3).id, "sdcam");
        EXPECT_NE(page()->mark(0), nullptr);
        EXPECT_EQ(text_of(page()->mark(0)), LV_SYMBOL_PLAY);
        EXPECT_EQ(page()->mark(1), nullptr);
        EXPECT_NE(page()->icon(2), nullptr);  // the PNG
        EXPECT_EQ(page()->icon(1), nullptr);  // the first character instead
        EXPECT_NE(wisp::test::find_label_containing(page()->card(1), "待"), nullptr);
        EXPECT_EQ(text_of(page()->name_label(1)), "待办");
        EXPECT_LT(lv_obj_get_style_opa(page()->card(3), LV_PART_MAIN), LV_OPA_COVER);
        EXPECT_EQ(lv_obj_get_style_opa(page()->card(0), LV_PART_MAIN), LV_OPA_COVER);
        // Cards at least 48 px, as many columns as the safe area holds.
        const lv_area_t a = wisp::test::coords_of(page()->card(0));
        EXPECT_GE(lv_area_get_width(&a), 48);
        EXPECT_GE(lv_area_get_height(&a), 48);
        EXPECT_GE(page()->columns(), 3);
    });
    // A tap on the greyed one does nothing.
    harness->tap(harness->centre_of(page()->card(3)));
    std::lock_guard<std::mutex> lock(mu);
    EXPECT_TRUE(opened.empty());
}

TEST_F(SceneListTest, TapOpensLongPressDeletes) {
    boot();
    {
        std::lock_guard<std::mutex> lock(mu);
        scenes = {scene("counter", "计数器"), scene("todo", "待办")};
        with_data = {"counter"};
    }
    open_page(2);
    harness->tap(harness->centre_of(page()->card(1)));
    {
        std::lock_guard<std::mutex> lock(mu);
        EXPECT_EQ(opened, (std::vector<std::string>{"todo"}));
    }
    // A long press opens the card: delete, with "delete its data too".
    harness->hold(harness->centre_of(page()->card(0)), 700);
    {
        std::lock_guard<std::mutex> lock(mu);
        EXPECT_EQ(opened.size(), 1u);  // no tap on release
    }
    ASSERT_TRUE(harness->ui_until([&] { return page()->popup_data() != nullptr; }));
    harness->on_ui([&] {
        ASSERT_NE(page()->popup(), nullptr);
        ASSERT_NE(page()->popup_data(), nullptr);
        EXPECT_EQ(lv_checkbox_get_text(page()->popup_data()), std::string("同时删除数据"));
        EXPECT_NE(wisp::test::find_button_with_label(page()->popup(), "删除"), nullptr);
        lv_obj_add_state(page()->popup_data(), LV_STATE_CHECKED);
        lv_obj_send_event(page()->popup_delete(), LV_EVENT_CLICKED, nullptr);
        EXPECT_EQ(page()->popup(), nullptr);
    });
    {
        std::lock_guard<std::mutex> lock(mu);
        ASSERT_EQ(deleted.size(), 1u);
        EXPECT_EQ(deleted[0], (std::pair<std::string, bool>{"counter", true}));
    }
    // A scene without data has no checkbox; the backdrop closes the card.
    harness->on_ui([&] { page()->long_press(1); });
    ASSERT_TRUE(harness->ui_until([&] { return !page()->popup_data_pending(); }));
    harness->on_ui([&] {
        ASSERT_NE(page()->popup(), nullptr);
        EXPECT_EQ(page()->popup_data(), nullptr);
    });
    harness->tap({5, 240});
    harness->on_ui([&] { EXPECT_EQ(page()->popup(), nullptr); });
    std::lock_guard<std::mutex> lock(mu);
    EXPECT_EQ(deleted.size(), 1u);
}

// Whether a scene has data is asked on the worker as its long-press card
// opens, not with the listing: until the answer the card has no "delete
// its data too", and data written after the listing still shows it.
TEST_F(SceneListTest, HasDataComputedOnLongPress) {
    boot();
    {
        std::lock_guard<std::mutex> lock(mu);
        scenes = {scene("counter", "计数器")};
    }
    open_page(1);
    {
        std::lock_guard<std::mutex> lock(mu);
        EXPECT_EQ(data_asks, 0);  // the listing asks nothing
    }
    // Held up on the worker: the card is up without the option.
    std::promise<void> gate;
    std::shared_future<void> held = gate.get_future().share();
    ASSERT_TRUE(agent.worker.submit([held] { held.wait(); }));
    harness->on_ui([&] { page()->long_press(0); });
    harness->on_ui([&] {
        ASSERT_NE(page()->popup(), nullptr);
        EXPECT_TRUE(page()->popup_data_pending());
        EXPECT_EQ(page()->popup_data(), nullptr);
        EXPECT_NE(page()->popup_delete(), nullptr);
    });
    gate.set_value();
    ASSERT_TRUE(harness->ui_until([&] { return !page()->popup_data_pending(); }));
    EXPECT_TRUE(on_worker_has_data.load());
    harness->on_ui([&] {
        EXPECT_EQ(page()->popup_data(), nullptr);  // no data yet
        page()->close_popup();
    });
    // The scene writes its first data; the listing is not read again, and
    // the next long press finds it.
    {
        std::lock_guard<std::mutex> lock(mu);
        with_data = {"counter"};
    }
    harness->on_ui([&] { page()->long_press(0); });
    ASSERT_TRUE(harness->ui_until([&] { return page()->popup_data() != nullptr; }));
    harness->on_ui([&] {
        // Above the delete button, the card still inside the safe area.
        EXPECT_LT(lv_obj_get_index(page()->popup_data()), lv_obj_get_index(page()->popup_delete()));
        lv_obj_update_layout(page()->popup());
        lv_obj_t* card = lv_obj_get_parent(page()->popup_delete());
        EXPECT_TRUE(wisp::test::box_visible(harness->shape(), wisp::test::coords_of(card)));
        lv_obj_add_state(page()->popup_data(), LV_STATE_CHECKED);
        lv_obj_send_event(page()->popup_delete(), LV_EVENT_CLICKED, nullptr);
    });
    std::lock_guard<std::mutex> lock(mu);
    EXPECT_EQ(data_asks, 2);
    ASSERT_EQ(deleted.size(), 1u);
    EXPECT_EQ(deleted[0], (std::pair<std::string, bool>{"counter", true}));
}

// An answer for a card already closed (or replaced by another) changes
// nothing.
TEST_F(SceneListTest, LateHasDataAnswerIsDropped) {
    boot();
    {
        std::lock_guard<std::mutex> lock(mu);
        scenes = {scene("counter", "计数器"), scene("todo", "待办")};
        with_data = {"counter"};
    }
    open_page(2);
    std::promise<void> gate;
    std::shared_future<void> held = gate.get_future().share();
    ASSERT_TRUE(agent.worker.submit([held] { held.wait(); }));
    harness->hold(harness->centre_of(page()->card(0)), 700);
    harness->on_ui([&] { page()->long_press(1); });  // todo's card instead
    gate.set_value();
    ASSERT_TRUE(harness->ui_until([&] { return !page()->popup_data_pending(); }));
    (void)agent.worker.drain(2000);
    harness->on_ui([&] {
        ASSERT_NE(page()->popup(), nullptr);
        EXPECT_EQ(page()->popup_data(), nullptr);
    });
}

// A listing that names an icon by its hash alone: read once through
// read_scene_icon, then reused for as long as the hash stays.
TEST_F(SceneListTest, IconByHashReadOnlyWhenUnknown) {
    boot();
    const uint64_t hash = wisp::shell::icon_hash(icon()->span());
    {
        ShellScene clock = scene("clock", "时钟");
        clock.icon_hash = hash;
        std::lock_guard<std::mutex> lock(mu);
        scenes = {std::move(clock)};
    }
    open_page(1);
    ASSERT_TRUE(harness->ui_until([&] { return page()->icon(0) != nullptr; }));
    harness->show(Page::Home);
    harness->show(Page::Scenes);
    (void)agent.worker.drain(5000);
    harness->on_ui([&] { EXPECT_NE(page()->icon(0), nullptr); });
    {
        std::lock_guard<std::mutex> lock(mu);
        EXPECT_EQ(icon_reads, (std::vector<std::string>{"clock"}));
        // Another hash: read again.
        scenes[0].icon_hash = hash + 1;
    }
    harness->bus().publish({"scene.state", "test", 0, {{"id", "clock"}, {"state", "installed"}}});
    EXPECT_TRUE(harness->ui_until([&] {
        std::lock_guard<std::mutex> lock(mu);
        return icon_reads.size() == 2;
    }));
}

TEST_F(SceneListTest, EmptyHint) {
    boot();
    open_page(0);
    harness->on_ui([&] {
        ASSERT_NE(page()->empty_label(), nullptr);
        EXPECT_EQ(text_of(page()->empty_label()), "还没有场景，试试对我说'做一个番茄钟'");
        EXPECT_TRUE(wisp::test::box_visible(harness->shape(),
                                            wisp::test::coords_of(page()->empty_label())));
    });
}

TEST_F(SceneListTest, SceneStateListsAgain) {
    boot();
    open_page(0);
    {
        std::lock_guard<std::mutex> lock(mu);
        scenes = {scene("todo", "待办")};
    }
    harness->bus().publish({"scene.state", "test", 0, {{"id", "todo"}, {"state", "installed"}}});
    ASSERT_TRUE(harness->ui_until([&] { return page()->cards() == 1; }));
    harness->on_ui([&] { EXPECT_EQ(page()->empty_label(), nullptr); });
}

// A page reopened while the last page's listing still waits behind other
// work on the worker lists for itself: it does not come up blank.
TEST_F(SceneListTest, ReopenedWhileAListingWaitsIsFilled) {
    boot();
    {
        std::lock_guard<std::mutex> lock(mu);
        scenes = {scene("todo", "待办")};
    }
    std::promise<void> gate;
    std::shared_future<void> opened_gate = gate.get_future().share();
    ASSERT_TRUE(agent.worker.submit([opened_gate] { opened_gate.wait(); }));
    harness->show(Page::Scenes);
    harness->show(Page::Home);
    harness->show(Page::Scenes);
    gate.set_value();
    ASSERT_TRUE(harness->ui_until([&] { return page() != nullptr && page()->cards() == 1; }));
}

// Icons are decoded off the UI thread, scaled once to the card's icon
// square, and decoded again only when the file changes.
TEST(SceneIcon, DecodedAndScaledToTheSquare) {
    // The decoder's buffers come from handlers lv_init() sets up.
    if (!lv_is_initialized()) lv_init();
    auto png = icon();
    auto big = wisp::shell::decode_scene_icon(*png, 48);
    ASSERT_NE(big, nullptr);
    EXPECT_EQ(big->image.header.cf, LV_COLOR_FORMAT_ARGB8888);
    EXPECT_EQ(big->image.header.w, 48u);
    EXPECT_EQ(big->image.header.h, 48u);
    EXPECT_EQ(big->image.header.stride, 48u * 4);
    auto same = wisp::shell::decode_scene_icon(*png, 16);
    auto small = wisp::shell::decode_scene_icon(*png, 8);
    ASSERT_NE(same, nullptr);
    ASSERT_NE(small, nullptr);
    EXPECT_EQ(small->image.header.w, 8u);
    // Opaque in, opaque out; an even colour stays that colour, scaled up
    // or down.
    const uint8_t* a = same->pixels.data();
    for (const auto* icon : {big.get(), small.get()}) {
        const uint8_t* b = icon->pixels.data();
        EXPECT_EQ(b[3], 255);
        EXPECT_EQ(std::memcmp(a, b, 4), 0);
    }

    // Not a PNG, or one too large: nothing.
    auto junk = wisp::os::LargeBuffer::allocate(64);
    std::memset(junk.data(), 0x55, junk.size());
    EXPECT_EQ(wisp::shell::decode_scene_icon(junk, 48), nullptr);
    auto wide = wisp::os::LargeBuffer::allocate(sizeof(kIconPng));
    std::memcpy(wide.data(), kIconPng, sizeof(kIconPng));
    wide.data()[18] = 0x01;  // width 0x0110 = 272 > kIconMaxSide
    wide.data()[19] = 0x10;
    EXPECT_EQ(wisp::shell::decode_scene_icon(wide, 48), nullptr);
    EXPECT_EQ(wisp::shell::decode_scene_icon(*png, 0), nullptr);
}

TEST_F(SceneListTest, IconDecodedOnceAcrossListings) {
    boot();
    {
        ShellScene clock = scene("clock", "时钟");
        clock.icon_png = icon();
        std::lock_guard<std::mutex> lock(mu);
        scenes = {std::move(clock)};
    }
    open_page(1);
    ASSERT_TRUE(harness->ui_until([&] { return page()->icon(0) != nullptr; }));
    const wisp::shell::SceneIcon* first = nullptr;
    harness->on_ui([&] {
        first = page()->scene(0).icon.get();
        ASSERT_NE(first, nullptr);
        EXPECT_EQ(page()->scene(0).icon_png, nullptr);  // the file's bytes are let go
        EXPECT_EQ(static_cast<int32_t>(first->image.header.w),
                  SceneList::icon_side(harness->shape()));
        EXPECT_EQ(lv_image_get_src(page()->icon(0)), &first->image);
    });
    // Read again (a fresh copy of the same bytes): the same pixels, there
    // with the card.
    {
        std::lock_guard<std::mutex> lock(mu);
        scenes[0].icon_png = icon();
        scenes.push_back(scene("todo", "待办"));
    }
    harness->bus().publish({"scene.state", "test", 0, {{"id", "todo"}, {"state", "installed"}}});
    ASSERT_TRUE(harness->ui_until([&] { return page()->cards() == 2; }));
    harness->on_ui([&] {
        EXPECT_EQ(page()->scene(0).icon.get(), first);
        EXPECT_NE(page()->icon(0), nullptr);
    });
    // The icon gone from the manifest: the card shows the first character.
    {
        std::lock_guard<std::mutex> lock(mu);
        scenes[0].icon_png = nullptr;
        scenes.pop_back();
    }
    harness->bus().publish({"scene.state", "test", 0, {{"id", "todo"}, {"state", "deleted"}}});
    ASSERT_TRUE(harness->ui_until([&] { return page()->cards() == 1; }));
    harness->on_ui([&] {
        EXPECT_EQ(page()->scene(0).icon, nullptr);
        EXPECT_EQ(page()->icon(0), nullptr);
    });
}

// A refresh while the long-press card is up keeps it, unless its scene
// went away.
TEST_F(SceneListTest, RefreshKeepsTheLongPressCard) {
    boot();
    {
        std::lock_guard<std::mutex> lock(mu);
        scenes = {scene("counter", "计数器"), scene("todo", "待办")};
    }
    open_page(2);
    harness->hold(harness->centre_of(page()->card(0)), 700);
    harness->on_ui([&] { ASSERT_NE(page()->popup(), nullptr); });
    harness->bus().publish({"scene.state", "test", 0, {{"id", "todo"}, {"state", "running"}}});
    // The listing ran and its answer, posted before drain() returns, is
    // handled before the next call on the UI thread.
    (void)agent.worker.drain(2000);
    harness->on_ui([&] { EXPECT_NE(page()->popup(), nullptr); });
    {
        std::lock_guard<std::mutex> lock(mu);
        scenes = {scene("todo", "待办")};
    }
    harness->bus().publish({"scene.state", "test", 0, {{"id", "counter"}, {"state", "deleted"}}});
    ASSERT_TRUE(harness->ui_until([&] { return page()->cards() == 1; }));
    harness->on_ui([&] { EXPECT_EQ(page()->popup(), nullptr); });
}

// A page drag that starts on a card neither opens its scene on the release
// nor long-presses it while the finger takes its time.
TEST_F(SceneListTest, DragFromACardOpensNothing) {
    scenes = {scene("a", "A"), scene("b", "B")};
    boot();
    open_page(2);
    const lv_point_t at = harness->centre_of(page()->card(0));
    harness->feed({{static_cast<int16_t>(at.x), static_cast<int16_t>(at.y), 0}});
    // Right is the scene list's way back; slowly, past LVGL's long press.
    for (int i = 1; i <= 12; ++i) {
        wisp::os::sleep_ms(60);
        harness->feed({{static_cast<int16_t>(at.x + 4 * i), static_cast<int16_t>(at.y), 0}});
    }
    harness->on_ui([&] {
        EXPECT_TRUE(harness->shell().sliding());
        EXPECT_EQ(page()->popup(), nullptr);
    });
    harness->feed({});
    ASSERT_TRUE(harness->ui_until([&] { return !harness->shell().sliding(); }));
    std::lock_guard<std::mutex> lock(mu);
    EXPECT_TRUE(opened.empty());
}

// The worker held up behind `gate` until it is let go (or the fixture
// goes), standing in for a slow storage read.
class HeldWorker {
public:
    explicit HeldWorker(wisp::test::AgentHarness& agent) {
        std::shared_future<void> f = gate_.get_future().share();
        held_ = agent.worker.submit([f] { f.wait(); });
    }
    ~HeldWorker() { release(); }
    HeldWorker(const HeldWorker&) = delete;
    HeldWorker& operator=(const HeldWorker&) = delete;
    bool held() const { return held_; }
    void release() {
        if (!released_) gate_.set_value();
        released_ = true;
    }

private:
    std::promise<void> gate_;
    bool held_ = false;
    bool released_ = false;
};

// Opened before storage answers: the page is up at once, loading.
TEST_F(SceneListTest, ScenesPageShowsLoadingAtOnce) {
    scenes = {scene("a", "A"), scene("b", "B")};
    boot();
    HeldWorker held(agent);
    ASSERT_TRUE(held.held());
    harness->show(Page::Scenes);
    harness->on_ui([&] {
        ASSERT_NE(page(), nullptr);
        EXPECT_GT(page()->placeholders(), 0u);
        EXPECT_EQ(page()->cards(), 0u);
        EXPECT_EQ(page()->empty_label(), nullptr);  // not "no scenes" yet
        EXPECT_EQ(page()->card_builds(), 0u);
    });
    held.release();
    ASSERT_TRUE(harness->ui_until([&] { return page()->cards() == 2; }));
    harness->on_ui([&] { EXPECT_EQ(page()->placeholders(), 0u); });
}

// Opened again: the last listing at once, and storage's same answer after
// it changes nothing.
TEST_F(SceneListTest, ScenesPageShowsCachedListAtOnce) {
    scenes = {scene("a", "A"), scene("b", "B")};
    boot();
    open_page(2);
    harness->show(Page::Home);
    HeldWorker held(agent);
    ASSERT_TRUE(held.held());
    harness->show(Page::Scenes);
    harness->on_ui([&] {
        ASSERT_NE(page(), nullptr);
        EXPECT_EQ(page()->cards(), 2u);
        EXPECT_EQ(page()->placeholders(), 0u);
        EXPECT_EQ(page()->card_builds(), 1u);
    });
    held.release();
    (void)agent.worker.drain(5000);
    harness->on_ui([&] {
        EXPECT_EQ(page()->cards(), 2u);
        EXPECT_EQ(page()->card_builds(), 1u);
    });
}

// A listing that differs from the one shown replaces it in place, at the
// same scroll position.
TEST_F(SceneListTest, CardsFillWhenListingArrives) {
    for (int i = 0; i < 30; ++i)
        scenes.push_back(scene("s" + std::to_string(i), "场景" + std::to_string(i)));
    boot();
    open_page(30);
    lv_obj_t* list = nullptr;
    int32_t y = 0;
    harness->on_ui([&] {
        list = lv_obj_get_parent(lv_obj_get_parent(page()->card(0)));
        lv_obj_scroll_to_y(list, 60, LV_ANIM_OFF);
        lv_obj_update_layout(list);
        y = lv_obj_get_scroll_y(list);
    });
    ASSERT_GT(y, 0);
    HeldWorker held(agent);
    ASSERT_TRUE(held.held());
    {
        std::lock_guard<std::mutex> lock(mu);
        scenes.push_back(scene("s30", "场景30"));
    }
    harness->bus().publish({"scene.state", "test", 0, {{"id", "s30"}, {"state", "installed"}}});
    harness->on_ui([&] { EXPECT_EQ(page()->cards(), 30u); });
    held.release();
    ASSERT_TRUE(harness->ui_until([&] { return page()->cards() == 31; }));
    harness->on_ui([&] {
        EXPECT_EQ(page()->card_builds(), 2u);
        EXPECT_EQ(lv_obj_get_scroll_y(list), y);
        // The title is still the list's first item, on its own line above
        // the grid of cards.
        lv_obj_t* title = lv_obj_get_child(list, 0);
        EXPECT_EQ(text_of(title), "场景");
        lv_obj_t* grid = lv_obj_get_parent(page()->card(0));
        EXPECT_EQ(lv_obj_get_child(list, 1), grid);
        EXPECT_GT(wisp::test::coords_of(grid).y1, wisp::test::coords_of(title).y2);
    });
}

// The cards come first, each with its first character; an icon not
// decoded before takes its place in the card already built.
TEST_F(SceneListTest, IconsSwapInAfterCards) {
    {
        ShellScene clock = scene("clock", "时钟");
        clock.icon_png = icon();
        scenes = {std::move(clock), scene("todo", "待办")};
    }
    boot();
    open_page(2);
    ASSERT_TRUE(harness->ui_until([&] { return page()->icon(0) != nullptr; }));
    harness->on_ui([&] {
        EXPECT_EQ(page()->card_builds(), 1u);
        EXPECT_EQ(page()->icon_swaps(), 1u);
        EXPECT_EQ(lv_image_get_src(page()->icon(0)), &page()->scene(0).icon->image);
        // The icon in front of the name, where the character was.
        EXPECT_EQ(lv_obj_get_index(page()->icon(0)), 0);
        EXPECT_EQ(lv_obj_get_child_count(page()->card(0)), lv_obj_get_child_count(page()->card(1)));
        EXPECT_EQ(page()->icon(1), nullptr);
    });
    // Opened again: the icon is in the cached listing, built with its card.
    harness->show(Page::Home);
    harness->show(Page::Scenes);
    harness->on_ui([&] {
        ASSERT_NE(page(), nullptr);
        EXPECT_NE(page()->icon(0), nullptr);
        EXPECT_EQ(page()->icon_swaps(), 0u);
    });
    (void)agent.worker.drain(5000);
    harness->on_ui([&] {
        EXPECT_EQ(page()->card_builds(), 1u);
        EXPECT_EQ(page()->icon_swaps(), 0u);
    });
}

// A long press that goes on into a drag: the card stays up over the page,
// and the page does not slide out from under it.
TEST_F(SceneListTest, NoDragWhileTheLongPressCardIsUp) {
    boot();
    {
        std::lock_guard<std::mutex> lock(mu);
        scenes = {scene("counter", "计数器")};
    }
    open_page(1);
    const lv_point_t at = harness->centre_of(page()->card(0));
    harness->feed({{static_cast<int16_t>(at.x), static_cast<int16_t>(at.y), 0}});
    ASSERT_TRUE(harness->ui_until([&] {
        // Held in place until the card opens.
        lv_indev_read(lv_indev_get_next(nullptr));
        return page()->popup() != nullptr;
    }));
    // Without lifting, right: the scene list's way back to Home.
    for (int16_t dx = 10; dx <= 240; dx += 10) {
        wisp::os::sleep_ms(5);
        harness->feed({{static_cast<int16_t>(at.x + dx), static_cast<int16_t>(at.y), 0}});
        harness->on_ui([&] { EXPECT_FALSE(harness->shell().sliding()) << dx; });
    }
    harness->feed({});
    harness->on_ui([&] {
        EXPECT_FALSE(harness->shell().sliding());
        EXPECT_EQ(harness->shell().page(), Page::Scenes);
        EXPECT_NE(page()->popup(), nullptr);
        page()->close_popup();
    });
}
