// SceneRuntime with scenes in the background (scene-device spec 3.1-3.4):
// going back, coming forward without a reload, timers with the screen off,
// the APIs a scene in the background may not use, and its errors.

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "lvgl.h"
#include "scene_message.hpp"
#include "support/fake_http.hpp"
#include "support/scene_runtime_fixture.hpp"
#include "support/wait.hpp"
#include "wisp/core/wakelock.hpp"
#include "wisp/net/scene_http.hpp"
#include "wisp/scene/scene_runtime.hpp"

namespace {

using wisp::core::SceneState;
using wisp::core::WakeLockKind;
using wisp::scene::BackgroundScene;
using wisp::scene::LaunchReason;
using wisp::scene::SceneNotice;
using wisp::scene::SceneNoticeKind;
using wisp::test::StateEvent;
using wisp::test::wait_until;

// scene.json of a scene that declares "background".
std::string bg_manifest(const std::string& id, const std::string& name = "Demo",
                        const std::string& extra = "") {
    return R"({"id":")" + id + R"(","name":")" + name +
           R"(","version":"1.0.0","schema_version":1,"background":true)" + extra + "}";
}

// Prints every lifecycle hook as it runs.
const char* kHooks = R"(
scene.on_show = function() print("show") end
scene.on_hide = function() print("hide") end
scene.on_close = function() print("close") end
scene.on_state = function(s) print("state:" .. s) end
)";

const char* kCounter = R"(
local count = state(0)
return ui.screen {
  ui.label { text = bind(count, function(v) return "count " .. v end) },
  ui.button { ref = "plus", text = "+1", on_click = function() count:set(count:get() + 1) end },
}
)";

class SceneBackground : public wisp::test::SceneRuntimeFixture {
protected:
    void SetUp() override {
        SceneRuntimeFixture::SetUp();
        configure_deps = [this](wisp::scene::SceneRuntimeDeps& d) {
            d.locks = &locks;
            d.on_notice = [this](const SceneNotice& n) {
                std::lock_guard<std::mutex> lock(notice_mu);
                notices.push_back(n);
            };
        };
    }

    void install_bg(const std::string& id, const std::string& lua) {
        install(id, lua, {{"scene.json", bg_manifest(id)}});
    }

    // The system layer's back gesture: a swipe in from the left edge.
    void back_gesture() {
        on_ui([&] { lv_obj_update_layout(lv_layer_top()); });
        feed({{4, 240, 0}});
        for (int16_t x : {4, 50, 100, 150, 200}) feed({{x, 240, 0}});
        feed({});
    }

    bool wait_state(const std::string& id, SceneState state, uint32_t ms = 2000) {
        return wait_until([&] { return rt->status(id).state == state; }, ms);
    }

    // What the scene printed: its log without the runtime's own lines.
    std::vector<std::string> printed(const std::string& id) {
        std::vector<std::string> out;
        for (auto& line : scene_log(id))
            if (line.rfind("rebuilt in ", 0) != 0) out.push_back(std::move(line));
        return out;
    }

    std::vector<std::string> background_ids() {
        std::vector<std::string> ids;
        for (const BackgroundScene& b : rt->background_scenes()) ids.push_back(b.id);
        return ids;
    }

    size_t count_log(const std::string& id, const std::string& line) {
        auto log = scene_log(id);
        return static_cast<size_t>(std::count(log.begin(), log.end(), line));
    }

    std::vector<SceneNotice> seen_notices() {
        std::lock_guard<std::mutex> lock(notice_mu);
        return notices;
    }

    wisp::core::WakeLockRegistry locks{bus};
    std::mutex notice_mu;
    std::vector<SceneNotice> notices;
};

