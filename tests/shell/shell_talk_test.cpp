// Shell's own wiring for the talk overlay (wisp/shell/talk_overlay.hpp,
// mvp spec 6.5): home_in_front()/request_talk()/update_talk()/
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
#include "wisp/net/http.hpp"
#include "wisp/shell/home.hpp"
#include "wisp/shell/limits.hpp"
#include "wisp/shell/shell.hpp"
#include "wisp/shell/talk_overlay.hpp"
#include "wisp/ui/strings.hpp"
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
            // The App's self-check screen stands in as a bare screen.
            .open_diagnostics =
                [this] {
                    diag = lv_obj_create(nullptr);
                    lv_screen_load(diag);
                },
            .close_diagnostics =
                [this] {
                    lv_obj_delete(diag);
                    diag = nullptr;
                },
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

    lv_obj_t* active() {
        lv_obj_t* a = nullptr;
        on_ui([&] { a = lv_screen_active(); });
        return a;
    }
    std::string overlay_text() {
        std::string out;
        on_ui([&] {
            lv_obj_t* label = wisp::test::find_obj_of_class(shell_->talk()->obj(), &lv_label_class);
            if (label != nullptr && !lv_obj_has_flag(label, LV_OBJ_FLAG_HIDDEN))
                out = lv_label_get_text(label);
        });
        return out;
    }
    bool overlay_shown() {
        bool shown = false;
        on_ui([&] { shown = !lv_obj_has_flag(shell_->talk()->obj(), LV_OBJ_FLAG_HIDDEN); });
        return shown;
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
    lv_obj_t* diag = nullptr;
};

}  // namespace

// Home shows the turn itself: a real hold there never reaches the overlay,
// even once capture genuinely starts.
TEST_F(ShellTalkTest, HoldOnHomeShowsNoOverlay) {
    make_shell();
    ASSERT_EQ(active(), shell_->home_screen());
    say_into_mic();
    conv->hold_begin();
    ASSERT_TRUE(wait_until([&] { return voice->state() == VoiceState::Listening; }, 2000));
    // Give request_talk()/update_talk() every chance to run before
    // asserting the negative.
    h->bus.wait_idle(500);
    EXPECT_EQ(phase(), Phase::Hidden);
    EXPECT_FALSE(overlay_shown());

    conv->hold_cancel();
    ASSERT_TRUE(settled());
}

// Every screen but Home gets the overlay: a sub-page ...
TEST_F(ShellTalkTest, HoldOnASubPageShowsTheOverlay) {
    make_shell();
    on_ui([&] { shell_->show(wisp::shell::Page::Settings); });
    ASSERT_EQ(active(), shell_->page_screen());
    say_into_mic();
    conv->hold_begin();
    ASSERT_TRUE(wait_until([&] { return voice->state() == VoiceState::Listening; }, 2000));
    ASSERT_TRUE(wait_phase(Phase::Listening, 2000));
    EXPECT_TRUE(overlay_shown());

    conv->hold_cancel();
    ASSERT_TRUE(settled());
}

// ... and the hardware diagnostics.
TEST_F(ShellTalkTest, HoldOnDiagnosticsShowsTheOverlay) {
    make_shell();
    on_ui([&] { shell_->open_diagnostics(); });
    ASSERT_NE(diag, nullptr);
    ASSERT_EQ(active(), diag);
    say_into_mic();
    conv->hold_begin();
    ASSERT_TRUE(wait_until([&] { return voice->state() == VoiceState::Listening; }, 2000));
    ASSERT_TRUE(wait_phase(Phase::Listening, 2000));
    EXPECT_TRUE(overlay_shown());

    conv->hold_cancel();
    ASSERT_TRUE(settled());
}

// A turn that fails while a sub-page is in front says why, in the same
// short words Home uses, for the overlay's five seconds.
TEST_F(ShellTalkTest, FailedTurnOnASubPageShowsItsNote) {
    make_shell();
    on_ui([&] { shell_->show(wisp::shell::Page::Settings); });
    // A connect failure, retried twice by the client: reason "network".
    for (int i = 0; i < 3; ++i)
        server.llm().push({.status = 0, .chunks = {}, .fail = wisp::net::TransportError::Connect});

    ASSERT_TRUE(conv->voice_text("提醒我喝水"));
    ASSERT_TRUE(wait_turns(1));
    ASSERT_TRUE(settled());
    ASSERT_EQ(conv->current().end_reason, "network");
    ASSERT_TRUE(wait_phase(Phase::Note, 3000));
    EXPECT_EQ(overlay_text(), wisp::ui::tr(wisp::ui::StrId::HomeTurnNetwork));
    EXPECT_TRUE(overlay_shown());

    virtual_ms += wisp::shell::kOverlaySayMs + 1;
    ASSERT_TRUE(wait_phase(Phase::Hidden, 2000));
    // Shown once: a later change of the voice does not bring it back.
    h->bus.publish({"voice.state", "test", 0, {{"state", "idle"}}});
    h->bus.wait_idle(500);
    EXPECT_EQ(phase(), Phase::Hidden);
}

