#include "wisp/shell/talk_overlay.hpp"

#include <algorithm>
#include <utility>

#include "outcome_text.hpp"
#include "wisp/shell/limits.hpp"
#include "wisp/ui/fonts.hpp"
#include "wisp/ui/strings.hpp"
#include "wisp/ui/system_layer.hpp"
#include "wisp/ui/theme_tokens.hpp"

namespace wisp::shell {

namespace {

// The card's least height (one line, or the dot and bar), how far it sits
// above the safe area's bottom edge, and the gap each side inside the row
// it draws on.
constexpr int32_t kCardH = 56;
constexpr int32_t kBottomGap = ui::theme::kGapMd;
constexpr int32_t kPad = ui::theme::kGapMd;
// Above and below the text once it takes more than a line.
constexpr int32_t kPadV = ui::theme::kGapMd;
constexpr int32_t kRowGap = ui::theme::kGapSm;
constexpr int32_t kDotSize = 14;
constexpr int32_t kBarH = 10;
constexpr int32_t kMaxLines = 3;
// Set on the label rather than inherited, so talk_overlay_top() knows the
// tallest card without a label to ask.
constexpr int32_t kLineSpace = 0;
constexpr uint32_t kPulseMs = 900;

// The card's box, `h` pixels tall (no taller than the safe area), its
// bottom kBottomGap above the safe area's bottom; as wide as the narrower
// of its top and bottom row (the visible area is convex, so no row
// between is narrower), less kPad each side -- the same approach as
// wisp/shell/home.cpp's band(). A taller card is narrower on a round
// screen.
lv_area_t overlay_rect(const hal::ScreenShape& s, int32_t h) {
    const hal::Area safe = hal::safe_rect(s);
    const int32_t y2 = safe.y2 - kBottomGap;
    const int32_t y1 = std::max(y2 - h + 1, safe.y1);
    const hal::Span a = hal::row_span(s, y1);
    const hal::Span b = hal::row_span(s, y2);
    if (a.empty() || b.empty()) return {safe.x1, y1, safe.x2, y2};
    return {std::max(a.x1, b.x1) + kPad, y1, std::min(a.x2, b.x2) - kPad, y2};
}

// The text's height at `lines` lines.
int32_t text_height(int32_t lines) {
    return lines * lv_font_get_line_height(ui::default_font()) + (lines - 1) * kLineSpace;
}

// The card's height for text `text_h` high.
int32_t card_height(int32_t text_h) {
    return std::max(kCardH, text_h + 2 * (kPadV + ui::theme::kBorderWidth));
}

bool same(const os::LargeString& a, const os::LargeString& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
}

}  // namespace

int32_t talk_overlay_top(const hal::ScreenShape& shape) {
    return overlay_rect(shape, card_height(text_height(kMaxLines))).y1;
}

int32_t talk_overlay_end_room(const hal::ScreenShape& shape) {
    return shape.height - talk_overlay_top(shape) + ui::theme::kGapSm;
}

TalkOverlay::TalkOverlay(ui::SystemLayer& layer, const hal::ScreenShape& shape,
                         std::function<void()> open_home)
    : layer_(layer), shape_(shape), open_home_(std::move(open_home)) {
    obj_ = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(obj_);
    lv_obj_remove_flag(obj_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_radius(obj_, ui::theme::kRadiusLg, 0);
    lv_obj_set_style_bg_color(obj_, lv_color_hex(ui::theme::kSurface2), 0);
    lv_obj_set_style_bg_opa(obj_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(obj_, lv_color_hex(ui::theme::kBorderStrong), 0);
    lv_obj_set_style_border_width(obj_, ui::theme::kBorderWidth, 0);
    lv_obj_set_style_pad_hor(obj_, kPad, 0);
    // No column gap: LVGL's flex row keeps one before every child but the
    // first even while the children ahead of it are hidden, which would
    // push the label (shown alone) off centre. The dot keeps its distance
    // from the bar with a margin instead (on the dot: a growing item's own
    // margin is not taken from the space it grows into).
    lv_obj_set_style_pad_column(obj_, 0, 0);
    lv_obj_set_flex_flow(obj_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(obj_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_event_cb(obj_, &TalkOverlay::click_cb, LV_EVENT_CLICKED, this);
    lv_obj_add_flag(obj_, LV_OBJ_FLAG_HIDDEN);

    dot_ = lv_obj_create(obj_);
    lv_obj_remove_style_all(dot_);
    lv_obj_remove_flag(dot_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(dot_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(dot_, kDotSize, kDotSize);
    lv_obj_set_style_margin_right(dot_, kRowGap, 0);
    lv_obj_set_style_radius(dot_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(dot_, lv_color_hex(ui::theme::kAccent), 0);
    lv_obj_set_style_bg_opa(dot_, LV_OPA_COVER, 0);
    lv_obj_add_flag(dot_, LV_OBJ_FLAG_HIDDEN);

    bar_ = lv_bar_create(obj_);
    lv_obj_remove_flag(bar_, LV_OBJ_FLAG_CLICKABLE);
    lv_bar_set_range(bar_, 0, 100);
    lv_obj_set_height(bar_, kBarH);
    lv_obj_set_flex_grow(bar_, 1);
    lv_obj_add_flag(bar_, LV_OBJ_FLAG_HIDDEN);

    label_ = lv_label_create(obj_);
    lv_obj_remove_flag(label_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_set_width(label_, 0);
    // LV_LABEL_LONG_MODE_DOTS only truncates with "..." when the label's
    // own size (not just its width) is kept fixed; an unbounded
    // (LV_SIZE_CONTENT) height would let it grow past the card instead,
    // clipped by the parent with no ellipsis to show it was cut. fit()
    // sets it to the lines the text takes.
    lv_obj_set_flex_grow(label_, 1);
    lv_label_set_long_mode(label_, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(label_, lv_color_hex(ui::theme::kText), 0);
    lv_obj_set_style_text_font(label_, ui::default_font(), 0);
    lv_obj_set_style_text_line_space(label_, kLineSpace, 0);
    lv_obj_set_style_text_align(label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text_static(label_, "");
    lv_obj_add_flag(label_, LV_OBJ_FLAG_HIDDEN);
    fit(nullptr);

    // Raised now so a confirm dialog already open (built on lv_layer_top()
    // ahead of us) ends up in front of this freshly appended card again;
    // one opened later is simply appended after it, already on top.
    layer_.raise();
}

TalkOverlay::~TalkOverlay() {
    if (obj_ != nullptr) lv_obj_delete(obj_);
}

void TalkOverlay::take(const TurnView& t, uint64_t now_ms) {
    // Another turn: what the last one showed is over.
    if (t.running && t.started_ms != seen_started_) timed_ = Timed::None;
    seen_started_ = t.started_ms;
    // Only a spoken turn shows its progress and its say: one from the web
    // page or device text can run for minutes, and the card would cover
    // the last rows of the page in front all that time.
    const bool voice = t.source == agent::Source::Voice;
    voice_running_ = t.running && voice;
    if (t.turn != seen_turn_ || !same(t.say, seen_say_)) {
        seen_turn_ = t.turn;
        seen_say_.assign(t.say.data(), t.say.size());
        if (!t.say.empty() && voice) {
            timed_ = Timed::Say;
            timed_since_ = now_ms;
        } else if (timed_ == Timed::Say) {
            timed_ = Timed::None;
        }
    }
    if (!t.running && !t.end_reason.empty() &&
        (t.turn != seen_end_turn_ || t.ended_ms != seen_end_ms_)) {
        seen_end_turn_ = t.turn;
        seen_end_ms_ = t.ended_ms;
        timed_ = Timed::Note;
        note_ = detail::turn_end_text(t.end_reason);
        timed_since_ = now_ms;
    }
    if (t.error != seen_error_) {
        seen_error_ = t.error;
        if (t.error) {
            timed_ = Timed::Note;
            note_ = detail::voice_error_text(*t.error);
            timed_since_ = now_ms;
        }
    }
    // The step under way, while thinking.
    const TurnStep* step = voice_running_ && !t.thinking ? t.running_step() : nullptr;
    if (step != nullptr) {
        step_shown_.assign(step->label.data(), step->label.size());
    } else {
        step_shown_.clear();
    }
}

TalkOverlay::Phase TalkOverlay::decide(const TurnView& t, voice::VoiceState v) const {
    if (v == voice::VoiceState::Listening) return Phase::Listening;
    if (v == voice::VoiceState::Transcribing) return Phase::Thinking;
    if (timed_ == Timed::Say) return Phase::Saying;
    if (timed_ == Timed::Note) return Phase::Note;
    if (voice_running_) return Phase::Thinking;
    return Phase::Hidden;
}

void TalkOverlay::on_turn(const TurnView& t, voice::VoiceState v, uint8_t level, uint64_t now_ms) {
    take(t, now_ms);
    // A capture is the user's next try: the last say or outcome goes.
    if (v == voice::VoiceState::Listening) timed_ = Timed::None;
    phase_ = decide(t, v);
    apply(level);
}

void TalkOverlay::on_home(const TurnView& t) {
    take(t, 0);
    timed_ = Timed::None;
    hide();
}

void TalkOverlay::tick(uint64_t now_ms) {
    if (timed_ == Timed::None || now_ms - timed_since_ < kOverlaySayMs) return;
    timed_ = Timed::None;
    if (phase_ != Phase::Saying && phase_ != Phase::Note) return;
    // A turn still running shows as thinking again.
    phase_ = voice_running_ ? Phase::Thinking : Phase::Hidden;
    apply(0);
}

void TalkOverlay::hide() {
    timed_ = Timed::None;
    if (phase_ == Phase::Hidden) return;
    phase_ = Phase::Hidden;
    apply(0);
}

void TalkOverlay::apply(uint8_t level) {
    const bool listening = phase_ == Phase::Listening;
    if (listening) {
        lv_obj_remove_flag(dot_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_remove_flag(bar_, LV_OBJ_FLAG_HIDDEN);
        lv_bar_set_value(bar_, level, LV_ANIM_OFF);
    } else {
        lv_obj_add_flag(dot_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(bar_, LV_OBJ_FLAG_HIDDEN);
    }
    set_pulse(listening);

    const char* text = nullptr;
    switch (phase_) {
        case Phase::Thinking:
            text = step_shown_.empty() ? ui::tr(ui::StrId::TalkThinking) : step_shown_.c_str();
            break;
        case Phase::Saying:
            text = seen_say_.c_str();
            break;
        case Phase::Note:
            text = ui::tr(note_);
            break;
        case Phase::Hidden:
        case Phase::Listening:
            break;
    }
    if (text != nullptr) {
        lv_label_set_text(label_, text);
        lv_obj_remove_flag(label_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(label_, LV_OBJ_FLAG_HIDDEN);
    }
    fit(text);

    if (phase_ == Phase::Hidden) {
        lv_obj_add_flag(obj_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(obj_, LV_OBJ_FLAG_HIDDEN);
    }
}

void TalkOverlay::fit(const char* text) {
    const lv_font_t* font = ui::default_font();
    const int32_t letter = lv_obj_get_style_text_letter_space(label_, LV_PART_MAIN);
    auto place = [&](int32_t text_h) {
        const lv_area_t box = overlay_rect(shape_, card_height(text_h));
        lv_obj_set_pos(obj_, box.x1, box.y1);
        lv_obj_set_size(obj_, std::max<int32_t>(lv_area_get_width(&box), 0),
                        lv_area_get_height(&box));
        lv_obj_set_height(label_, text_h);
    };
    // The fewest lines that hold the text at the width the label gets at
    // that height (a taller card is narrower on a round screen); past
    // kMaxLines the label ends it with "...". The width is read back from
    // the laid-out label rather than worked out from the card's.
    int32_t text_h = text_height(1);
    for (int32_t n = 1; text != nullptr && n <= kMaxLines; ++n) {
        text_h = text_height(n);
        place(text_h);
        lv_obj_update_layout(obj_);
        lv_point_t size{0, 0};
        lv_text_get_size(&size, text, font, letter, kLineSpace,
                         std::max<int32_t>(lv_obj_get_content_width(label_), 1), LV_TEXT_FLAG_NONE);
        if (size.y <= text_h) return;
    }
    if (text == nullptr) place(text_h);
}

void TalkOverlay::set_pulse(bool on) {
    if (on == pulsing_) return;
    pulsing_ = on;
    lv_anim_delete(dot_, nullptr);
    if (!on) {
        lv_obj_set_style_opa(dot_, LV_OPA_COVER, 0);
        return;
    }
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, dot_);
    lv_anim_set_exec_cb(&a, [](void* obj, int32_t v) {
        lv_obj_set_style_opa(static_cast<lv_obj_t*>(obj), static_cast<lv_opa_t>(v), 0);
    });
    lv_anim_set_values(&a, LV_OPA_30, LV_OPA_COVER);
    lv_anim_set_duration(&a, kPulseMs);
    lv_anim_set_reverse_duration(&a, kPulseMs);
    lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
    lv_anim_start(&a);
}

void TalkOverlay::click_cb(lv_event_t* e) {
    auto* self = static_cast<TalkOverlay*>(lv_event_get_user_data(e));
    if (self->open_home_) self->open_home_();
}

}  // namespace wisp::shell
