#pragma once

// TalkOverlay (MVP spec 6.5): the feedback shown while holding to talk
// with a Lua scene in front, so the scene's own screen is left alone. A
// top-layer piece, like SystemLayer's own pieces (wisp/ui/overlay.hpp):
// built once, next to lv_layer_top(), and raised above whatever is
// already there (a confirm dialog opened before it) so later dialogs,
// appended after it, stay on top without any further raise().
//
// Shell owns the one instance and decides when to drive it: on_turn()/
// tick() only while a scene is in front, hide() on every "scene.state"
// event (the scene that was in front may no longer be).
//
// Phase, from on_turn()'s VoiceState and TurnView: Listening while
// recording (a pulsing dot and the level bar), Thinking while
// transcribing, or once a turn is running without a say yet ("思考中…"),
// Saying once the turn's say text is non-empty. Saying holds for
// kOverlaySayMs (wisp/shell/limits.hpp) after the say text it shows was
// last set; tick() ends it. The text is spoken by others
// (ConversationController), not here.
//
// A tap calls `open_home` (Shell::show(Home), after it has sent the
// foreground scene back -- the same path as the back gesture).
//
// UI thread only, like every lv_* call.

#include <cstdint>
#include <functional>

#include "lvgl.h"
#include "wisp/hal/display_geometry.hpp"
#include "wisp/os/large.hpp"
#include "wisp/shell/conversation.hpp"
#include "wisp/voice/voice_service.hpp"

namespace wisp::ui {
class SystemLayer;
}  // namespace wisp::ui

namespace wisp::shell {

class TalkOverlay {
public:
    enum class Phase : uint8_t { Hidden, Listening, Thinking, Saying };

    TalkOverlay(ui::SystemLayer& layer, const hal::ScreenShape& shape,
                std::function<void()> open_home);
    ~TalkOverlay();

    TalkOverlay(const TalkOverlay&) = delete;
    TalkOverlay& operator=(const TalkOverlay&) = delete;

    // Shows what `t`/`v`/`level` call for now.
    void on_turn(const TurnView& t, voice::VoiceState v, uint8_t level, uint64_t now_ms);
    // Saying -> Hidden once kOverlaySayMs has passed since the say text
    // shown was last set.
    void tick(uint64_t now_ms);
    // Hidden at once (e.g. the foreground scene changed).
    void hide();

    Phase phase() const { return phase_; }
    lv_obj_t* obj() const { return obj_; }

private:
    Phase decide(const TurnView& t, voice::VoiceState v) const;
    void apply(uint8_t level);
    void set_pulse(bool on);

    static void click_cb(lv_event_t* e);

    ui::SystemLayer& layer_;
    hal::ScreenShape shape_;
    std::function<void()> open_home_;

    lv_obj_t* obj_ = nullptr;
    lv_obj_t* dot_ = nullptr;
    lv_obj_t* bar_ = nullptr;
    lv_obj_t* label_ = nullptr;
    bool pulsing_ = false;

    Phase phase_ = Phase::Hidden;
    os::LargeString say_shown_;
    uint64_t say_since_ = 0;
};

}  // namespace wisp::shell
