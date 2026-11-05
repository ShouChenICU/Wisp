// SceneRuntime with scenes in the background (scene-device spec 3.1-3.4):
// going back, coming forward without a reload, timers with the screen off,
// the APIs a scene in the background may not use, and its errors.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <sstream>
#include <string>
#include <vector>

#include "lvgl.h"
#include "scene_message.hpp"
#include "support/fake_http.hpp"
#include "support/scene_runtime_fixture.hpp"
#include "support/wait.hpp"
#include "wisp/base/json.hpp"
#include "wisp/core/scene_install.hpp"
#include "wisp/core/wakelock.hpp"
#include "wisp/core/zip.hpp"
#include "wisp/net/scene_http.hpp"
#include "wisp/scene/scene_runtime.hpp"
#include "wisp/ui/strings.hpp"

namespace {

using wisp::Map;
using wisp::Value;
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

// Spec 3.5: a fourth scene sent to the background closes the one least
// recently in front, which runs on_close like any close, and says so.
TEST_F(SceneBackground, FourthBackgroundEvictsOldestWithNotice) {
    start();
    for (const char* id : {"a", "b", "c", "d"})
        install(id, std::string(kHooks) + "return ui.screen { ui.label { text = 'x' } }",
                {{"scene.json", bg_manifest(id, std::string("Name ") + id)}});
    ASSERT_TRUE(rt->open("a"));
    ASSERT_TRUE(rt->open("b"));
    ASSERT_TRUE(rt->open("c"));
    ASSERT_TRUE(rt->open("d"));
    // Least recently in front first.
    EXPECT_EQ(background_ids(), (std::vector<std::string>{"a", "b", "c"}));
    EXPECT_TRUE(seen_notices().empty());
    ASSERT_TRUE(rt->close("d"));
    EXPECT_EQ(rt->status("d").state, SceneState::Background);
    EXPECT_EQ(rt->status("a").state, SceneState::Stopped);
    EXPECT_EQ(count_log("a", "close"), 1u);
    EXPECT_EQ(scene_log("a").back(), "close");
    EXPECT_EQ(count_log("d", "close"), 0u);
    EXPECT_EQ(background_ids(), (std::vector<std::string>{"b", "c", "d"}));
    auto seen = seen_notices();
    ASSERT_EQ(seen.size(), 1u);
    EXPECT_EQ(seen[0].kind, SceneNoticeKind::EvictedCount);
    EXPECT_EQ(seen[0].id, "a");
    EXPECT_EQ(seen[0].name, "Name a");
    EXPECT_TRUE(log_has("scene", "a: evicted (too many in the background)"));

    // Bringing one forward makes room for the one it replaces.
    ASSERT_TRUE(rt->open("b"));
    EXPECT_EQ(rt->status("b").state, SceneState::Running);
    ASSERT_TRUE(rt->open("a"));
    EXPECT_EQ(rt->status("a").state, SceneState::Running);
    EXPECT_EQ(background_ids(), (std::vector<std::string>{"c", "d", "b"}));
    EXPECT_EQ(seen_notices().size(), 1u);
}

// Spec 3.5: opening a scene closes background scenes, least recently in
// front first, until the one in front can have its full VM limit.
TEST_F(SceneBackground, ForegroundReserveEvictsBackground) {
    configure_config = [](wisp::scene::SceneRuntimeConfig& c) {
        c.vm.memory_bytes = 768 * 1024;
        c.vm_budget = 1024 * 1024;  // 256 KB left for the background
    };
    start();
    install("hog", std::string(kHooks) + R"(
global keep
keep = string.rep("h", 400 * 1024)
return ui.screen { ui.label { text = "hog" } }
)",
            {{"scene.json", bg_manifest("hog", "Hog")}});
    install_bg("small", std::string(kHooks) + "return ui.screen { ui.label { text = 'small' } }");
    install("big", R"(
global keep
keep = string.rep("b", 500 * 1024)
return ui.screen { ui.label { text = "big" } }
)");
    ASSERT_TRUE(rt->open("small"));
    back_gesture();
    ASSERT_TRUE(wait_state("small", SceneState::Background));
    // Never in front, so the least recently there.
    ASSERT_TRUE(rt->launch("hog", LaunchReason::Boot));
    // Nothing in front: memory evicts nothing.
    EXPECT_EQ(background_ids(), (std::vector<std::string>{"hog", "small"}));
    EXPECT_TRUE(seen_notices().empty());

