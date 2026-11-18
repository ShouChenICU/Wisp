// StatusShell (scene-device spec 5.3, 5.6): the pull-down panel lists
// background scenes and unread notifications and acts on them. Posted
// notifications also show banners.

#include "status_shell.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <string>
#include <vector>

#include "notification_router.hpp"
#include "support/bus_flood.hpp"
#include "support/lvgl_probe.hpp"
#include "support/scene_runtime_fixture.hpp"
#include "support/wait.hpp"
#include "wisp/core/log.hpp"
#include "wisp/core/notification_service.hpp"
#include "wisp/core/state_cell.hpp"
#include "wisp/hal/device.hpp"
#include "wisp/net/wifi_snapshot.hpp"
#include "wisp/scene/minute_ticker.hpp"
#include "wisp/ui/clock_text.hpp"
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

// A background scene with one tool, so a tool call starts it headless.
const char* kHeadlessTool = R"(
wisp.tools.register("get", function() return 1 end)
return ui.screen { ui.label { text = "tool scene" } }
)";

std::string tool_manifest(const std::string& id, const std::string& name) {
    return R"({"id":")" + id + R"(","name":")" + name +
           R"(","version":"1.0.0","schema_version":1,"background":true,)" +
           R"("tools":[{"name":"get","description":"Get one"}]})";
}

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
            .notes = *notes,
            .router = *router,
            .scenes = rt.get(),
            .device_status =
                [this] {
                    return Value(Map{
                        {"battery",
                         Value(Map{{"percent", Value(int64_t{80})}, {"charging", Value(false)}})},
                        {"wifi",
                         Value(wifi_off.load() || wifi_disconnected.load() ? "off" : "connected")},
                        {"wifi_enabled", Value(!wifi_off.load())}});
                },
            .utc_offset_min = [] { return 480; },
            .time_24h = [this] { return time_24h.load(); },
            .wall_s = [this] { return wall.load(); },
            .repo = repo.get(),
            .submit_io =
                [this](std::function<void()> job) { return worker.submit(std::move(job)); },
            .power = &power,
            .wifi = &wifi,
        });
        shell->start();
    }

    // Whether the panel is open, read on the UI thread.
    bool panel_is_open() {
        bool open = false;
        on_ui([&] { open = layer->panel_open(); });
        return open;
    }

    // The body of the panel's note from `source`/`id`, "" if none.
    std::string note_body(const std::string& source, const std::string& id) {
        std::string body;
        on_ui([&] {
            for (const auto& n : shell->panel_model().notes)
                if (n.source == std::string_view(source) && n.id == std::string_view(id))
                    body = std::string(n.body);
        });
        return body;
    }

    bool wait_body_starts(const std::string& id, const std::string& prefix) {
        const bool ok =
            wait_until([&] { return note_body("system", id).rfind(prefix, 0) == 0; }, 2000);
        EXPECT_TRUE(ok) << note_body("system", id);
        return ok;
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

    bool wait_counts(size_t background, size_t unread) {
        auto matches = [&] {
            bool ok = false;
            on_ui([&] {
                const auto m = shell->panel_model();
                ok = m.scenes.size() == background && m.notes.size() == unread;
            });
            return ok;
        };
        const bool ok = wait_until(matches, 2000);
        EXPECT_TRUE(ok) << "background=" << background << " unread=" << unread;
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

    // The owners of the cells the shell follows, declared before it so its
    // subscriptions are released first.
    wisp::core::StateCell<wisp::hal::PowerLevel> power{bus, wisp::hal::PowerLevel::Active};
    wisp::core::StateCell<wisp::net::WifiSnapshot> wifi{bus,
                                                        wisp::net::WifiSnapshot{.enabled = true}};
    std::unique_ptr<NotificationService> notes;
    std::unique_ptr<NotificationRouter> router;
    std::unique_ptr<StatusShell> shell;
    std::atomic<int64_t> wall{0};
    std::atomic<bool> time_24h{true};
    std::atomic<bool> wifi_off{false};
    std::atomic<bool> wifi_disconnected{false};
};

TEST_F(StatusShellTest, PanelCountsFollowScenesAndNotifications) {
    start_shell();
    to_background("a", "Alpha");
    wait_counts(1, 0);
    on_ui([&] { EXPECT_TRUE(layer->visible_pieces().empty()); });
    EXPECT_TRUE(
        wait_until([&] { return log_has("status", "status: background=1 unread=0"); }, 2000));

    post("a", "n1", "First");
    post("a", "n2", "Second");
    wait_counts(1, 2);
    ASSERT_TRUE(notes->dismiss("a", "n1", DismissReason::Dismiss));
    wait_counts(1, 1);
    EXPECT_TRUE(
        wait_until([&] { return log_has("status", "status: background=1 unread=1"); }, 2000));

    ASSERT_TRUE(rt->exit("a"));
    wait_counts(0, 1);
    notes->clear_all();
    wait_counts(0, 0);
}

// Notifications that are already unread when it starts are counted at once.
TEST_F(StatusShellTest, CountsWhatIsThereAtStart) {
    start("circle");
    post("a", "n1", "First");
    router = std::make_unique<NotificationRouter>(
        NotificationRouterDeps{.notes = *notes, .scenes = rt.get(), .ui = ui.get()});
    router->start();
    shell = std::make_unique<StatusShell>(
        StatusShellDeps{.ui = *ui, .layer = *layer, .notes = *notes, .router = *router});
    shell->start();
    wait_counts(0, 1);
}

// The header's time follows the Settings time format (ui::format_clock()).
TEST_F(StatusShellTest, PanelHeaderFollowsTheTimeFormat) {
    start_shell();
    // 12:00 UTC is 20:00 at UTC+8.
    wall = kNoon + 59;
    on_ui([&] { EXPECT_EQ(shell->panel_model().header.time, "20:00"); });
    time_24h = false;
    on_ui([&] {
        EXPECT_EQ(std::string_view(shell->panel_model().header.time),
                  wisp::ui::format_clock(20, 0, false, wisp::ui::lang()));
    });
}

TEST_F(StatusShellTest, PanelListsBackgroundScenesAndNotes) {
    start_shell();
    to_background("a", "Alpha");
    post("a", "n1", "First");
    post("a", "n2", "Second");
    wait_counts(1, 2);

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

// Background scenes remain accessible from the pull-down panel.
TEST_F(StatusShellTest, TopSwipeThenSceneRowBringsSceneForward) {
    start_shell();
    to_background("a", "Alpha");
    wait_counts(1, 0);
    feed({{240, 4, 0}});
    feed({{240, 4, 0}});
    feed({{240, 400, 0}});
    feed({});
    wisp::os::sleep_ms(300);
    on_ui([&] { EXPECT_TRUE(layer->panel_open()); });
    tap(locate(row_of("Alpha")));
    EXPECT_TRUE(
        wait_until([&] { return rt->foreground() == std::optional<std::string>("a"); }, 2000));
    EXPECT_TRUE(wait_until([&] { return rt->status("a").state == SceneState::Running; }, 2000));
    EXPECT_TRUE(wait_label("scene body"));
    on_ui([&] { EXPECT_FALSE(layer->panel_open()); });
    wait_counts(0, 0);
}

TEST_F(StatusShellTest, PanelActions) {
    start_shell();
    to_background("a", "Alpha");
    to_background("b", "Beta");
    wait_counts(2, 0);

    // The close button exits the scene; the panel stays open.
    open_panel();
    tap(locate(close_in("Beta")));
    EXPECT_TRUE(wait_until([&] { return rt->status("b").state == SceneState::Stopped; }, 2000));
    on_ui([&] { EXPECT_TRUE(layer->panel_open()); });
    EXPECT_TRUE(wait_until([&] { return !panel_has("Beta"); }, 2000));
    on_ui([&] { layer->close_panel(); });

    // A tap on a notification opens its scene, which hears about it.
    post("a", "n1", "First");
    wait_counts(1, 1);
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
    wait_counts(1, 3);
    open_panel();
    tap(locate(close_in("Second")));
    EXPECT_FALSE(notes->find("a", "n2").has_value());
    EXPECT_TRUE(wait_until([&] { return !panel_has("Second"); }, 2000));
    tap(locate([](lv_obj_t* panel) {
        return find_button_with_label(panel, wisp::ui::tr(StrId::PanelClearAll));
    }));
    EXPECT_EQ(notes->unread_count(), 0u);
    wait_counts(1, 0);
}

TEST_F(StatusShellTest, PanelUpdatesWhileOpen) {
    start_shell();
    post("a", "n1", "First");
    wait_counts(0, 1);
    open_panel();
    EXPECT_TRUE(panel_has("First"));
    EXPECT_FALSE(panel_has("Second"));
    post("a", "n2", "Second");
    EXPECT_TRUE(wait_until([&] { return panel_has("Second"); }, 2000));
    EXPECT_TRUE(panel_has("First"));
    // The screen going off closes it.
    power.set(wisp::hal::PowerLevel::Standby);
    EXPECT_TRUE(wait_until([&] { return !panel_is_open(); }, 2000));
}

TEST_F(StatusShellTest, SystemNoticeRenderedInUiLanguage) {
    start_shell();
    to_background("a", "Alpha");
    notes->post_system(wisp::core::notice::kSceneEvictedMemory, "天气");
    notes->post_system(wisp::core::notice::kWakeLockLong, "scene.a",
                       wisp::core::notice::kActionReleaseLock, "7");
    notes->post_system(wisp::core::notice::kWakeLockLong, "system.ota");
    wait_counts(1, 3);
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
        EXPECT_EQ(m.notes[1].body.rfind("Alpha已让设备", 0), 0u) << m.notes[1].body;
    });
}

