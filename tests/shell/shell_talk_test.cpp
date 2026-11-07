// Shell's own wiring for the talk overlay (wisp/shell/talk_overlay.hpp,
// mvp spec 6.5): scene_in_front()/request_talk()/update_talk()/
// on_voice_bus() and the tick timer, driven end to end over a real
// ConversationController/AgentService/VoiceService (shell/shell_fixture.hpp)
// and a real Shell on a live UiRuntime (as shell/shell_test.cpp drives one,
// with the same scene stand-in). TalkOverlay's own phase/layout logic is
// covered by shell/talk_overlay_test.cpp; this file only checks that Shell
// drives it correctly from the real conversation and voice.state.

#include <gtest/gtest.h>

#include <memory>
#include <string>

#include "shell/shell_fixture.hpp"
#include "support/fake_devices.hpp"
#include "support/lvgl_probe.hpp"
#include "support/wait.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/hal/display_geometry.hpp"
#include "wisp/shell/limits.hpp"
#include "wisp/shell/shell.hpp"
#include "wisp/shell/talk_overlay.hpp"
#include "wisp/ui/system_layer.hpp"
#include "wisp/ui/ui_runtime.hpp"

using wisp::test::wait_until;
using wisp::voice::VoiceState;

namespace {

using Phase = wisp::shell::TalkOverlay::Phase;

// ShellFixture (shell/shell_fixture.hpp) already gives a real bus, agent,
// voice and ConversationController; this adds a live Shell on top of it
// with a scene stand-in, the same way shell/shell_test.cpp's own
// (file-local) ShellFixture does over a bare EventBus.
class ShellTalkTest : public wisp::test::ShellFixture {
protected:
    void make_shell(const std::string& shape = "rect", int32_t width = 480, int32_t height = 480) {
        start();
        display_ =
            std::make_shared<wisp::test::FakeDisplay>("display.main", width, height, 1, shape);
        shape_ = wisp::hal::screen_shape_from_caps(display_->info().caps).value();
        ui_ = std::make_unique<wisp::ui::UiRuntime>(display_, nullptr);
        ASSERT_TRUE(ui_->start());
        on_ui([&] { layer_ = std::make_unique<wisp::ui::SystemLayer>(shape_); });
        shell_ = std::make_unique<wisp::shell::Shell>(wisp::shell::ShellDeps{
            .ui = *ui_,
            .layer = *layer_,
            .shape = shape_,
            .bus = h->bus,
            .conversation = conv.get(),
            .leave_scene = [this]() -> wisp::Result<bool> {
                ++leaves;
                if (scene == nullptr) return wisp::Result<bool>::ok(false);
                if (refuse) return wisp::Result<bool>::err(wisp::ErrorCode::Busy, "queue full");
                leaving = true;
                return wisp::Result<bool>::ok(true);
            },
            // The same clock VoiceDeps already runs on (ShellFixture's
            // virtual_ms), so bumping it for the Saying timeout doesn't
            // also have to fast-forward a real 5s sleep.
            .now_ms = [this] { return wisp::os::now_ms() + virtual_ms.load(); },
        });
        on_ui([&] { shell_->start(); });
    }

    void TearDown() override {
        if (shell_ != nullptr) on_ui([&] { shell_->stop(); });
        shell_.reset();
        if (layer_) on_ui([&] { layer_.reset(); });
        if (ui_) ui_->stop();
        ShellFixture::TearDown();
    }

    void on_ui(const std::function<void()>& fn) { wisp::test::run_on_ui_blocking(ui_.get(), fn); }

    // A scene's view put in front, as the scene runtime does
    // (shell/shell_test.cpp's own ShellFixture).
    void open_scene() {
        on_ui([&] {
            scene = lv_obj_create(nullptr);
            lv_screen_load(scene);
        });
    }
    void drop_scene() {
        on_ui([&] {
            if (lv_screen_active() == scene) lv_screen_load(shell_->home_screen());
            lv_obj_delete(scene);
            scene = nullptr;
        });
    }

    Phase phase() {
        Phase p = Phase::Hidden;
        on_ui([&] { p = shell_->talk()->phase(); });
        return p;
    }
    bool wait_phase(Phase want, uint32_t ms) {
        return wait_until([&] { return phase() == want; }, ms);
    }

    std::shared_ptr<wisp::test::FakeDisplay> display_;
    wisp::hal::ScreenShape shape_;
    std::unique_ptr<wisp::ui::UiRuntime> ui_;
    std::unique_ptr<wisp::ui::SystemLayer> layer_;
    std::unique_ptr<wisp::shell::Shell> shell_;

    int leaves = 0;
    bool refuse = false;
    bool leaving = false;
    lv_obj_t* scene = nullptr;
};

}  // namespace