    // The two hold more than the background may: hog, the least recently
    // in front, goes; that leaves room, so small stays.
    ASSERT_TRUE(rt->open("big"));
    EXPECT_EQ(rt->status("big").state, SceneState::Running);
    EXPECT_EQ(rt->status("hog").state, SceneState::Stopped);
    EXPECT_EQ(count_log("hog", "close"), 1u);
    EXPECT_EQ(background_ids(), (std::vector<std::string>{"small"}));
    auto seen = seen_notices();
    ASSERT_EQ(seen.size(), 1u);
    EXPECT_EQ(seen[0].kind, SceneNoticeKind::EvictedMemory);
    EXPECT_EQ(seen[0].id, "hog");
    EXPECT_EQ(seen[0].name, "Hog");
}

// Spec 3.5: a scene in the background that grows past what the background
// may hold is closed before the next message of the scene in front.
TEST_F(SceneBackground, BackgroundGrowthEvictsBeforeForegroundMessage) {
    configure_config = [](wisp::scene::SceneRuntimeConfig& c) {
        c.vm.memory_bytes = 768 * 1024;
        c.vm_budget = 1024 * 1024;
    };
    start();
    install("grow", std::string(kHooks) + R"(
global keep
scene.on_state = function(s)
  if s == "background" then
    wisp.timer.after(50, function() keep = string.rep("g", 400 * 1024) print("grown") end)
  end
end
return ui.screen { ui.label { text = "grow" } }
)",
            {{"scene.json", bg_manifest("grow", "Grow")}});
    install("front", kCounter);
    ASSERT_TRUE(rt->open("grow"));
    ASSERT_TRUE(rt->open("front"));
    ASSERT_TRUE(wait_until([&] { return count_log("grow", "grown") == 1; }, 2000));
    ASSERT_TRUE(wait_label("count 0"));
    ASSERT_TRUE(rt->click_ref("plus"));
    ASSERT_TRUE(wait_state("grow", SceneState::Stopped));
    EXPECT_TRUE(wait_until([&] { return label("count") == "count 1"; }, 1000));
    auto seen = seen_notices();
    ASSERT_EQ(seen.size(), 1u);
    EXPECT_EQ(seen[0].kind, SceneNoticeKind::EvictedMemory);
    EXPECT_EQ(seen[0].id, "grow");
}

// One device event reaches every scene in the same pass. A scene in the
// background that grows in its handler is closed before the handler of
// the scene in front runs, which keeps its guaranteed share (spec 3.5).
TEST_F(SceneBackground, BackgroundGrowthEvictsBeforeForegroundHandlerInSamePass) {
    configure_config = [](wisp::scene::SceneRuntimeConfig& c) {
        c.vm.memory_bytes = 768 * 1024;
        c.vm_budget = 1024 * 1024;
    };
    wisp::ui::set_lang(wisp::ui::Lang::Zh);
    start();
    install("grow", R"(
global keep
wisp.device.on("lang", function() keep = string.rep("g", 400 * 1024) print("grown") end)
return ui.screen { ui.label { text = "grow" } }
)",
            {{"scene.json", bg_manifest("grow", "Grow")}});
    install("front", R"(
global keep
wisp.device.on("lang", function() keep = string.rep("f", 500 * 1024) print("fits") end)
return ui.screen { ui.label { text = "front" } }
)");
    // grow's slot comes first, so its handler runs first.
    ASSERT_TRUE(rt->open("grow"));
    ASSERT_TRUE(rt->open("front"));
    ASSERT_EQ(rt->status("grow").state, SceneState::Background);
    wisp::core::Event e;
    e.topic = "system.settings";
    e.source = "test";
    e.payload = {{"language", Value("en")}};
    bus.publish(std::move(e));
    ASSERT_TRUE(wait_until([&] { return count_log("front", "fits") == 1; }, 2000))
        << ::testing::PrintToString(scene_log("front")) << rt->status("front").error;
    EXPECT_EQ(count_log("grow", "grown"), 1u);
    EXPECT_EQ(rt->status("front").state, SceneState::Running);
    EXPECT_EQ(rt->status("grow").state, SceneState::Stopped);
    auto seen = seen_notices();
    ASSERT_EQ(seen.size(), 1u);
    EXPECT_EQ(seen[0].kind, SceneNoticeKind::EvictedMemory);
    EXPECT_EQ(seen[0].id, "grow");
    wisp::ui::set_lang(wisp::ui::Lang::Zh);
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

