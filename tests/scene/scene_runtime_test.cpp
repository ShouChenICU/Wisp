// SceneRuntime: scenes run on their own task, with the lifecycle, the
// error panel and the back gesture (scene-runtime-core spec 3.1-3.6).

#include "wisp/scene/scene_runtime.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <functional>
#include <future>
#include <iterator>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "lvgl.h"
#include "scene_message.hpp"
#include "support/fake_devices.hpp"
#include "support/lvgl_probe.hpp"
#include "support/scene_runtime_fixture.hpp"
#include "support/temp_dir.hpp"
#include "support/wait.hpp"
#include "wisp/core/device_registry.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/core/log.hpp"
#include "wisp/core/scene_repository.hpp"
#include "wisp/core/storage_manager.hpp"
#include "wisp/core/storage_worker.hpp"
#include "wisp/core/zip.hpp"
#include "wisp/hal/display_geometry.hpp"
#include "wisp/scene/patch_batch.hpp"
#include "wisp/ui/strings.hpp"
#include "wisp/ui/system_layer.hpp"
#include "wisp/ui/ui_runtime.hpp"

namespace {

namespace fs = std::filesystem;

using wisp::ErrorCode;
using wisp::core::SceneState;
using wisp::core::SourceFile;
using wisp::scene::SceneMessage;
using wisp::scene::SceneRuntime;
using wisp::test::run_on_ui_blocking;
using wisp::test::wait_until;

using wisp::test::Files;
using wisp::test::manifest;
using wisp::test::StateEvent;

// 6x4 PNG (the one scene_view_media_test decodes).
const uint8_t kIconPng[] = {
    0x89, 0x50, 0x4e, 0x47, 0x0d, 0x0a, 0x1a, 0x0a, 0x00, 0x00, 0x00, 0x0d, 0x49, 0x48,
    0x44, 0x52, 0x00, 0x00, 0x00, 0x06, 0x00, 0x00, 0x00, 0x04, 0x08, 0x06, 0x00, 0x00,
    0x00, 0xad, 0x04, 0x4e, 0x43, 0x00, 0x00, 0x00, 0x1a, 0x49, 0x44, 0x41, 0x54, 0x78,
    0xda, 0x63, 0x64, 0xf8, 0xcf, 0xf0, 0x9f, 0x01, 0x06, 0x18, 0xff, 0x37, 0xc2, 0x98,
    0x4c, 0x0c, 0x38, 0x00, 0xe9, 0x12, 0x00, 0x42, 0x96, 0x03, 0x87, 0xf7, 0xaf, 0x9f,
    0xc6, 0x00, 0x00, 0x00, 0x00, 0x49, 0x45, 0x4e, 0x44, 0xae, 0x42, 0x60, 0x82,
};

const char* kCounter = R"(local count = state(0)

return ui.screen {
  ui.column { pad = 16, gap = 12, align = "center",
    ui.label { text = bind(count, function(v) return "已点击 " .. v .. " 次" end), font = "xl" },
    ui.button { ref = "plus", text = "+1", on_click = function() count:set(count:get() + 1) end },
  },
}
)";

class SceneRuntimeTest : public wisp::test::SceneRuntimeFixture {};

TEST_F(SceneRuntimeTest, OpensCounterAndAppliesClick) {
    start();
    install("counter", kCounter);
    auto opened = rt->open("counter");
    ASSERT_TRUE(opened) << opened.error().message;
    EXPECT_EQ(rt->status("counter").state, SceneState::Running);
    EXPECT_EQ(rt->foreground(), std::optional<std::string>("counter"));
    ASSERT_TRUE(wait_label("已点击"));
    EXPECT_EQ(label("已点击"), "已点击 0 次");

    ASSERT_TRUE(rt->click_ref("plus"));
    EXPECT_TRUE(wait_until([&] { return label("已点击") == "已点击 1 次"; }, 1000))
        << label("已点击");
    EXPECT_EQ(rt->click_ref("nope").error().code, ErrorCode::NotFound);
    EXPECT_EQ(events(), (std::vector<StateEvent>{{"counter", "running"}}));

    EXPECT_EQ(rt->open("missing").error().code, ErrorCode::NotFound);
    EXPECT_EQ(rt->foreground(), std::optional<std::string>("counter"));
}

TEST_F(SceneRuntimeTest, LifecycleHookOrder) {
    start();
    install("life", R"(print("open")
scene.on_show = function() print("show") end
scene.on_hide = function() print("hide") end
scene.on_close = function() print("close") end
return ui.screen { ui.label { text = "x" } }
)");
    ASSERT_TRUE(rt->open("life"));
    EXPECT_EQ(scene_log("life"), (std::vector<std::string>{"open", "show"}));
    rt->set_visible(false);
    ASSERT_TRUE(wait_until([&] { return scene_log("life").size() == 3; }, 1000));
    rt->set_visible(true);
    ASSERT_TRUE(wait_until([&] { return scene_log("life").size() == 4; }, 1000));
    ASSERT_TRUE(rt->close("life"));
    EXPECT_EQ(scene_log("life"),
              (std::vector<std::string>{"open", "show", "hide", "show", "hide", "close"}));
    EXPECT_EQ(rt->status("life").state, SceneState::Stopped);
    EXPECT_EQ(rt->foreground(), std::nullopt);

    // Opened while the screen is off: no on_show until it is on again, and
    // closing then has no on_hide to run.
    rt->set_visible(false);
    ASSERT_TRUE(rt->open("life"));
    ASSERT_TRUE(rt->close("life"));
    EXPECT_EQ(scene_log("life"), (std::vector<std::string>{"open", "show", "hide", "show", "hide",
                                                           "close", "open", "close"}));
}

