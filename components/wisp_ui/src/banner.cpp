// Notification banners (scene-device spec 5.3): one at a time near the top
// of the safe area, for SystemLayer::kBannerMs each. See
// wisp/ui/system_layer.hpp.

#include <algorithm>
#include <string>
#include <utility>

#include "system_layer_internal.hpp"
#include "wisp/ui/overlay.hpp"
#include "wisp/ui/system_layer.hpp"

namespace wisp::ui {

namespace {

constexpr int32_t kBadgeSize = 40;
// How far in from the visible edge of its rows a banner stays on each
// side, like hal::safe_rect()'s corner margin.
constexpr int32_t kBannerInset = 4;
// An upward swipe this long puts a banner away.
constexpr int32_t kSwipe = SystemLayer::kTopTriggerDy;
// The body's lines at most.
constexpr int32_t kBodyLines = 2;

lv_point_t g_press_at{};

}  // namespace

void SystemLayer::show_banner(BannerInfo b, std::function<void()> on_tap) {
    banners_.push_back({std::move(b), std::move(on_tap)});
    if (banner_ == nullptr) show_next_banner();
}

void SystemLayer::show_next_banner() {
    if (banner_ != nullptr || banners_.empty()) return;
    QueuedBanner next = std::move(banners_.front());
    banners_.pop_front();
    const BannerInfo& b = next.info;

    // As wide as all the rows it covers show, less the inset.
    const int32_t top = safe_.y1 + kCapsuleTopGap;
    hal::Span ext = detail::visible_extent(shape_, top, top + kBannerH - 1, kBannerInset);
    if (ext.empty()) ext = {safe_.x1, safe_.x2};

    lv_obj_t* card = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(card);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_set_pos(card, ext.x1, top);
    lv_obj_set_size(card, ext.x2 - ext.x1 + 1, kBannerH);
    lv_obj_set_style_radius(card, theme::kRadiusLg, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(theme::kSurface2), 0);
    lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    lv_obj_set_style_bg_color(card, lv_color_hex(theme::pressed(theme::kSurface2, theme::kText)),
                              LV_STATE_PRESSED);
    lv_obj_set_style_border_color(card, lv_color_hex(theme::kBorderStrong), 0);
    lv_obj_set_style_border_width(card, theme::kBorderWidth, 0);
    lv_obj_set_style_pad_hor(card, theme::kGapMd, 0);
    lv_obj_set_style_pad_ver(card, theme::kGapXs, 0);
    lv_obj_set_style_pad_column(card, theme::kGapMd, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_add_event_cb(card, &SystemLayer::banner_cb, LV_EVENT_PRESSED, this);
    lv_obj_add_event_cb(card, &SystemLayer::banner_cb, LV_EVENT_CLICKED, this);

    detail::make_badge(card, b.badge, b.source, kBadgeSize);

    // The scene's name and the title on one line, the body under them.
    lv_obj_t* text = lv_obj_create(card);
    lv_obj_remove_style_all(text);
    lv_obj_remove_flag(text, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(text, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_height(text, LV_SIZE_CONTENT);
    lv_obj_set_width(text, 0);
    lv_obj_set_flex_grow(text, 1);
    lv_obj_set_flex_flow(text, LV_FLEX_FLOW_COLUMN);

    lv_obj_t* head = lv_obj_create(text);
    lv_obj_remove_style_all(head);
    lv_obj_remove_flag(head, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(head, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(head, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(head, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_pad_column(head, theme::kGapSm, 0);
    if (!b.name.empty()) {
        lv_obj_t* name = lv_label_create(head);
        lv_label_set_text(name, b.name.c_str());
        lv_obj_set_style_text_color(name, lv_color_hex(theme::kTextDim), 0);
        lv_label_set_long_mode(name, LV_LABEL_LONG_MODE_DOTS);
        lv_obj_set_style_max_width(name, LV_PCT(40), 0);
    }
    lv_obj_t* title = lv_label_create(head);
    lv_label_set_text(title, b.title.c_str());
    lv_obj_set_style_text_color(title, lv_color_hex(theme::kText), 0);
    lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(title, 0);
    lv_obj_set_flex_grow(title, 1);

    if (!b.body.empty()) {
        lv_obj_t* body = lv_label_create(text);
        lv_label_set_text(body, b.body.c_str());
        lv_obj_set_style_text_color(body, lv_color_hex(theme::kTextDim), 0);
        lv_obj_set_width(body, LV_PCT(100));
        lv_label_set_long_mode(body, LV_LABEL_LONG_MODE_DOTS);
        const lv_font_t* f = lv_obj_get_style_text_font(body, LV_PART_MAIN);
        const int32_t line =
            lv_font_get_line_height(f) + lv_obj_get_style_text_line_space(body, LV_PART_MAIN);
        // Whatever the rows above leave, at most kBodyLines lines.
        const int32_t room =
            kBannerH - 2 * theme::kGapXs - 2 * theme::kBorderWidth - lv_font_get_line_height(f);
        lv_obj_set_height(body, std::min(kBodyLines * line, room / line * line));
    }

    banner_ = card;
    banner_tap_ = std::move(next.on_tap);
    mark_opaque_overlay(card);
    lv_obj_update_layout(card);
    banner_timer_ = lv_timer_create(&SystemLayer::banner_timer_cb, kBannerMs, this);
    lv_timer_set_repeat_count(banner_timer_, 1);
    raise();
}

void SystemLayer::hide_banner() {
    if (banner_timer_ != nullptr) {
        lv_timer_delete(banner_timer_);
        banner_timer_ = nullptr;
    }
    if (banner_ != nullptr) {
        if (banner_event_depth_ > 0) {
            // Inside one of its events LVGL still uses it afterwards.
            lv_obj_add_flag(banner_, LV_OBJ_FLAG_HIDDEN);
            lv_obj_delete_async(banner_);
        } else {
            lv_obj_delete(banner_);
        }
        banner_ = nullptr;
    }
    banner_tap_ = nullptr;
    show_next_banner();
}

void SystemLayer::banner_timer_cb(lv_timer_t* t) {
    auto* self = static_cast<SystemLayer*>(lv_timer_get_user_data(t));
    // A one-shot timer: LVGL deletes it once this returns.
    self->banner_timer_ = nullptr;
    self->hide_banner();
}

void SystemLayer::banner_cb(lv_event_t* e) {
    auto* self = static_cast<SystemLayer*>(lv_event_get_user_data(e));
    lv_indev_t* indev = lv_indev_active();
    lv_point_t at{};
    if (indev != nullptr) lv_indev_get_point(indev, &at);
    if (lv_event_get_code(e) == LV_EVENT_PRESSED) {
        g_press_at = at;
        return;
    }
    const detail::PressEnd end =
        indev != nullptr ? detail::classify_press(g_press_at, at, kSwipe) : detail::PressEnd::Tap;
    if (end != detail::PressEnd::Tap && end != detail::PressEnd::SwipeUp) return;
    // A swipe up only puts it away; the notification stays unread.
    std::function<void()> on_tap = end == detail::PressEnd::Tap ? self->banner_tap_ : nullptr;
    ++self->banner_event_depth_;
    self->hide_banner();
    --self->banner_event_depth_;
    if (on_tap) on_tap();
}

}  // namespace wisp::ui