// Spec 3.2: on_hide is the scene's last step in front, so what it pairs
// with on_show still works there.
TEST_F(SceneBackground, OnHideRunsWhileStillInFront) {
    start();
    install_bg("a", R"(
scene.on_show = function() print("show", wisp.power.keep_screen_on(true)) end
scene.on_hide = function()
  print("hide", wisp.scene.state(), wisp.power.keep_screen_on(false))
  print("focus", ui.ref("name"):focus() == nil)
end
scene.on_state = function(s) print("state:" .. s) end
return ui.screen { ui.input { ref = "name" } }
)");
    ASSERT_TRUE(rt->open("a"));
    EXPECT_TRUE(locks.held(WakeLockKind::Screen));
    ASSERT_TRUE(rt->close("a"));
    ASSERT_EQ(rt->status("a").state, SceneState::Background);
    EXPECT_EQ(scene_log("a"),
              (std::vector<std::string>{"show\ttrue", "state:foreground", "hide\tforeground\ttrue",
                                        "focus\ttrue", "state:background"}));
    EXPECT_FALSE(locks.held(WakeLockKind::Screen));
    EXPECT_TRUE(wait_until([&] { return active_screen() == home; }, 1000));
}

// The user asked to leave: an error in on_hide does not bring the scene
// back as a panel. It faults as a scene in the background does.
TEST_F(SceneBackground, OnHideErrorStillLeavesTheFront) {
    start();
    install("a", R"(
scene.on_hide = function() error("hide boom") end
return ui.screen { ui.label { text = "scene a" } }
)",
            {{"scene.json", bg_manifest("a", "Alpha")}});
    ASSERT_TRUE(rt->open("a"));
    ASSERT_TRUE(wait_label("scene a"));
    back_gesture();
    ASSERT_TRUE(wait_state("a", SceneState::Error));
    EXPECT_NE(rt->status("a").error.find("hide boom"), std::string::npos) << rt->status("a").error;
    ASSERT_TRUE(wait_until([&] { return seen_notices().size() == 1; }, 1000));
    EXPECT_EQ(seen_notices()[0].kind, SceneNoticeKind::Faulted);
    EXPECT_EQ(seen_notices()[0].id, "a");
    EXPECT_EQ(rt->foreground(), std::nullopt);
    EXPECT_TRUE(rt->background_scenes().empty());
    EXPECT_EQ(wisp::scene::scene_instance(*rt, "a"), 0u);
    EXPECT_TRUE(wait_until([&] { return active_screen() == home; }, 1000));
    bool panel = true;
    on_ui([&] { panel = layer->error_visible(); });
    EXPECT_FALSE(panel);
    EXPECT_EQ(events(), (std::vector<StateEvent>{{"a", "running"}, {"a", "error"}}));

    // Opened again: a fresh start.
    ASSERT_TRUE(rt->open("a"));
    EXPECT_EQ(rt->status("a").state, SceneState::Running);
}

