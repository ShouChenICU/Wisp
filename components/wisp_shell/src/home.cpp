#include "wisp/shell/home.hpp"

#include <algorithm>
#include <utility>

#include "wisp/core/log.hpp"
#include "wisp/shell/body_text.hpp"
#include "wisp/shell/limits.hpp"
#include "wisp/shell/mood.hpp"
#include "wisp/ui/fonts.hpp"
#include "wisp/ui/strings.hpp"
#include "wisp/ui/system_layer.hpp"
#include "wisp/ui/theme_tokens.hpp"
#include "wisp/ui/ui_runtime.hpp"
#include "wisp/ui/wisp_view.hpp"

namespace wisp::shell {

namespace {

constexpr const char* kTag = "shell";
// Side margin inside a band's visible row, and the gap between bands.
constexpr int32_t kPad = ui::theme::kGapMd;
constexpr int32_t kGap = ui::theme::kGapSm;
// Between the column and the hint below it: more than a line's leading,
// so a line the column cuts does not read as running into the hint.
constexpr int32_t kHintGap = ui::theme::kGapMd;

constexpr lv_area_t kEmpty{0, 0, -1, -1};

int32_t span_width(const hal::Span& s) { return s.empty() ? 0 : s.x2 - s.x1 + 1; }

// The rows text may use: the safe area's, grown up and down while rows
// stay at least `percent` of the screen wide.
void text_rows(const hal::ScreenShape& s, int32_t& top, int32_t& bottom,
               int32_t percent = kTextRowPercent) {
    const hal::Area safe = hal::safe_rect(s);
    const int32_t min_w = s.width * percent / 100;
    top = safe.y1;
    while (top > 0 && span_width(hal::row_span(s, top - 1)) >= min_w) --top;
    bottom = safe.y2;
    while (bottom < s.height - 1 && span_width(hal::row_span(s, bottom + 1)) >= min_w) ++bottom;
}

// Rows y1..y2, as wide as the narrower of its two end rows (the visible
// area is convex, so no row between is narrower), less kPad each side.
lv_area_t band(const hal::ScreenShape& s, int32_t y1, int32_t y2) {
    const hal::Span a = hal::row_span(s, y1);
    const hal::Span b = hal::row_span(s, y2);
    if (a.empty() || b.empty() || y2 < y1) return kEmpty;
    return lv_area_t{std::max(a.x1, b.x1) + kPad, y1, std::min(a.x2, b.x2) - kPad, y2};
}

lv_area_t wisp_box(const hal::Area& safe, int32_t size, int32_t cy) {
    const int32_t h = size * 5 / 4;
    const int32_t cx = (safe.x1 + safe.x2 + 1) / 2;
    const int32_t x1 = cx - size / 2;
    const int32_t y1 = cy - h / 2;
    return lv_area_t{x1, y1, x1 + size - 1, y1 + h - 1};
}

std::optional<agent::AgentState> parse_agent(const std::string& s) {
    using agent::AgentState;
    for (AgentState a : {AgentState::Idle, AgentState::Thinking, AgentState::Tool,
                         AgentState::Output, AgentState::WaitingConfirm, AgentState::Error}) {
        if (s == agent::to_string(a)) return a;
    }
    return std::nullopt;
}

std::optional<voice::VoiceState> parse_voice(const std::string& s) {
    using voice::VoiceState;
    for (VoiceState v : {VoiceState::Idle, VoiceState::Listening, VoiceState::Transcribing,
                         VoiceState::Speaking, VoiceState::Error}) {
        if (s == voice::to_string(v)) return v;
    }
    return std::nullopt;
}

std::string payload_str(const core::Event& e, const char* key) {
    auto it = e.payload.find(key);
    return it != e.payload.end() && it->second.type() == Value::Type::String
               ? it->second.as_string()
               : std::string();
}

ui::StrId error_text(voice::VoiceError e) {
    switch (e) {
        case voice::VoiceError::NoAsrProfile:
            return ui::StrId::VoiceErrNoAsr;
        case voice::VoiceError::NoTtsProfile:
            return ui::StrId::VoiceErrNoTts;
        case voice::VoiceError::Offline:
            return ui::StrId::VoiceErrOffline;
        case voice::VoiceError::AsrFailed:
            return ui::StrId::VoiceErrAsr;
        case voice::VoiceError::TtsFailed:
            return ui::StrId::VoiceErrTts;
        case voice::VoiceError::MicUnavailable:
            return ui::StrId::VoiceErrMic;
    }
    return ui::StrId::VoiceErrAsr;
}

// `a` in screen coordinates, `obj` in a parent whose top left is `origin`.
void place(lv_obj_t* obj, const lv_area_t& a, lv_point_t origin = {0, 0}) {
    lv_obj_set_pos(obj, a.x1 - origin.x, a.y1 - origin.y);
    lv_obj_set_size(obj, std::max<int32_t>(lv_area_get_width(&a), 0),
                    std::max<int32_t>(lv_area_get_height(&a), 0));
}

bool same(const os::LargeString& a, const os::LargeString& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
}

}  // namespace

HomeLayout home_layout(const hal::ScreenShape& s, bool in_turn, bool hint_visible) {
    const hal::Area safe = hal::safe_rect(s);
    int32_t top = 0, bottom = 0;
    text_rows(s, top, bottom);
    HomeLayout l{kEmpty, kEmpty, kEmpty, kEmpty, kEmpty, kEmpty};
    if (!in_turn) {
        l.wisp =
            wisp_box(safe, ui::wisp_size_px(safe, kWispIdlePercent), (safe.y1 + safe.y2 + 1) / 2);
        const int32_t y1 = l.wisp.y2 + 1 + kGap;
        if (hint_visible)
            l.hint = band(s, y1, std::min(y1 + kHintIdleLines * kBodyLinePx - 1, bottom - kGap));
        return l;
    }
    // Below the status capsule, which may show over the home screen.
    const hal::Area capsule = ui::SystemLayer::status_area(s);
    top = std::max(top + kGap, capsule.y2 + 1 + kGap);
    bottom -= kGap;
    // The turn's one-line hint, cut with "...", may use narrower rows than
    // the text: below the column where the screen has them.
    int32_t unused = 0, hint_bottom = 0;
    text_rows(s, unused, hint_bottom, kHintRowPercent);
    hint_bottom -= kGap;
    const int32_t hint_top = hint_bottom - kHintTurnLines * kBodyLinePx + 1;
    // Where heard and the wisp leave the column less than kMinColumnPx
    // with a hint, heard gets one line (the same with or without a hint,
    // so the column does not jump when one comes or goes).
    const int32_t size = ui::wisp_size_px(safe, kWispTurnPercent);
    const int32_t below_heard = kGap + size * 5 / 4 + kGap;
    const int32_t col_room = std::min(bottom, hint_top - 1 - kHintGap) -
                             (top + kHeardMaxLines * kBodyLinePx + below_heard) + 1;
    const int32_t heard_lines = col_room >= kMinColumnPx ? kHeardMaxLines : 1;
    l.heard = band(s, top, top + heard_lines * kBodyLinePx - 1);
    const int32_t wisp_top = l.heard.y2 + 1 + kGap;
    l.wisp = wisp_box(safe, size, wisp_top + size * 5 / 4 / 2);
    const int32_t say_top = l.wisp.y2 + 1 + kGap;
    int32_t col_bottom = bottom;
    if (hint_visible) {
        l.hint = band(s, std::max(hint_top, say_top), hint_bottom);
        col_bottom = std::max(say_top, std::min(bottom, l.hint.y1 - 1 - kHintGap));
    }
    l.column = band(s, say_top, col_bottom);
    l.say = l.column;
    l.say.y2 = std::min(l.column.y1 + kSayLinePx - 1, l.column.y2);
    l.more = l.column;
    l.more.y1 = std::max(l.column.y1, l.column.y2 - kMorePx + 1);
    return l;
}

// --- TurnFade -------------------------------------------------------------

void TurnFade::turn_started() {
    shown_ = true;
    running_ = true;
}

void TurnFade::turn_ended(uint64_t now) {
    if (!shown_) return;
    running_ = false;
    last_ = now;
}

void TurnFade::interacted(uint64_t now) {
    if (!running_ && !faded(now)) last_ = std::max(last_, now);
}

bool TurnFade::faded(uint64_t now) const {
    if (!shown_) return true;
    if (running_) return false;
    return now >= last_ + kFadeAfterMs;
}

uint64_t TurnFade::fades_at() const { return shown_ && !running_ ? last_ + kFadeAfterMs : 0; }

// --- Home -----------------------------------------------------------------

Home::Home(HomeDeps deps) : deps_(std::move(deps)) {
    if (!deps_.now_ms) deps_.now_ms = os::now_ms;
}

Home::~Home() = default;

bool Home::start() {
    if (screen_ != nullptr) return true;
    screen_ = lv_obj_create(nullptr);
    // No theme styles: the screen is one flat surface the wisp is drawn on
    // (it draws no background of its own).
    lv_obj_remove_style_all(screen_);
    const hal::PanelKind panel = ui::display_panel(lv_obj_get_display(screen_));
    lv_obj_set_style_bg_color(screen_, ui::wisp_background(panel), 0);
    lv_obj_set_style_bg_opa(screen_, LV_OPA_COVER, 0);
    // The theme's font went with its styles; the labels inherit this one
    // (the body font, with its CJK glyphs).
    lv_obj_set_style_text_font(screen_, ui::default_font(), 0);
    lv_obj_remove_flag(screen_, LV_OBJ_FLAG_SCROLLABLE);
    // A press stays with what it started on: LVGL would otherwise hand it
    // to whatever the finger slides onto, with a PRESS_LOST that ends the
    // swipe.
    lv_obj_add_flag(screen_, LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_add_event_cb(screen_, &Home::event_cb, LV_EVENT_ALL, this);

    const bool colour = hal::panel_is_colour(panel);
    const lv_color_t text = colour ? lv_color_hex(ui::theme::kText) : ui::wisp_colour(panel);
    const lv_color_t dim = colour ? lv_color_hex(ui::theme::kTextDim) : ui::wisp_colour(panel);

    // The turn's text, under the wisp so a press on the wisp reaches it;
    // faded as one, and only as large as the text (apply_layout()) since
    // the fade draws it through a layer. It takes no press itself:
    // presses fall through to the screen, and the column's bubble up to it.
    turn_box_ = lv_obj_create(screen_);
    lv_obj_remove_style_all(turn_box_);
    lv_obj_remove_flag(turn_box_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(turn_box_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(turn_box_, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_flag(turn_box_, LV_OBJ_FLAG_HIDDEN);

    heard_ = lv_label_create(turn_box_);
    lv_obj_set_style_text_color(heard_, dim, 0);
    lv_obj_set_style_text_align(heard_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(heard_, LV_LABEL_LONG_DOT);
    lv_label_set_text_static(heard_, "");

    column_ = lv_obj_create(turn_box_);
    lv_obj_remove_style_all(column_);
    lv_obj_set_flex_flow(column_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(column_, kGap, 0);
    lv_obj_set_scroll_dir(column_, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(column_, LV_SCROLLBAR_MODE_OFF);
    lv_obj_add_flag(column_, LV_OBJ_FLAG_EVENT_BUBBLE);
    lv_obj_add_flag(column_, LV_OBJ_FLAG_PRESS_LOCK);
    lv_obj_add_event_cb(column_, &Home::column_cb, LV_EVENT_SCROLL_BEGIN, this);
    lv_obj_add_event_cb(column_, &Home::column_cb, LV_EVENT_SCROLL, this);
    lv_obj_add_event_cb(column_, &Home::column_cb, LV_EVENT_SCROLL_END, this);

    say_ = lv_label_create(column_);
    lv_obj_set_width(say_, lv_pct(100));
    lv_obj_set_style_text_font(say_, ui::font(kSayFontPx), 0);
    lv_obj_set_style_text_color(say_, text, 0);
    lv_obj_set_style_text_align(say_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text_static(say_, "");
    lv_obj_add_flag(say_, LV_OBJ_FLAG_HIDDEN);

    // Over the column's bottom edge, not in it (it does not scroll), and
    // not clickable, so presses there still reach the column.
    if (colour) {
        more_ = lv_obj_create(turn_box_);
        lv_obj_remove_style_all(more_);
        const lv_color_t bg = ui::wisp_background(panel);
        lv_obj_set_style_bg_color(more_, bg, 0);
        lv_obj_set_style_bg_grad_color(more_, bg, 0);
        lv_obj_set_style_bg_grad_dir(more_, LV_GRAD_DIR_VER, 0);
        lv_obj_set_style_bg_main_opa(more_, LV_OPA_TRANSP, 0);
        lv_obj_set_style_bg_grad_opa(more_, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_opa(more_, LV_OPA_COVER, 0);
    } else {
        // Two-colour panels have no fade: a chevron, from the built-in
        // font (the body font has no symbols).
        more_ = lv_label_create(turn_box_);
        lv_obj_set_style_text_font(more_, LV_FONT_DEFAULT, 0);
        lv_obj_set_style_text_color(more_, text, 0);
        lv_obj_set_style_text_align(more_, LV_TEXT_ALIGN_CENTER, 0);
        lv_label_set_text_static(more_, LV_SYMBOL_DOWN);
    }
    lv_obj_remove_flag(more_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_flag(more_, LV_OBJ_FLAG_HIDDEN);

    hint_ = lv_label_create(screen_);
    // Cut to its band ("..." on the last line) rather than run over the
    // text above or off the screen.
    lv_label_set_long_mode(hint_, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(hint_, dim, 0);
    lv_obj_set_style_text_align(hint_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text_static(hint_, "");
    lv_obj_add_flag(hint_, LV_OBJ_FLAG_HIDDEN);

    const hal::Area safe = hal::safe_rect(deps_.shape);
    wisp_ = ui::WispView::create(screen_, ui::wisp_size_px(safe, kWispIdlePercent));
    if (wisp_ != nullptr) {
        lv_obj_set_align(wisp_->obj(), LV_ALIGN_TOP_LEFT);
        // Its presses reach the screen's handler too (the swipes), and
        // stay its own when the finger slides off it.
        lv_obj_add_flag(wisp_->obj(), LV_OBJ_FLAG_EVENT_BUBBLE);
        lv_obj_add_flag(wisp_->obj(), LV_OBJ_FLAG_PRESS_LOCK);
    } else {
        WLOG_E(kTag, "no memory for the wisp; the home screen is empty");
    }

    alive_ = std::make_shared<bool>(true);
    subs_[0] = deps_.bus.subscribe("agent.state", [this](const core::Event& e) { on_bus(e); });
    subs_[1] = deps_.bus.subscribe("voice.state", [this](const core::Event& e) { on_bus(e); });
    subs_[2] = deps_.bus.subscribe("power.state", [this](const core::Event& e) { on_bus(e); });
    timer_ = lv_timer_create(&Home::timer_cb, kTickMs, this);

    laid_out_ = false;
    refresh();
    return true;
}

void Home::stop() {
    if (screen_ == nullptr) return;
    for (core::SubscriptionId& id : subs_) {
        if (id != 0) deps_.bus.unsubscribe(id);
        id = 0;
    }
    set_conversation(nullptr);
    *alive_ = false;
    alive_.reset();
    lv_timer_delete(timer_);
    timer_ = nullptr;
    lv_obj_delete(screen_);  // frees the wisp with its object, and the fade
    screen_ = turn_box_ = heard_ = column_ = say_ = more_ = hint_ = nullptr;
    blocks_.clear();
    wisp_ = nullptr;
}

void Home::set_conversation(ConversationController* conv) {
    if (conv == conv_) return;
    if (conv_ != nullptr && listener_ != 0) conv_->remove_listener(listener_);
    listener_ = 0;
    conv_ = conv;
    view_ = TurnView{};
    view_changed_ = true;
    if (conv_ != nullptr) {
        listener_ = conv_->add_listener([this](const TurnView&) { request(true); });
        conv_dirty_.store(true);
    }
    if (screen_ != nullptr) refresh();
}

void Home::show_view(const TurnView& v) {
    view_ = v;
    view_changed_ = true;
    refresh();
}

// --- refresh requests ------------------------------------------------------

void Home::request(bool conv_changed) {
    if (conv_changed) conv_dirty_.store(true);
    if (posted_.exchange(true)) return;
    std::shared_ptr<bool> alive = alive_;
    if (!alive) {
        posted_.store(false);
        return;
    }
    // A full queue: the timer's next tick picks the change up.
    if (!deps_.ui.post([this, alive] {
            if (!*alive) return;
            posted_.store(false);
            on_request();
        }))
        posted_.store(false);
}

void Home::on_request() {
    const uint64_t now = deps_.now_ms();
    const uint64_t since = now - last_refresh_;
    if (since >= kRefreshMs || now < last_refresh_) {
        refresh();
        return;
    }
    // Held back: the timer runs it once the interval has passed.
    if (!quick_) {
        quick_ = true;
        lv_timer_set_period(timer_, static_cast<uint32_t>(kRefreshMs - since));
        lv_timer_reset(timer_);
    }
}

void Home::timer_cb(lv_timer_t* t) {
    auto* self = static_cast<Home*>(lv_timer_get_user_data(t));
    if (self->quick_) {
        self->quick_ = false;
        lv_timer_set_period(t, kTickMs);
    }
    self->refresh();
}

void Home::on_bus(const core::Event& e) {
    // The bus's dispatch thread: note it and ask for a refresh.
    const std::string state = payload_str(e, "state");
    if (e.topic == "agent.state") {
        if (auto a = parse_agent(state)) {
            agent_state_.store(*a);
            if (*a == agent::AgentState::Error)
                error_at_.store(std::max<uint64_t>(deps_.now_ms(), 1));
        }
    } else if (e.topic == "voice.state") {
        if (auto v = parse_voice(state)) {
            voice_state_.store(*v);
            if (*v == voice::VoiceState::Error)
                error_at_.store(std::max<uint64_t>(deps_.now_ms(), 1));
            auto it = e.payload.find("level");
            level_.store(
                it != e.payload.end() && it->second.type() == Value::Type::Int
                    ? static_cast<uint8_t>(std::clamp<int64_t>(it->second.as_int(), 0, 100))
                    : 0);
        }
    } else if (e.topic == "power.state") {
        standby_.store(state == "standby" || state == "deep_standby");
    } else {
        return;
    }
    request(false);
}

// --- refresh ----------------------------------------------------------------

void Home::refresh() {
    if (screen_ == nullptr) return;
    const uint64_t now = deps_.now_ms();
    last_refresh_ = now;

    if (conv_dirty_.exchange(false) && conv_ != nullptr) {
        view_ = conv_->current();
        view_changed_ = true;
    }
    bool new_turn = false;
    if (view_changed_) {
        // A new turn: another turn number, or (before the agent numbers
        // it) another heard text.
        const bool has_turn = view_.running || view_.turn != 0 || !view_.heard.empty();
        if (has_turn && (view_.turn != shown_turn_ || view_.heard != shown_turn_heard_ ||
                         (view_.running && !was_running_))) {
            shown_turn_ = view_.turn;
            shown_turn_heard_ = view_.heard;
            fade_.turn_started();
            was_running_ = true;
            new_turn = true;
        }
        if (was_running_ && !view_.running) {
            fade_.turn_ended(now);
            was_running_ = false;
        }
        // The hint follows the last error or "not heard" from when it is
        // first seen.
        if (view_.error != hint_error_ || view_.not_heard != hint_not_heard_) {
            hint_error_ = view_.error;
            hint_not_heard_ = view_.not_heard;
            hint_since_ = now;
        }
    }

    // The wisp.
    if (wisp_ != nullptr) {
        const uint64_t err = error_at_.load();
        const bool recent_error = err != 0 && now >= err && now - err < kErrorMoodMs;
        const agent::AgentState a = agent_state_.load();
        const voice::VoiceState v = voice_state_.load();
        const ui::WispMood mood = mood_for(a, v, recent_error);
        wisp_->set_mood(mood);
        wisp_->set_level(
            mood == ui::WispMood::Listening || mood == ui::WispMood::Speaking ? level_.load() : 0);
        wisp_->set_busy(a == agent::AgentState::Tool);
        wisp_->set_paused(standby_.load());
    }

    const bool has_text = !view_.heard.empty() || !view_.say.empty() || !view_.body.empty();
    const bool in_turn = (view_.running || has_text) && !fade_.faded(now);

    // While the turn's text shows, the error hint's wait does not run.
    if (in_turn && (hint_error_ || hint_not_heard_) && now - hint_since_ < kFadeAfterMs)
        hint_since_ = now;
    // The short form while the turn layout is up, fading included.
    const std::string hint = hint_text(now, in_turn || shown_in_turn_);
    const bool hint_visible = !hint.empty();
    if (hint != shown_hint_) {
        shown_hint_ = hint;
        lv_label_set_text(hint_, hint.c_str());
    }
    if (in_turn) {
        if (fading_) {
            lv_anim_delete(turn_box_, nullptr);
            fading_ = false;
        }
        lv_obj_set_style_opa(turn_box_, LV_OPA_COVER, 0);
        if (!shown_in_turn_ || hint_visible != hint_visible_ || !laid_out_) {
            apply_layout(true, hint_visible);
        }
    } else if (shown_in_turn_) {
        if (!fading_) start_fade_out();
    } else if (!laid_out_ || hint_visible != hint_visible_) {
        apply_layout(false, hint_visible);
    }
    if (view_changed_) {
        view_changed_ = false;
        render_text();
        // A new turn reads from its start, wherever the last one was left.
        if (new_turn) lv_obj_scroll_to_y(column_, 0, LV_ANIM_OFF);
        if (shown_in_turn_) {
            lv_obj_update_layout(screen_);
            update_more();
        }
    }
}

std::string Home::hint_text(uint64_t now, bool in_turn) {
    // One line during a turn: the address waits for the idle layout.
    auto with_url = [&](ui::StrId id) {
        std::string s = ui::tr(id);
        if (in_turn || !deps_.admin_url) return s;
        const std::string url = deps_.admin_url();
        if (!url.empty()) s += "\n" + url;
        return s;
    };
    switch (conv_ != nullptr ? conv_->readiness() : Readiness::Ready) {
        case Readiness::NoWifi:
            return ui::tr(ui::StrId::HomeNoWifi);
        case Readiness::NoLlm:
            return with_url(ui::StrId::HomeNoLlm);
        case Readiness::Ready:
            break;
    }
    if (now - hint_since_ >= kFadeAfterMs) return {};
    if (hint_error_) {
        const voice::VoiceError e = *hint_error_;
        if (e == voice::VoiceError::NoAsrProfile || e == voice::VoiceError::NoTtsProfile)
            return with_url(error_text(e));
        return ui::tr(error_text(e));
    }
    if (hint_not_heard_) return ui::tr(ui::StrId::HomeNotHeard);
    return {};
}

void Home::apply_layout(bool in_turn, bool hint_visible) {
    laid_out_ = true;
    shown_in_turn_ = in_turn;
    hint_visible_ = hint_visible;
    const HomeLayout l = home_layout(deps_.shape, in_turn, hint_visible);
    if (wisp_ != nullptr) {
        wisp_->set_size(lv_area_get_width(&l.wisp));
        lv_obj_set_pos(wisp_->obj(), l.wisp.x1, l.wisp.y1);
    }
    place(hint_, l.hint);
    if (hint_visible) {
        lv_obj_remove_flag(hint_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(hint_, LV_OBJ_FLAG_HIDDEN);
    }
    if (!in_turn) {
        lv_obj_add_flag(turn_box_, LV_OBJ_FLAG_HIDDEN);
        // No finger on it: column_cb() does not take this for reading.
        lv_obj_scroll_to_y(column_, 0, LV_ANIM_OFF);
        lv_obj_update_layout(screen_);
        return;
    }
    const lv_area_t box{std::min(l.heard.x1, l.column.x1), l.heard.y1,
                        std::max(l.heard.x2, l.column.x2), l.column.y2};
    place(turn_box_, box);
    const lv_point_t origin{box.x1, box.y1};
    place(heard_, l.heard, origin);
    place(column_, l.column, origin);
    place(more_, l.more, origin);
    lv_obj_set_style_opa(turn_box_, LV_OPA_COVER, 0);
    lv_obj_remove_flag(turn_box_, LV_OBJ_FLAG_HIDDEN);
    // Now, not at the next frame: a press arriving before it is hit-tested
    // against these positions.
    lv_obj_update_layout(screen_);
    update_more();
}

void Home::update_more() {
    if (more_ == nullptr) return;
    if (lv_obj_get_scroll_bottom(column_) > 0) {
        lv_obj_remove_flag(more_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(more_, LV_OBJ_FLAG_HIDDEN);
    }
}

void Home::start_fade_out() {
    fading_ = true;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, turn_box_);
    lv_anim_set_values(&a, LV_OPA_COVER, LV_OPA_TRANSP);
    lv_anim_set_duration(&a, kFadeOutMs);
    lv_anim_set_exec_cb(&a, [](void* obj, int32_t v) {
        lv_obj_set_style_opa(static_cast<lv_obj_t*>(obj), static_cast<lv_opa_t>(v), 0);
    });
    lv_anim_set_user_data(&a, this);
    lv_anim_set_completed_cb(&a, &Home::fade_done_cb);
    lv_anim_start(&a);
}

void Home::fade_done_cb(lv_anim_t* a) {
    auto* self = static_cast<Home*>(lv_anim_get_user_data(a));
    self->fading_ = false;
    self->apply_layout(false, self->hint_visible_);
    self->refresh();  // the hint's full form, now the layout is idle
}

void Home::render_text() {
    if (view_.heard != shown_heard_) {
        shown_heard_ = view_.heard;
        lv_label_set_text(heard_, shown_heard_.c_str());
    }
    if (!same(view_.say, shown_say_)) {
        shown_say_ = view_.say;
        lv_label_set_text(say_, shown_say_.c_str());
        if (shown_say_.empty()) {
            lv_obj_add_flag(say_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(say_, LV_OBJ_FLAG_HIDDEN);
        }
    }
    if (!same(view_.body, shown_body_)) {
        shown_body_ = view_.body;
        render_body();
    }
}

void Home::render_body() {
    const os::LargeVector<BodyBlock> blocks =
        light_markdown(std::string_view(shown_body_.data(), shown_body_.size()));
    const hal::PanelKind panel = ui::display_panel(lv_obj_get_display(screen_));
    const bool colour = hal::panel_is_colour(panel);
    const lv_color_t text = colour ? lv_color_hex(ui::theme::kText) : ui::wisp_colour(panel);
    while (blocks_.size() > blocks.size()) {
        lv_obj_delete(blocks_.back());
        blocks_.pop_back();
    }
    while (blocks_.size() < blocks.size()) {
        lv_obj_t* l = lv_label_create(column_);
        lv_obj_set_width(l, lv_pct(100));
        lv_obj_set_style_text_color(l, text, 0);
        blocks_.push_back(l);
    }
    for (size_t i = 0; i < blocks.size(); ++i) {
        lv_obj_t* l = blocks_[i];
        const BodyBlock& b = blocks[i];
        const char* old = lv_label_get_text(l);
        if (old == nullptr ||
            std::string_view(old) != std::string_view(b.text.data(), b.text.size()))
            lv_label_set_text(l, b.text.c_str());
        // No monospaced font on the device: code is the body font on a
        // muted background (a frame on two-colour panels).
        if (b.code) {
            lv_obj_set_style_pad_all(l, kGap, 0);
            lv_obj_set_style_radius(l, ui::theme::kRadiusSm, 0);
            if (colour) {
                lv_obj_set_style_bg_color(l, lv_color_hex(ui::theme::kMutedSoft), 0);
                lv_obj_set_style_bg_opa(l, LV_OPA_COVER, 0);
            } else {
                lv_obj_set_style_border_color(l, text, 0);
                lv_obj_set_style_border_width(l, 1, 0);
            }
        } else {
            lv_obj_set_style_pad_all(l, 0, 0);
            lv_obj_set_style_bg_opa(l, LV_OPA_TRANSP, 0);
            lv_obj_set_style_border_width(l, 0, 0);
        }
    }
}

// --- touch ------------------------------------------------------------------

void Home::event_cb(lv_event_t* e) { static_cast<Home*>(lv_event_get_user_data(e))->on_event(e); }

void Home::column_cb(lv_event_t* e) {
    auto* self = static_cast<Home*>(lv_event_get_user_data(e));
    const lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_SCROLL_BEGIN) self->update_more();
    // Only a finger on the column is reading (a drag, or its throw after
    // the release): LVGL scrolls it too, for the reset when the text
    // fades and for layout, and those must not keep the text up.
    if (lv_indev_active() == nullptr) return;
    if (code == LV_EVENT_SCROLL_BEGIN) self->scrolled_ = true;
    // Reading the body keeps the turn on the screen.
    self->fade_.interacted(self->deps_.now_ms());
}

void Home::act(WispPress::Action a) {
    if (conv_ == nullptr) return;
    switch (a) {
        case WispPress::Action::Tap:
            // No voice effect; before setup the hint below the wisp is
            // already up and says what is missing.
            conv_->tap();
            break;
        case WispPress::Action::HoldBegin:
            conv_->hold_begin();
            break;
        case WispPress::Action::HoldEnd:
            conv_->hold_end();
            break;
        case WispPress::Action::HoldCancel:
            conv_->hold_cancel();
            break;
        case WispPress::Action::None:
            break;
    }
}

void Home::on_event(lv_event_t* e) {
    const lv_event_code_t code = lv_event_get_code(e);
    if (code != LV_EVENT_PRESSED && code != LV_EVENT_PRESSING && code != LV_EVENT_RELEASED &&
        code != LV_EVENT_PRESS_LOST)
        return;
    lv_indev_t* indev = lv_indev_active();
    if (indev == nullptr) return;
    lv_point_t p;
    lv_indev_get_point(indev, &p);
    const uint64_t now = deps_.now_ms();
    switch (code) {
        case LV_EVENT_PRESSED:
            tracking_ = true;
            start_ = p;
            from_top_zone_ = ui::SystemLayer::in_top_zone(deps_.shape, p.x, p.y);
            scrolled_ = false;
            key_cancelled_ = false;
            on_wisp_ = wisp_ != nullptr && lv_event_get_target_obj(e) == wisp_->obj();
            if (on_wisp_) act(press_.pressed(p, now));
            break;
        case LV_EVENT_PRESSING:
            if (!tracking_) break;
            if (on_wisp_) act(press_.moved(p, now));
            // The talk key is held (not the wisp): a swipe up drops it.
            if (conv_ != nullptr && !key_cancelled_ && !press_.holding() && conv_->holding() &&
                classify_swipe(start_, p) == Swipe::Up) {
                key_cancelled_ = true;
                conv_->hold_cancel();
            }
            break;
        case LV_EVENT_RELEASED: {
            if (!tracking_) break;
            tracking_ = false;
            bool held = false;
            if (on_wisp_) {
                act(press_.released(p, now));
                held = press_.held();
            }
            const Swipe s = classify_swipe(start_, p);
            if (key_cancelled_ || held || scrolled_) break;
            if (s == Swipe::Up && conv_ != nullptr && conv_->holding()) {
                conv_->hold_cancel();
                break;
            }
            if (auto page = navigate(Page::Home, s, from_top_zone_, true); page && deps_.navigate)
                deps_.navigate(*page);
            break;
        }
        case LV_EVENT_PRESS_LOST:
            if (!tracking_) break;
            tracking_ = false;
            if (on_wisp_) act(press_.lost());
            break;
        default:
            break;
    }
}

}  // namespace wisp::shell