TEST_F(SceneBackground, BackGestureSendsDeclaredSceneToBackground) {
    start();
    install("a", std::string(kHooks) + "return ui.screen { ui.label { text = 'scene a' } }",
            {{"scene.json", bg_manifest("a", "Alpha")}});
    ASSERT_TRUE(rt->open("a"));
    ASSERT_TRUE(wait_label("scene a"));
    back_gesture();
    ASSERT_TRUE(wait_state("a", SceneState::Background));
    EXPECT_EQ(scene_log("a"),
              (std::vector<std::string>{"show", "state:foreground", "hide", "state:background"}));
    EXPECT_TRUE(wait_until([&] { return active_screen() == home; }, 1000));
    EXPECT_TRUE(label("scene a").empty());
    EXPECT_EQ(rt->foreground(), std::nullopt);
    ASSERT_EQ(rt->background_scenes().size(), 1u);
    EXPECT_EQ(rt->background_scenes()[0].id, "a");
    EXPECT_EQ(rt->background_scenes()[0].name, "Alpha");
    EXPECT_GT(rt->background_scenes()[0].since_ms, 0u);
    EXPECT_EQ(events(), (std::vector<StateEvent>{{"a", "running"}, {"a", "background"}}));
    EXPECT_TRUE(log_has("scene", "a: background"));
}

TEST_F(SceneBackground, BackGestureClosesUndeclaredScene) {
    start();
    install("a", std::string(kHooks) + "return ui.screen { ui.label { text = 'scene a' } }");
    ASSERT_TRUE(rt->open("a"));
    ASSERT_TRUE(wait_label("scene a"));
    back_gesture();
    ASSERT_TRUE(wait_state("a", SceneState::Stopped));
    EXPECT_EQ(scene_log("a"),
              (std::vector<std::string>{"show", "state:foreground", "hide", "close"}));
    EXPECT_TRUE(wait_until([&] { return active_screen() == home; }, 1000));
    EXPECT_TRUE(rt->background_scenes().empty());
    EXPECT_EQ(events(), (std::vector<StateEvent>{{"a", "running"}, {"a", "stopped"}}));
}

TEST_F(SceneBackground, OpeningAnotherSendsDeclaredToBackground) {
    start();
    install_bg("a", std::string(kHooks) + "return ui.screen { ui.label { text = 'scene a' } }");
    install("b", "return ui.screen { ui.label { text = 'scene b' } }");
    ASSERT_TRUE(rt->open("a"));
    ASSERT_TRUE(rt->open("b"));
    EXPECT_EQ(rt->status("a").state, SceneState::Background);
    EXPECT_EQ(rt->status("b").state, SceneState::Running);
    EXPECT_EQ(rt->foreground(), std::optional<std::string>("b"));
    EXPECT_EQ(background_ids(), (std::vector<std::string>{"a"}));
    EXPECT_EQ(count_log("a", "close"), 0u);
    EXPECT_EQ(count_log("a", "state:background"), 1u);
    ASSERT_TRUE(wait_label("scene b"));
    EXPECT_TRUE(label("scene a").empty());

    // Back from b (which closes) leaves a where it is.
    ASSERT_TRUE(rt->close("b"));
    EXPECT_EQ(rt->status("b").state, SceneState::Stopped);
    EXPECT_EQ(rt->status("a").state, SceneState::Background);
    EXPECT_TRUE(wait_until([&] { return active_screen() == home; }, 1000));
}

TEST_F(SceneBackground, OpeningBackgroundSceneResumesWithoutReload) {
    start();
    install_bg("a", R"(
print("main")
local count = state(0)
scene.on_show = function() print("show") end
scene.on_state = function(s)
  print("state:" .. s)
  if s == "background" then count:set(count:get() + 10) end
end
return ui.screen {
  ui.label { text = bind(count, function(v) return "count " .. v end) },
  ui.button { ref = "plus", text = "+1", on_click = function() count:set(count:get() + 1) end },
}
)");
    ASSERT_TRUE(rt->open("a"));
    const uint32_t instance = wisp::scene::scene_instance(*rt, "a");
    ASSERT_NE(instance, 0u);
    ASSERT_TRUE(wait_label("count 0"));
    ASSERT_TRUE(rt->click_ref("plus"));
    ASSERT_TRUE(wait_until([&] { return label("count") == "count 1"; }, 1000));
    back_gesture();
    ASSERT_TRUE(wait_state("a", SceneState::Background));

    ASSERT_TRUE(rt->open("a"));
    EXPECT_EQ(rt->status("a").state, SceneState::Running);
    EXPECT_EQ(rt->foreground(), std::optional<std::string>("a"));
    EXPECT_EQ(wisp::scene::scene_instance(*rt, "a"), instance);
    EXPECT_TRUE(rt->background_scenes().empty());
    // The value it changed in the background is on the new view.
    EXPECT_TRUE(wait_until([&] { return label("count") == "count 11"; }, 1000)) << label("count");
    EXPECT_EQ(printed("a"),
              (std::vector<std::string>{"main", "show", "state:foreground", "state:background",
                                        "show", "state:foreground"}));
    EXPECT_TRUE(log_has("scene.a", "rebuilt in "));
    EXPECT_EQ(events(),
              (std::vector<StateEvent>{{"a", "running"}, {"a", "background"}, {"a", "running"}}));
}

