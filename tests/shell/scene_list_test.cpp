// The scene list page (wisp/shell/scene_list.hpp, mvp spec 6.4) in the
// shell: listed on the storage worker, sorted by SceneRecents, greyed and
// marked, opened with a tap and deleted from the long-press card.

#include "wisp/shell/scene_list.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstring>
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
            return scenes;
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
    std::atomic<bool> on_worker_listing{false};
};

TEST_F(SceneListTest, SortedMarkedAndGreyed) {
    boot();
    {
        ShellScene clock = scene("clock", "时钟");
        clock.icon_png = icon();
        ShellScene counter = scene("counter", "计数器");
        counter.background = true;
        counter.has_data = true;
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
        ShellScene counter = scene("counter", "计数器");
        counter.has_data = true;
        std::lock_guard<std::mutex> lock(mu);
        scenes = {std::move(counter), scene("todo", "待办")};
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
    harness->hold(harness->centre_of(page()->card(1)), 700);
    harness->on_ui([&] {
        ASSERT_NE(page()->popup(), nullptr);
        EXPECT_EQ(page()->popup_data(), nullptr);
    });
    harness->tap({5, 240});
    harness->on_ui([&] { EXPECT_EQ(page()->popup(), nullptr); });
    std::lock_guard<std::mutex> lock(mu);
    EXPECT_EQ(deleted.size(), 1u);
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