TEST_F(SceneRuntimeTest, HomeScreenHiddenNotRebuilt) {
    start();
    install("counter", kCounter);
    ASSERT_TRUE(rt->open("counter"));
    ASSERT_TRUE(wait_label("已点击"));
    EXPECT_NE(active_screen(), home);
    ASSERT_TRUE(rt->close("counter"));
    EXPECT_TRUE(wait_until([&] { return active_screen() == home; }, 1000));
    bool valid = false;
    on_ui([&] { valid = lv_obj_is_valid(home); });
    EXPECT_TRUE(valid);
    EXPECT_EQ(events(), (std::vector<StateEvent>{{"counter", "running"}, {"counter", "stopped"}}));
}

TEST_F(SceneRuntimeTest, ErrorShowsPanelAndDestroysVm) {
    start();
    install("bad", R"(local clicks = 0
return ui.screen {
  ui.button { ref = "go", text = "go", on_click = function()
    clicks = clicks + 1
    print("click " .. clicks)
    error("boom")
  end },
}
)");
    ASSERT_TRUE(rt->open("bad"));
    ASSERT_TRUE(wait_label("go"));
    ASSERT_TRUE(rt->click_ref("go"));
    ASSERT_TRUE(wait_until([&] { return rt->status("bad").state == SceneState::Error; }, 1000));
    EXPECT_EQ(rt->status("bad").error, "main.lua:6: boom");
    EXPECT_TRUE(wait_until([&] { return error_panel_has("main.lua:6"); }, 1000));
    EXPECT_TRUE(error_panel_has("boom"));
    EXPECT_TRUE(log_has("scene.bad", "main.lua:6: boom"));
    EXPECT_EQ(events(), (std::vector<StateEvent>{{"bad", "running"}, {"bad", "error"}}));
    // The VM is gone: nothing handles the button any more.
    EXPECT_FALSE(rt->click_ref("go"));
    wisp::os::sleep_ms(50);
    EXPECT_EQ(scene_log("bad"), (std::vector<std::string>{"click 1", "main.lua:6: boom"}));

    ASSERT_TRUE(rt->reload("bad"));
    EXPECT_EQ(rt->status("bad").state, SceneState::Running);
    EXPECT_TRUE(rt->status("bad").error.empty());
    bool panel = true;
    on_ui([&] { panel = layer->error_visible(); });
    EXPECT_FALSE(panel);
    ASSERT_TRUE(wait_label("go"));
    ASSERT_TRUE(rt->click_ref("go"));
    EXPECT_TRUE(wait_until([&] { return rt->status("bad").state == SceneState::Error; }, 1000));
    // A fresh VM: its counter started again.
    auto log = scene_log("bad");
    EXPECT_EQ(std::count(log.begin(), log.end(), "click 1"), 2);
    EXPECT_EQ(std::count(log.begin(), log.end(), "click 2"), 0);
}

TEST_F(SceneRuntimeTest, ErrorPanelButtons) {
    start();
    install("bad", R"(return ui.screen {
  ui.button { ref = "go", text = "go", on_click = function() error("boom") end },
}
)");
    ASSERT_TRUE(rt->open("bad"));
    ASSERT_TRUE(wait_label("go"));
    ASSERT_TRUE(rt->click_ref("go"));
    ASSERT_TRUE(wait_until([&] { return error_panel_has("boom"); }, 1000));
    // "返回": back to the home screen, still in the error state.
    on_ui([&] {
        lv_obj_t* back = wisp::test::find_button_with_label(
            lv_layer_top(), wisp::ui::tr(wisp::ui::StrId::SceneBack));
        ASSERT_NE(back, nullptr);
        lv_obj_send_event(back, LV_EVENT_CLICKED, nullptr);
    });
    EXPECT_TRUE(wait_until([&] { return active_screen() == home; }, 1000));
    EXPECT_TRUE(wait_until([&] { return !rt->foreground(); }, 1000));
    EXPECT_EQ(rt->status("bad").state, SceneState::Error);

    // "重载" opens it again.
    ASSERT_TRUE(rt->open("bad"));
    ASSERT_TRUE(wait_label("go"));
    ASSERT_TRUE(rt->click_ref("go"));
    ASSERT_TRUE(wait_until([&] { return error_panel_has("boom"); }, 1000));
    on_ui([&] {
        lv_obj_t* reload = wisp::test::find_button_with_label(
            lv_layer_top(), wisp::ui::tr(wisp::ui::StrId::SceneReload));
        ASSERT_NE(reload, nullptr);
        lv_obj_send_event(reload, LV_EVENT_CLICKED, nullptr);
    });
    EXPECT_TRUE(wait_until([&] { return rt->status("bad").state == SceneState::Running; }, 1000));
    EXPECT_TRUE(wait_label("go"));
}

