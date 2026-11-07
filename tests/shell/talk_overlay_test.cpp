// TalkOverlay (wisp/shell/talk_overlay.hpp, mvp spec 6.5): the feedback
// shown over a scene while holding to talk.

#include "wisp/shell/talk_overlay.hpp"

#include <gtest/gtest.h>

#include <functional>
#include <memory>
#include <string>

#include "support/fake_devices.hpp"
#include "support/lvgl_probe.hpp"
#include "support/visible.hpp"
#include "wisp/hal/display_geometry.hpp"
#include "wisp/shell/conversation.hpp"
#include "wisp/shell/limits.hpp"
#include "wisp/ui/strings.hpp"
#include "wisp/ui/system_layer.hpp"
#include "wisp/ui/ui_runtime.hpp"

using wisp::shell::kOverlaySayMs;
using wisp::shell::TalkOverlay;
using wisp::shell::TurnView;
using wisp::test::box_visible;
using wisp::test::FakeDisplay;
using wisp::test::FakeTouch;
using wisp::test::find_obj_of_class;
using wisp::test::run_on_ui_blocking;
using wisp::ui::SystemLayer;
using wisp::voice::VoiceState;

namespace {

constexpr int kSide = 480;

lv_area_t coords_of(lv_obj_t* o) {
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    return a;
}

bool is_hidden(lv_obj_t* o) { return o == nullptr || lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN); }

std::string label_text(lv_obj_t* o) {
    const char* t = lv_label_get_text(o);
    return t != nullptr ? t : "";
}

class TalkOverlayTest : public ::testing::Test {
protected:
    void start(const std::string& shape = "rect", int32_t width = kSide, int32_t height = kSide) {
        wisp::ui::set_lang(wisp::ui::Lang::Zh);
        display_ = std::make_shared<FakeDisplay>("display.main", width, height, 1, shape);
        touch_ = std::make_shared<FakeTouch>("touch.main");
        ui_ = std::make_unique<wisp::ui::UiRuntime>(display_, touch_);
        ASSERT_TRUE(ui_->start());
        shape_ = wisp::hal::screen_shape_from_caps(display_->info().caps).value();
        on_ui([&] { layer_ = std::make_unique<SystemLayer>(shape_); });
    }

    void TearDown() override {
        if (!ui_) return;
        on_ui([&] {
            overlay_.reset();
            layer_.reset();
        });
        ui_->stop();
    }

    void on_ui(const std::function<void()>& fn) { run_on_ui_blocking(ui_.get(), fn); }

    // Builds the overlay on the UI thread, counting open_home() calls.
    void make(int* home_calls = nullptr) {
        on_ui([&] {
            overlay_ = std::make_unique<TalkOverlay>(*layer_, shape_, [home_calls] {
                if (home_calls != nullptr) ++*home_calls;
            });
        });
    }

    std::shared_ptr<FakeDisplay> display_;
    std::shared_ptr<FakeTouch> touch_;
    std::unique_ptr<wisp::ui::UiRuntime> ui_;
    wisp::hal::ScreenShape shape_;
    std::unique_ptr<SystemLayer> layer_;
    std::unique_ptr<TalkOverlay> overlay_;
};

}  // namespace

TEST_F(TalkOverlayTest, OnlyOverAScene) {
    start();
    lv_obj_t* scene_screen = nullptr;
    on_ui([&] {
        // Stands in for a Lua scene's own screen.
        scene_screen = lv_obj_create(nullptr);
        lv_screen_load(scene_screen);
    });
    make();
    on_ui([&] {
        // Hidden until driven, and a direct top-layer child even then: it
        // shows over whatever screen is active without belonging to it.
        EXPECT_EQ(overlay_->phase(), TalkOverlay::Phase::Hidden);
        ASSERT_NE(overlay_->obj(), nullptr);
        EXPECT_TRUE(is_hidden(overlay_->obj()));
        EXPECT_EQ(lv_obj_get_parent(overlay_->obj()), lv_layer_top());

        overlay_->on_turn(TurnView{}, VoiceState::Listening, 50, 0);
        EXPECT_EQ(overlay_->phase(), TalkOverlay::Phase::Listening);
        EXPECT_FALSE(is_hidden(overlay_->obj()));
        EXPECT_EQ(lv_screen_active(), scene_screen);
        EXPECT_EQ(lv_obj_get_parent(overlay_->obj()), lv_layer_top());
        // Away from the scene screen before deleting it: LVGL does not
        // allow deleting the active screen outright (wisp/shell/shell.cpp
        // loads a stand-in the same way before dropping one it replaces).
        lv_screen_load(lv_obj_create(nullptr));
        lv_obj_delete(scene_screen);
    });
}