// Spec 3.2/3.3: a scene in the background keeps getting "minute",
// "battery" and "lang", with the screen off and another scene in front.
TEST_F(SceneBackground, DeviceEventsReachBackgroundScene) {
    // 2026-11-04 12:00:59 UTC.
    std::atomic<int64_t> wall{1793793659};
    std::mutex status_mu;
    Value status(
        Map{{"battery", Value(Map{{"percent", Value(int64_t{80})}, {"charging", Value(true)}})}});
    wisp::ui::set_lang(wisp::ui::Lang::Zh);
    auto base = configure_deps;
    configure_deps = [&, base](wisp::scene::SceneRuntimeDeps& d) {
        base(d);
        d.device_status = [&] {
            std::lock_guard<std::mutex> lock(status_mu);
            return status;
        };
        d.wall_clock = [&] { return wall.load(); };
    };
    auto publish = [&](const std::string& topic, wisp::Properties payload) {
        wisp::core::Event e;
        e.topic = topic;
        e.source = "test";
        e.payload = std::move(payload);
        bus.publish(std::move(e));
    };
    start();
    install_bg("a", R"(
wisp.device.on("minute", function(i) print("minute", i.time) end)
wisp.device.on("battery", function(i) print("battery", i.battery, i.charging) end)
wisp.device.on("lang", function(i) print("lang", i.lang) end)
return ui.screen { ui.label { text = "scene a" } }
)");
    install("b", "return ui.screen { ui.label { text = 'scene b' } }");
    ASSERT_TRUE(rt->open("a"));
    ASSERT_TRUE(rt->open("b"));
    ASSERT_EQ(rt->status("a").state, SceneState::Background);
    rt->set_visible(false);
    // A round trip through the scene queue: the ticker has taken its
    // bearings at the old minute.
    ASSERT_TRUE(rt->check_sources({}));
    EXPECT_TRUE(scene_log("a").empty()) << ::testing::PrintToString(scene_log("a"));

    wall = 1793793660;
    ASSERT_TRUE(wait_until([&] { return scene_log("a").size() == 1; }, 2500));
    {
        std::lock_guard<std::mutex> lock(status_mu);
        status = Value(Map{
            {"battery", Value(Map{{"percent", Value(int64_t{79})}, {"charging", Value(false)}})}});
    }
    publish("power.charging.changed", {{"charging", Value(false)}});
    ASSERT_TRUE(wait_until([&] { return scene_log("a").size() == 2; }, 2000));
    publish("system.settings", {{"language", Value("en")}, {"brightness", Value(int64_t{50})}});
    ASSERT_TRUE(wait_until([&] { return scene_log("a").size() == 3; }, 2000));
    EXPECT_EQ(scene_log("a"),
              (std::vector<std::string>{"minute\t1793793660", "battery\t79\tfalse", "lang\ten"}));
    EXPECT_EQ(rt->status("a").state, SceneState::Background);
    EXPECT_EQ(rt->foreground(), std::optional<std::string>("b"));
    rt->stop();
    wisp::ui::set_lang(wisp::ui::Lang::Zh);
}

// Three scenes in the background and one in front, all with timers: each
// callback runs in its own scene, and the one in front still takes taps.
TEST_F(SceneBackground, SeveralBackgroundScenesRunTheirOwnTimers) {
    start();
    for (const char* id : {"a", "b", "c", "d"}) {
        const std::string tick = std::string("print(\"tick ") + id + "\", wisp.scene.state())";
        install_bg(
            id, "local count = state(0)\nwisp.timer.every(50, function() " + tick + " end)\n" + R"(
return ui.screen {
  ui.label { text = bind(count, function(v) return "count " .. v end) },
  ui.button { ref = "plus", text = "+1", on_click = function() count:set(count:get() + 1) end },
}
)");
    }
    for (const char* id : {"a", "b", "c", "d"}) ASSERT_TRUE(rt->open(id));
    ASSERT_EQ(background_ids(), (std::vector<std::string>{"a", "b", "c"}));
    for (const char* id : {"a", "b", "c", "d"}) {
        const std::string state = std::string(id) == "d" ? "foreground" : "background";
        const std::string mine = std::string("tick ") + id + "\t" + state;
        EXPECT_TRUE(wait_until([&] { return count_log(id, mine) >= 3; }, 3000)) << id;
    }
    ASSERT_TRUE(wait_label("count 0"));
    ASSERT_TRUE(rt->click_ref("plus"));
    EXPECT_TRUE(wait_until([&] { return label("count") == "count 1"; }, 1000)) << label("count");
    rt->stop();
    for (const char* id : {"a", "b", "c", "d"}) {
        for (const auto& line : scene_log(id))
            EXPECT_EQ(line.rfind(std::string("tick ") + id + "\t", 0), 0u) << id << ": " << line;
    }
}

