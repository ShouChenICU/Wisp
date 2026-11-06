// The status capsule (scene-device spec 5.5): how many scenes run in the
// background and how many notifications are unread, at the top of the
// safe area. See wisp/ui/system_layer.hpp.

#include <string>

#include "wisp/ui/overlay.hpp"
#include "wisp/ui/system_layer.hpp"

namespace wisp::ui {

namespace {

// "9+" above 9 keeps the widest text, "<play>9+ <bell>9+", inside
// kCapsuleW at the body font.
std::string count_text(int n) { return n > 9 ? "9+" : std::to_string(n); }

std::string capsule_text(StatusCounts c) {
    std::string text;
    if (c.background > 0) {
        text += LV_SYMBOL_PLAY;
        text += count_text(c.background);
    }
    if (c.unread > 0) {
        if (!text.empty()) text += " ";
        text += LV_SYMBOL_BELL;
        text += count_text(c.unread);
    }
    return text;
}

}  // namespace

hal::Area SystemLayer::status_area(const hal::ScreenShape& s) {
    const hal::Area safe = hal::safe_rect(s);
    const int32_t x1 = (s.width - kCapsuleW) / 2;
    const int32_t y1 = safe.y1 + kCapsuleTopGap;
    return {x1, y1, x1 + kCapsuleW - 1, y1 + kCapsuleH - 1};
}

void SystemLayer::set_status(StatusCounts c) {
    if (c.background <= 0 && c.unread <= 0) {
        if (capsule_ != nullptr) lv_obj_add_flag(capsule_, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    if (capsule_ == nullptr) {
        const hal::Area a = status_area(shape_);
        capsule_ = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(capsule_);
        lv_obj_remove_flag(capsule_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(capsule_, LV_OBJ_FLAG_CLICK_FOCUSABLE);
        lv_obj_set_pos(capsule_, a.x1, a.y1);
        lv_obj_set_size(capsule_, kCapsuleW, kCapsuleH);
        lv_obj_set_style_radius(capsule_, theme::kRadiusPill, 0);
        lv_obj_set_style_bg_color(capsule_, lv_color_hex(theme::kSurface3), 0);
        lv_obj_set_style_bg_opa(capsule_, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(capsule_,
                                  lv_color_hex(theme::pressed(theme::kSurface3, theme::kText)),
                                  LV_STATE_PRESSED);
        // The drawn pill is kCapsuleH tall; its touch target reaches
        // kPanelRowMinH like the other system controls. Rows above the
        // screen's top take no touches anyway.
        lv_obj_set_ext_click_area(capsule_, (kPanelRowMinH - kCapsuleH) / 2);
        lv_obj_add_event_cb(capsule_, &SystemLayer::capsule_cb, LV_EVENT_CLICKED, this);
        capsule_label_ = lv_label_create(capsule_);
        lv_obj_set_style_text_color(capsule_label_, lv_color_hex(theme::kText), 0);
        lv_label_set_long_mode(capsule_label_, LV_LABEL_LONG_MODE_CLIP);
        lv_obj_center(capsule_label_);
        // Its text changes only through set_status(), inside its box.
        mark_opaque_overlay(capsule_);
    }
    update_capsule_touch();
    lv_label_set_text(capsule_label_, capsule_text(c).c_str());
    lv_obj_remove_flag(capsule_, LV_OBJ_FLAG_HIDDEN);
    // Laid out now, so a touch read before the next frame hits it where it
    // is drawn.
    lv_obj_update_layout(capsule_);
    raise();
}

void SystemLayer::update_capsule_touch() {
    if (capsule_ == nullptr) return;
    // Only with a panel to open does it take touches; otherwise they reach
    // the screen below.
    if (has_panel()) {
        lv_obj_add_flag(capsule_, LV_OBJ_FLAG_CLICKABLE);
    } else {
        lv_obj_remove_flag(capsule_, LV_OBJ_FLAG_CLICKABLE);
    }
}

lv_obj_t* SystemLayer::capsule() const {
    if (capsule_ == nullptr || lv_obj_has_flag(capsule_, LV_OBJ_FLAG_HIDDEN)) return nullptr;
    return capsule_;
}

void SystemLayer::capsule_cb(lv_event_t* e) {
    auto* self = static_cast<SystemLayer*>(lv_event_get_user_data(e));
    self->open_panel();
}

}  // namespace wisp::ui