// A wake-lock holder reads as its scene's name wherever the scene is: in
// front, in the background, or installed and not running (read from its
// manifest off the UI thread); only a scene that is not installed reads
// as its id.
TEST_F(StatusShellTest, WakeLockHolderNamedWhereverTheSceneIs) {
    start_shell();
    install("f", kScene, {{"scene.json", wisp::test::manifest("f", "Front")}});
    install("g", kScene, {{"scene.json", wisp::test::manifest("g", "Gamma")}});
    ASSERT_TRUE(rt->open("f"));
    ASSERT_TRUE(
        wait_until([&] { return rt->foreground() == std::optional<std::string>("f"); }, 2000));
    notes->post_system(wisp::core::notice::kWakeLockLong, "scene.f");
    notes->post_system(wisp::core::notice::kWakeLockLong, "scene.g");
    notes->post_system(wisp::core::notice::kWakeLockLong, "scene.zz");
    wait_body_starts("wakelock_long:scene.f", "Front已让设备");
    wait_body_starts("wakelock_long:scene.g", "Gamma已让设备");
    wait_body_starts("wakelock_long:scene.zz", "zz已让设备");
    // Once the scene has stopped its name still shows.
    ASSERT_TRUE(rt->exit("f"));
    ASSERT_TRUE(wait_until([&] { return rt->status("f").state == SceneState::Stopped; }, 2000));
    wait_body_starts("wakelock_long:scene.f", "Front已让设备");
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

// The text of the banner shown now, "" without one.
std::string banner_text(lv_obj_t* banner) {
    if (banner == nullptr) return {};
    std::vector<std::pair<std::string, lv_area_t>> labels;
    wisp::test::collect_labels(banner, labels);
    std::string text;
    for (const auto& [t, a] : labels) text += t + "|";
    return text;
}

class StatusShellBannerTest : public StatusShellTest {
protected:
    // The banner shown now, read on the UI thread.
    std::string shown() {
        std::string text;
        on_ui([&] { text = banner_text(layer->banner()); });
        return text;
    }
    bool wait_shown(const std::string& needle) {
        const bool ok = wait_until([&] { return shown().find(needle) != std::string::npos; }, 2000);
        EXPECT_TRUE(ok) << shown();
        return ok;
    }
    bool wait_none() {
        const bool ok = wait_until([&] { return shown().empty(); }, 2000);
        EXPECT_TRUE(ok) << shown();
        return ok;
    }
};

// Clearing every notice takes its banner down at once, and the banners
// waiting behind it never show.
TEST_F(StatusShellBannerTest, ClearAllTakesTheBannersDown) {
    start_shell();
    post("a", "n1", "First");
    post("a", "n2", "Second");
    ASSERT_TRUE(wait_shown("First"));
    notes->clear_all();
    ASSERT_TRUE(wait_none());
    wisp::os::sleep_ms(50);
    EXPECT_EQ(shown(), "");
}

// Dismissing one notice (its close button in the panel, a swipe) takes
// down its banner, shown or waiting, and only its.
TEST_F(StatusShellBannerTest, DismissTakesItsBannerDown) {
    start_shell();
    post("a", "n1", "First");
    post("a", "n2", "Second");
    post("a", "n3", "Third");
    ASSERT_TRUE(wait_shown("First"));
    ASSERT_TRUE(notes->dismiss("a", "n2", wisp::core::DismissReason::Dismiss));
    wisp::os::sleep_ms(50);
    EXPECT_NE(shown().find("First"), std::string::npos);
    ASSERT_TRUE(notes->dismiss("a", "n1", wisp::core::DismissReason::Dismiss));
    EXPECT_TRUE(wait_shown("Third"));
}

// A notice gone before its banner reaches the UI thread gets none.
TEST_F(StatusShellBannerTest, NoBannerForANoticeAlreadyGone) {
    start_shell();
    on_ui([&] {
        post("a", "n1", "First");
        notes->clear_all();
    });
    wisp::os::sleep_ms(50);
    EXPECT_EQ(shown(), "");
}

// A foreground scene's own post is a banner only, never listed: the
// changes of the list do not take it down.
TEST_F(StatusShellBannerTest, BannerOnlyPostStaysThroughListChanges) {
    start_shell();
    Notification n;
    n.source = "f";
    n.id = "own";
    n.source_name = "Front";
    n.title = "Own";
    ASSERT_TRUE(notes->post(std::move(n), true));
    ASSERT_TRUE(wait_shown("Own"));
    post("a", "n1", "Listed");
    notes->clear_all();
    wisp::os::sleep_ms(50);
    EXPECT_NE(shown().find("Own"), std::string::npos);
}

// The pull-down panel lists every notice: while it is there no banner
// shows over it, and those it hid do not come back once it closes.
TEST_F(StatusShellBannerTest, PanelHidesBanners) {
    start_shell();
    post("a", "n1", "First");
    post("a", "n2", "Second");
    ASSERT_TRUE(wait_shown("First"));
    open_panel();
    EXPECT_EQ(shown(), "");
    post("a", "n3", "Third");
    wisp::os::sleep_ms(50);
    EXPECT_EQ(shown(), "");
    EXPECT_TRUE(panel_has("Third"));
    on_ui([&] { layer->close_panel(); });
    wisp::os::sleep_ms(50);
    EXPECT_EQ(shown(), "");
}

// stop() takes the handlers and the banner away before the layer goes.
TEST_F(StatusShellTest, StopUninstalls) {
    start_shell();
    post("a", "n1", "First");
    wait_counts(0, 1);
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

// The shell follows its cells on the UI thread. In the "Flooded" tests
// wisp.bus is held and flooded, so no scene.state, notification.* or
// power.state event reaches anyone: what refreshes or closes the panel is
// a cell's callback. The shell has no timer of its own and nothing else
// rebuilds an open panel's model, so the LVGL timers may keep running.
TEST_F(StatusShellTest, StandbyClosesThePanelFlooded) {
    start_shell();
    open_panel();
    wisp::test::BusFlood flood(bus);
    flood.hold();
    power.set(wisp::hal::PowerLevel::Standby);
    flood.flood(256);
    EXPECT_TRUE(wait_until([&] { return !panel_is_open(); }, 3000));
    flood.release();
}

TEST_F(StatusShellTest, NewNoticeRefreshesThePanelFlooded) {
    start_shell();
    open_panel();
    EXPECT_FALSE(panel_has("First"));
    wisp::test::BusFlood flood(bus);
    flood.hold();
    post("a", "n1", "First");
    flood.flood(256);
    EXPECT_TRUE(wait_until([&] { return panel_has("First"); }, 3000));
    EXPECT_TRUE(
        wait_until([&] { return log_has("status", "status: background=0 unread=1"); }, 3000));
    flood.release();
}

// A headless scene sent to the background leaves the headless count and
// joins the background list: the overview moves, and the open panel lists
// the scene.
TEST_F(StatusShellTest, HeadlessSceneSentToTheBackgroundIsListedFlooded) {
    start_shell();
    install("h", kHeadlessTool, {{"scene.json", tool_manifest("h", "Helper")}});
    auto r = rt->call_tool("h", "get", "{}", 5000, {});
    ASSERT_TRUE(r) << r.error().message;
    ASSERT_TRUE(r.value().ok);
    ASSERT_EQ(rt->status("h").state, SceneState::Headless);
    open_panel();
    EXPECT_FALSE(panel_has("Helper"));
    wisp::test::BusFlood flood(bus);
    flood.hold();
    ASSERT_TRUE(rt->open_background("h"));
    ASSERT_EQ(rt->status("h").state, SceneState::Background);
    flood.flood(256);
    EXPECT_TRUE(wait_until([&] { return panel_has("Helper"); }, 3000));
    flood.release();
}

// Started while the screen is off, the shell closes a panel left open at
// once: the first call of its power cell delivers the current level.
TEST_F(StatusShellTest, StartedInStandbyShowsNoPanel) {
    start("circle");
    on_ui([&] {
        wisp::ui::PanelHandlers h;
        h.on_open = [] {};
        layer->set_panel_handlers(std::move(h));
        layer->open_panel();
        ASSERT_TRUE(layer->panel_open());
    });
    power.set(wisp::hal::PowerLevel::Standby);
    router = std::make_unique<NotificationRouter>(
        NotificationRouterDeps{.notes = *notes, .scenes = rt.get(), .ui = ui.get()});
    router->start();
    shell = std::make_unique<StatusShell>(StatusShellDeps{
        .ui = *ui, .layer = *layer, .notes = *notes, .router = *router, .power = &power});
    shell->start();
    EXPECT_TRUE(wait_until([&] { return !panel_is_open(); }, 3000));
}

}  // namespace

TEST_F(StatusShellTest, DisabledWifiHasNoPanelIndicator) {
    start_shell();
    on_ui([&] { EXPECT_FALSE(shell->panel_model().header.wifi.empty()); });
    open_panel();
    EXPECT_TRUE(panel_has(wisp::ui::tr(StrId::NetConnected)));
    wifi_off = true;
    wifi.update([](wisp::net::WifiSnapshot& w) { w.enabled = false; });
    EXPECT_TRUE(wait_until([&] { return !panel_has(wisp::ui::tr(StrId::NetConnected)); }, 2000));
    on_ui([&] { EXPECT_TRUE(shell->panel_model().header.wifi.empty()); });
}

TEST_F(StatusShellTest, EnabledButDisconnectedWifiRetainsPanelIndicator) {
    start_shell();
    wifi_disconnected = true;
    on_ui([&] { EXPECT_EQ(shell->panel_model().header.wifi, wisp::ui::tr(StrId::NetOff)); });
}
