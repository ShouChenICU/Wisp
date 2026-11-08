// Notification banners (scene-device spec 5.3): one at a time near the top
// of the safe area, for SystemLayer::kBannerMs each. See
// wisp/ui/system_layer.hpp.

#include <algorithm>
#include <string>
#include <utility>

#include "system_layer_internal.hpp"
#include "wisp/core/log.hpp"
#include "wisp/ui/overlay.hpp"
#include "wisp/ui/system_layer.hpp"
#include "wisp/ui/theme.hpp"

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
// The button row of a banner with actions, and the gap above it.
constexpr int32_t kActionsH = SystemLayer::kPanelRowMinH;
constexpr int32_t kActionsGap = theme::kGapXs;

}  // namespace

void SystemLayer::show_banner(BannerInfo b, std::function<void()> on_tap,
                              std::function<void(int index)> on_action) {
    banners_.push_back({std::move(b), std::move(on_tap), std::move(on_action)});
    if (banner_ == nullptr) show_next_banner();
}

void SystemLayer::show_next_banner() {
    if (banner_ != nullptr || banners_.empty()) return;
    QueuedBanner next = std::move(banners_.front());
    banners_.pop_front();
    const BannerInfo& b = next.info;
    const bool actions = !b.action_label.empty() || !b.action2_label.empty();
    const int32_t height = actions ? kBannerH + kActionsGap + kActionsH : kBannerH;

    // As wide as all the rows it covers show, less the inset.
    const int32_t top = safe_.y1 + kCapsuleTopGap;
    hal::Span ext = detail::visible_extent(shape_, top, top + height - 1, kBannerInset);
    if (ext.empty()) ext = {safe_.x1, safe_.x2};

    // Logged (never the text: it may be private) so that a script reading
    // only the log can tell when a banner is up.
    WLOG_I("ui", "banner: shown %s/%s", b.source.c_str(), b.id.c_str());
    lv_obj_t* card = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(card);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(card, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_set_pos(card, ext.x1, top);
    lv_obj_set_size(card, ext.x2 - ext.x1 + 1, height);
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

    // The badge and the text in a row as tall as a plain banner's content,
    // the buttons under it.
    lv_obj_t* content = card;
    if (actions) {
        lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_style_pad_row(card, kActionsGap, 0);
        content = lv_obj_create(card);
        lv_obj_remove_style_all(content);
        lv_obj_remove_flag(content, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(content, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(content, LV_PCT(100),
                        kBannerH - 2 * theme::kGapXs - 2 * theme::kBorderWidth);
        lv_obj_set_style_pad_column(content, theme::kGapMd, 0);
        lv_obj_set_flex_flow(content, LV_FLEX_FLOW_ROW);
        lv_obj_set_flex_align(content, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                              LV_FLEX_ALIGN_CENTER);
    }

    detail::make_badge(content, b.badge, b.source, kBadgeSize);

    // The scene's name and the title on one line, the body under them.
    lv_obj_t* text = lv_obj_create(content);
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

    if (actions) {
        lv_obj_t* row = lv_obj_create(card);
        lv_obj_remove_style_all(row);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(row, LV_PCT(100), kActionsH);
        lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
        lv_obj_set_style_pad_column(row, theme::kGapSm, 0);
        const std::string* labels[] = {&b.action_label, &b.action2_label};
        for (uintptr_t i = 0; i < 2; ++i) {
            if (labels[i]->empty()) continue;
            lv_obj_t* btn = lv_button_create(row);
            set_button_role(btn, ButtonRole::Secondary);
            lv_obj_remove_flag(btn, LV_OBJ_FLAG_CLICK_FOCUSABLE);
            lv_obj_set_height(btn, kActionsH);
            lv_obj_set_width(btn, 0);
            lv_obj_set_flex_grow(btn, 1);
            lv_obj_set_style_pad_hor(btn, theme::kGapXs, 0);
            lv_obj_set_user_data(btn, reinterpret_cast<void*>(i));
            lv_obj_add_event_cb(btn, &SystemLayer::banner_action_cb, LV_EVENT_CLICKED, this);
            lv_obj_t* label = lv_label_create(btn);
            lv_label_set_text(label, labels[i]->c_str());
            lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
            lv_obj_set_width(label, LV_PCT(100));
            lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
            lv_obj_center(label);
        }
    }

    banner_ = card;
    banner_tap_ = std::move(next.on_tap);
    banner_action_ = std::move(next.on_action);
    mark_opaque_overlay(card);
    lv_obj_update_layout(card);
    banner_timer_ =
        lv_timer_create(&SystemLayer::banner_timer_cb, actions ? kActionBannerMs : kBannerMs, this);
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
        WLOG_I("ui", "banner: gone");
    }
    banner_tap_ = nullptr;
    banner_action_ = nullptr;
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
        self->press_at_ = at;
        return;
    }
    const detail::PressEnd end = indev != nullptr
                                     ? detail::classify_press(self->press_at_, at, kSwipe)
                                     : detail::PressEnd::Tap;
    if (end != detail::PressEnd::Tap && end != detail::PressEnd::SwipeUp) return;
    // A swipe up only puts it away; the notification stays unread.
    std::function<void()> on_tap = end == detail::PressEnd::Tap ? self->banner_tap_ : nullptr;
    ++self->banner_event_depth_;
    self->hide_banner();
    --self->banner_event_depth_;
    if (on_tap) on_tap();
}

Result<void> SystemLayer::tap_banner_action(int index) {
    if (banner_ == nullptr) return Result<void>::err(ErrorCode::NotFound, "no banner is shown");
    // The buttons sit in the card's last child, the action row, and carry
    // their index as user data (show_next_banner()).
    const uint32_t n = lv_obj_get_child_count(banner_);
    lv_obj_t* row = n > 1 ? lv_obj_get_child(banner_, static_cast<int32_t>(n - 1)) : nullptr;
    const uint32_t buttons = row != nullptr ? lv_obj_get_child_count(row) : 0;
    for (uint32_t i = 0; i < buttons && index >= 0; ++i) {
        lv_obj_t* btn = lv_obj_get_child(row, static_cast<int32_t>(i));
        if (!lv_obj_has_class(btn, &lv_button_class)) continue;
        if (reinterpret_cast<uintptr_t>(lv_obj_get_user_data(btn)) != static_cast<uintptr_t>(index))
            continue;
        lv_obj_send_event(btn, LV_EVENT_CLICKED, nullptr);
        return Result<void>::ok();
    }
    return Result<void>::err(ErrorCode::NotFound, "no such action");
}

void SystemLayer::banner_action_cb(lv_event_t* e) {
    auto* self = static_cast<SystemLayer*>(lv_event_get_user_data(e));
    auto* btn = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    const int index = static_cast<int>(reinterpret_cast<uintptr_t>(lv_obj_get_user_data(btn)));
    // A copy: hiding the banner lets its callbacks go.
    std::function<void(int)> on_action = self->banner_action_;
    ++self->banner_event_depth_;
    self->hide_banner();
    --self->banner_event_depth_;
    if (on_action) on_action(index);
}

}  // namespace wisp::ui
