// The sub-pages on every screen shape (AGENTS.md, screen shape; spec 5.6
// of the base spec): the title is the list's first item and scrolls with
// it; scrolled to the top, the title (below the status capsule) and the
// first row lie wholly on the visible screen, scrolled to the bottom the
// last row does, and every row or card is a thumb's height at least.

#include <gtest/gtest.h>

#include <algorithm>
#include <future>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "shell/page_fixture.hpp"
#include "support/agent_fixture.hpp"
#include "wisp/agent/agent_service.hpp"
#include "wisp/shell/history.hpp"
#include "wisp/shell/scene_list.hpp"
#include "wisp/shell/settings_page.hpp"
#include "wisp/ui/system_layer.hpp"

using wisp::shell::Page;
using wisp::test::box_visible;
using wisp::test::coords_of;
using wisp::test::is_hidden;

namespace {

struct ShapeCase {
    const char* shape;
    int32_t width;
    int32_t height;
};

std::string str(const lv_area_t& a) {
    return std::to_string(a.x1) + "," + std::to_string(a.y1) + " " + std::to_string(a.x2) + "," +
           std::to_string(a.y2);
}

class SubPageShapes : public ::testing::TestWithParam<ShapeCase> {
protected:
    void SetUp() override {
        agent_h = std::make_unique<wisp::test::AgentHarness>();
        ASSERT_TRUE(agent_h->worker.run_sync(
            [&] {
                for (uint32_t t = 1; t <= 12; ++t) {
                    wisp::agent::Message u;
                    u.turn = t;
                    u.role = wisp::agent::Role::User;
                    u.text.assign("问一个问题");
                    u.source = wisp::agent::Source::Voice;
                    ASSERT_TRUE(agent_h->store->append(u));
                    wisp::agent::Message a;
                    a.turn = t;
                    a.role = wisp::agent::Role::Assistant;
                    a.text.assign("<say>这是一个回答，长到要折一行才放得下。</say>细节");
                    ASSERT_TRUE(agent_h->store->append(a));
                }
            },
            wisp::os::kForever));
        agent = std::make_unique<wisp::agent::AgentService>(agent_h->deps());
        ASSERT_TRUE(agent->start());

        const ShapeCase& c = GetParam();
        h = std::make_unique<wisp::test::PageHarness>(c.shape, c.width, c.height);
        ASSERT_TRUE(h->started());
        auto& d = h->deps();
        d.agent = agent.get();
        d.submit_io = [this](std::function<void()> job) {
            return agent_h->worker.submit(std::move(job));
        };
        d.list_scenes = [] {
            std::vector<wisp::shell::ShellScene> out;
            for (int i = 1; i <= 9; ++i) {
                wisp::shell::ShellScene s;
                s.id = "scene" + std::to_string(i);
                s.name = "场景 " + std::to_string(i);
                out.push_back(std::move(s));
            }
            return wisp::shell::ShellListing{std::move(out), false};
        };
        // The long-press card at its tallest, with "delete its data too".
        d.scene_has_data = [](const std::string&) { return true; };
        d.settings = [] {
            wisp::core::SystemSettingsValues v{};
            v.brightness = 200;
            v.volume = 60;
            v.screen_off_s = 60;
            return v;
        };
        d.set_settings = [](std::map<std::string, wisp::Value>) {};
        d.delete_scene = [](const std::string&, bool) { return wisp::Result<void>::ok(); };
        d.wifi_status = [] { return std::string("已连接 WispLab  10.0.0.7"); };
        d.admin_url = [] { return std::string("http://10.0.0.7:8900"); };
        d.firmware_version = [] { return std::string("0.4.2"); };
        h->boot();
    }

    void TearDown() override {
        if (agent_h) (void)agent_h->worker.drain(5000);
        h.reset();
        if (agent) agent->stop();
        agent.reset();
        agent_h.reset();
    }

    // The visible rows of `list` under its title, top to bottom.
    static std::vector<lv_obj_t*> rows(lv_obj_t* list) {
        std::vector<lv_obj_t*> out;
        for (uint32_t i = 1; i < lv_obj_get_child_count(list); ++i) {
            lv_obj_t* c = lv_obj_get_child(list, static_cast<int32_t>(i));
            if (!is_hidden(c)) out.push_back(c);
        }
        return out;
    }

    // The page's title: the list's first item.
    static lv_obj_t* title_of(lv_obj_t* list) { return lv_obj_get_child(list, 0); }

    // Scrolled to the top the title and `first` are visible, the title
    // below the status capsule; to the bottom `last` is. In between the
    // title moves with the rows. Left scrolled to the bottom.
    void check_ends(lv_obj_t* list, lv_obj_t* first, lv_obj_t* last) {
        const wisp::hal::ScreenShape& s = h->shape();
        lv_obj_t* title = title_of(list);
        lv_obj_scroll_to_y(list, 0, LV_ANIM_OFF);
        lv_obj_update_layout(list);
        const lv_area_t t = coords_of(title);
        EXPECT_TRUE(box_visible(s, t)) << "title " << str(t);
        EXPECT_GT(t.y1, wisp::ui::SystemLayer::status_area(s).y2) << "title " << str(t);
        EXPECT_GT(coords_of(first).y1, t.y2);
        EXPECT_TRUE(box_visible(s, coords_of(first))) << "first " << str(coords_of(first));
        // The list starts at the screen's top: the title's rows are the
        // list's, not a fixed band above it.
        EXPECT_EQ(coords_of(list).y1, 0);
        lv_obj_scroll_to_y(list, LV_COORD_MAX, LV_ANIM_OFF);
        lv_obj_update_layout(list);
        EXPECT_LE(lv_obj_get_scroll_bottom(list), 0);
        EXPECT_TRUE(box_visible(s, coords_of(last))) << "last " << str(coords_of(last));
        const int32_t n = std::min<int32_t>(40, lv_obj_get_scroll_y(list));
        lv_obj_scroll_to_y(list, n, LV_ANIM_OFF);
        lv_obj_update_layout(list);
        EXPECT_EQ(coords_of(title).y1, t.y1 - n);
        lv_obj_scroll_to_y(list, LV_COORD_MAX, LV_ANIM_OFF);
        lv_obj_update_layout(list);
    }

