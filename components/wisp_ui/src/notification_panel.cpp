// The pull-down panel (scene-device spec 5.6): the time, the battery and
// Wi-Fi, the background scenes and the unread notifications. See
// wisp/ui/system_layer.hpp.

#include <algorithm>
#include <cstdlib>
#include <string>
#include <utility>

#include "system_layer_internal.hpp"
#include "wisp/core/log.hpp"
#include "wisp/ui/dialog.hpp"
#include "wisp/ui/fonts.hpp"
#include "wisp/ui/overlay.hpp"
#include "wisp/ui/system_layer.hpp"

namespace wisp::ui {

namespace detail {

lv_obj_t* make_badge(lv_obj_t* parent, std::string_view text, std::string_view key, int32_t size) {
    // Saturated theme colours that dark ink reads on.
    static constexpr uint32_t kFills[] = {theme::kAccent, theme::kOk, theme::kWarn, theme::kErr,
                                          theme::kAccentStrong};
    lv_obj_t* badge = lv_obj_create(parent);
    lv_obj_remove_style_all(badge);
    lv_obj_remove_flag(badge, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(badge, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(badge, size, size);
    lv_obj_set_style_radius(badge, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_opa(badge, LV_OPA_COVER, 0);
    lv_obj_t* label = lv_label_create(badge);
    if (text.empty()) {
        lv_obj_set_style_bg_color(badge, lv_color_hex(theme::kSurface3), 0);
        lv_obj_set_style_text_color(label, lv_color_hex(theme::kText), 0);
        lv_label_set_text(label, LV_SYMBOL_BELL);
    } else {
        // FNV-1a: stable across builds, so a scene keeps its colour.
        uint32_t h = 2166136261u;
        for (char c : key) h = (h ^ static_cast<uint8_t>(c)) * 16777619u;
        lv_obj_set_style_bg_color(badge, lv_color_hex(kFills[h % std::size(kFills)]), 0);
        lv_obj_set_style_text_color(label, lv_color_hex(theme::kAccentInk), 0);
        lv_label_set_text(label, std::string(text).c_str());
    }
    lv_obj_center(label);
    return badge;
}

std::string first_char(std::string_view s) {
    if (s.empty()) return {};
    const auto lead = static_cast<uint8_t>(s[0]);
    const size_t n = lead >= 0xF0 ? 4 : lead >= 0xE0 ? 3 : lead >= 0xC0 ? 2 : 1;
    return std::string(s.substr(0, std::min(n, s.size())));
}

PressEnd classify_press(lv_point_t from, lv_point_t to, int32_t swipe) {
    const int32_t dx = to.x - from.x;
    const int32_t dy = to.y - from.y;
    if (-dy >= swipe && -dy > std::abs(dx)) return PressEnd::SwipeUp;
    if (-dx >= swipe && -dx > std::abs(dy)) return PressEnd::SwipeLeft;
    // Half the swipe distance: LVGL itself clicks on a press that moved
    // without scrolling anything, which a slow swipe would otherwise be.
    if (std::abs(dx) < swipe / 2 && std::abs(dy) < swipe / 2) return PressEnd::Tap;
    return PressEnd::Other;
}

}  // namespace detail

namespace {

// What a touchable part of the panel does, kept in its user data with the
// index of the scene or notification it belongs to.
enum class Part : uintptr_t {
    Blank,
    Header,
    SceneRow,
    SceneClose,
    NoteRow,
    NoteClose,
    NoteAction,
    NoteAction2,
    ClearAll
};

constexpr uintptr_t kPartBits = 4;

void* encode(Part part, size_t index) {
    return reinterpret_cast<void*>((index << kPartBits) | static_cast<uintptr_t>(part));
}

Part part_of(lv_obj_t* obj) {
    return static_cast<Part>(reinterpret_cast<uintptr_t>(lv_obj_get_user_data(obj)) &
                             ((uintptr_t{1} << kPartBits) - 1));
}

size_t index_of(lv_obj_t* obj) {
    return reinterpret_cast<uintptr_t>(lv_obj_get_user_data(obj)) >> kPartBits;
}

constexpr int32_t kBadgeSize = 32;
// A swipe on the panel: as far as the top-edge swipe that opens it.
constexpr int32_t kSwipe = SystemLayer::kTopTriggerDy;

lv_obj_t* make_text(lv_obj_t* parent, const char* text, uint32_t color) {
    lv_obj_t* label = lv_label_create(parent);
    lv_label_set_text(label, text);
    lv_obj_set_style_text_color(label, lv_color_hex(color), 0);
    return label;
}

// A label that takes what is left of its flex row and ends in "..." when
// the text is longer.
lv_obj_t* make_fill_text(lv_obj_t* parent, const char* text, uint32_t color) {
    lv_obj_t* label = make_text(parent, text, color);
    lv_obj_set_width(label, 0);
    lv_obj_set_flex_grow(label, 1);
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_DOTS);
    return label;
}

lv_obj_t* make_box(lv_obj_t* parent, lv_flex_flow_t flow) {
    lv_obj_t* box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_size(box, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(box, flow);
    lv_obj_set_flex_align(box, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(box, theme::kGapSm, 0);
    lv_obj_set_style_pad_row(box, theme::kGapXs, 0);
    return box;
}

// How long ago `since_ms` was, in the panel's words.
std::string elapsed_text(uint64_t now_ms, uint64_t since_ms) {
    const uint64_t ms = now_ms > since_ms ? now_ms - since_ms : 0;
    const uint64_t min = ms / 60000;
    if (min == 0) return tr(StrId::PanelJustNow);
    if (min < 60) return tr_fill(StrId::PanelMinutes, std::to_string(min));
    return tr_fill(StrId::PanelHours, std::to_string(min / 60));
}

std::string battery_text(const PanelHeader& h) {
    if (h.battery < 0) return {};
    const char* icon = h.charging        ? LV_SYMBOL_CHARGE
                       : h.battery >= 80 ? LV_SYMBOL_BATTERY_FULL
                       : h.battery >= 55 ? LV_SYMBOL_BATTERY_3
                       : h.battery >= 30 ? LV_SYMBOL_BATTERY_2
                       : h.battery >= 10 ? LV_SYMBOL_BATTERY_1
                                         : LV_SYMBOL_BATTERY_EMPTY;
    return std::string(icon) + " " + std::to_string(h.battery) + "%";
}

}  // namespace

bool SystemLayer::has_panel() const {
    const PanelHandlers& h = panel_handlers_;
    return h.on_open || h.open_scene || h.exit_scene || h.tap_note || h.dismiss_note ||
           h.note_action || h.clear_all;
}

void SystemLayer::set_panel_handlers(PanelHandlers h) {
    panel_handlers_ = std::move(h);
    if (!has_panel()) close_panel();
    update_capsule_touch();
    update_pointer_hook();
}

bool SystemLayer::can_open_panel() const {
    // Under the keyboard the user would type into a textarea the panel
    // hides; under the error panel or a dialog it would show up only once
    // they go, and take the next back swipe.
    return has_panel() && panel_ == nullptr && kb_ == nullptr && error_panel_ == nullptr &&
           confirm_box_ == nullptr;
}

void SystemLayer::open_panel() {
    if (!can_open_panel()) return;
    // A copy: the owner may replace the handlers from it.
    if (std::function<void()> on_open = panel_handlers_.on_open) on_open();
    if (!can_open_panel()) return;
    build_panel();
    raise();
    WLOG_I("ui", "panel: shown");
}

void SystemLayer::close_panel() { discard_panel(); }

// --- Following the finger, sliding ---

void SystemLayer::begin_panel_follow(bool opening) {
    if (opening) {
        if (!can_open_panel()) return;
        if (std::function<void()> on_open = panel_handlers_.on_open) on_open();
        if (!can_open_panel()) return;
        build_panel();
        raise();
    } else if (panel_ == nullptr) {
        return;
    }
    // No slide runs now (observe() ignores a press made during one), but
    // one would carry the sheet away from the finger.
    lv_anim_delete(this, &SystemLayer::panel_slide_cb);
    pause_panel_marks(true);
    motion_ = PanelMotion::Follow;
    follow_opening_ = opening;
    // From where the finger went down: a swipe quick from the start is a
    // flick, however few reads it took to cross the trigger.
    panel_fling_.reset();
    panel_fling_.add(0, press_ms_);
}

void SystemLayer::follow_panel(lv_point_t p) {
    if (panel_ == nullptr || motion_ != PanelMotion::Follow) return;
    const int32_t along = follow_opening_ ? p.y - start_.y : start_.y - p.y;
    panel_fling_.add(along, now_ms());
    const int32_t moved = std::clamp(along, int32_t{0}, shape_.height);
    place_panel(follow_opening_ ? moved : shape_.height - moved);
}

void SystemLayer::release_panel(lv_point_t p) {
    if (panel_ == nullptr || motion_ != PanelMotion::Follow) return;
    follow_panel(p);
    const int32_t moved = follow_opening_ ? panel_offset_ : shape_.height - panel_offset_;
    const bool complete = drag_completes(moved, shape_.height, panel_fling_.fling());
    panel_fling_.reset();
    slide_panel(follow_opening_ == complete);
}

void SystemLayer::place_panel(int32_t offset) {
    panel_offset_ = offset;
    // Only positions: the content keeps its layout and scroll.
    const int32_t dy = offset - shape_.height;
    lv_obj_set_y(panel_backdrop_, dy);
    lv_obj_set_y(panel_, safe_.y1 + dy);
    lv_obj_set_y(panel_handle_, panel_handle_y_ + dy);
}

void SystemLayer::slide_panel(bool open) {
    if (panel_ == nullptr) return;
    pause_panel_marks(true);
    motion_ = PanelMotion::Slide;
    slide_open_ = open;
    const int32_t end = open ? shape_.height : 0;
    lv_anim_t a;
    lv_anim_init(&a);
    lv_anim_set_var(&a, this);
    lv_anim_set_values(&a, panel_offset_, end);
    // Never 0 ms: the end may delete the panel, which must happen on the
    // animation timer and not inside the event (a tap on the panel) or
    // read that started the slide.
    lv_anim_set_duration(&a, slide_ms(end - panel_offset_, shape_.height));
    lv_anim_set_path_cb(&a, lv_anim_path_ease_out);
    lv_anim_set_exec_cb(&a, &SystemLayer::panel_slide_cb);
    lv_anim_set_user_data(&a, this);
    lv_anim_set_completed_cb(&a, &SystemLayer::panel_slide_done_cb);
    lv_anim_start(&a);
}

void SystemLayer::panel_slide_cb(void* self, int32_t offset) {
    auto* layer = static_cast<SystemLayer*>(self);
    if (layer->panel_ != nullptr) layer->place_panel(offset);
}

void SystemLayer::panel_slide_done_cb(lv_anim_t* a) {
    static_cast<SystemLayer*>(lv_anim_get_user_data(a))->finish_slide();
}

void SystemLayer::finish_slide() {
    if (panel_ == nullptr || motion_ != PanelMotion::Slide) return;
    if (!slide_open_) {
        discard_panel();
        return;
    }
    motion_ = PanelMotion::Still;
    place_panel(shape_.height);
    pause_panel_marks(false);
    WLOG_I("ui", "panel: shown");
}

void SystemLayer::pause_panel_marks(bool paused) {
    pause_opaque_overlay(panel_backdrop_, paused);
    pause_opaque_overlay(panel_, paused);
    pause_opaque_overlay(panel_handle_, paused);
}

void SystemLayer::update_panel(PanelModel m) {
    model_ = std::move(m);
    if (panel_ == nullptr) return;
    if (panel_event_depth_ == 0) {
        fill_panel();
    } else if (!refill_queued_) {
        // From inside a row's event: the row must outlive it.
        refill_queued_ = true;
        lv_async_call(&SystemLayer::panel_refill_cb, this);
    }
}

void SystemLayer::panel_refill_cb(void* self_ptr) {
    auto* self = static_cast<SystemLayer*>(self_ptr);
    self->refill_queued_ = false;
    if (self->panel_ != nullptr) self->fill_panel();
}

void SystemLayer::build_panel() {
    // The whole screen: whatever is under the panel is hidden, and a touch
    // outside the content box closes it. It has no children and never
    // changes, so what scrolls under it is cut although it spans the
    // screen.
    panel_backdrop_ = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(panel_backdrop_);
    lv_obj_set_size(panel_backdrop_, shape_.width, shape_.height);
    lv_obj_set_style_bg_color(panel_backdrop_, lv_color_hex(theme::kBg), 0);
    lv_obj_set_style_bg_opa(panel_backdrop_, LV_OPA_COVER, 0);
    lv_obj_remove_flag(panel_backdrop_, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_remove_flag(panel_backdrop_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_user_data(panel_backdrop_, encode(Part::Blank, 0));
    lv_obj_add_event_cb(panel_backdrop_, &SystemLayer::panel_cb, LV_EVENT_PRESSED, this);
    lv_obj_add_event_cb(panel_backdrop_, &SystemLayer::panel_cb, LV_EVENT_CLICKED, this);

    // The handle sits a fixed gap above the sheet's bottom edge, which
    // shows the edge in the middle of a drag as well. On a round or
    // rounded screen the middle columns reach down that far, and a pill
    // that narrow is inside them.
    panel_handle_y_ = shape_.height - theme::kGapMd - kPanelHandleH;
    // The content: the safe area, ended above the handle where the safe
    // area reaches down to it, so every row of it is visible wherever it
    // scrolls to; it scrolls when the rows are taller.
    const int32_t content_y2 = std::min(safe_.y2, panel_handle_y_ - theme::kGapSm - 1);
    panel_ = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(panel_);
    lv_obj_set_pos(panel_, safe_.x1, safe_.y1);
    lv_obj_set_size(panel_, safe_.x2 - safe_.x1 + 1, content_y2 - safe_.y1 + 1);
    lv_obj_set_style_bg_color(panel_, lv_color_hex(theme::kBg), 0);
    lv_obj_set_style_bg_opa(panel_, LV_OPA_COVER, 0);
    lv_obj_set_style_pad_all(panel_, theme::kGapSm, 0);
    lv_obj_set_style_pad_row(panel_, theme::kGapSm, 0);
    lv_obj_set_flex_flow(panel_, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(panel_, LV_DIR_VER);
    lv_obj_remove_flag(panel_, LV_OBJ_FLAG_CLICK_FOCUSABLE);
    lv_obj_set_user_data(panel_, encode(Part::Blank, 0));
    lv_obj_add_event_cb(panel_, &SystemLayer::panel_cb, LV_EVENT_PRESSED, this);
    lv_obj_add_event_cb(panel_, &SystemLayer::panel_cb, LV_EVENT_CLICKED, this);

    // Not clickable: a touch on it reaches the backdrop and closes the
    // panel, like any touch outside the content.
    panel_handle_ = lv_obj_create(lv_layer_top());
    lv_obj_remove_style_all(panel_handle_);
    lv_obj_set_size(panel_handle_, kPanelHandleW, kPanelHandleH);
    lv_obj_set_pos(panel_handle_, (shape_.width - kPanelHandleW) / 2, panel_handle_y_);
    lv_obj_set_style_radius(panel_handle_, theme::kRadiusPill, 0);
    // Opaque, so it can be marked: an unmarked object would keep every
    // area touching it from being cut.
    lv_obj_set_style_bg_color(panel_handle_,
                              lv_color_hex(theme::blend(theme::kTextFaint, theme::kBg, 60)), 0);
    lv_obj_set_style_bg_opa(panel_handle_, LV_OPA_COVER, 0);
    lv_obj_remove_flag(panel_handle_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(panel_handle_, LV_OBJ_FLAG_SCROLLABLE);

    mark_opaque_overlay(panel_backdrop_, OwnChanges::InOwnEvents);
    mark_opaque_overlay(panel_);
    mark_opaque_overlay(panel_handle_);
    motion_ = PanelMotion::Still;
    panel_offset_ = shape_.height;
    fill_panel();
}

void SystemLayer::fill_panel() {
    const int32_t scroll_y = lv_obj_get_scroll_y(panel_);
    lv_obj_clean(panel_);
    const PanelModel& m = model_;

    auto touchable = [this](lv_obj_t* obj, Part part, size_t index) {
        lv_obj_remove_flag(obj, LV_OBJ_FLAG_CLICK_FOCUSABLE);
        lv_obj_set_user_data(obj, encode(part, index));
        lv_obj_add_event_cb(obj, &SystemLayer::panel_cb, LV_EVENT_PRESSED, this);
        lv_obj_add_event_cb(obj, &SystemLayer::panel_cb, LV_EVENT_CLICKED, this);
    };
    auto make_row = [&](Part part, size_t index) {
        lv_obj_t* row = make_box(panel_, LV_FLEX_FLOW_ROW);
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_set_style_min_height(row, kPanelRowMinH, 0);
        lv_obj_set_style_radius(row, theme::kRadius, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(theme::kSurface), 0);
        lv_obj_set_style_bg_opa(row, LV_OPA_COVER, 0);
        lv_obj_set_style_bg_color(row, lv_color_hex(theme::pressed(theme::kSurface, theme::kText)),
                                  LV_STATE_PRESSED);
        lv_obj_set_style_pad_left(row, theme::kGapSm, 0);
        lv_obj_set_style_pad_ver(row, theme::kGapXs, 0);
        touchable(row, part, index);
        return row;
    };
    auto make_close = [&](lv_obj_t* parent, Part part, size_t index) {
        lv_obj_t* btn = lv_button_create(parent);
        set_button_role(btn, ButtonRole::Secondary);
        lv_obj_set_size(btn, kPanelRowMinH, kPanelRowMinH);
        lv_obj_set_style_pad_all(btn, 0, 0);
        lv_obj_t* x = lv_label_create(btn);
        lv_label_set_text(x, LV_SYMBOL_CLOSE);
        lv_obj_center(x);
        touchable(btn, part, index);
        return btn;
    };
    auto heading = [&](StrId id) { make_text(panel_, tr(id), theme::kTextDim); };

    // Header: the time, then the battery and Wi-Fi.
    lv_obj_t* header = make_box(panel_, LV_FLEX_FLOW_ROW);
    lv_obj_set_style_min_height(header, kPanelRowMinH, 0);
    // A tap on the time or the battery is not a tap on empty space.
    lv_obj_add_flag(header, LV_OBJ_FLAG_CLICKABLE);
    touchable(header, Part::Header, 0);
    lv_obj_t* time = make_text(header, m.header.time.c_str(), theme::kText);
    lv_obj_set_style_text_font(time, font(24), 0);
    std::string status = battery_text(m.header);
    if (!m.header.wifi.empty()) {
        if (!status.empty()) status += "  ";
        status += LV_SYMBOL_WIFI " ";
        status.append(m.header.wifi.data(), m.header.wifi.size());
    }
    lv_obj_t* status_label = make_fill_text(header, status.c_str(), theme::kTextDim);
    lv_obj_set_style_text_align(status_label, LV_TEXT_ALIGN_RIGHT, 0);

    if (!m.scenes.empty()) heading(StrId::PanelBackground);
    for (size_t i = 0; i < m.scenes.size(); ++i) {
        const PanelScene& sc = m.scenes[i];
        lv_obj_t* row = make_row(Part::SceneRow, i);
        detail::make_badge(row, detail::first_char(sc.name), sc.id, kBadgeSize);
        make_fill_text(row, sc.name.c_str(), theme::kText);
        make_text(row, elapsed_text(m.now_ms, sc.since_ms).c_str(), theme::kTextFaint);
        make_close(row, Part::SceneClose, i);
    }

    heading(StrId::PanelNotifications);
    if (m.notes.empty()) make_text(panel_, tr(StrId::PanelEmpty), theme::kTextFaint);
    for (size_t i = 0; i < m.notes.size(); ++i) {
        const PanelNote& n = m.notes[i];
        lv_obj_t* row = make_row(Part::NoteRow, i);
        lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START);
        detail::make_badge(row, n.badge, n.source, kBadgeSize);
        lv_obj_t* text = make_box(row, LV_FLEX_FLOW_COLUMN);
        lv_obj_set_width(text, 0);
        lv_obj_set_flex_grow(text, 1);
        lv_obj_set_style_pad_top(text, theme::kGapXs, 0);
        lv_obj_t* title = make_text(text, n.title.c_str(), theme::kText);
        lv_obj_set_width(title, LV_PCT(100));
        lv_label_set_long_mode(title, LV_LABEL_LONG_MODE_DOTS);
        if (!n.body.empty()) {
            lv_obj_t* body = make_text(text, n.body.c_str(), theme::kTextDim);
            lv_obj_set_width(body, LV_PCT(100));
            lv_label_set_long_mode(body, LV_LABEL_LONG_MODE_WRAP);
        }
        if (!n.action_label.empty() || !n.action2_label.empty()) {
            // Side by side, wrapping when the row is too narrow for both.
            lv_obj_t* actions = make_box(text, LV_FLEX_FLOW_ROW_WRAP);
            const std::pair<const os::LargeString*, Part> buttons[] = {
                {&n.action_label, Part::NoteAction}, {&n.action2_label, Part::NoteAction2}};
            for (const auto& [label_text, part] : buttons) {
                if (label_text->empty()) continue;
                lv_obj_t* action = lv_button_create(actions);
                set_button_role(action, ButtonRole::Secondary);
                lv_obj_set_height(action, LV_SIZE_CONTENT);
                lv_obj_set_style_min_height(action, kPanelRowMinH, 0);
                lv_obj_t* label = lv_label_create(action);
                lv_label_set_text(label, label_text->c_str());
                lv_obj_center(label);
                touchable(action, part, i);
            }
        }
        make_close(row, Part::NoteClose, i);
    }
    if (!m.notes.empty()) {
        lv_obj_t* actions = make_dialog_actions(panel_);
        lv_obj_t* clear =
            add_dialog_button(actions, tr(StrId::PanelClearAll), ButtonRole::Secondary);
        touchable(clear, Part::ClearAll, 0);
    }

    // Laid out now, so a touch read before the next frame hits the rows
    // where they are drawn; the scroll stays where it was, within range.
    lv_obj_update_layout(panel_);
    const int32_t max_y =
        std::max(int32_t{0}, lv_obj_get_scroll_y(panel_) + lv_obj_get_scroll_bottom(panel_));
    lv_obj_scroll_to_y(panel_, std::min(scroll_y, max_y), LV_ANIM_OFF);
}

Result<void> SystemLayer::tap_panel_row(size_t index) {
    if (panel_ == nullptr) return Result<void>::err(ErrorCode::NotFound, "the panel is not open");
    if (panel_moving()) return Result<void>::err(ErrorCode::Busy, "the panel is moving");
    size_t seen = 0;
    const uint32_t n = lv_obj_get_child_count(panel_);
    for (uint32_t i = 0; i < n; ++i) {
        lv_obj_t* row = lv_obj_get_child(panel_, static_cast<int32_t>(i));
        const Part part = part_of(row);
        if (part != Part::SceneRow && part != Part::NoteRow) continue;
        if (seen++ != index) continue;
        // Sent by code, without a pointer: panel_cb takes it for a tap.
        lv_obj_send_event(row, LV_EVENT_CLICKED, nullptr);
        return Result<void>::ok();
    }
    return Result<void>::err(ErrorCode::NotFound, "no such row");
}

void SystemLayer::discard_panel() {
    if (panel_ == nullptr) return;
    lv_anim_delete(this, &SystemLayer::panel_slide_cb);
    motion_ = PanelMotion::Still;
    if (panel_event_depth_ > 0) {
        // Inside one of its events LVGL still uses the object afterwards.
        lv_obj_add_flag(panel_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(panel_backdrop_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_add_flag(panel_handle_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_delete_async(panel_);
        lv_obj_delete_async(panel_backdrop_);
        lv_obj_delete_async(panel_handle_);
    } else {
        lv_obj_delete(panel_);
        lv_obj_delete(panel_backdrop_);
        lv_obj_delete(panel_handle_);
    }
    panel_ = nullptr;
    panel_backdrop_ = nullptr;
    panel_handle_ = nullptr;
    // A closed panel keeps nothing it showed; on_open builds the next model.
    model_ = PanelModel{};
    WLOG_I("ui", "panel: gone");
    // A copy: the owner may replace the handlers from it.
    if (std::function<void()> on_close = panel_handlers_.on_close) on_close();
}

void SystemLayer::panel_cb(lv_event_t* e) {
    auto* self = static_cast<SystemLayer*>(lv_event_get_user_data(e));
    auto* obj = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    lv_indev_t* indev = lv_indev_active();
    lv_point_t at{};
    if (indev != nullptr) lv_indev_get_point(indev, &at);
    if (lv_event_get_code(e) == LV_EVENT_PRESSED) {
        self->press_at_ = at;
        return;
    }
    // While the sheet follows a finger or slides its rows are not where
    // they will rest: no press reaches them then (observe() ignores one
    // made meanwhile), and no click sent by code may tap them either.
    if (self->motion_ != PanelMotion::Still) return;
    // The rows drawn now are an older model's until the queued refill
    // runs; an index may name another notification by then.
    if (self->refill_queued_) return;
    // A click without a pointer (sent by code) is a tap.
    const detail::PressEnd end = indev != nullptr
                                     ? detail::classify_press(self->press_at_, at, kSwipe)
                                     : detail::PressEnd::Tap;
    const Part part = part_of(obj);
    const size_t i = index_of(obj);
    const PanelModel& m = self->model_;
    const PanelHandlers& h = self->panel_handlers_;
    // A row drawn from an older model whose refill is still queued.
    const bool scene_part = part == Part::SceneRow || part == Part::SceneClose;
    const bool note_part = part == Part::NoteRow || part == Part::NoteClose ||
                           part == Part::NoteAction || part == Part::NoteAction2;
    if ((scene_part && i >= m.scenes.size()) || (note_part && i >= m.notes.size())) return;

    // Copies: closing the panel or the handler itself may change both.
    std::function<void()> call;
    bool close = false;
    if (end == detail::PressEnd::SwipeUp) {
        close = true;
    } else if (end == detail::PressEnd::SwipeLeft && note_part) {
        // A swipe that starts on the row's buttons swipes the row.
        call = [fn = h.dismiss_note, n = m.notes[i]] {
            if (fn) fn(std::string(n.source), std::string(n.id));
        };
    } else if (end == detail::PressEnd::Tap) {
        switch (part) {
            case Part::Blank:
                close = true;
                break;
            case Part::Header:
                break;
            case Part::SceneRow:
                close = true;
                call = [fn = h.open_scene, id = std::string(m.scenes[i].id)] {
                    if (fn) fn(id);
                };
                break;
            case Part::SceneClose:
                call = [fn = h.exit_scene, id = std::string(m.scenes[i].id)] {
                    if (fn) fn(id);
                };
                break;
            case Part::NoteRow:
                close = true;
                call = [fn = h.tap_note, n = m.notes[i]] {
                    if (fn) fn(std::string(n.source), std::string(n.id));
                };
                break;
            case Part::NoteClose:
                call = [fn = h.dismiss_note, n = m.notes[i]] {
                    if (fn) fn(std::string(n.source), std::string(n.id));
                };
                break;
            case Part::NoteAction:
            case Part::NoteAction2:
                call = [fn = h.note_action, n = m.notes[i], index = part == Part::NoteAction2] {
                    if (fn) fn(std::string(n.source), std::string(n.id), index ? 1 : 0);
                };
                break;
            case Part::ClearAll:
                call = h.clear_all;
                break;
        }
    }
    ++self->panel_event_depth_;
    if (close && call) {
        // The row opens something: the panel goes at once.
        self->close_panel();
    } else if (close) {
        self->slide_panel(false);
    }
    if (call) call();
    --self->panel_event_depth_;
}

}  // namespace wisp::ui