TEST_F(SceneBackground, BackgroundUpdatesTreeWithoutRendering) {
    start();
    install_bg("a", R"(
local n = state(0)
wisp.timer.every(50, function()
  if wisp.scene.state() == "background" then
    n:set(n:get() + 1)
    print("tick")
  end
end)
return ui.screen { ui.label { text = bind(n, function(v) return "n " .. v end) } }
)");
    ASSERT_TRUE(rt->open("a"));
    ASSERT_TRUE(wait_label("n 0"));
    back_gesture();
    ASSERT_TRUE(wait_state("a", SceneState::Background));
    ASSERT_TRUE(wait_until([&] { return active_screen() == home; }, 1000));
    // With no frames, anything the scene sent would stay queued.
    ui->set_paused(true);
    ASSERT_TRUE(wait_until([&] { return count_log("a", "tick") >= 3; }, 2000));
    EXPECT_EQ(wisp::scene::pending_patches(*rt), 0u);
    ui->set_paused(false);

    ASSERT_TRUE(rt->open("a"));
    ASSERT_TRUE(wait_label("n "));
    EXPECT_NE(label("n "), "n 0");
}

TEST_F(SceneBackground, RebuildKeepsRefsWorking) {
    start();
    install_bg("a", R"(
local count = state(0)
return ui.screen {
  ui.label { text = bind(count, function(v) return "count " .. v end) },
  ui.input { ref = "name", text = "Ada" },
  ui.button { ref = "plus", text = "+1", on_click = function()
    count:set(count:get() + 1)
    print("name " .. ui.ref("name"):value())
  end },
}
)");
    ASSERT_TRUE(rt->open("a"));
    ASSERT_TRUE(wait_label("count 0"));
    ASSERT_TRUE(rt->close("a"));
    ASSERT_EQ(rt->status("a").state, SceneState::Background);
    ASSERT_TRUE(rt->open("a"));
    ASSERT_TRUE(wait_label("count 0"));
    ASSERT_TRUE(rt->click_ref("plus"));
    EXPECT_TRUE(wait_until([&] { return label("count") == "count 1"; }, 1000)) << label("count");
    EXPECT_TRUE(wait_until([&] { return count_log("a", "name Ada") == 1; }, 1000));
    EXPECT_EQ(rt->status("a").state, SceneState::Running);
}

TEST_F(SceneBackground, ExitClosesFromAnyState) {
    start();
    install_bg("a", std::string(kHooks) + "return ui.screen { ui.label { text = 'scene a' } }");
    ASSERT_TRUE(rt->open("a"));
    ASSERT_TRUE(rt->close("a"));
    ASSERT_EQ(rt->status("a").state, SceneState::Background);
    ASSERT_TRUE(rt->exit("a"));
    EXPECT_EQ(rt->status("a").state, SceneState::Stopped);
    // It was hidden when it went to the background: no second on_hide.
    EXPECT_EQ(scene_log("a"), (std::vector<std::string>{"show", "state:foreground", "hide",
                                                        "state:background", "close"}));
    EXPECT_TRUE(rt->background_scenes().empty());
    EXPECT_EQ(wisp::scene::scene_instance(*rt, "a"), 0u);
    EXPECT_EQ(events(),
              (std::vector<StateEvent>{{"a", "running"}, {"a", "background"}, {"a", "stopped"}}));

    // From the foreground: closed, not sent to the background.
    ASSERT_TRUE(rt->open("a"));
    ASSERT_TRUE(rt->exit("a"));
    EXPECT_EQ(rt->status("a").state, SceneState::Stopped);
    EXPECT_TRUE(rt->background_scenes().empty());
    EXPECT_TRUE(wait_until([&] { return active_screen() == home; }, 1000));
    // Not running: nothing to do.
    EXPECT_TRUE(rt->exit("a"));
    EXPECT_TRUE(rt->exit("nope"));
}