TEST_F(SceneRuntimeTest, TimeoutFaultsScene) {
    start();
    install("spin", R"(return ui.screen {
  ui.button { ref = "go", text = "go", on_click = function() while true do end end },
}
)");
    ASSERT_TRUE(rt->open("spin"));
    ASSERT_TRUE(wait_label("go"));
    ASSERT_TRUE(rt->click_ref("go"));
    ASSERT_TRUE(wait_until([&] { return rt->status("spin").state == SceneState::Error; }, 2000));
    EXPECT_NE(rt->status("spin").error.find("execution exceeded 200 ms"), std::string::npos)
        << rt->status("spin").error;
    // Logged after the handler returns, which is after the state flips.
    EXPECT_TRUE(wait_until([&] { return log_has("scene.spin", "slow message event"); }, 1000));
}

TEST_F(SceneRuntimeTest, BindErrorDuringFlushShowsPanelWithoutPartialPatches) {
    start();
    install("bind", R"(local count = state(0)
return ui.screen {
  ui.label { text = count },
  ui.label { text = bind(count, function(v)
    if v == 1 then error("bad value") end
    return "b" .. v
  end) },
  ui.button { ref = "go", text = "go", on_click = function() count:set(1) end },
}
)");
    ASSERT_TRUE(rt->open("bind"));
    ASSERT_TRUE(wait_label("b0"));
    ASSERT_TRUE(rt->click_ref("go"));
    ASSERT_TRUE(wait_until([&] { return rt->status("bind").state == SceneState::Error; }, 1000));
    EXPECT_EQ(rt->status("bind").error, "main.lua:5: bad value");
    EXPECT_TRUE(wait_until([&] { return error_panel_has("main.lua:5"); }, 1000));
    EXPECT_EQ(wisp::scene::pending_patches(*rt), 0u);
    wisp::os::sleep_ms(50);
    auto texts = labels();
    EXPECT_NE(std::find(texts.begin(), texts.end(), "0"), texts.end());
    EXPECT_NE(std::find(texts.begin(), texts.end(), "b0"), texts.end());
    EXPECT_EQ(std::find(texts.begin(), texts.end(), "1"), texts.end());
}

TEST_F(SceneRuntimeTest, OnCloseErrorStillCloses) {
    start();
    // A timeout faults the VM for good, so the hook that runs first errs
    // and the second one times out.
    install("rude", R"(scene.on_hide = function() error("no") end
scene.on_close = function() while true do end end
return ui.screen { ui.label { text = "rude" } }
)");
    ASSERT_TRUE(rt->open("rude"));
    ASSERT_TRUE(wait_label("rude"));
    ASSERT_TRUE(rt->close("rude"));
    EXPECT_EQ(rt->foreground(), std::nullopt);
    EXPECT_EQ(rt->status("rude").state, SceneState::Stopped);
    EXPECT_TRUE(wait_until([&] { return active_screen() == home; }, 1000));
    EXPECT_TRUE(log_has("scene.rude", "execution exceeded 200 ms"));
    EXPECT_TRUE(log_has("scene.rude", "on_hide: main.lua:1: no"));
}

TEST_F(SceneRuntimeTest, BackGestureClosesToHome) {
    start();
    install("counter", kCounter);
    ASSERT_TRUE(rt->open("counter"));
    ASSERT_TRUE(wait_label("已点击"));
    // The gesture strip is laid out at the next refresh; a real finger
    // never comes that fast.
    on_ui([&] { lv_obj_update_layout(lv_layer_top()); });
    feed({{4, 240, 0}});
    for (int16_t x : {4, 50, 100, 150, 200}) feed({{x, 240, 0}});
    feed({});
    EXPECT_TRUE(wait_until([&] { return !rt->foreground(); }, 1000));
    EXPECT_TRUE(wait_until([&] { return active_screen() == home; }, 1000));
    EXPECT_EQ(rt->status("counter").state, SceneState::Stopped);
}

// Every state change and every close is in the log, so a log reader (the
// console, sim_smoke) can follow a scene without the event bus; closing a
// scene that failed while it built its tree publishes nothing, as it stays
// in the error state, but is still logged once the home screen is back.
TEST_F(SceneRuntimeTest, LogsStateChangesAndCloses) {
    start();
    install("counter", kCounter);
    ASSERT_TRUE(rt->open("counter"));
    EXPECT_TRUE(log_has("scene", "counter: running"));
    ASSERT_TRUE(rt->close("counter"));
    EXPECT_TRUE(log_has("scene", "counter: stopped"));
    EXPECT_TRUE(wait_until([&] { return log_has("scene", "counter: closed"); }, 1000));

    install("broken", "local missing = nil\nreturn ui.screen { ui.label { text = missing.x } }\n");
    EXPECT_FALSE(rt->open("broken"));
    EXPECT_TRUE(log_has("scene", "broken: error"));
    ASSERT_TRUE(wait_until([&] { return error_panel_has("main.lua:2"); }, 1000));
    on_ui([&] { lv_obj_update_layout(lv_layer_top()); });
    feed({{4, 240, 0}});
    for (int16_t x : {4, 50, 100, 150, 200}) feed({{x, 240, 0}});
    feed({});
    EXPECT_TRUE(wait_until([&] { return active_screen() == home; }, 1000));
    EXPECT_TRUE(wait_until([&] { return log_has("scene", "broken: closed"); }, 1000));
    EXPECT_EQ(rt->status("broken").state, SceneState::Error);
}