// Boot launches queued back to back from a thread that must not wait (the
// storage worker, where the restore of the background list runs) all
// come up.
TEST_F(SceneBackground, BootLaunchesQueuedTogetherAllComeUp) {
    start();
    for (const char* id : {"a", "b", "c"})
        install_bg(id, "return ui.screen { ui.label { text = 'x' } }");
    std::atomic<int> queued{0};
    ASSERT_TRUE(worker.submit([&] {
        for (const char* id : {"a", "b", "c"})
            if (rt->launch(id, LaunchReason::Boot)) ++queued;
    }));
    ASSERT_TRUE(wait_until([&] { return queued.load() == 3; }, 2000));
    for (const char* id : {"a", "b", "c"})
        EXPECT_TRUE(wait_state(id, SceneState::Background)) << id;
    std::vector<std::string> ids = background_ids();
    std::sort(ids.begin(), ids.end());
    EXPECT_EQ(ids, (std::vector<std::string>{"a", "b", "c"}));
    EXPECT_EQ(rt->foreground(), std::nullopt);
}

// A boot launch and a user open loading at the same time: neither
// cancels the other.
TEST_F(SceneBackground, BootLaunchDuringUserOpenKeepsBoth) {
    start();
    install("u", "return ui.screen { ui.label { text = 'scene u' } }");
    install_bg("b", "return ui.screen { ui.label { text = 'scene b' } }");
    std::atomic<int> queued{0};
    ASSERT_TRUE(worker.submit([&] {
        if (rt->open("u")) ++queued;
        if (rt->launch("b", LaunchReason::Boot)) ++queued;
    }));
    ASSERT_TRUE(wait_until([&] { return queued.load() == 2; }, 2000));
    EXPECT_TRUE(wait_state("u", SceneState::Running));
    EXPECT_TRUE(wait_state("b", SceneState::Background));
    EXPECT_EQ(rt->foreground(), std::optional<std::string>("u"));
    EXPECT_TRUE(wait_label("scene u"));

    // The other way round too.
    ASSERT_TRUE(rt->exit("b"));
    ASSERT_TRUE(rt->exit("u"));
    queued = 0;
    ASSERT_TRUE(worker.submit([&] {
        if (rt->launch("b", LaunchReason::Boot)) ++queued;
        if (rt->open("u")) ++queued;
    }));
    ASSERT_TRUE(wait_until([&] { return queued.load() == 2; }, 2000));
    EXPECT_TRUE(wait_state("u", SceneState::Running));
    EXPECT_TRUE(wait_state("b", SceneState::Background));
    EXPECT_EQ(rt->foreground(), std::optional<std::string>("u"));
}

// A boot launch of a scene already in the background does nothing, and
// does not cancel a user open still loading.
TEST_F(SceneBackground, NoOpBootLaunchKeepsUserOpen) {
    start();
    install_bg("a", std::string(kHooks) + "return ui.screen { ui.label { text = 'scene a' } }");
    install("u", "return ui.screen { ui.label { text = 'scene u' } }");
    ASSERT_TRUE(rt->launch("a", LaunchReason::Boot));
    ASSERT_EQ(rt->status("a").state, SceneState::Background);
    std::atomic<int> queued{0};
    ASSERT_TRUE(worker.submit([&] {
        if (rt->open("u")) ++queued;
        if (rt->launch("a", LaunchReason::Boot)) ++queued;
        if (rt->launch("u", LaunchReason::Boot)) ++queued;
    }));
    ASSERT_TRUE(wait_until([&] { return queued.load() == 3; }, 2000));
    EXPECT_TRUE(wait_state("u", SceneState::Running));
    EXPECT_EQ(rt->foreground(), std::optional<std::string>("u"));
    EXPECT_EQ(rt->status("a").state, SceneState::Background);
    EXPECT_EQ(scene_log("a"), (std::vector<std::string>{"state:background"}));
    EXPECT_EQ(background_ids(), (std::vector<std::string>{"a"}));
}