TEST_F(SceneBackground, ErrorInBackgroundStopsAndNotifies) {
    start();
    install("a", R"(
scene.on_state = function(s)
  if s == "background" then wisp.timer.after(50, function() error("bg boom") end) end
end
return ui.screen { ui.label { text = "scene a" } }
)",
            {{"scene.json", bg_manifest("a", "Alpha")}});
    install("b", "return ui.screen { ui.label { text = 'scene b' } }");
    ASSERT_TRUE(rt->open("a"));
    ASSERT_TRUE(rt->open("b"));
    ASSERT_TRUE(wait_state("a", SceneState::Error));
    EXPECT_NE(rt->status("a").error.find("bg boom"), std::string::npos) << rt->status("a").error;
    EXPECT_TRUE(log_has("scene.a", "bg boom"));
    ASSERT_TRUE(wait_until([&] { return seen_notices().size() == 1; }, 1000));
    EXPECT_EQ(seen_notices()[0].kind, SceneNoticeKind::Faulted);
    EXPECT_EQ(seen_notices()[0].id, "a");
    EXPECT_EQ(seen_notices()[0].name, "Alpha");
    EXPECT_TRUE(rt->background_scenes().empty());
    // The scene in front is untouched, and no panel covers it.
    EXPECT_EQ(rt->status("b").state, SceneState::Running);
    EXPECT_TRUE(wait_label("scene b"));
    bool panel = true;
    on_ui([&] { panel = layer->error_visible(); });
    EXPECT_FALSE(panel);
    EXPECT_EQ(events(),
              (std::vector<StateEvent>{
                  {"a", "running"}, {"a", "background"}, {"b", "running"}, {"a", "error"}}));

    // Opened again from the error state: a fresh start.
    ASSERT_TRUE(rt->open("a"));
    EXPECT_EQ(rt->status("a").state, SceneState::Running);
}

TEST_F(SceneBackground, TimersRunWhileScreenOffForDeclaredScene) {
    start();
    install_bg("a", R"(
scene.on_hide = function() print("hide") end
wisp.timer.every(100, function() print("tick") end)
return ui.screen { ui.label { text = "scene a" } }
)");
    ASSERT_TRUE(rt->open("a"));
    rt->set_visible(false);
    ASSERT_TRUE(wait_until([&] { return count_log("a", "hide") == 1; }, 1000));
    const size_t before = count_log("a", "tick");
    EXPECT_TRUE(wait_until([&] { return count_log("a", "tick") >= before + 3; }, 2000));
    EXPECT_EQ(rt->status("a").state, SceneState::Running);

    // In the background with the screen off too.
    ASSERT_TRUE(rt->close("a"));
    ASSERT_EQ(rt->status("a").state, SceneState::Background);
    const size_t then = count_log("a", "tick");
    EXPECT_TRUE(wait_until([&] { return count_log("a", "tick") >= then + 3; }, 2000));
}

TEST_F(SceneBackground, UndeclaredSceneTimersStillPause) {
    start();
    install("a", R"(
scene.on_hide = function() print("hide") end
wisp.timer.every(100, function() print("tick") end)
return ui.screen { ui.label { text = "scene a" } }
)");
    ASSERT_TRUE(rt->open("a"));
    ASSERT_TRUE(wait_until([&] { return count_log("a", "tick") >= 1; }, 2000));
    rt->set_visible(false);
    ASSERT_TRUE(wait_until([&] { return count_log("a", "hide") == 1; }, 1000));
    const size_t paused = count_log("a", "tick");
    wisp::os::sleep_ms(400);
    EXPECT_EQ(count_log("a", "tick"), paused);
    rt->set_visible(true);
    EXPECT_TRUE(wait_until([&] { return count_log("a", "tick") > paused; }, 2000));
}