TEST_F(SceneRuntimeTest, OpeningAnotherClosesFirst) {
    start();
    install("a", R"(scene.on_close = function() print("a closed") end
return ui.screen { ui.label { text = "scene a" } })");
    install("b", "return ui.screen { ui.label { text = 'scene b' } }");
    ASSERT_TRUE(rt->open("a"));
    ASSERT_TRUE(wait_label("scene a"));
    ASSERT_TRUE(rt->open("b"));
    EXPECT_EQ(rt->foreground(), std::optional<std::string>("b"));
    EXPECT_EQ(scene_log("a"), (std::vector<std::string>{"a closed"}));
    EXPECT_EQ(rt->status("a").state, SceneState::Stopped);
    EXPECT_EQ(rt->status("b").state, SceneState::Running);
    ASSERT_TRUE(wait_label("scene b"));
    EXPECT_TRUE(label("scene a").empty());
    ASSERT_TRUE(rt->close("b"));
    EXPECT_TRUE(wait_until([&] { return active_screen() == home; }, 1000));
}

TEST_F(SceneRuntimeTest, StaleMessagesAreDropped) {
    start();
    install("counter", kCounter);
    ASSERT_TRUE(rt->open("counter"));
    uint32_t first = wisp::scene::foreground_instance(*rt);
    ASSERT_NE(first, 0u);
    ASSERT_TRUE(rt->reload("counter"));
    EXPECT_NE(wisp::scene::foreground_instance(*rt), first);
    for (int i = 0; i < 2; ++i) {
        auto msg = wisp::os::make_large<SceneMessage>();
        msg->scene_id = "counter";
        msg->instance = first;
        if (i == 0)
            msg->body = wisp::scene::msg::HttpDone{};
        else
            msg->body = wisp::scene::msg::StoreDone{};
        ASSERT_TRUE(wisp::scene::post_scene_message(*rt, std::move(msg)));
    }
    EXPECT_TRUE(wait_until([&] { return rt->stale_dropped() == 2; }, 1000));
    EXPECT_EQ(rt->status("counter").state, SceneState::Running);
    ASSERT_TRUE(wait_label("已点击"));
    ASSERT_TRUE(rt->click_ref("plus"));
    EXPECT_TRUE(wait_until([&] { return label("已点击") == "已点击 1 次"; }, 1000));
}

TEST_F(SceneRuntimeTest, CheckSourcesRunsOnSceneTask) {
    start();
    const std::string bad = "local x = 1\nlocal = 2\n";
    const std::string good = "local x = 1\nreturn x\n";
    const std::string lib = "return {}\n";
    std::vector<SourceFile> files = {{"main.lua", bad}, {"lib/util.lua", lib}};
    auto r = rt->check_sources(files);
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(r.error().message.rfind("main.lua:2:", 0), 0u) << r.error().message;
    files[0].text = good;
    EXPECT_TRUE(rt->check_sources(files));
    // The scene task's stack does the compiling: without it, nothing.
    rt->stop();
    EXPECT_FALSE(rt->check_sources(files));
}

// Waiting up to 10 s for the scene task would freeze the UI thread, and
// on the storage worker it would hold up every file job (the scene task
// may be waiting on one), so both get Busy at once.
TEST_F(SceneRuntimeTest, CheckSourcesRefusesTheUiThreadAndTheWorker) {
    start();
    const std::string good = "return 1\n";
    std::vector<SourceFile> files = {{"main.lua", good}};
    std::atomic<int> on_worker{-1};
    ASSERT_TRUE(worker.submit([&] {
        auto r = rt->check_sources(files);
        on_worker = r ? 0 : static_cast<int>(r.error().code);
    }));
    ASSERT_TRUE(worker.drain(5000));
    EXPECT_EQ(on_worker.load(), static_cast<int>(ErrorCode::Busy));
    int from_ui = -1;
    on_ui([&] {
        auto r = rt->check_sources(files);
        from_ui = r ? 0 : static_cast<int>(r.error().code);
    });
    EXPECT_EQ(from_ui, static_cast<int>(ErrorCode::Busy));
    EXPECT_TRUE(rt->check_sources(files));
}

// close() returns once the scene's last writes are on disk, not merely
// queued: an install moves data/ right after it, and a flush landing
// during that move would be lost (spec 6.3).
TEST_F(SceneRuntimeTest, CloseWaitsForTheFinalStoreFlush) {
    start();
    install("keep", R"(
scene.on_close = function() wisp.store.set("k", 42) end
return ui.screen{ui.label{text = "keep"}}
)");
    auto opened = rt->open("keep");
    ASSERT_TRUE(opened) << opened.error().message;
    // Hold the storage worker so the final flush stays queued for a while.
    ASSERT_TRUE(worker.submit([] { std::this_thread::sleep_for(std::chrono::milliseconds(300)); }));
    ASSERT_TRUE(rt->close("keep"));
    std::ifstream kv(repo->data_dir("keep") + "/kv.log");
    const std::string text((std::istreambuf_iterator<char>(kv)), std::istreambuf_iterator<char>());
    EXPECT_NE(text.find("42"), std::string::npos) << text;
}

