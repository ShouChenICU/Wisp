#pragma once

// The home screen (MVP spec 6.2, 6.3): the wisp, the current turn's text,
// a one-line hint below the wisp, and the home screen's gestures.
//
// Layout (home_layout()). Idle, the wisp is kWispIdlePercent of the safe
// area's short side, centred across the safe area with its centre
// kWispIdleCentrePercent down it, and the hint band (up to kHintIdleLines)
// below it. During a turn the wisp shrinks to
// kWispTurnPercent and moves up; above it the heard text (kTextDim, at
// most kHeardMaxLines lines, cut with "..."), below it one scrolling
// column: the say text at kSayFontPx, wrapped, then the body
// (light_markdown(), the 16 px body font). The say text is never cut: it
// is also what is spoken and kept in the history, so it scrolls with the
// body. While the column has more below, a strip at its bottom says so (a
// fade to the background on colour panels, a chevron on the others). A
// hint during a turn takes kHintTurnLines at the bottom, without the admin
// address; the full hint shows once the text has faded. Every band is as
// wide as the narrowest visible row it covers (hal::row_span()), less a
// margin, and lies on rows at least kTextRowPercent of the screen wide, so
// text never runs off a rounded or circular screen, and the column's first
// and last lines are fully visible at its two scroll ends (spec 5.6 of the
// base spec).
//
// The agent's steps (TurnView::steps, activity.hpp). While a turn that
// has had a step runs and no say or body text has come yet (Activity::
// List), the column's room shows the newest steps that fit, one line each
// (cut with "..." to that row's visible width) behind a status mark: a dot
// while it runs (pulsing on colour panels), a tick once done, a cross when
// it failed; and a passing "思考中" step while the model works on the
// next one. Under them a muted line counts the steps and the time ("第 N
// 步 · 42 秒", redrawn on the clock's ticks). Once text comes, and after
// the turn (Activity::Summary), only that line stays, as the column's
// first line, scrolling with it ("用了 N 步 · 1 分 12 秒" once the turn is
// over). A turn without steps shows neither. Neither moves anything else:
// the layout is the same with or without them.
//
// The turn's text fades kFadeAfterMs after the later of the turn's end
// and the last time a finger scrolled the column, and the wisp goes back
// to the centre. Nothing else keeps it, and nothing but a new turn brings
// it back: a press on the wisp after the fade (a tap, or a hold to talk)
// leaves the screen idle. Turns from the web page show the same way; the
// controller only leaves them unspoken.
//
// The hint: readiness first (NoWifi: HomeNoWifi; NoLlm: HomeNoLlm and the
// admin address), else the controller's last voice error (VoiceErr*, with
// the address for a missing profile) or "not heard" (HomeNotHeard), for
// kFadeAfterMs after it was first seen; while the turn's text shows the
// wait does not run, so the full hint still gets its time after the fade.
//
// The wisp's mood follows "agent.state" and "voice.state" (mood_for());
// listening and speaking pass on their level, a running tool sets
// set_busy(true), an "error" state shows Error for kErrorMoodMs. With the
// screen off ("power.state" standby or deep_standby) the wisp is paused.
//
// Gestures (spec 6.3), tracked on the screen object from its own and its
// children's PRESSED / PRESSING / RELEASED events; PRESS_LOST (the system
// layer took the press) drops the press:
//   - the page drags to the sub-pages are the shell's (wisp/shell/shell.hpp),
//     which asks may_drag() before it starts one and tells drag_started()
//     when it has: a drag is no tap and no hold on the wisp;
//   - a press on the wisp goes through WispPress: HoldBegin / HoldEnd /
//     HoldCancel -> the controller's hold_begin() / hold_end() /
//     hold_cancel(); a tap -> tap(), which has no voice effect;
//   - while the controller is holding for the talk key, a swipe up past
//     kSwipeMinPx anywhere on the screen -> hold_cancel().
//
// Threading. start(), stop(), set_conversation(), refresh() and every
// LVGL callback run on the UI thread. The controller's listener and the
// bus handlers run on their own threads: they only note what changed and
// post one refresh at a time; refreshes run at most every kRefreshMs (a
// later one waits on the home screen's timer), which also re-checks the
// clock every kTickMs for the fade, the error mood and the readiness. A
// post still queued after stop() does nothing.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "lvgl.h"
#include "wisp/agent/agent_service.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/hal/display_geometry.hpp"
#include "wisp/os/large.hpp"
#include "wisp/shell/conversation.hpp"
#include "wisp/shell/gestures.hpp"
#include "wisp/shell/shell.hpp"
#include "wisp/voice/voice_service.hpp"

namespace wisp::ui {
class UiRuntime;
class WispView;
}  // namespace wisp::ui

