#pragma once

// TalkOverlay (MVP spec 6.5): the feedback shown while talking with any
// screen but Home in front (a Lua scene, a sub-page, the diagnostics), so
// that screen is left alone; Home shows the turn itself. A top-layer
// piece, like SystemLayer's own pieces (wisp/ui/overlay.hpp): built once,
// next to lv_layer_top(), so it stays over whatever screen is loaded or
// slides in, and raised above whatever is already there (a confirm
// dialog opened before it) so later dialogs, appended after it, stay on
// top without any further raise().
//
// Shell owns the one instance and decides when to drive it: on_turn()/
// tick() while Home is not in front, on_home() while it is (and the
// moment it comes in front), hide() on every "scene.state" event (the
// scene that was in front may no longer be).
//
// Phase, from on_turn()'s VoiceState and TurnView: Listening while
// recording (a pulsing dot and the level bar), Thinking while
// transcribing, or once a turn is running with nothing newer to show
// ("思考中…", or the label of the agent's step under way,
// TurnView::running_step()), Saying once the turn's say text is new, and
// Note once a turn ends with a reason (TurnView::end_reason) or a voice
// error is new: the same short words Home's hint uses (not the agent's
// full note, which does not fit the small round screens). Saying and Note
// each hold for kOverlaySayMs (wisp/shell/limits.hpp) after what they show
// was first seen (a say: last changed); tick() ends them, and what one
// showed is not shown again when the view is next seen unchanged (the
// speech ending, say). A capture ends either at once: it is the user's
// next try. The text is spoken by others (ConversationController), not
// here.
//
// The card sits above the safe area's bottom edge and grows upward to fit
// its text, three lines at most, then the text ends with an ellipsis; at
// every height it keeps to the rows the screen shows.
//
// A tap calls `open_home` (Shell::show(Home): a scene in front is sent
// back first, as the back gesture does; a sub-page or the diagnostics
// close as their own way back closes them). The card takes only the
// presses on itself.
//
// UI thread only, like every lv_* call.

#include <cstdint>
#include <functional>
#include <optional>

#include "lvgl.h"
#include "wisp/hal/display_geometry.hpp"
#include "wisp/os/large.hpp"
#include "wisp/shell/conversation.hpp"
#include "wisp/ui/strings.hpp"
#include "wisp/voice/voice_service.hpp"

namespace wisp::ui {
class SystemLayer;
}  // namespace wisp::ui

namespace wisp::shell {

class TalkOverlay {
public:
    enum class Phase : uint8_t { Hidden, Listening, Thinking, Saying, Note };

    TalkOverlay(ui::SystemLayer& layer, const hal::ScreenShape& shape,
                std::function<void()> open_home);
    ~TalkOverlay();

    TalkOverlay(const TalkOverlay&) = delete;
    TalkOverlay& operator=(const TalkOverlay&) = delete;

    // Shows what `t`/`v`/`level` call for now.
    void on_turn(const TurnView& t, voice::VoiceState v, uint8_t level, uint64_t now_ms);
    // Saying or Note -> Hidden once kOverlaySayMs has passed since what it
    // shows was set.
    void tick(uint64_t now_ms);
    // Home in front: hidden, and what `t` holds now counts as seen, so it
    // does not show once Home goes.
    void on_home(const TurnView& t);
    // Hidden at once (e.g. the foreground scene changed).
    void hide();

    Phase phase() const { return phase_; }
    lv_obj_t* obj() const { return obj_; }

private:
    // What a Saying or Note shows, and since when.
    enum class Timed : uint8_t { None, Say, Note };

    // Takes in what is new in `t` since the last view seen: a new turn
    // ends what showed, a new say or outcome starts showing.
    void take(const TurnView& t, uint64_t now_ms);
    Phase decide(const TurnView& t, voice::VoiceState v) const;
    void apply(uint8_t level);
    // The card's size and place for the label's text (one line for the
    // dot and bar).
    void fit(const char* text);
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
    os::LargeString step_shown_;  // the running step's label, while thinking

    bool running_ = false;  // the last view's turn still runs

    // The last view seen: its turn, its say, how its last turn ended and
    // its voice error, so only what changed starts a Saying or a Note.
    uint32_t seen_turn_ = 0;
    uint64_t seen_started_ = 0;
    os::LargeString seen_say_;
    uint32_t seen_end_turn_ = 0;
    uint64_t seen_end_ms_ = 0;
    std::optional<voice::VoiceError> seen_error_;

    Timed timed_ = Timed::None;
    uint64_t timed_since_ = 0;
    ui::StrId note_ = ui::StrId::HomeTurnServer;
};

}  // namespace wisp::shell