TEST(SceneRuntimeConfig, TaskConfigIsPsramStack) {
    wisp::os::TaskConfig t = SceneRuntime::task_config({});
    EXPECT_STREQ(t.name, "wisp.scene");
    EXPECT_EQ(t.stack_bytes, 128u * 1024u);
    EXPECT_FALSE(t.flash_access);
    EXPECT_EQ(t.priority, 3);
    EXPECT_EQ(t.core, -1);
}

TEST_F(SceneRuntimeTest, ImageAssetsReachView) {
    start("rect");
    install("pic", "return ui.screen { ui.image { src = 'assets/icon.png' } }",
            {{"assets/icon.png",
              std::string(reinterpret_cast<const char*>(kIconPng), sizeof(kIconPng))}});
    ASSERT_TRUE(rt->open("pic"));
    uint32_t w = 0, h = 0;
    EXPECT_TRUE(wait_until(
        [&] {
            on_ui([&] {
                lv_obj_t* img = wisp::test::find_obj_of_class(lv_screen_active(), &lv_image_class);
                if (img == nullptr) return;
                const void* src = lv_image_get_src(img);
                if (src == nullptr || lv_image_src_get_type(src) != LV_IMAGE_SRC_VARIABLE) return;
                const auto* dsc = static_cast<const lv_image_dsc_t*>(src);
                w = dsc->header.w;
                h = dsc->header.h;
            });
            return w != 0;
        },
        1000));
    EXPECT_EQ(w, 6u);
    EXPECT_EQ(h, 4u);
}

TEST_F(SceneRuntimeTest, ChangeEventSetsValueThenCallsOnChange) {
    start();
    install("form", R"(return ui.screen {
  ui.input { ref = "name", on_change = function(v)
    print("change " .. v .. " " .. ui.ref("name"):value())
  end },
  ui.switch { ref = "on" },
  ui.button { ref = "show", text = "show", on_click = function()
    print("on " .. tostring(ui.ref("on"):value()))
  end },
}
)");
    ASSERT_TRUE(rt->open("form"));
    ASSERT_TRUE(wait_label("show"));
    on_ui([&] {
        lv_obj_t* ta = wisp::test::find_obj_of_class(lv_screen_active(), &lv_textarea_class);
        ASSERT_NE(ta, nullptr);
        lv_textarea_set_text(ta, "Ada");
        // Focusing reports nothing the scene handles.
        lv_obj_send_event(ta, LV_EVENT_FOCUSED, nullptr);
        lv_obj_t* sw = wisp::test::find_obj_of_class(lv_screen_active(), &lv_switch_class);
        ASSERT_NE(sw, nullptr);
        lv_obj_add_state(sw, LV_STATE_CHECKED);
        lv_obj_send_event(sw, LV_EVENT_VALUE_CHANGED, nullptr);
    });
    ASSERT_TRUE(rt->click_ref("show"));
    EXPECT_TRUE(wait_until([&] { return scene_log("form").size() == 2; }, 1000));
    EXPECT_EQ(scene_log("form"), (std::vector<std::string>{"change Ada Ada", "on true"}));
    EXPECT_EQ(rt->stale_dropped(), 0u);
    EXPECT_EQ(rt->status("form").state, SceneState::Running);
}