TEST_F(SceneBackground, ForbiddenApisInBackground) {
    start();
    install_bg("a", R"(
scene.on_state = function(s)
  print(s, wisp.power.keep_screen_on(true))
  print(s, ui.ref("name"):focus())
end
return ui.screen { ui.input { ref = "name" } }
)");
    ASSERT_TRUE(rt->open("a"));
    ASSERT_TRUE(rt->close("a"));
    ASSERT_EQ(rt->status("a").state, SceneState::Background);
    EXPECT_EQ(scene_log("a"), (std::vector<std::string>{"foreground\ttrue", "foreground",
                                                        "background\tnil\tnot_in_background",
                                                        "background\tfalse\tnot_in_background"}));
    EXPECT_FALSE(locks.held(WakeLockKind::Screen));
}

TEST_F(SceneBackground, KeepScreenOnWithoutLocksIsUnavailable) {
    configure_deps = {};
    start();
    install("a", R"(
print(wisp.power.keep_screen_on(true))
return ui.screen { ui.label { text = "x" } }
)");
    ASSERT_TRUE(rt->open("a"));
    EXPECT_EQ(scene_log("a"), (std::vector<std::string>{"nil\tunavailable"}));
}

TEST_F(SceneBackground, ScreenLockFollowsForeground) {
    start();
    install_bg("a", R"(
scene.on_show = function() wisp.power.keep_screen_on(true) end
return ui.screen {
  ui.button { ref = "off", text = "off", on_click = function()
    print(wisp.power.keep_screen_on(false))
  end },
}
)");
    ASSERT_TRUE(rt->open("a"));
    EXPECT_TRUE(locks.held(WakeLockKind::Screen));
    auto held = locks.list();
    ASSERT_EQ(held.size(), 1u);
    EXPECT_EQ(held[0].holder, "scene.a");
    EXPECT_EQ(held[0].reason, "keep_screen_on");
    ASSERT_TRUE(rt->close("a"));
    ASSERT_EQ(rt->status("a").state, SceneState::Background);
    EXPECT_FALSE(locks.held(WakeLockKind::Screen));

    // Back in front, on_show takes it again; the scene can drop it, and
    // closing drops whatever it still holds.
    ASSERT_TRUE(rt->open("a"));
    EXPECT_TRUE(locks.held(WakeLockKind::Screen));
    ASSERT_TRUE(wait_label("off"));
    ASSERT_TRUE(rt->click_ref("off"));
    ASSERT_TRUE(wait_until([&] { return count_log("a", "true") == 1; }, 1000));
    EXPECT_FALSE(locks.held(WakeLockKind::Screen));
    ASSERT_TRUE(rt->open("a"));
    ASSERT_TRUE(rt->exit("a"));
    EXPECT_TRUE(locks.list().empty());
}

TEST_F(SceneBackground, SceneCloseApiMeansBack) {
    start();
    const char* lua = R"(
return ui.screen {
  ui.button { ref = "back", text = "back", on_click = function() wisp.scene.close() end },
  ui.button { ref = "quit", text = "quit", on_click = function() wisp.scene.exit() end },
}
)";
    install_bg("a", lua);
    install("b", lua);
    ASSERT_TRUE(rt->open("a"));
    ASSERT_TRUE(wait_label("back"));
    ASSERT_TRUE(rt->click_ref("back"));
    ASSERT_TRUE(wait_state("a", SceneState::Background));
    EXPECT_TRUE(wait_until([&] { return active_screen() == home; }, 1000));

    ASSERT_TRUE(rt->open("a"));
    ASSERT_TRUE(wait_label("quit"));
    ASSERT_TRUE(rt->click_ref("quit"));
    ASSERT_TRUE(wait_state("a", SceneState::Stopped));
    EXPECT_TRUE(rt->background_scenes().empty());

    ASSERT_TRUE(rt->open("b"));
    ASSERT_TRUE(wait_label("back"));
    ASSERT_TRUE(rt->click_ref("back"));
    ASSERT_TRUE(wait_state("b", SceneState::Stopped));
}