namespace wisp::shell {

// Line heights the layout reserves: the 16 px body font, and the say text
// at kSayFontPx.
inline constexpr int32_t kBodyLinePx = 20, kSayLinePx = 36;
// Text bands only use rows at least this wide, in percent of the screen;
// the one-line hint during a turn, at least kHintRowPercent.
inline constexpr int32_t kTextRowPercent = 60, kHintRowPercent = 45;
// The turn's column holds at least the say text's first two lines and,
// under them, the body's first line, on any screen; where it would not,
// the heard text gets one line.
inline constexpr int32_t kMinColumnPx = 2 * kSayLinePx + 8 + kBodyLinePx;
// The hint's lines below the idle wisp, and during a turn.
inline constexpr int32_t kHintIdleLines = 3, kHintTurnLines = 1;
// The column's "more below" strip.
inline constexpr int32_t kMorePx = 16;
// The step list's rows: the steps kept and the summary line.
inline constexpr size_t kActivityRows = kTurnSteps + 1;

// What the turn shows of the agent's steps (see the file header).
enum class Activity : uint8_t { None, List, Summary };

struct HomeLayout {
    static constexpr lv_area_t kNone{0, 0, -1, -1};
    lv_area_t wisp = kNone;   // the wisp's box (its width is its size)
    lv_area_t heard = kNone;  // in a turn: the heard text
    // In a turn: the say text's first line, at the top of the column.
    lv_area_t say = kNone;
    // In a turn: the scrolling column, say and body together, above the
    // hint when one shows.
    lv_area_t column = kNone;
    // In a turn: the "more below" strip, the bottom kMorePx of the column.
    lv_area_t more = kNone;
    lv_area_t hint = kNone;  // the hint band, when one shows
    // Activity::List: the step rows and then the summary row, one line
    // each, top down over the (empty) column, as many as fit in it (at
    // most kActivityRows). Unused otherwise: the summary alone is the
    // column's first line.
    std::array<lv_area_t, kActivityRows> rows{};
    size_t row_count = 0;
};

// Screen coordinates of what is drawn. Idle (`in_turn` false) leaves
// heard, say, column and more empty (x1 > x2), and rows unused; hint is
// empty unless `hint_visible`.
HomeLayout home_layout(const hal::ScreenShape& s, bool in_turn, bool hint_visible,
                       Activity activity = Activity::None);

// What `v` shows of its steps: None without any, List while it runs and
// no say or body text has come, Summary otherwise.
Activity activity_for(const TurnView& v);

// When the turn's text fades: kFadeAfterMs after the later of the turn's
// end and the last interaction. Never while a turn runs; before the first
// turn there is nothing to show (faded). Once faded, an interaction
// changes nothing: only the next turn shows text again.
class TurnFade {
public:
    void turn_started();
    void turn_ended(uint64_t now);
    void interacted(uint64_t now);
    bool faded(uint64_t now) const;
    // When faded() turns true; 0 while a turn runs or before any.
    uint64_t fades_at() const;

private:
    bool shown_ = false;
    bool running_ = false;
    uint64_t last_ = 0;
};

struct HomeDeps {
    ui::UiRuntime& ui;
    hal::ScreenShape shape;
    core::EventBus& bus;
    std::function<uint64_t()> now_ms;
    // "http://<ip>:<port>", or "" while there is none. UI thread.
    std::function<std::string()> admin_url;
};

class Home {
public:
    static constexpr uint32_t kRefreshMs = 50, kTickMs = 250, kFadeOutMs = 300;

    explicit Home(HomeDeps deps);
    // Makes no LVGL call: stop() first when started.
    ~Home();

    Home(const Home&) = delete;
    Home& operator=(const Home&) = delete;

    // Builds the screen (not loaded) and subscribes. False without memory
    // for it. UI thread.
    bool start();
    // Unsubscribes, lets go of the controller and deletes the screen
    // (which must not be the active one). Idempotent. UI thread.
    void stop();

    // Follows `conv` (nullptr: none, no readiness hint and no voice). UI
    // thread.
    void set_conversation(ConversationController* conv);

    // Re-reads what changed and the clock, and shows it. UI thread.
    void refresh();
    // Shows `v` as if the controller had reported it, until it reports
    // again (tests). UI thread.
    void show_view(const TurnView& v);

    lv_obj_t* screen() const { return screen_; }
    // Everything the screen shows, in one container as large as the
    // screen (page_frame.hpp's make_root()): what a page drag moves.
    lv_obj_t* root() const { return root_; }