// Bound values that run past the patch queue's backlog are sent again
// (UiBatch::refresh): labels bound to `a` lose their update in the
// collapse and must still end up showing it.
TEST_F(SceneRuntimeTest, RefreshRequestResendsBoundValues) {
    start();
    install("many", R"(local a, b = state(0), state(0)
local kids = {}
for i = 1, 700 do kids[#kids + 1] = ui.label { text = a } end
for i = 1, 700 do kids[#kids + 1] = ui.label { text = b } end
kids[#kids + 1] = ui.button { ref = "a", text = "inc a", on_click = function()
  a:set(a:get() + 1) print("a") end }
kids[#kids + 1] = ui.button { ref = "b", text = "inc b", on_click = function()
  b:set(b:get() + 1) print("b") end }
return ui.screen { ui.scroll(kids) }
)");
    ASSERT_TRUE(rt->open("many"));
    ASSERT_TRUE(wait_label("inc b", 3000));
    ASSERT_TRUE(wait_until([&] { return wisp::scene::pending_patches(*rt) == 0; }, 3000));
    ui->set_paused(true);
    ASSERT_TRUE(rt->click_ref("a"));
    ASSERT_TRUE(wait_until([&] { return scene_log("many").size() == 1; }, 1000));
    ASSERT_TRUE(rt->click_ref("b"));
    ASSERT_TRUE(wait_until([&] { return scene_log("many").size() == 2; }, 1000));
    ui->set_paused(false);
    ASSERT_TRUE(wait_until([&] { return wisp::scene::pending_patches(*rt) == 0; }, 5000));
    auto texts = labels();
    EXPECT_EQ(std::count(texts.begin(), texts.end(), "1"), 1400);
    EXPECT_EQ(std::count(texts.begin(), texts.end(), "0"), 0);
}

// string.gsub recursion is the deepest C stack use per C call level
// (vm.hpp): it must end in a Lua error, not overflow the scene task.
TEST_F(SceneRuntimeTest, GsubRecursionEndsInLuaError) {
    start();
    install("deep", R"(return ui.screen {
  ui.button { ref = "go", text = "go", on_click = function()
    local function g(s) return (string.gsub(s, ".", g)) end
    g("ab")
  end },
}
)");
    ASSERT_TRUE(rt->open("deep"));
    ASSERT_TRUE(wait_label("go"));
    ASSERT_TRUE(rt->click_ref("go"));
    ASSERT_TRUE(wait_until([&] { return rt->status("deep").state == SceneState::Error; }, 2000));
    EXPECT_NE(rt->status("deep").error.find("C stack overflow"), std::string::npos)
        << rt->status("deep").error;
}

TEST_F(SceneRuntimeTest, StopClosesTheForegroundScene) {
    start();
    install("life", R"(scene.on_close = function() print("close") end
return ui.screen { ui.label { text = "life" } })");
    ASSERT_TRUE(rt->open("life"));
    ASSERT_TRUE(wait_label("life"));
    rt->stop();
    EXPECT_EQ(scene_log("life"), (std::vector<std::string>{"close"}));
    EXPECT_EQ(active_screen(), home);
    EXPECT_EQ(rt->open("life").error().code, ErrorCode::Busy);
}

TEST_F(SceneRuntimeTest, OnInstalledReloadsOrPublishesStopped) {
    start();
    install("a", "print('v1') return ui.screen { ui.label { text = 'v1' } }");
    rt->on_installed("a");
    EXPECT_TRUE(wait_until([&] { return events().size() == 1; }, 1000));
    EXPECT_EQ(events(), (std::vector<StateEvent>{{"a", "stopped"}}));
    ASSERT_TRUE(rt->open("a"));
    install("a", "print('v2') return ui.screen { ui.label { text = 'v2' } }");
    rt->on_installed("a");
    EXPECT_TRUE(wait_label("v2"));
    EXPECT_EQ(scene_log("a"), (std::vector<std::string>{"v1", "v2"}));
    EXPECT_EQ(rt->status("a").state, SceneState::Running);
}

// A package of `id` as an upload would bring it.
wisp::os::LargeBuffer package_zip(const std::string& id, const std::string& main_lua) {
    wisp::core::ZipWriter w;
    auto add = [&](const std::string& path, const std::string& body) {
        w.add(path,
              std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(body.data()), body.size()));
    };
    add("scene.json", manifest(id));
    add("main.lua", main_lua);
    return w.finish();
}

// Holds the storage worker until release() (or destruction).
class WorkerGate {
public:
    explicit WorkerGate(wisp::core::StorageWorker& worker) {
        EXPECT_TRUE(worker.submit([f = released_.get_future().share()] { f.wait(); }));
    }
    ~WorkerGate() { release(); }
    void release() {
        if (!done_) released_.set_value();
        done_ = true;
    }

private:
    std::promise<void> released_;
    bool done_ = false;
};

// Spec 6.3 and install_scene(): while a package is being replaced, an open
// of the same scene is refused at once, and one already on its way (its
// load queued before the install began) is dropped when it arrives, so no
// scene runs on a package that is being swapped.
TEST_F(SceneRuntimeTest, OpenIsBusyWhileItsPackageIsBeingInstalled) {
    start();
    install("todo", "return ui.screen { ui.label { text = 'v0' } }");

    std::promise<void> checking;
    std::promise<void> finish_check;
    std::shared_future<void> finish = finish_check.get_future().share();
    // An open queued before the install, held in the worker until the
    // install has begun.
    auto gate = std::make_unique<WorkerGate>(worker);
    auto early = std::async(std::launch::async, [&] { return rt->open("todo"); });
    std::this_thread::sleep_for(std::chrono::milliseconds(100));

    wisp::os::LargeBuffer zip =
        package_zip("todo", "return ui.screen { ui.label { text = 'v1' } }");
    auto installing = std::async(std::launch::async, [&] {
        return repo->install(zip.span(), [&](std::span<const SourceFile>) {
            checking.set_value();
            finish.wait();
            return wisp::Result<void>::ok();
        });
    });
    checking.get_future().wait();
    EXPECT_TRUE(repo->installing("todo"));

    auto refused = rt->open("todo");
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, ErrorCode::Busy);

    gate->release();
    auto late = early.get();
    ASSERT_FALSE(late);
    EXPECT_EQ(late.error().code, ErrorCode::Busy);
    EXPECT_FALSE(rt->foreground().has_value());

    finish_check.set_value();
    ASSERT_TRUE(installing.get());
    EXPECT_FALSE(repo->installing("todo"));
    ASSERT_TRUE(rt->open("todo"));
    EXPECT_TRUE(wait_label("v1"));
}