TEST_F(TalkOverlayTest, ListeningThinkingSaying) {
    start();
    make();
    on_ui([&] {
        overlay_->on_turn(TurnView{}, VoiceState::Listening, 80, 1000);
        EXPECT_EQ(overlay_->phase(), TalkOverlay::Phase::Listening);
        lv_obj_t* bar = find_obj_of_class(overlay_->obj(), &lv_bar_class);
        ASSERT_NE(bar, nullptr);
        EXPECT_FALSE(is_hidden(bar));
        EXPECT_EQ(lv_bar_get_value(bar), 80);

        overlay_->on_turn(TurnView{}, VoiceState::Transcribing, 0, 1100);
        EXPECT_EQ(overlay_->phase(), TalkOverlay::Phase::Thinking);
        EXPECT_TRUE(is_hidden(bar));
        lv_obj_t* label = find_obj_of_class(overlay_->obj(), &lv_label_class);
        ASSERT_NE(label, nullptr);
        EXPECT_EQ(label_text(label), wisp::ui::tr(wisp::ui::StrId::TalkThinking));

        TurnView v;
        v.say.assign("好的，明天早上8点提醒你带伞。");
        overlay_->on_turn(v, VoiceState::Idle, 0, 1200);
        EXPECT_EQ(overlay_->phase(), TalkOverlay::Phase::Saying);
        EXPECT_EQ(label_text(label), "好的，明天早上8点提醒你带伞。");
    });
}

// Thinking with a step under way shows that step; a done step, or the
// passing thinking step, shows the plain text again.
TEST_F(TalkOverlayTest, ThinkingShowsTheCurrentStep) {
    start();
    make();
    on_ui([&] {
        TurnView v;
        v.running = true;
        v.steps_total = 1;
        v.step_count = 1;
        v.steps[0].label.assign("写入文件 main.lua");
        overlay_->on_turn(v, VoiceState::Idle, 0, 1000);
        ASSERT_EQ(overlay_->phase(), TalkOverlay::Phase::Thinking);
        lv_obj_t* label = find_obj_of_class(overlay_->obj(), &lv_label_class);
        ASSERT_NE(label, nullptr);
        EXPECT_EQ(label_text(label), "写入文件 main.lua");

        v.steps[0].st = wisp::shell::TurnStep::State::Done;
        v.thinking = true;
        overlay_->on_turn(v, VoiceState::Idle, 0, 1100);
        EXPECT_EQ(overlay_->phase(), TalkOverlay::Phase::Thinking);
        EXPECT_EQ(label_text(label), wisp::ui::tr(wisp::ui::StrId::TalkThinking));
    });
}

TEST_F(TalkOverlayTest, SayHidesAfterFiveSeconds) {
    start();
    make();
    on_ui([&] {
        TurnView v;
        v.say.assign("好的。");
        overlay_->on_turn(v, VoiceState::Idle, 0, 1000);
        ASSERT_EQ(overlay_->phase(), TalkOverlay::Phase::Saying);

        overlay_->tick(1000 + kOverlaySayMs - 1);
        EXPECT_EQ(overlay_->phase(), TalkOverlay::Phase::Saying);

        overlay_->tick(1000 + kOverlaySayMs);
        EXPECT_EQ(overlay_->phase(), TalkOverlay::Phase::Hidden);
        EXPECT_TRUE(is_hidden(overlay_->obj()));
    });
}