TEST_F(SceneBackground, StateAndLaunchQueries) {
    start();
    install_bg("a", R"(
print(wisp.scene.state(), wisp.scene.launch())
scene.on_state = function(s)
  if s == "background" then
    wisp.timer.after(50, function() print("later", wisp.scene.state()) end)
  end
end
return ui.screen { ui.label { text = "x" } }
)");
    ASSERT_TRUE(rt->open("a"));
    ASSERT_TRUE(rt->close("a"));
    ASSERT_TRUE(wait_until([&] { return scene_log("a").size() == 2; }, 1000))
        << ::testing::PrintToString(scene_log("a"));
    EXPECT_EQ(scene_log("a"), (std::vector<std::string>{"foreground\tuser", "later\tbackground"}));
}

TEST_F(SceneBackground, BootLaunchStartsInBackgroundWithoutView) {
    start();
    install_bg("a", std::string(kHooks) + R"(
print(wisp.scene.state(), wisp.scene.launch())
return ui.screen { ui.label { text = "scene a" } }
)");
    ASSERT_TRUE(rt->launch("a", LaunchReason::Boot));
    EXPECT_EQ(rt->status("a").state, SceneState::Background);
    EXPECT_EQ(rt->foreground(), std::nullopt);
    EXPECT_EQ(background_ids(), (std::vector<std::string>{"a"}));
    EXPECT_EQ(scene_log("a"), (std::vector<std::string>{"background\tboot", "state:background"}));
    EXPECT_EQ(active_screen(), home);
    EXPECT_EQ(events(), (std::vector<StateEvent>{{"a", "background"}}));
    // Already running: nothing happens.
    ASSERT_TRUE(rt->launch("a", LaunchReason::Boot));
    EXPECT_EQ(scene_log("a").size(), 2u);

    ASSERT_TRUE(rt->open("a"));
    EXPECT_EQ(rt->status("a").state, SceneState::Running);
    EXPECT_TRUE(wait_label("scene a"));
}

class SceneBackgroundHttp : public SceneBackground {
protected:
    void SetUp() override {
        SceneBackground::SetUp();
        http = std::make_unique<wisp::net::SceneHttp>(client);
        ASSERT_TRUE(http->start());
        auto base = configure_deps;
        configure_deps = [this, base](wisp::scene::SceneRuntimeDeps& d) {
            base(d);
            d.http = http.get();
        };
    }

    void TearDown() override {
        client.hold = false;
        SceneBackground::TearDown();
        http.reset();
    }

    wisp::test::FakeHttpClient client;
    std::unique_ptr<wisp::net::SceneHttp> http;
};

TEST_F(SceneBackgroundHttp, CallbacksSurviveBackgroundAndDieWithExit) {
    start();
    install("h", R"(
local n = 0
return ui.screen {
  ui.button { ref = "go", text = "go", on_click = function()
    n = n + 1
    local mine = n
    wisp.http.get("https://api.example.com/x", nil, function(err, res)
      print("got " .. mine, err, res and res.status)
    end)
  end },
}
)",
            {{"scene.json",
              bg_manifest("h", "Demo", R"(,"permissions":{"http":["api.example.com"]})")}});
    client.hold = true;
    ASSERT_TRUE(rt->open("h"));
    ASSERT_TRUE(wait_label("go"));
    ASSERT_TRUE(rt->click_ref("go"));
    ASSERT_TRUE(wait_until([&] { return client.held.load() == 1; }, 2000));
    ASSERT_TRUE(rt->close("h"));
    ASSERT_EQ(rt->status("h").state, SceneState::Background);
    client.hold = false;
    EXPECT_TRUE(wait_until([&] { return count_log("h", "got 1\tnil\t200") == 1; }, 2000))
        << ::testing::PrintToString(scene_log("h"));

    // Sent in front, still out when the scene exits from the background:
    // no callback.
    client.hold = true;
    ASSERT_TRUE(rt->open("h"));
    ASSERT_TRUE(wait_label("go"));
    ASSERT_TRUE(rt->click_ref("go"));
    ASSERT_TRUE(wait_until([&] { return client.held.load() == 1; }, 2000));
    ASSERT_TRUE(rt->close("h"));
    const uint32_t instance = wisp::scene::scene_instance(*rt, "h");
    ASSERT_NE(instance, 0u);
    ASSERT_TRUE(rt->exit("h"));
    EXPECT_TRUE(wait_until([&] { return client.saw_cancel.load(); }, 2000));
    client.hold = false;
    wisp::os::sleep_ms(100);
    EXPECT_EQ(count_log("h", "got 2\tnil\t200"), 0u);

    // A result already on its way for it is dropped as stale.
    const uint64_t stale = rt->stale_dropped();
    auto msg = wisp::os::make_large<wisp::scene::SceneMessage>();
    msg->scene_id = "h";
    msg->instance = instance;
    msg->body = wisp::scene::msg::HttpDone{2, {}};
    ASSERT_TRUE(wisp::scene::post_scene_message(*rt, std::move(msg)));
    EXPECT_TRUE(wait_until([&] { return rt->stale_dropped() == stale + 1; }, 1000));
    EXPECT_EQ(rt->status("h").state, SceneState::Stopped);
}