// A faulted scene's store was flushed and closed when it faulted, but that
// flush may still be queued: closing it (from its error panel, or before an
// install moves its data) waits for it like any final flush.
TEST_F(SceneRuntimeTest, CloseOfAFaultedSceneWaitsForItsLastFlush) {
    start();
    install("bad", R"(return ui.screen {
  ui.button { ref = "go", text = "go", on_click = function()
    wisp.store.set("k", 7)
    error("boom")
  end },
}
)");
    ASSERT_TRUE(rt->open("bad"));
    ASSERT_TRUE(wait_label("go"));
    WorkerGate gate(worker);
    ASSERT_TRUE(rt->click_ref("go"));
    ASSERT_TRUE(wait_until([&] { return rt->status("bad").state == SceneState::Error; }, 2000));

    auto closing = std::async(std::launch::async, [&] { return rt->close("bad"); });
    EXPECT_EQ(closing.wait_for(std::chrono::milliseconds(200)), std::future_status::timeout);
    gate.release();
    ASSERT_TRUE(closing.get());
    std::ifstream kv(repo->data_dir("bad") + "/kv.log");
    const std::string text((std::istreambuf_iterator<char>(kv)), std::istreambuf_iterator<char>());
    EXPECT_NE(text.find('7'), std::string::npos) << text;

    // A scene that is not in the foreground: the same wait, for whatever
    // file work is queued.
    WorkerGate again(worker);
    auto other = std::async(std::launch::async, [&] { return rt->close("bad"); });
    EXPECT_EQ(other.wait_for(std::chrono::milliseconds(200)), std::future_status::timeout);
    again.release();
    EXPECT_TRUE(other.get());
}