    // The press under way: whether a page drag may take it (not while it
    // holds the wisp to talk, or after it cancelled the talk key's hold),
    // and that one did. UI thread, from inside the press's events.
    bool may_drag() const;
    void drag_started();
    ui::WispView* wisp() const { return wisp_; }
    // For tests: the labels (heard, say, hint), the column, its "more
    // below" strip and whether the turn layout is showing.
    lv_obj_t* heard_label() const { return heard_; }
    lv_obj_t* say_label() const { return say_; }
    lv_obj_t* hint_label() const { return hint_; }
    lv_obj_t* column() const { return column_; }
    lv_obj_t* more() const { return more_; }
    bool in_turn() const { return shown_in_turn_; }
    // The step rows (i < kTurnSteps + 1: the steps and a passing step),
    // their status marks, the summary line and what the layout shows.
    lv_obj_t* step_mark(size_t i) const { return rows_[i].mark; }
    lv_obj_t* step_label(size_t i) const { return rows_[i].text; }
    lv_obj_t* summary_label() const { return summary_; }
    Activity activity() const { return shown_activity_; }

private:
    void request(bool conv_changed);
    void on_request();
    void on_bus(const core::Event& e);
    void apply_layout(bool in_turn, bool hint_visible);
    void render_text();
    void render_body();
    void render_activity(uint64_t now);
    // Sets row `i`'s mark and label; the width they take unclipped.
    int32_t fill_row(size_t i, std::string_view label, TurnStep::State st);
    // Places row `i` on `a` (turn_box_ coordinates) as part of a block
    // `block_w` wide, centred on it.
    void place_row(size_t i, const lv_area_t& a, int32_t block_w);
    void hide_row(size_t i);
    struct Row;
    void set_pulse(Row& r, bool on);
    std::string hint_text(uint64_t now, bool in_turn);
    void update_more();
    void start_fade_out();
    void act(WispPress::Action a);

    static void event_cb(lv_event_t* e);
    static void column_cb(lv_event_t* e);
    static void timer_cb(lv_timer_t* t);
    static void fade_done_cb(lv_anim_t* a);
    void on_event(lv_event_t* e);

    HomeDeps deps_;
    lv_obj_t* screen_ = nullptr;
    lv_obj_t* root_ = nullptr;
    lv_obj_t* turn_box_ = nullptr;  // heard and column; faded as one
    lv_obj_t* heard_ = nullptr;
    lv_obj_t* column_ = nullptr;
    lv_obj_t* say_ = nullptr;
    std::vector<lv_obj_t*> blocks_;  // body blocks, in the column after say_
    lv_obj_t* more_ = nullptr;       // the column's "more below" strip
    struct Row {
        lv_obj_t* mark = nullptr;
        lv_obj_t* text = nullptr;
        bool pulsing = false;
        int32_t text_w = 0;  // the label's width unclipped
    };
    // In turn_box_: the step rows (the steps kept and a passing one).
    std::array<Row, kTurnSteps + 1> rows_{};
    // The summary line: in turn_box_ under the list, or the column's
    // first child (Activity::Summary).
    lv_obj_t* summary_ = nullptr;
    lv_obj_t* hint_ = nullptr;
    ui::WispView* wisp_ = nullptr;  // owned by its object
    lv_timer_t* timer_ = nullptr;
    bool quick_ = false;  // timer_ runs a held-back refresh

    ConversationController* conv_ = nullptr;
    uint32_t listener_ = 0;
    core::SubscriptionId subs_[3] = {0, 0, 0};
    std::shared_ptr<bool> alive_;

    // Written by the bus handlers, read on the UI thread.
    std::atomic<agent::AgentState> agent_state_{agent::AgentState::Idle};
    std::atomic<voice::VoiceState> voice_state_{voice::VoiceState::Idle};
    std::atomic<uint8_t> level_{0};
    std::atomic<uint64_t> error_at_{0};  // 0: never
    std::atomic<bool> standby_{false};
    std::atomic<bool> conv_dirty_{false};
    std::atomic<bool> posted_{false};

    // UI thread.
    uint64_t last_refresh_ = 0;
    TurnView view_;
    bool view_changed_ = false;
    uint32_t shown_turn_ = 0;
    std::string shown_turn_heard_;
    bool was_running_ = false;
    TurnFade fade_;
    bool shown_in_turn_ = false;
    Activity shown_activity_ = Activity::None;
    HomeLayout layout_;
    lv_point_t box_origin_{0, 0};  // turn_box_'s top left on the screen
    bool fading_ = false;
    bool hint_visible_ = false;
    bool laid_out_ = false;
    std::string shown_heard_;
    os::LargeString shown_say_, shown_body_;
    std::string shown_hint_;
    // The voice error / not-heard the hint shows, and since when.
    std::optional<voice::VoiceError> hint_error_;
    bool hint_not_heard_ = false;
    uint64_t hint_since_ = 0;

    // The press being tracked.
    bool tracking_ = false;
    bool on_wisp_ = false;
    bool scrolled_ = false;
    bool key_cancelled_ = false;
    lv_point_t start_{};
    WispPress press_;
};

}  // namespace wisp::shell