// A boot launch that rides on a user open still loading (the same scene)
// is not lost when a later user open supersedes that one: the scene still
// comes up, in the background, as a boot launch.
TEST_F(SceneBackground, BootLaunchSurvivesTheUserOpenItRodeOn) {
    start();
    install_bg("x", std::string(kHooks) + R"(
print(wisp.scene.launch())
return ui.screen { ui.label { text = "scene x" } }
)");
    install("y", "return ui.screen { ui.label { text = 'scene y' } }");
    std::atomic<int> queued{0};
    ASSERT_TRUE(worker.submit([&] {
        if (rt->open("x")) ++queued;
        if (rt->launch("x", LaunchReason::Boot)) ++queued;
        if (rt->open("y")) ++queued;
    }));
    ASSERT_TRUE(wait_until([&] { return queued.load() == 3; }, 2000));
    EXPECT_TRUE(wait_state("y", SceneState::Running));
    EXPECT_TRUE(wait_state("x", SceneState::Background));
    EXPECT_EQ(rt->foreground(), std::optional<std::string>("y"));
    EXPECT_EQ(scene_log("x"), (std::vector<std::string>{"boot", "state:background"}));
    EXPECT_TRUE(seen_notices().empty());
}

// The same when the boot launch came first and a user open of the scene
// replaced it before a user open of another scene replaced that.
TEST_F(SceneBackground, BootLaunchSurvivesAUserOpenThatReplacedIt) {
    start();
    install_bg("x", "print(wisp.scene.launch()) return ui.screen { ui.label { text = 'x' } }");
    install("y", "return ui.screen { ui.label { text = 'scene y' } }");
    std::atomic<int> queued{0};
    ASSERT_TRUE(worker.submit([&] {
        if (rt->launch("x", LaunchReason::Boot)) ++queued;
        if (rt->open("x")) ++queued;
        if (rt->open("y")) ++queued;
    }));
    ASSERT_TRUE(wait_until([&] { return queued.load() == 3; }, 2000));
    EXPECT_TRUE(wait_state("y", SceneState::Running));
    EXPECT_TRUE(wait_state("x", SceneState::Background));
    EXPECT_EQ(scene_log("x"), (std::vector<std::string>{"boot"}));
}

// A boot launch is for scenes that declare "background": any other is
// refused, and the restore that asked for it hears of it.
TEST_F(SceneBackground, BootLaunchOfUndeclaredSceneIsRefused) {
    start();
    install("u", "print('ran') return ui.screen { ui.label { text = 'u' } }",
            {{"scene.json", wisp::test::manifest("u", "Undeclared")}});
    auto r = rt->launch("u", LaunchReason::Boot);
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, wisp::ErrorCode::InvalidArgument);
    EXPECT_EQ(rt->status("u").state, SceneState::Stopped);
    EXPECT_TRUE(scene_log("u").empty());
    EXPECT_TRUE(background_ids().empty());
    auto seen = seen_notices();
    ASSERT_EQ(seen.size(), 1u);
    EXPECT_EQ(seen[0].kind, SceneNoticeKind::RestoreFailed);
    EXPECT_EQ(seen[0].id, "u");
    EXPECT_EQ(seen[0].name, "Undeclared");
}

// The background list on disk (spec 3.6) and the restore at boot.
class SceneRestore : public SceneBackground {
protected:
    void SetUp() override {
        SceneBackground::SetUp();
        list_path = in + "/system/scenes/background.json";
        auto base = configure_deps;
        configure_deps = [this, base](wisp::scene::SceneRuntimeDeps& d) {
            base(d);
            d.background_list_path = list_path;
        };
    }

    void write_list(const std::string& json) {
        std::filesystem::create_directories(std::filesystem::path(list_path).parent_path());
        std::ofstream(list_path, std::ios::trunc) << json;
    }

    // The ids in the file; nullopt if there is none or it does not parse.
    std::optional<std::vector<std::string>> read_list() {
        std::ifstream f(list_path);
        if (!f) return std::nullopt;
        std::stringstream ss;
        ss << f.rdbuf();
        auto v = wisp::from_json(ss.str());
        if (!v) return std::nullopt;
        const Value* ids = v.value().find("ids");
        if (ids == nullptr || ids->type() != Value::Type::List) return std::nullopt;
        std::vector<std::string> out;
        for (const Value& id : ids->as_list()) out.push_back(id.as_string());
        return out;
    }

    // The list is written only once restore_background() has read it.
    void start_restored() {
        start();
        rt->restore_background();
        ASSERT_TRUE(worker.drain(1000));
    }