// close_async(): never waits, and its callback runs once the final flush is
// on disk -- what deleting a scene from its confirmation dialog (on the UI
// thread) builds on.
TEST_F(SceneRuntimeTest, CloseAsyncCallsBackAfterTheFinalFlush) {
    start();
    install("keep", R"(
scene.on_close = function() wisp.store.set("k", 42) end
return ui.screen{ui.label{text = "keep"}}
)");
    ASSERT_TRUE(rt->open("keep"));
    WorkerGate gate(worker);
    std::promise<std::string> flushed;
    on_ui([&] {
        rt->close_async("keep", [&](wisp::Result<void> r) {
            std::ifstream kv(repo->data_dir("keep") + "/kv.log");
            std::string text((std::istreambuf_iterator<char>(kv)),
                             std::istreambuf_iterator<char>());
            flushed.set_value(r ? text : "error: " + r.error().message);
        });
    });
    auto result = flushed.get_future();
    EXPECT_EQ(result.wait_for(std::chrono::milliseconds(200)), std::future_status::timeout);
    gate.release();
    ASSERT_EQ(result.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    EXPECT_NE(result.get().find("42"), std::string::npos);
    EXPECT_TRUE(wait_until([&] { return !rt->foreground().has_value(); }, 1000));

    // Nothing to close: still called back.
    std::promise<bool> idle;
    rt->close_async("nothing", [&](wisp::Result<void> r) { idle.set_value(r.is_ok()); });
    auto idle_result = idle.get_future();
    ASSERT_EQ(idle_result.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    EXPECT_TRUE(idle_result.get());
}

// Holds the UI thread inside a job, with its post() queue full, until
// release(): nothing the scene task hands the UI meanwhile can run.
class UiStall {
public:
    explicit UiStall(wisp::ui::UiRuntime& ui) {
        auto started = std::make_shared<std::promise<void>>();
        EXPECT_TRUE(ui.post([started, f = released_.get_future().share()] {
            started->set_value();
            f.wait();
        }));
        started->get_future().wait();
        while (ui.post([] {})) {
        }
    }
    ~UiStall() { release(); }
    void release() {
        if (!done_) released_.set_value();
        done_ = true;
    }

private:
    std::promise<void> released_;
    bool done_ = false;
};

// Whoever close() or close_async() answers sees the scene Stopped, its
// "stopped" already published: a "deleted" the caller publishes next (as
// a confirmed delete does) can never come before it. The UI is stalled so
// that nothing the scene task hands it can be what orders the two.
TEST_F(SceneRuntimeTest, CloseAnswersOnlyOnceTheSceneIsStopped) {
    start();
    install("counter", kCounter);
    ASSERT_TRUE(rt->open("counter"));
    ASSERT_TRUE(wait_label("已点击"));
    {
        UiStall stall(*ui);
        std::promise<SceneState> seen;
        rt->close_async("counter", [&](wisp::Result<void> r) {
            EXPECT_TRUE(r) << r.error().message;
            seen.set_value(rt->status("counter").state);
            wisp::core::Event e;
            e.topic = "scene.state";
            e.source = "scene";
            e.payload = {{"id", wisp::Value("counter")}, {"state", wisp::Value("deleted")}};
            bus.publish(std::move(e));
        });
        auto state = seen.get_future();
        ASSERT_EQ(state.wait_for(std::chrono::seconds(5)), std::future_status::ready);
        EXPECT_EQ(state.get(), SceneState::Stopped);
        EXPECT_EQ(rt->foreground(), std::nullopt);
    }
    // The callback answers before it publishes "deleted", and the bus
    // delivers on its own thread: wait for all three before comparing.
    ASSERT_TRUE(wait_until([&] { return events().size() == 3; }, 1000));
    EXPECT_EQ(events(),
              (std::vector<StateEvent>{
                  {"counter", "running"}, {"counter", "stopped"}, {"counter", "deleted"}}));

    ASSERT_TRUE(rt->open("counter"));
    ASSERT_TRUE(wait_label("已点击"));
    {
        UiStall stall(*ui);
        ASSERT_TRUE(rt->close("counter"));
        EXPECT_EQ(rt->status("counter").state, SceneState::Stopped);
    }
}

// The UI changes that open and close a scene (the view coming up, going
// away) are never dropped, however long the UI thread is held up with its
// queue full: they run once it is free again.
TEST_F(SceneRuntimeTest, OpenAndCloseSurviveALongUiStall) {
    start();
    install("counter", kCounter);
    {
        UiStall stall(*ui);
        ASSERT_TRUE(rt->open("counter"));
        // Longer than any bounded retry would wait.
        wisp::os::sleep_ms(2500);
    }
    EXPECT_TRUE(wait_label("已点击"));
    {
        UiStall stall(*ui);
        ASSERT_TRUE(rt->close("counter"));
        wisp::os::sleep_ms(2500);
    }
    EXPECT_TRUE(wait_until([&] { return active_screen() == home; }, 1000));
}

// A close of a scene whose load is still on the worker cancels that open:
// the scene does not come up afterwards on files a delete may be removing.
TEST_F(SceneRuntimeTest, CloseDuringItsLoadCancelsTheOpen) {
    start();
    install("todo", "return ui.screen { ui.label { text = 'v0' } }");
    WorkerGate gate(worker);
    // From the UI thread an open is only queued.
    on_ui([&] { EXPECT_TRUE(rt->open("todo")); });
    // The scene task has handled it: the load waits on the worker.
    ASSERT_TRUE(rt->check_sources({}));
    std::promise<bool> closed;
    rt->close_async("todo", [&](wisp::Result<void> r) { closed.set_value(r.is_ok()); });
    ASSERT_TRUE(rt->check_sources({}));
    gate.release();
    auto result = closed.get_future();
    ASSERT_EQ(result.wait_for(std::chrono::seconds(5)), std::future_status::ready);
    EXPECT_TRUE(result.get());
    // The load came back before the close was answered; this is after it.
    ASSERT_TRUE(rt->check_sources({}));
    EXPECT_FALSE(rt->foreground().has_value());
    EXPECT_NE(rt->status("todo").state, SceneState::Running);
    EXPECT_TRUE(events().empty());
    ASSERT_TRUE(rt->open("todo"));
    EXPECT_TRUE(wait_label("v0"));
}

// While a scene is being deleted (SceneRepository::removing()), it is not
// opened: a new open is refused at once, and one whose load was already
// on the worker is dropped when the load comes back.
TEST_F(SceneRuntimeTest, OpenIsBusyWhileTheSceneIsBeingDeleted) {
    start();
    install("todo", "return ui.screen { ui.label { text = 'v0' } }");
    auto gate = std::make_unique<WorkerGate>(worker);
    on_ui([&] { EXPECT_TRUE(rt->open("todo")); });
    ASSERT_TRUE(rt->check_sources({}));
    repo->begin_removing("todo");

    auto refused = rt->open("todo");
    ASSERT_FALSE(refused);
    EXPECT_EQ(refused.error().code, ErrorCode::Busy);

    gate->release();
    ASSERT_TRUE(worker.drain(5000));
    ASSERT_TRUE(rt->check_sources({}));
    EXPECT_FALSE(rt->foreground().has_value());
    EXPECT_TRUE(events().empty());

    repo->end_removing("todo");
    ASSERT_TRUE(rt->open("todo"));
    EXPECT_TRUE(wait_label("v0"));
}

// When the screen a scene covered is gone by the time it closes, an empty
// screen stands in for it: one, made once and reused, not a new one per
// close.
TEST_F(SceneRuntimeTest, OneStandInScreenForAHomeThatIsGone) {
    start();
    install("counter", kCounter);
    ASSERT_TRUE(rt->open("counter"));
    ASSERT_TRUE(wait_label("已点击"));
    lv_obj_t* scene_screen = active_screen();
    on_ui([&] { lv_obj_delete(home); });
    ASSERT_TRUE(rt->close("counter"));
    ASSERT_TRUE(wait_until([&] { return active_screen() != scene_screen; }, 1000));
    lv_obj_t* stand_in = active_screen();

    // Something else is on screen when the scene opens again, and is gone
    // when it closes.
    lv_obj_t* other = nullptr;
    on_ui([&] {
        other = lv_obj_create(nullptr);
        lv_screen_load(other);
    });
    ASSERT_TRUE(rt->open("counter"));
    ASSERT_TRUE(wait_label("已点击"));
    scene_screen = active_screen();
    on_ui([&] { lv_obj_delete(other); });
    ASSERT_TRUE(rt->close("counter"));
    ASSERT_TRUE(wait_until([&] { return active_screen() != scene_screen; }, 1000));
    EXPECT_EQ(active_screen(), stand_in);
}

}  // namespace