// A turn started from the web page shows nothing over Settings while it
// runs (it may run for minutes, and the card would cover the page's last
// rows), nor its say once it ends well; a web turn that fails still says
// why.
TEST_F(ShellTalkTest, WebTurnOverSettingsShowsOnlyItsFailure) {
    make_shell();
    on_ui([&] { shell_->show(wisp::shell::Page::Settings); });
    server.llm().push(wisp::test::llm_text("<say>场景做好了。</say>", /*hold_after=*/0));

    ASSERT_TRUE(agent->submit("做一个番茄钟场景", wisp::agent::Source::Web));
    ASSERT_TRUE(wait_until([&] { return server.llm().held() == 1; }, 5000));
    ASSERT_TRUE(wait_until([&] { return conv->current().running; }, 2000));
    h->bus.wait_idle(500);
    on_ui([] {});
    EXPECT_EQ(phase(), Phase::Hidden);
    EXPECT_FALSE(overlay_shown());

    server.release();
    ASSERT_TRUE(wait_turns(1));
    ASSERT_TRUE(wait_until([&] { return !conv->current().running; }, 2000));
    on_ui([] {});
    EXPECT_EQ(phase(), Phase::Hidden);

    for (int i = 0; i < 3; ++i)
        server.llm().push({.status = 0, .chunks = {}, .fail = wisp::net::TransportError::Connect});
    ASSERT_TRUE(agent->submit("再做一个", wisp::agent::Source::Web));
    ASSERT_TRUE(wait_turns(2));
    ASSERT_TRUE(wait_phase(Phase::Note, 3000));
    EXPECT_EQ(overlay_text(), wisp::ui::tr(wisp::ui::StrId::HomeTurnNetwork));
    EXPECT_TRUE(overlay_shown());
}

// A turn that fails with Settings in front, Home shown within a second:
// Home's hint line says why, in the same short words.
TEST_F(ShellTalkTest, FailedTurnThenHomeShowsTheShortReason) {
    make_shell();
    on_ui([&] { shell_->show(wisp::shell::Page::Settings); });
    for (int i = 0; i < 3; ++i)
        server.llm().push({.status = 0, .chunks = {}, .fail = wisp::net::TransportError::Connect});
    ASSERT_TRUE(conv->voice_text("提醒我喝水"));
    ASSERT_TRUE(wait_turns(1));
    ASSERT_TRUE(settled());
    ASSERT_TRUE(wait_phase(Phase::Note, 3000));

    virtual_ms += 900;
    std::string hint;
    on_ui([&] {
        shell_->show(wisp::shell::Page::Home);
        wisp::shell::Home* home = shell_->home();
        ASSERT_NE(home, nullptr);
        home->refresh();
        lv_obj_t* label = home->hint_label();
        if (label != nullptr && !lv_obj_has_flag(label, LV_OBJ_FLAG_HIDDEN))
            hint = lv_label_get_text(label);
    });
    EXPECT_EQ(active(), shell_->home_screen());
    EXPECT_EQ(hint, wisp::ui::tr(wisp::ui::StrId::HomeTurnNetwork));
    EXPECT_EQ(phase(), Phase::Hidden);
}

// A spoken turn over Settings shows that it is being worked on.
TEST_F(ShellTalkTest, VoiceTurnOverSettingsShowsThinking) {
    make_shell();
    on_ui([&] { shell_->show(wisp::shell::Page::Settings); });
    server.llm().push(wisp::test::llm_text("<say>好的。</say>", /*hold_after=*/0));
    server.push_tts(wisp::test::tts_pcm());

    ASSERT_TRUE(conv->voice_text("提醒我喝水"));
    ASSERT_TRUE(wait_until([&] { return server.llm().held() == 1; }, 5000));
    ASSERT_TRUE(wait_phase(Phase::Thinking, 2000));
    EXPECT_TRUE(overlay_shown());

    server.release();
    ASSERT_TRUE(wait_turns(1));
    ASSERT_TRUE(settled());
}

// Home coming to the front hides the overlay: it shows the same itself.
TEST_F(ShellTalkTest, HomeInFrontHidesTheOverlay) {
    make_shell();
    on_ui([&] { shell_->show(wisp::shell::Page::History); });
    say_into_mic();
    conv->hold_begin();
    ASSERT_TRUE(wait_until([&] { return voice->state() == VoiceState::Listening; }, 2000));
    ASSERT_TRUE(wait_phase(Phase::Listening, 2000));

    on_ui([&] {
        shell_->show(wisp::shell::Page::Home);
        // At once, not on the next change.
        EXPECT_EQ(shell_->talk()->phase(), Phase::Hidden);
    });
    h->bus.wait_idle(500);
    EXPECT_EQ(phase(), Phase::Hidden);
    EXPECT_FALSE(overlay_shown());

    conv->hold_cancel();
    ASSERT_TRUE(settled());
}

