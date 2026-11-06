// StatusShell (scene-device spec 5.3, 5.5, 5.6): the status capsule counts
// the background scenes and the unread notifications, the pull-down panel
// lists them and acts on them, and a posted notification shows a banner.

#include "status_shell.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "notification_router.hpp"
#include "support/lvgl_probe.hpp"
#include "support/scene_runtime_fixture.hpp"
#include "support/wait.hpp"
#include "wisp/core/log.hpp"
#include "wisp/core/notification_service.hpp"
#include "wisp/scene/minute_ticker.hpp"
#include "wisp/ui/strings.hpp"
#include "wisp/ui/system_layer.hpp"
#include "wisp/ui/system_notice.hpp"

namespace {

using wisp::Map;
using wisp::Value;
using wisp::app::NotificationRouter;
using wisp::app::NotificationRouterDeps;
using wisp::app::StatusShell;
using wisp::app::StatusShellDeps;
using wisp::core::DismissReason;
using wisp::core::Notification;
using wisp::core::NotificationService;
using wisp::core::SceneState;
using wisp::test::find_button_with_label;
using wisp::test::find_label_containing;
using wisp::test::wait_until;
using wisp::ui::StrId;

// 2026-11-04 12:00:00 UTC.
constexpr int64_t kNoon = 1793793600;

std::string bg_manifest(const std::string& id, const std::string& name) {
    return R"({"id":")" + id + R"(","name":")" + name +
           R"(","version":"1.0.0","schema_version":1,"background":true})";
}

const char* kScene = R"(
scene.on_notification = function(id) print("notified", id) end
return ui.screen { ui.label { text = "scene body" } }
)";