TEST_F(TalkOverlayTest, TapOpensHome) {
    start();
    int calls = 0;
    make(&calls);
    on_ui([&] {
        overlay_->on_turn(TurnView{}, VoiceState::Listening, 10, 0);
        lv_obj_send_event(overlay_->obj(), LV_EVENT_CLICKED, nullptr);
        overlay_->hide();  // stop the pulsing dot before teardown
    });
    EXPECT_EQ(calls, 1);
}

TEST_F(TalkOverlayTest, SceneChangeHides) {
    start();
    make();
    on_ui([&] {
        overlay_->on_turn(TurnView{}, VoiceState::Listening, 10, 0);
        ASSERT_EQ(overlay_->phase(), TalkOverlay::Phase::Listening);
        overlay_->hide();
        EXPECT_EQ(overlay_->phase(), TalkOverlay::Phase::Hidden);
        EXPECT_TRUE(is_hidden(overlay_->obj()));
    });
}

TEST_F(TalkOverlayTest, ConfirmDialogStaysOnTop) {
    start();
    lv_obj_t* box = nullptr;
    on_ui([&] {
        box = layer_->open_confirm(wisp::ui::StrId::ResetTitle, "really?", [](bool) {});
        ASSERT_NE(box, nullptr);
    });
    make();
    on_ui([&] {
        overlay_->on_turn(TurnView{}, VoiceState::Listening, 10, 0);
        ASSERT_FALSE(is_hidden(overlay_->obj()));
        lv_obj_t* backdrop = lv_obj_get_parent(box);
        EXPECT_GT(lv_obj_get_index(backdrop), lv_obj_get_index(overlay_->obj()));
        overlay_->hide();  // stop the pulsing dot before teardown
    });
}

// The three screen shapes the home screen and the sub-pages are already
// checked on (AGENTS.md, screen shape; spec 5.6 of the base spec).
struct ShapeCase {
    const char* shape;
    int32_t width;
    int32_t height;
};

class TalkOverlayShapes : public ::testing::TestWithParam<ShapeCase> {
protected:
    void SetUp() override {
        const ShapeCase& c = GetParam();
        wisp::ui::set_lang(wisp::ui::Lang::Zh);
        display_ = std::make_shared<FakeDisplay>("display.main", c.width, c.height, 1, c.shape);
        touch_ = std::make_shared<FakeTouch>("touch.main");
        ui_ = std::make_unique<wisp::ui::UiRuntime>(display_, touch_);
        ASSERT_TRUE(ui_->start());
        shape_ = wisp::hal::screen_shape_from_caps(display_->info().caps).value();
        on_ui([&] {
            layer_ = std::make_unique<SystemLayer>(shape_);
            overlay_ = std::make_unique<TalkOverlay>(*layer_, shape_, [] {});
        });
    }

    void TearDown() override {
        on_ui([&] {
            overlay_.reset();
            layer_.reset();
        });
        ui_->stop();
    }

    void on_ui(const std::function<void()>& fn) { run_on_ui_blocking(ui_.get(), fn); }

    std::shared_ptr<FakeDisplay> display_;
    std::shared_ptr<FakeTouch> touch_;
    std::unique_ptr<wisp::ui::UiRuntime> ui_;
    wisp::hal::ScreenShape shape_;
    std::unique_ptr<SystemLayer> layer_;
    std::unique_ptr<TalkOverlay> overlay_;
};

TEST_P(TalkOverlayShapes, InsideVisibleArea) {
    on_ui([&] {
        TurnView v;
        v.say.assign("好的，已经记下来了。");
        overlay_->on_turn(v, VoiceState::Idle, 0, 0);
        ASSERT_EQ(overlay_->phase(), TalkOverlay::Phase::Saying);
        lv_obj_update_layout(overlay_->obj());
        EXPECT_TRUE(box_visible(shape_, coords_of(overlay_->obj())));
    });
}

INSTANTIATE_TEST_SUITE_P(
    Shapes, TalkOverlayShapes,
    ::testing::Values(ShapeCase{"rect", 480, 480}, ShapeCase{"rounded_rect", 480, 480},
                      ShapeCase{"circle", 480, 480}, ShapeCase{"circle", 360, 360}),
    [](const ::testing::TestParamInfo<ShapeCase>& info) {
        return std::string(info.param.shape) + "_" + std::to_string(info.param.width);
    });