// Leaving Home while capturing shows the overlay on the page that came in
// front, without waiting for the next change.
TEST_F(ShellTalkTest, LeavingHomeShowsTheOverlay) {
    make_shell();
    say_into_mic();
    conv->hold_begin();
    ASSERT_TRUE(wait_until([&] { return voice->state() == VoiceState::Listening; }, 2000));
    h->bus.wait_idle(500);
    ASSERT_EQ(phase(), Phase::Hidden);

    on_ui([&] { shell_->show(wisp::shell::Page::Scenes); });
    ASSERT_TRUE(wait_phase(Phase::Listening, 2000));

    conv->hold_cancel();
    ASSERT_TRUE(settled());
}

// On a sub-page a tap on the card goes back to Home the way show(Home)
// leaves it: the page is deleted and nothing is sent back.
TEST_F(ShellTalkTest, TapOnASubPageShowsHome) {
    make_shell();
    on_ui([&] { shell_->show(wisp::shell::Page::Settings); });
    say_into_mic();
    conv->hold_begin();
    ASSERT_TRUE(wait_until([&] { return voice->state() == VoiceState::Listening; }, 2000));
    ASSERT_TRUE(wait_phase(Phase::Listening, 2000));

    on_ui([&] { lv_obj_send_event(shell_->talk()->obj(), LV_EVENT_CLICKED, nullptr); });
    // The posted navigation runs on the next round-trip.
    EXPECT_EQ(active(), shell_->home_screen());
    lv_obj_t* page = nullptr;
    on_ui([&] { page = shell_->page_screen(); });
    EXPECT_EQ(page, nullptr);
    EXPECT_FALSE(leaving);
    EXPECT_EQ(phase(), Phase::Hidden);

    conv->hold_cancel();
    ASSERT_TRUE(settled());
}

// On the diagnostics the tap closes them, as their back gesture does.
TEST_F(ShellTalkTest, TapOnDiagnosticsShowsHome) {
    make_shell();
    on_ui([&] { shell_->open_diagnostics(); });
    say_into_mic();
    conv->hold_begin();
    ASSERT_TRUE(wait_until([&] { return voice->state() == VoiceState::Listening; }, 2000));
    ASSERT_TRUE(wait_phase(Phase::Listening, 2000));

    on_ui([&] { lv_obj_send_event(shell_->talk()->obj(), LV_EVENT_CLICKED, nullptr); });
    EXPECT_EQ(active(), shell_->home_screen());
    bool open = true;
    on_ui([&] { open = shell_->diagnostics_open(); });
    EXPECT_FALSE(open);
    EXPECT_EQ(diag, nullptr);

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

// A scene.state event only has the overlay look again: while a screen but
// Home is still in front it keeps showing; once the scene's going brings
// Home in front, Home's loading hides it.
TEST_F(ShellTalkTest, SceneStateReevaluatesTheOverlay) {
    make_shell();
    open_scene();
    say_into_mic();
    conv->hold_begin();
    ASSERT_TRUE(wait_until([&] { return voice->state() == VoiceState::Listening; }, 2000));
    ASSERT_TRUE(wait_phase(Phase::Listening, 2000));

    h->bus.publish({"scene.state", "test", 0, {{"id", "demo"}, {"state", "background"}}});
    h->bus.wait_idle(500);
    // Two round-trips: the bus handler's post, then request_talk()'s own.
    on_ui([] {});
    EXPECT_EQ(phase(), Phase::Listening);
    EXPECT_TRUE(overlay_shown());

    on_ui([&] { lv_screen_load(shell_->home_screen()); });
    EXPECT_EQ(phase(), Phase::Hidden);
    EXPECT_FALSE(overlay_shown());

    conv->hold_cancel();
    ASSERT_TRUE(settled());
    drop_scene();
}

// A scene.state event that arrives while a turn's failure note shows over
// a sub-page (a scene closing as the user goes back to the Scenes page)
// keeps the note: the overlay records a note as seen when it takes it, so
// hiding it here would lose it for good.
TEST_F(ShellTalkTest, SceneStateKeepsAFailureNote) {
    make_shell();
    on_ui([&] { shell_->show(wisp::shell::Page::Scenes); });
    for (int i = 0; i < 3; ++i)
        server.llm().push({.status = 0, .chunks = {}, .fail = wisp::net::TransportError::Connect});

    ASSERT_TRUE(conv->voice_text("提醒我喝水"));
    ASSERT_TRUE(wait_turns(1));
    ASSERT_TRUE(settled());
    ASSERT_TRUE(wait_phase(Phase::Note, 3000));

    h->bus.publish({"scene.state", "test", 0, {{"id", "demo"}, {"state", "closed"}}});
    h->bus.wait_idle(500);
    on_ui([] {});
    EXPECT_EQ(phase(), Phase::Note);
    EXPECT_EQ(overlay_text(), wisp::ui::tr(wisp::ui::StrId::HomeTurnNetwork));
    EXPECT_TRUE(overlay_shown());
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
