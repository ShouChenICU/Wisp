#include "wisp/shell/talk_overlay.hpp"

#include <algorithm>
#include <utility>

#include "wisp/shell/limits.hpp"
#include "wisp/ui/fonts.hpp"
#include "wisp/ui/strings.hpp"
#include "wisp/ui/system_layer.hpp"
#include "wisp/ui/theme_tokens.hpp"

namespace wisp::shell {

namespace {

// The card's fixed height and how far it sits above the safe area's
// bottom edge; the gap each side inside the row it draws on.
constexpr int32_t kCardH = 56;
constexpr int32_t kBottomGap = ui::theme::kGapMd;
constexpr int32_t kPad = ui::theme::kGapMd;
constexpr int32_t kRowGap = ui::theme::kGapSm;
constexpr int32_t kDotSize = 14;
constexpr int32_t kBarH = 10;
constexpr uint32_t kPulseMs = 900;

// The card's box: kCardH pixels above the safe area's bottom, as wide as
// the narrower of its top and bottom row (the visible area is convex, so
// no row between is narrower), less kPad each side -- the same approach
// as wisp/shell/home.cpp's band().
lv_area_t overlay_rect(const hal::ScreenShape& s) {
    const hal::Area safe = hal::safe_rect(s);
    const int32_t y2 = safe.y2 - kBottomGap;
    const int32_t y1 = y2 - kCardH + 1;
    const hal::Span a = hal::row_span(s, y1);
    const hal::Span b = hal::row_span(s, y2);
    if (a.empty() || b.empty()) return {safe.x1, y1, safe.x2, y2};
    return {std::max(a.x1, b.x1) + kPad, y1, std::min(a.x2, b.x2) - kPad, y2};
}

bool same(const os::LargeString& a, const os::LargeString& b) {
    return a.size() == b.size() && std::equal(a.begin(), a.end(), b.begin());
}

}  // namespace

TalkOverlay::TalkOverlay(ui::SystemLayer& layer, const hal::ScreenShape& shape,
                         std::function<void()> open_home)
    : layer_(layer), shape_(shape), open_home_(std::move(open_home)) {
    const lv_area_t box = overlay_rect(shape_);
    obj_ = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(obj_);
    lv_obj_remove_flag(obj_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_pos(obj_, box.x1, box.y1);
    lv_obj_set_size(obj_, std::max<int32_t>(lv_area_get_width(&box), 0), kCardH);
    lv_obj_set_style_radius(obj_, ui::theme::kRadiusLg, 0);
    lv_obj_set_style_bg_color(obj_, lv_color_hex(ui::theme::kSurface2), 0);
    lv_obj_set_style_bg_opa(obj_, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(obj_, lv_color_hex(ui::theme::kBorderStrong), 0);
    lv_obj_set_style_border_width(obj_, ui::theme::kBorderWidth, 0);
    lv_obj_set_style_pad_hor(obj_, kPad, 0);
    lv_obj_set_style_pad_column(obj_, kRowGap, 0);
    lv_obj_set_flex_flow(obj_, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(obj_, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_event_cb(obj_, &TalkOverlay::click_cb, LV_EVENT_CLICKED, this);
    lv_obj_add_flag(obj_, LV_OBJ_FLAG_HIDDEN);

    dot_ = lv_obj_create(obj_);
    lv_obj_remove_style_all(dot_);
    lv_obj_remove_flag(dot_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(dot_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(dot_, kDotSize, kDotSize);
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
    lv_obj_set_flex_grow(label_, 1);
    lv_label_set_long_mode(label_, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_color(label_, lv_color_hex(ui::theme::kText), 0);
    lv_obj_set_style_text_align(label_, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text_static(label_, "");
    lv_obj_add_flag(label_, LV_OBJ_FLAG_HIDDEN);

    // Raised now so a confirm dialog already open (built on lv_layer_top()
    // ahead of us) ends up in front of this freshly appended card again;
    // one opened later is simply appended after it, already on top.
    layer_.raise();
}

TalkOverlay::~TalkOverlay() {
    if (obj_ != nullptr) lv_obj_delete(obj_);
}

TalkOverlay::Phase TalkOverlay::decide(const TurnView& t, voice::VoiceState v) const {
    if (v == voice::VoiceState::Listening) return Phase::Listening;
    if (v == voice::VoiceState::Transcribing) return Phase::Thinking;
    if (!t.say.empty()) return Phase::Saying;
    if (t.running) return Phase::Thinking;
    return Phase::Hidden;
}

void TalkOverlay::on_turn(const TurnView& t, voice::VoiceState v, uint8_t level, uint64_t now_ms) {
    const Phase next = decide(t, v);
    if (next == Phase::Saying) {
        if (phase_ != Phase::Saying || !same(t.say, say_shown_)) {
            say_shown_.assign(t.say.data(), t.say.size());
            say_since_ = now_ms;
        }
    } else {
        say_shown_.clear();
    }
    phase_ = next;
    apply(level);
}

void TalkOverlay::tick(uint64_t now_ms) {
    if (phase_ == Phase::Saying && now_ms - say_since_ >= kOverlaySayMs) hide();
}

void TalkOverlay::hide() {
    if (phase_ == Phase::Hidden) return;
    phase_ = Phase::Hidden;
    say_shown_.clear();
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

    if (phase_ == Phase::Thinking) {
        lv_label_set_text(label_, ui::tr(ui::StrId::TalkThinking));
        lv_obj_remove_flag(label_, LV_OBJ_FLAG_HIDDEN);
    } else if (phase_ == Phase::Saying) {
        lv_label_set_text(label_, say_shown_.c_str());
        lv_obj_remove_flag(label_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_add_flag(label_, LV_OBJ_FLAG_HIDDEN);
    }

    if (phase_ == Phase::Hidden) {
        lv_obj_add_flag(obj_, LV_OBJ_FLAG_HIDDEN);
    } else {
        lv_obj_remove_flag(obj_, LV_OBJ_FLAG_HIDDEN);
    }
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