    std::unique_ptr<wisp::test::AgentHarness> agent_h;
    std::unique_ptr<wisp::agent::AgentService> agent;
    std::unique_ptr<wisp::test::PageHarness> h;
};

}  // namespace

TEST_P(SubPageShapes, EverythingVisibleAtScrollEnds) {
    {
        SCOPED_TRACE("history");
        h->show(Page::History);
        h->on_ui([&] {
            wisp::shell::HistoryPage* p = h->shell().history();
            ASSERT_NE(p, nullptr);
            ASSERT_EQ(p->built(), 12u);
            const auto r = rows(p->list());
            for (lv_obj_t* row : r) EXPECT_GE(lv_obj_get_height(row), 48);
            check_ends(p->list(), r.front(), r.back());
        });
    }
    {
        SCOPED_TRACE("scene list");
        h->show(Page::Scenes);
        ASSERT_TRUE(h->ui_until([&] { return h->shell().scene_list()->cards() == 9; }));
        h->on_ui([&] {
            wisp::shell::SceneList* p = h->shell().scene_list();
            lv_obj_update_layout(lv_screen_active());
            for (size_t i = 0; i < 9; ++i) {
                const lv_area_t a = coords_of(p->card(i));
                EXPECT_GE(lv_area_get_width(&a), 48);
                EXPECT_GE(lv_area_get_height(&a), 48);
            }
            lv_obj_t* list = lv_obj_get_parent(lv_obj_get_parent(p->card(0)));
            check_ends(list, p->card(0), p->card(8));
            // A card is wholly on the screen at the end it is at.
            lv_obj_scroll_to_y(list, LV_COORD_MAX, LV_ANIM_OFF);
            lv_obj_update_layout(list);
            EXPECT_TRUE(box_visible(h->shape(), coords_of(p->card(6))));
        });
        // The long-press card fits too.
        h->hold(h->centre_of(h->shell().scene_list()->card(8)), 700);
        ASSERT_TRUE(h->ui_until([&] { return h->shell().scene_list()->popup_data() != nullptr; }));
        h->on_ui([&] {
            wisp::shell::SceneList* p = h->shell().scene_list();
            ASSERT_NE(p->popup(), nullptr);
            lv_obj_update_layout(p->popup());
            lv_obj_t* card = lv_obj_get_child(p->popup(), 0);
            EXPECT_TRUE(box_visible(h->shape(), coords_of(card))) << str(coords_of(card));
            p->close_popup();
        });
    }
    {
        SCOPED_TRACE("settings");
        h->show(Page::Settings);
        h->on_ui([&] {
            wisp::shell::SettingsPage* p = h->shell().settings_page();
            ASSERT_NE(p, nullptr);
            const auto r = rows(p->list());
            ASSERT_GE(r.size(), 5u);
            for (lv_obj_t* row : r) EXPECT_GE(lv_obj_get_height(row), 48);
            check_ends(p->list(), r.front(), r.back());
            EXPECT_TRUE(box_visible(h->shape(), coords_of(p->diagnostics())));
        });
    }
}

// Opened before storage answers, the scene list's loading placeholders
// all lie on the visible screen, under the title.
TEST_P(SubPageShapes, LoadingStateInsideVisibleArea) {
    std::promise<void> gate;
    std::shared_future<void> f = gate.get_future().share();
    ASSERT_TRUE(agent_h->worker.submit([f] { f.wait(); }));
    h->show(Page::Scenes);
    h->on_ui([&] {
        wisp::shell::SceneList* p = h->shell().scene_list();
        ASSERT_NE(p, nullptr);
        lv_obj_update_layout(lv_screen_active());
        ASSERT_GT(p->placeholders(), 0u);
        lv_obj_t* list = lv_obj_get_parent(lv_obj_get_parent(p->placeholder(0)));
        lv_obj_t* title = title_of(list);
        const lv_area_t t = coords_of(title);
        EXPECT_TRUE(box_visible(h->shape(), t)) << "title " << str(t);
        for (size_t i = 0; i < p->placeholders(); ++i) {
            const lv_area_t a = coords_of(p->placeholder(i));
            EXPECT_TRUE(box_visible(h->shape(), a)) << i << " " << str(a);
            EXPECT_GT(a.y1, t.y2) << i;
        }
    });
    gate.set_value();
    ASSERT_TRUE(h->ui_until([&] { return h->shell().scene_list()->cards() == 9; }));
}

INSTANTIATE_TEST_SUITE_P(
    Shapes, SubPageShapes,
    ::testing::Values(ShapeCase{"rect", 480, 480}, ShapeCase{"rounded_rect", 480, 480},
                      ShapeCase{"circle", 480, 480}, ShapeCase{"circle", 466, 466},
                      ShapeCase{"circle", 360, 360}, ShapeCase{"circle", 360, 480}),
    [](const ::testing::TestParamInfo<ShapeCase>& info) {
        return std::string(info.param.shape) + "_" + std::to_string(info.param.width) + "x" +
               std::to_string(info.param.height);
    });