    bool wait_list(const std::vector<std::string>& want, uint32_t ms = 2000) {
        return wait_until([&] { return read_list() == want; }, ms);
    }

    std::string list_path;
};

TEST_F(SceneRestore, BackgroundListWrittenOnTransitions) {
    start_restored();
    install_bg("a", "return ui.screen { ui.label { text = 'a' } }");
    install_bg("b", "return ui.screen { ui.label { text = 'b' } }");
    install("c", "return ui.screen { ui.label { text = 'c' } }");
    ASSERT_TRUE(rt->open("a"));
    EXPECT_EQ(read_list(), std::nullopt);
    ASSERT_TRUE(rt->open("b"));
    EXPECT_TRUE(wait_list({"a"})) << ::testing::PrintToString(read_list());
    ASSERT_TRUE(rt->open("c"));
    EXPECT_TRUE(wait_list({"a", "b"})) << ::testing::PrintToString(read_list());
    ASSERT_TRUE(rt->open("a"));  // c closes; a comes forward
    EXPECT_TRUE(wait_list({"b"})) << ::testing::PrintToString(read_list());
    ASSERT_TRUE(rt->exit("b"));
    EXPECT_TRUE(wait_list({})) << ::testing::PrintToString(read_list());
}

TEST_F(SceneRestore, RestoresInBackgroundWithoutView) {
    write_list(R"({"ids":["a","b"]})");
    start();
    for (const char* id : {"a", "b"})
        install_bg(id, std::string(kHooks) + R"(
print(wisp.scene.state(), wisp.scene.launch())
return ui.screen { ui.label { text = "restored" } }
)");
    rt->restore_background();
    EXPECT_TRUE(wait_state("a", SceneState::Background));
    EXPECT_TRUE(wait_state("b", SceneState::Background));
    for (const char* id : {"a", "b"})
        EXPECT_EQ(scene_log(id), (std::vector<std::string>{"background\tboot", "state:background"}))
            << id;
    EXPECT_EQ(background_ids(), (std::vector<std::string>{"a", "b"}));
    EXPECT_EQ(rt->foreground(), std::nullopt);
    EXPECT_EQ(active_screen(), home);
    EXPECT_TRUE(label("restored").empty());
    EXPECT_TRUE(seen_notices().empty());
    // Nothing changed: the list stays as it was.
    worker.drain(1000);
    EXPECT_EQ(read_list(), (std::vector<std::string>{"a", "b"}));
}

TEST_F(SceneRestore, FailedRestoreSkippedNotifiedAndDropped) {
    write_list(R"({"ids":["a","gone","u","bad","b"]})");
    start();
    install_bg("a", "return ui.screen { ui.label { text = 'a' } }");
    install_bg("b", "return ui.screen { ui.label { text = 'b' } }");
    install("u", "return ui.screen { ui.label { text = 'u' } }",
            {{"scene.json", wisp::test::manifest("u", "Undeclared")}});
    install("bad", "error('boom')", {{"scene.json", bg_manifest("bad", "Bad")}});
    rt->restore_background();
    EXPECT_TRUE(wait_state("a", SceneState::Background));
    EXPECT_TRUE(wait_state("b", SceneState::Background));
    EXPECT_TRUE(wait_state("bad", SceneState::Error));
    EXPECT_TRUE(wait_list({"a", "b"})) << ::testing::PrintToString(read_list());
    EXPECT_EQ(background_ids(), (std::vector<std::string>{"a", "b"}));
    auto seen = seen_notices();
    std::vector<std::string> got;
    for (const auto& n : seen)
        got.push_back(
            std::string(n.kind == SceneNoticeKind::RestoreFailed ? "restore " : "other ") + n.id +
            "/" + n.name);
    std::sort(got.begin(), got.end());
    EXPECT_EQ(got, (std::vector<std::string>{"restore bad/Bad", "restore gone/gone",
                                             "restore u/Undeclared"}));
    EXPECT_TRUE(log_has("scene", "gone: not restored"));
}