// 只在场景在前台时显示: the shell's own home screen is active, so a real
// hold never reaches the overlay even once capture genuinely starts.
TEST_F(ShellTalkTest, HoldWithNoSceneShowsNothing) {
    make_shell();
    say_into_mic();
    conv->hold_begin();
    ASSERT_TRUE(wait_until([&] { return voice->state() == VoiceState::Listening; }, 2000));
    // Give request_talk()/update_talk() every chance to run before
    // asserting the negative.
    h->bus.wait_idle(500);
    EXPECT_EQ(phase(), Phase::Hidden);

    conv->hold_cancel();
    ASSERT_TRUE(settled());
}

// Listening (recording) -> Thinking (transcribing, then the turn running
// with no say yet) -> Saying (the reply's say text), over a real hold and
// a real turn.
TEST_F(ShellTalkTest, SceneHoldGoesListeningThinkingSaying) {
    make_shell();
    open_scene();
    say_into_mic();
    server.push_asr(wisp::test::asr_text("提醒我喝水"));
    // Held right before any content streams: the turn is running with no
    // say yet for as long as the test needs to observe Thinking, rather
    // than racing a FakeModelServer reply that would otherwise resolve
    // (and so reach Saying) within the same millisecond.
    server.llm().push(wisp::test::llm_text("<say>收到，我会提醒你。</say>", /*hold_after=*/0));
    server.push_tts(wisp::test::tts_pcm());

    conv->hold_begin();
    ASSERT_TRUE(wait_until([&] { return voice->state() == VoiceState::Listening; }, 2000));
    ASSERT_TRUE(wait_phase(Phase::Listening, 2000));

    ASSERT_TRUE(wait_until([&] { return mic->script_done(); }, 3000));
    conv->hold_end();

    ASSERT_TRUE(wait_until([&] { return server.llm().held() == 1; }, 5000));
    ASSERT_TRUE(wait_phase(Phase::Thinking, 2000));

    server.release();
    ASSERT_TRUE(wait_turns(1));
    ASSERT_TRUE(wait_phase(Phase::Saying, 3000));

    ASSERT_TRUE(settled());
    drop_scene();
}

// scene.state hides the overlay at once, mid-capture, regardless of what
// VoiceState/the conversation are doing.
TEST_F(ShellTalkTest, SceneStateHidesTheOverlay) {
    make_shell();
    open_scene();
    say_into_mic();
    conv->hold_begin();
    ASSERT_TRUE(wait_until([&] { return voice->state() == VoiceState::Listening; }, 2000));
    ASSERT_TRUE(wait_phase(Phase::Listening, 2000));

    h->bus.publish({"scene.state", "test", 0, {{"id", "demo"}, {"state", "background"}}});
    ASSERT_TRUE(wait_phase(Phase::Hidden, 2000));

    conv->hold_cancel();
    ASSERT_TRUE(settled());
    drop_scene();
}

// A tap sends the foreground scene back through the existing leave_scene()
// path (the same one the back gesture uses) and shows Home.
TEST_F(ShellTalkTest, TapClosesTheSceneThroughLeaveSceneAndShowsHome) {
    make_shell();
    open_scene();
    say_into_mic();
    conv->hold_begin();
    ASSERT_TRUE(wait_until([&] { return voice->state() == VoiceState::Listening; }, 2000));
    ASSERT_TRUE(wait_phase(Phase::Listening, 2000));

    on_ui([&] { lv_obj_send_event(shell_->talk()->obj(), LV_EVENT_CLICKED, nullptr); });
    // The tap's open_home() posts its own navigation (shell.cpp: the same
    // way a back swipe's is, since it arrives from inside an LVGL event);
    // a second round-trip on the UI thread lets that queued post run.
    lv_obj_t* active = nullptr;
    on_ui([&] { active = lv_screen_active(); });
    EXPECT_TRUE(leaving);
    EXPECT_EQ(active, shell_->home_screen());

    conv->hold_cancel();
    ASSERT_TRUE(settled());
    drop_scene();
}

// tick() (the shell's 250ms timer) moves Saying to Hidden once
// kOverlaySayMs has passed, without needing a real 5s sleep: it reads the
// same virtual clock VoiceDeps runs on.
TEST_F(ShellTalkTest, TickHidesSayingAfterFiveSeconds) {
    make_shell();
    open_scene();
    server.llm().push(wisp::test::llm_text("<say>好的。</say>"));
    server.push_tts(wisp::test::tts_pcm());

    ASSERT_TRUE(conv->voice_text("提醒我喝水"));
    ASSERT_TRUE(wait_turns(1));
    ASSERT_TRUE(wait_phase(Phase::Saying, 3000));

    virtual_ms += wisp::shell::kOverlaySayMs + 1;
    ASSERT_TRUE(wait_phase(Phase::Hidden, 2000));

    ASSERT_TRUE(settled());
    drop_scene();
}