lv_point_t center_of(lv_obj_t* obj) {
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    return {(a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2};
}

// The nearest clickable ancestor of `obj` (or `obj` itself).
lv_obj_t* clickable_of(lv_obj_t* obj) {
    while (obj != nullptr && !lv_obj_has_flag(obj, LV_OBJ_FLAG_CLICKABLE)) {
        obj = lv_obj_get_parent(obj);
    }
    return obj;
}

class StatusShellTest : public wisp::test::SceneRuntimeFixture {
protected:
    void SetUp() override {
        SceneRuntimeFixture::SetUp();
        wisp::ui::set_lang(wisp::ui::Lang::Zh);
        notes = std::make_unique<NotificationService>(wisp::core::NotificationDeps{
            .bus = bus, .storage = *storage, .submit_io = {}, .make_timer = {}});
        configure_deps = [this](wisp::scene::SceneRuntimeDeps& d) {
            d.notifications = notes.get();
        };
    }

    void TearDown() override {
        if (shell) shell->stop();
        shell.reset();
        if (router) router->stop();
        router.reset();
        rt.reset();
        SceneRuntimeFixture::TearDown();
        notes.reset();
        wisp::ui::set_lang(wisp::ui::Lang::Zh);
    }

    // The runtime on a circle, the router and the shell.
    void start_shell() {
        start("circle");
        router = std::make_unique<NotificationRouter>(
            NotificationRouterDeps{.notes = *notes, .scenes = rt.get(), .ui = ui.get()});
        router->start();
        shell = std::make_unique<StatusShell>(StatusShellDeps{
            .ui = *ui,
            .layer = *layer,
            .bus = bus,
            .notes = *notes,
            .router = *router,
            .scenes = rt.get(),
            .device_status =
                [] {
                    return Value(Map{{"battery", Value(Map{{"percent", Value(int64_t{80})},
                                                           {"charging", Value(false)}})},
                                     {"wifi", Value("connected")}});
                },
            .utc_offset_min = [] { return 480; },
            .wall_s = [this] { return wall.load(); },
        });
        shell->start();
    }

    // Scene `id` named `name`, opened and sent to the background.
    void to_background(const std::string& id, const std::string& name) {
        install(id, kScene, {{"scene.json", bg_manifest(id, name)}});
        ASSERT_TRUE(rt->open(id));
        ASSERT_TRUE(rt->close(id));
        ASSERT_TRUE(
            wait_until([&] { return rt->status(id).state == SceneState::Background; }, 2000));
    }

    std::string post(const std::string& source, const std::string& id, const std::string& title) {
        Notification n;
        n.source = source;
        n.id = id;
        n.source_name = "Alpha";
        n.title = title;
        n.body = "body of " + title;
        auto r = notes->post(std::move(n), false);
        EXPECT_TRUE(r) << r.error().message;
        return r ? r.value() : std::string();
    }

    // The capsule's text, "" while it is hidden.
    std::string capsule() {
        std::string text;
        on_ui([&] {
            lv_obj_t* cap = layer->capsule();
            if (cap == nullptr) return;
            lv_obj_t* l = wisp::test::find_obj_of_class(cap, &lv_label_class);
            if (l != nullptr) text = lv_label_get_text(l);
        });
        return text;
    }

    bool wait_capsule(const std::string& want) {
        const bool ok = wait_until([&] { return capsule() == want; }, 2000);
        EXPECT_EQ(capsule(), want);
        return ok;
    }

    bool panel_has(const char* needle) {
        bool found = false;
        on_ui([&] {
            found = layer->panel_open() && find_label_containing(layer->panel(), needle) != nullptr;
        });
        return found;
    }

    void open_panel() {
        on_ui([&] { layer->open_panel(); });
        on_ui([&] { ASSERT_TRUE(layer->panel_open()); });
    }

    void tap(lv_point_t p) {
        const auto x = static_cast<int16_t>(p.x);
        const auto y = static_cast<int16_t>(p.y);
        feed({{x, y, 0}});
        feed({{x, y, 0}});
        feed({});
    }

    // The centre of what `find` picks out of the open panel, scrolled into
    // view.
    lv_point_t locate(const std::function<lv_obj_t*(lv_obj_t*)>& find) {
        lv_point_t p{-1, -1};
        on_ui([&] {
            ASSERT_TRUE(layer->panel_open());
            lv_obj_t* o = find(layer->panel());
            ASSERT_NE(o, nullptr);
            lv_obj_scroll_to_view_recursive(o, LV_ANIM_OFF);
            lv_obj_update_layout(lv_layer_top());
            p = center_of(o);
        });
        return p;
    }

    static std::function<lv_obj_t*(lv_obj_t*)> row_of(std::string text) {
        return [text](lv_obj_t* panel) {
            return clickable_of(find_label_containing(panel, text.c_str()));
        };
    }

    static std::function<lv_obj_t*(lv_obj_t*)> close_in(std::string text) {
        return [text](lv_obj_t* panel) {
            lv_obj_t* row = clickable_of(find_label_containing(panel, text.c_str()));
            return row != nullptr ? find_button_with_label(row, LV_SYMBOL_CLOSE) : nullptr;
        };
    }

    bool scene_printed(const std::string& id, const std::string& line) {
        return wait_until(
            [&] {
                for (const auto& l : scene_log(id))
                    if (l == line) return true;
                return false;
            },
            2000);
    }

    std::unique_ptr<NotificationService> notes;
    std::unique_ptr<NotificationRouter> router;
    std::unique_ptr<StatusShell> shell;
    std::atomic<int64_t> wall{0};
};

TEST_F(StatusShellTest, CapsuleCountsFollowScenesAndNotifications) {
    start_shell();
    on_ui([&] { EXPECT_EQ(layer->capsule(), nullptr); });
    to_background("a", "Alpha");
    wait_capsule(LV_SYMBOL_PLAY "1");
    EXPECT_TRUE(
        wait_until([&] { return log_has("status", "status: background=1 unread=0"); }, 2000));

    post("a", "n1", "First");
    post("a", "n2", "Second");
    wait_capsule(LV_SYMBOL_PLAY "1 " LV_SYMBOL_BELL "2");
    ASSERT_TRUE(notes->dismiss("a", "n1", DismissReason::Dismiss));
    wait_capsule(LV_SYMBOL_PLAY "1 " LV_SYMBOL_BELL "1");
    EXPECT_TRUE(
        wait_until([&] { return log_has("status", "status: background=1 unread=1"); }, 2000));

    ASSERT_TRUE(rt->exit("a"));
    wait_capsule(LV_SYMBOL_BELL "1");
    notes->clear_all();
    EXPECT_TRUE(wait_until([&] { return capsule().empty(); }, 2000));
}

// Notifications that are already unread when it starts are counted at once.
TEST_F(StatusShellTest, CountsWhatIsThereAtStart) {
    start("circle");
    post("a", "n1", "First");
    router = std::make_unique<NotificationRouter>(
        NotificationRouterDeps{.notes = *notes, .scenes = rt.get(), .ui = ui.get()});
    router->start();
    shell = std::make_unique<StatusShell>(StatusShellDeps{
        .ui = *ui, .layer = *layer, .bus = bus, .notes = *notes, .router = *router});
    shell->start();
    wait_capsule(LV_SYMBOL_BELL "1");
}

TEST_F(StatusShellTest, PanelListsBackgroundScenesAndNotes) {
    start_shell();
    to_background("a", "Alpha");
    post("a", "n1", "First");
    post("a", "n2", "Second");
    wait_capsule(LV_SYMBOL_PLAY "1 " LV_SYMBOL_BELL "2");

    // The clock is unset.
    on_ui([&] {
        const wisp::ui::PanelModel m = shell->panel_model();
        EXPECT_EQ(m.header.time, "--:--");
        EXPECT_EQ(m.header.battery, 80);
        EXPECT_FALSE(m.header.charging);
        EXPECT_EQ(m.header.wifi, wisp::ui::tr(StrId::NetConnected));
        ASSERT_EQ(m.scenes.size(), 1u);
        EXPECT_EQ(m.scenes[0].id, "a");
        EXPECT_EQ(m.scenes[0].name, "Alpha");
        ASSERT_EQ(m.notes.size(), 2u);
        EXPECT_EQ(m.notes[0].title, "Second");
        EXPECT_EQ(m.notes[0].badge, "A");
        EXPECT_EQ(m.notes[0].body, "body of Second");
        EXPECT_EQ(m.notes[1].title, "First");
    });

    // 12:00 UTC is 20:00 at UTC+8.
    wall = kNoon + 59;
    open_panel();
    EXPECT_TRUE(log_has("status", "panel: open"));
    EXPECT_TRUE(panel_has("20:00"));
    EXPECT_TRUE(panel_has("Alpha"));
    lv_area_t first{}, second{};
    on_ui([&] {
        lv_obj_t* f = find_label_containing(layer->panel(), "First");
        lv_obj_t* s = find_label_containing(layer->panel(), "Second");
        ASSERT_NE(f, nullptr);
        ASSERT_NE(s, nullptr);
        lv_obj_get_coords(f, &first);
        lv_obj_get_coords(s, &second);
    });
    EXPECT_LT(second.y1, first.y1);  // newest on top

    on_ui([&] { layer->close_panel(); });
    EXPECT_TRUE(log_has("status", "panel: closed"));
}

// The flow of spec 5.5/5.6 end to end: a scene goes to the background, the
// capsule shows, a tap on it opens the panel, and a tap on the scene's row
// brings it to the foreground.
TEST_F(StatusShellTest, CapsuleTapThenSceneRowBringsSceneForward) {
    start_shell();
    to_background("a", "Alpha");
    wait_capsule(LV_SYMBOL_PLAY "1");
    lv_point_t at{};
    on_ui([&] {
        lv_obj_update_layout(lv_layer_top());
        at = center_of(layer->capsule());
    });
    tap(at);
    on_ui([&] { EXPECT_TRUE(layer->panel_open()); });
    tap(locate(row_of("Alpha")));
    EXPECT_TRUE(
        wait_until([&] { return rt->foreground() == std::optional<std::string>("a"); }, 2000));
    EXPECT_TRUE(wait_until([&] { return rt->status("a").state == SceneState::Running; }, 2000));
    EXPECT_TRUE(wait_label("scene body"));
    on_ui([&] { EXPECT_FALSE(layer->panel_open()); });
    EXPECT_TRUE(wait_until([&] { return capsule().empty(); }, 2000));
}

TEST_F(StatusShellTest, PanelActions) {
    start_shell();
    to_background("a", "Alpha");
    to_background("b", "Beta");
    wait_capsule(LV_SYMBOL_PLAY "2");

    // The close button exits the scene; the panel stays open.
    open_panel();
    tap(locate(close_in("Beta")));
    EXPECT_TRUE(wait_until([&] { return rt->status("b").state == SceneState::Stopped; }, 2000));
    on_ui([&] { EXPECT_TRUE(layer->panel_open()); });
    EXPECT_TRUE(wait_until([&] { return !panel_has("Beta"); }, 2000));
    on_ui([&] { layer->close_panel(); });

    // A tap on a notification opens its scene, which hears about it.
    post("a", "n1", "First");
    wait_capsule(LV_SYMBOL_PLAY "1 " LV_SYMBOL_BELL "1");
    open_panel();
    tap(locate(row_of("First")));
    EXPECT_TRUE(scene_printed("a", "notified\tn1"));
    EXPECT_FALSE(notes->find("a", "n1").has_value());
    EXPECT_TRUE(
        wait_until([&] { return rt->foreground() == std::optional<std::string>("a"); }, 2000));
    ASSERT_TRUE(rt->close("a"));

    // The close button dismisses one; "clear all" the rest.
    post("a", "n2", "Second");
    post("a", "n3", "Third");
    post("a", "n4", "Fourth");
    wait_capsule(LV_SYMBOL_PLAY "1 " LV_SYMBOL_BELL "3");
    open_panel();
    tap(locate(close_in("Second")));
    EXPECT_FALSE(notes->find("a", "n2").has_value());
    EXPECT_TRUE(wait_until([&] { return !panel_has("Second"); }, 2000));
    tap(locate([](lv_obj_t* panel) {
        return find_button_with_label(panel, wisp::ui::tr(StrId::PanelClearAll));
    }));
    EXPECT_EQ(notes->unread_count(), 0u);
    wait_capsule(LV_SYMBOL_PLAY "1");
}

TEST_F(StatusShellTest, PanelUpdatesWhileOpen) {
    start_shell();
    post("a", "n1", "First");
    wait_capsule(LV_SYMBOL_BELL "1");
    open_panel();
    EXPECT_TRUE(panel_has("First"));
    EXPECT_FALSE(panel_has("Second"));
    post("a", "n2", "Second");
    EXPECT_TRUE(wait_until([&] { return panel_has("Second"); }, 2000));
    EXPECT_TRUE(panel_has("First"));
    // The screen going off closes it.
    bus.publish({"power.state", "power", 0, {{"state", Value("standby")}}});
    EXPECT_TRUE(wait_until(
        [&] {
            bool open = true;
            on_ui([&] { open = layer->panel_open(); });
            return !open;
        },
        2000));
}

TEST_F(StatusShellTest, SystemNoticeRenderedInUiLanguage) {
    start_shell();
    to_background("a", "Alpha");
    notes->post_system(wisp::core::notice::kSceneEvictedMemory, "天气");
    notes->post_system(wisp::core::notice::kWakeLockLong, "scene.a",
                       wisp::core::notice::kActionReleaseLock, "7");
    notes->post_system(wisp::core::notice::kWakeLockLong, "system.ota");
    wait_capsule(LV_SYMBOL_PLAY "1 " LV_SYMBOL_BELL "3");
    open_panel();
    EXPECT_TRUE(panel_has("天气已因内存不足关闭"));
    EXPECT_TRUE(panel_has("Alpha已让设备保持唤醒 30 分钟"));
    const std::string ota = wisp::ui::holder_label("system.ota", "") + "已让设备保持唤醒";
    EXPECT_TRUE(panel_has(ota.c_str()));
    on_ui([&] {
        EXPECT_NE(find_button_with_label(layer->panel(), wisp::ui::tr(StrId::NoticeRelease)),
                  nullptr);
        const wisp::ui::PanelModel m = shell->panel_model();
        ASSERT_EQ(m.notes.size(), 3u);
        EXPECT_EQ(m.notes[2].title, wisp::ui::tr(StrId::NoticeSystemTitle));
        EXPECT_EQ(m.notes[2].badge, "");
        EXPECT_EQ(m.notes[1].action_label, wisp::ui::tr(StrId::NoticeRelease));
        EXPECT_EQ(m.notes[0].action_label, "");
    });
    // A scene holder without a running scene reads as its id.
    on_ui([&] { layer->close_panel(); });
    ASSERT_TRUE(rt->exit("a"));
    ASSERT_TRUE(wait_until([&] { return rt->background_scenes().empty(); }, 2000));
    on_ui([&] {
        const wisp::ui::PanelModel m = shell->panel_model();
        ASSERT_EQ(m.notes.size(), 3u);
        EXPECT_EQ(m.notes[1].body.rfind("a已让设备", 0), 0u) << m.notes[1].body;
    });
}

TEST_F(StatusShellTest, BannerOnPostAndTapOpensScene) {
    start_shell();
    install("a", kScene, {{"scene.json", wisp::test::manifest("a", "Alpha")}});
    post("a", "n1", "Ding");
    ASSERT_TRUE(wait_until(
        [&] {
            bool shown = false;
            on_ui([&] {
                shown = layer->banner() != nullptr &&
                        find_label_containing(layer->banner(), "Ding") != nullptr;
            });
            return shown;
        },
        2000));
    lv_point_t at{};
    on_ui([&] {
        EXPECT_NE(find_label_containing(layer->banner(), "Alpha"), nullptr);
        lv_obj_update_layout(lv_layer_top());
        at = center_of(layer->banner());
    });
    tap(at);
    EXPECT_TRUE(scene_printed("a", "notified\tn1"));
    EXPECT_FALSE(notes->find("a", "n1").has_value());
    EXPECT_TRUE(
        wait_until([&] { return rt->foreground() == std::optional<std::string>("a"); }, 2000));

    // A system notice's banner reads in the UI language.
    notes->post_system(wisp::core::notice::kSceneError, "天气");
    EXPECT_TRUE(wait_until(
        [&] {
            bool shown = false;
            on_ui([&] {
                shown = layer->banner() != nullptr &&
                        find_label_containing(layer->banner(), "天气出错已停止") != nullptr;
            });
            return shown;
        },
        6000));
}

// stop() takes the handlers and the banner away before the layer goes.
TEST_F(StatusShellTest, StopUninstalls) {
    start_shell();
    post("a", "n1", "First");
    wait_capsule(LV_SYMBOL_BELL "1");
    shell->stop();
    on_ui([&] {
        layer->open_panel();
        EXPECT_FALSE(layer->panel_open());
    });
    post("a", "n2", "Second");
    wisp::os::sleep_ms(50);
    on_ui([&] {
        EXPECT_TRUE(layer->banner() == nullptr ||
                    find_label_containing(layer->banner(), "Second") == nullptr);
    });
    shell->stop();
}

}  // namespace