TEST_F(SceneRestore, StopKeepsTheList) {
    start_restored();
    install_bg("a", "return ui.screen { ui.label { text = 'a' } }");
    install_bg("b", "return ui.screen { ui.label { text = 'b' } }");
    ASSERT_TRUE(rt->open("a"));
    ASSERT_TRUE(rt->open("b"));
    back_gesture();
    ASSERT_TRUE(wait_state("b", SceneState::Background));
    ASSERT_TRUE(wait_list({"a", "b"})) << ::testing::PrintToString(read_list());
    rt->stop();
    EXPECT_EQ(rt->status("a").state, SceneState::Stopped);
    EXPECT_EQ(rt->status("b").state, SceneState::Stopped);
    ASSERT_TRUE(worker.drain(2000));
    EXPECT_EQ(read_list(), (std::vector<std::string>{"a", "b"}));
}

// Until the restore has read the list, nothing overwrites it: a scene
// sent to the background before then does not wipe the last boot's list.
TEST_F(SceneRestore, NothingWrittenBeforeTheRestoreReadsTheList) {
    write_list(R"({"ids":["a"]})");
    start();
    install_bg("a", "return ui.screen { ui.label { text = 'a' } }");
    install_bg("b", "return ui.screen { ui.label { text = 'b' } }");
    ASSERT_TRUE(rt->open("b"));
    back_gesture();
    ASSERT_TRUE(wait_state("b", SceneState::Background));
    ASSERT_TRUE(worker.drain(1000));
    EXPECT_EQ(read_list(), (std::vector<std::string>{"a"}));
    rt->restore_background();
    EXPECT_TRUE(wait_state("a", SceneState::Background));
    // a never was in front, so it is the least recently in front.
    EXPECT_TRUE(wait_list({"a", "b"})) << ::testing::PrintToString(read_list());
}

// A bad list restores nothing and breaks nothing.
TEST_F(SceneRestore, UnreadableListRestoresNothing) {
    write_list("not json");
    start();
    install_bg("a", "return ui.screen { ui.label { text = 'a' } }");
    rt->restore_background();
    ASSERT_TRUE(worker.drain(1000));
    ASSERT_TRUE(rt->open("a"));
    EXPECT_EQ(rt->status("a").state, SceneState::Running);
    EXPECT_TRUE(background_ids().empty());
    EXPECT_TRUE(seen_notices().empty());
}

// scene-runtime-core spec 6.3: installing over a running scene is a reload.
// For one in the background that means starting it there again, on the
// new package, as it was launched before and where it was in the order
// of least recently in front (the list keeps it there).
TEST_F(SceneRestore, ReinstallRelaunchesBackgroundScene) {
    start_restored();
    install_bg("a", "print('v1') return ui.screen { ui.label { text = 'a' } }");
    install_bg("b", "return ui.screen { ui.label { text = 'b' } }");
    install_bg("c", "return ui.screen { ui.label { text = 'c' } }");
    ASSERT_TRUE(rt->open("a"));
    ASSERT_TRUE(rt->open("c"));
    ASSERT_TRUE(rt->open("b"));
    ASSERT_EQ(rt->status("a").state, SceneState::Background);
    ASSERT_TRUE(wait_list({"a", "c"}));

    wisp::core::ZipWriter w;
    auto add = [&](const std::string& path, const std::string& body) {
        w.add(path,
              std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(body.data()), body.size()));
    };
    add("scene.json", bg_manifest("a"));
    add("main.lua",
        "print('v2', wisp.scene.state(), wisp.scene.launch()) "
        "return ui.screen { ui.label { text = 'a' } }");
    wisp::os::LargeBuffer zip = w.finish();
    auto r =
        wisp::core::install_scene(*repo, *rt, std::span<const uint8_t>(zip.data(), zip.size()));
    ASSERT_TRUE(r) << r.error().message;
    EXPECT_TRUE(wait_state("a", SceneState::Background));
    EXPECT_TRUE(wait_until([&] { return count_log("a", "v2\tbackground\tuser") == 1; }, 2000))
        << ::testing::PrintToString(scene_log("a"));
    EXPECT_EQ(rt->foreground(), std::optional<std::string>("b"));
    EXPECT_EQ(background_ids(), (std::vector<std::string>{"a", "c"}));
    EXPECT_TRUE(wait_list({"a", "c"})) << ::testing::PrintToString(read_list());
    EXPECT_TRUE(seen_notices().empty());
}

}  // namespace