// Until the background has its eviction: a scene that would be the
// fourth there closes instead.
TEST_F(SceneBackground, FourthBackgroundIsClosedForNow) {
    start();
    for (const char* id : {"a", "b", "c", "d"})
        install_bg(id, std::string(kHooks) + "return ui.screen { ui.label { text = 'x' } }");
    ASSERT_TRUE(rt->open("a"));
    ASSERT_TRUE(rt->open("b"));
    ASSERT_TRUE(rt->open("c"));
    ASSERT_TRUE(rt->open("d"));
    // Least recently in front first.
    EXPECT_EQ(background_ids(), (std::vector<std::string>{"a", "b", "c"}));
    ASSERT_TRUE(rt->close("d"));
    EXPECT_EQ(rt->status("d").state, SceneState::Stopped);
    EXPECT_EQ(count_log("d", "close"), 1u);
    EXPECT_EQ(background_ids(), (std::vector<std::string>{"a", "b", "c"}));

    // Bringing one forward makes room for the one it replaces.
    ASSERT_TRUE(rt->open("d"));
    ASSERT_TRUE(rt->open("a"));
    EXPECT_EQ(rt->status("a").state, SceneState::Running);
    EXPECT_EQ(rt->status("d").state, SceneState::Background);
    EXPECT_EQ(background_ids(), (std::vector<std::string>{"b", "c", "d"}));
}

// A view that would only come up to go again (the UI thread was held up
// the whole time) is never made: the drop cancels the show still waiting,
// and with it the package the show held.
TEST_F(SceneBackground, DropCancelsAShowStillWaiting) {
    start();
    install_bg("a", "return ui.screen { ui.label { text = 'scene a' } }");
    install("b", "return ui.screen { ui.label { text = 'scene b' } }");
    {
        wisp::test::UiStall stall(*ui);
        ASSERT_TRUE(rt->open("a"));
        EXPECT_EQ(wisp::scene::pending_ui_jobs(*rt), 1u);
        ASSERT_TRUE(rt->close("a"));
        // The show is gone; only the drop (a no-op now) waits.
        EXPECT_EQ(wisp::scene::pending_ui_jobs(*rt), 1u);
        ASSERT_TRUE(rt->open("b"));
        ASSERT_TRUE(rt->close("b"));
        EXPECT_EQ(wisp::scene::pending_ui_jobs(*rt), 2u);
    }
    EXPECT_TRUE(wait_until([&] { return wisp::scene::pending_ui_jobs(*rt) == 0; }, 2000));
    EXPECT_EQ(active_screen(), home);
    EXPECT_TRUE(wait_until([&] { return log_has("scene", "b: closed"); }, 1000));
    // Coming forward again works as ever.
    ASSERT_TRUE(rt->open("a"));
    EXPECT_TRUE(wait_label("scene a"));
}

}  // namespace
