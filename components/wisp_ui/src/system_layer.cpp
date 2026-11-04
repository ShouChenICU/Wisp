#include "wisp/ui/system_layer.hpp"

#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>

#include "lvgl_private.h"  // lv_hit_test_info_t
#include "wisp/ui/overlay.hpp"

namespace wisp::ui {

namespace {

// Height of the pinyin candidate row above the keyboard.
constexpr int32_t kCandidateHeight = 44;

constexpr auto width(int w) { return static_cast<lv_buttonmatrix_ctrl_t>(w); }
constexpr auto key(int w) {
    return static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_POPOVER | w);
}
constexpr auto checked(lv_buttonmatrix_ctrl_t c) {
    return static_cast<lv_buttonmatrix_ctrl_t>(LV_BUTTONMATRIX_CTRL_CHECKED | c);
}
constexpr auto control(int w) {
    return static_cast<lv_buttonmatrix_ctrl_t>(LV_KEYBOARD_CTRL_BUTTON_FLAGS | w);
}

// LVGL's lower and upper case layouts with a language key beside the
// close key; the other layouts stay LVGL's. lv_keyboard_set_map() stores
// the maps for every keyboard and lv_buttonmatrix keeps the pointer, so
// they live for the whole program. The language key's slot is filled in
// from ui::tr() (static text) before each keyboard is built, on the UI
// thread.
constexpr size_t kLangKeySlot = 39;
// clang-format off
const char* g_lower_map[] = {
    "1#", "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", LV_SYMBOL_BACKSPACE, "\n",
    "ABC", "a", "s", "d", "f", "g", "h", "j", "k", "l", LV_SYMBOL_NEW_LINE, "\n",
    "_", "-", "z", "x", "c", "v", "b", "n", "m", ".", ",", ":", "\n",
    LV_SYMBOL_KEYBOARD, nullptr /* kLangKeySlot */, LV_SYMBOL_LEFT, " ", LV_SYMBOL_RIGHT,
    LV_SYMBOL_OK, ""};
const char* g_upper_map[] = {
    "1#", "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", LV_SYMBOL_BACKSPACE, "\n",
    "abc", "A", "S", "D", "F", "G", "H", "J", "K", "L", LV_SYMBOL_NEW_LINE, "\n",
    "_", "-", "Z", "X", "C", "V", "B", "N", "M", ".", ",", ":", "\n",
    LV_SYMBOL_CLOSE, nullptr /* kLangKeySlot */, LV_SYMBOL_LEFT, " ", LV_SYMBOL_RIGHT,
    LV_SYMBOL_OK, ""};
// One entry per key (the "\n" row breaks have none). Both layouts share
// it, as LVGL's own lower and upper case control maps are equal.
const lv_buttonmatrix_ctrl_t g_ctrl_map[] = {
    control(5), key(4), key(4), key(4), key(4), key(4), key(4), key(4), key(4), key(4), key(4),
    checked(width(7)),
    control(6), key(3), key(3), key(3), key(3), key(3), key(3), key(3), key(3), key(3),
    checked(width(7)),
    checked(key(1)), checked(key(1)), key(1), key(1), key(1), key(1), key(1), key(1), key(1),
    checked(key(1)), checked(key(1)), checked(key(1)),
    control(2), control(2), checked(width(2)), width(4), checked(width(2)), control(2)};
// clang-format on
static_assert(sizeof(g_lower_map) == sizeof(g_upper_map));

int32_t width_of(const hal::Area& a) { return a.x2 - a.x1 + 1; }
int32_t height_of(const hal::Area& a) { return a.y2 - a.y1 + 1; }

// A plain, non-clickable box for laying out the panel's contents.
lv_obj_t* make_container(lv_obj_t* parent) {
    lv_obj_t* c = lv_obj_create(parent);
    lv_obj_remove_style_all(c);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    return c;
}

}  // namespace

SystemLayer::SystemLayer(const hal::ScreenShape& shape)
    : shape_(shape), safe_(hal::safe_rect(shape)), pinyin_(lang() == Lang::Zh) {}

SystemLayer::~SystemLayer() {
    close_confirm(confirm_box_);
    hide_error();
    discard_keyboard(true);
    if (strip_ != nullptr) {
        lv_obj_delete(strip_);
        strip_ = nullptr;
    }
}

// --- Back gesture ---

bool SystemLayer::in_back_zone(const hal::ScreenShape& s, int32_t x, int32_t y) {
    const hal::Area safe = hal::safe_rect(s);
    if (y < safe.y1 || y > safe.y2) return false;
    const hal::Span span = hal::row_span(s, y);
    if (span.empty()) return false;
    return x >= span.x1 && x < span.x1 + kBackZoneWidth;
}

bool SystemLayer::is_back_swipe(const hal::ScreenShape& s, lv_point_t start, lv_point_t end) {
    if (!in_back_zone(s, start.x, start.y)) return false;
    const int32_t dx = end.x - start.x;
    const int32_t dy = end.y - start.y;
    return dx > s.width / 4 && std::abs(dy) < dx;
}

void SystemLayer::set_back_handler(std::function<void()> on_back) {
    on_back_ = std::move(on_back);
    if (!on_back_) {
        pressing_ = false;
        if (strip_ != nullptr) lv_obj_add_flag(strip_, LV_OBJ_FLAG_HIDDEN);
        return;
    }
    if (strip_ == nullptr) {
        // From the leftmost to the rightmost row start, plus the zone: the
        // hit test (in_back_zone()) then follows the edge row by row.
        int32_t min_x1 = shape_.width;
        int32_t max_x1 = 0;
        for (int32_t y = safe_.y1; y <= safe_.y2; ++y) {
            const hal::Span span = hal::row_span(shape_, y);
            if (span.empty()) continue;
            min_x1 = std::min(min_x1, span.x1);
            max_x1 = std::max(max_x1, span.x1);
        }
        if (min_x1 > max_x1) min_x1 = max_x1 = safe_.x1;
        strip_ = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(strip_);  // transparent: nothing to draw
        lv_obj_add_flag(strip_, LV_OBJ_FLAG_ADV_HITTEST);
        lv_obj_add_flag(strip_, LV_OBJ_FLAG_PRESS_LOCK);
        lv_obj_remove_flag(strip_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_remove_flag(strip_, LV_OBJ_FLAG_SCROLL_CHAIN);
        lv_obj_remove_flag(strip_, LV_OBJ_FLAG_GESTURE_BUBBLE);
        lv_obj_remove_flag(strip_, LV_OBJ_FLAG_CLICK_FOCUSABLE);
        lv_obj_set_pos(strip_, min_x1, safe_.y1);
        lv_obj_set_size(strip_, max_x1 + kBackZoneWidth - min_x1, height_of(safe_));
        for (lv_event_code_t code :
             {LV_EVENT_HIT_TEST, LV_EVENT_PRESSED, LV_EVENT_RELEASED, LV_EVENT_PRESS_LOST}) {
            lv_obj_add_event_cb(strip_, &SystemLayer::strip_event_cb, code, this);
        }
    }
    lv_obj_remove_flag(strip_, LV_OBJ_FLAG_HIDDEN);
    raise();
}

void SystemLayer::strip_event_cb(lv_event_t* e) {
    auto* self = static_cast<SystemLayer*>(lv_event_get_user_data(e));
    switch (lv_event_get_code(e)) {
        case LV_EVENT_HIT_TEST: {
            auto* info = static_cast<lv_hit_test_info_t*>(lv_event_get_param(e));
            info->res = in_back_zone(self->shape_, info->point->x, info->point->y);
            break;
        }
        case LV_EVENT_PRESSED:
            lv_indev_get_point(lv_indev_active(), &self->press_start_);
            self->pressing_ = true;
            break;
        case LV_EVENT_RELEASED: {
            if (!self->pressing_) break;
            self->pressing_ = false;
            lv_point_t end;
            lv_indev_get_point(lv_indev_active(), &end);
            if (self->on_back_ && is_back_swipe(self->shape_, self->press_start_, end)) {
                // A copy: the handler may replace or clear itself.
                std::function<void()> on_back = self->on_back_;
                on_back();
            }
            break;
        }
        default:
            self->pressing_ = false;
            break;
    }
}

// --- Error panel ---

void SystemLayer::show_error(ErrorPanelInfo info, std::function<void()> on_reload,
                             std::function<void()> on_back) {
    hide_error();
    on_reload_ = std::move(on_reload);
    on_error_back_ = std::move(on_back);

    lv_obj_t* panel = lv_obj_create(lv_layer_top());
    lv_obj_set_pos(panel, safe_.x1, safe_.y1);
    lv_obj_set_size(panel, width_of(safe_), height_of(safe_));
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(panel, 0, 0);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_flex_flow(panel, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_style_pad_row(panel, 8, 0);

    lv_obj_t* title = lv_label_create(panel);
    lv_label_set_text(title, tr(StrId::SceneErrorTitle));
    lv_obj_set_style_text_color(title, lv_palette_main(LV_PALETTE_RED), 0);

    std::string where = info.file;
    if (info.line > 0) where += ":" + std::to_string(info.line);
    std::string body = info.scene_name + "\n" + where + "\n" + info.message;
    lv_obj_t* text = lv_label_create(panel);
    lv_label_set_text(text, body.c_str());
    lv_label_set_long_mode(text, LV_LABEL_LONG_MODE_DOTS);
    lv_obj_set_width(text, LV_PCT(100));
    lv_obj_set_flex_grow(text, 1);

    lv_obj_t* row = make_container(panel);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_EVENLY, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    for (StrId id : {StrId::SceneReload, StrId::SceneBack}) {
        lv_obj_t* btn = lv_button_create(row);
        lv_label_set_text(lv_label_create(btn), tr(id));
        lv_obj_add_event_cb(btn, &SystemLayer::error_btn_cb, LV_EVENT_CLICKED, this);
        if (id == StrId::SceneReload) error_reload_ = btn;
    }

    error_panel_ = panel;
    mark_opaque_overlay(panel);
    // Laid out now, so a touch read before the next frame hits the
    // buttons where they are drawn.
    lv_obj_update_layout(panel);
    raise();
}

void SystemLayer::hide_error() {
    if (error_panel_ == nullptr) return;
    lv_obj_delete(error_panel_);
    error_panel_ = nullptr;
    error_reload_ = nullptr;
    on_reload_ = nullptr;
    on_error_back_ = nullptr;
}

void SystemLayer::error_btn_cb(lv_event_t* e) {
    auto* self = static_cast<SystemLayer*>(lv_event_get_user_data(e));
    const bool reload = lv_event_get_target(e) == self->error_reload_;
    std::function<void()> fn =
        reload ? std::move(self->on_reload_) : std::move(self->on_error_back_);
    self->hide_error();
    if (fn) fn();
}

// --- Keyboard ---

void SystemLayer::show_keyboard(lv_obj_t* textarea) {
    if (textarea == nullptr) return;
    if (kb_ != nullptr) {
        if (lv_keyboard_get_textarea(kb_) != textarea) lv_keyboard_set_textarea(kb_, textarea);
        return;
    }
    build_keyboard(textarea);
    raise();
}

void SystemLayer::hide_keyboard() { discard_keyboard(kb_event_depth_ == 0); }

void SystemLayer::build_keyboard(lv_obj_t* textarea) {
    lv_obj_t* top = lv_layer_top();
    kb_ = lv_keyboard_create(top);
    // key_cb() takes the language key and hands every other key to LVGL's
    // own handler, which would otherwise type the key's label.
    lv_obj_remove_event_cb(kb_, lv_keyboard_def_event_cb);
    lv_obj_add_event_cb(kb_, &SystemLayer::key_cb, LV_EVENT_VALUE_CHANGED, this);
    lv_obj_add_event_cb(kb_, &SystemLayer::kb_done_cb, LV_EVENT_READY, this);
    lv_obj_add_event_cb(kb_, &SystemLayer::kb_done_cb, LV_EVENT_CANCEL, this);
    const char* lang_key = tr(pinyin_ ? StrId::KeyboardZh : StrId::KeyboardEn);
    g_lower_map[kLangKeySlot] = lang_key;
    g_upper_map[kLangKeySlot] = lang_key;
    lv_keyboard_set_map(kb_, LV_KEYBOARD_MODE_TEXT_LOWER, g_lower_map, g_ctrl_map);
    lv_keyboard_set_map(kb_, LV_KEYBOARD_MODE_TEXT_UPPER, g_upper_map, g_ctrl_map);

    // The lower half of the safe area.
    const int32_t top_y = safe_.y1 + height_of(safe_) / 2;
    lv_obj_set_align(kb_, LV_ALIGN_TOP_LEFT);
    lv_obj_set_pos(kb_, safe_.x1, top_y);
    lv_obj_set_size(kb_, width_of(safe_), safe_.y2 - top_y + 1);
    lv_obj_set_style_bg_opa(kb_, LV_OPA_COVER, 0);
    lv_keyboard_set_textarea(kb_, textarea);

    if (pinyin_) {
        ime_ = lv_ime_pinyin_create(top);
        // LVGL only reads the dictionary; the setter is just not const.
        lv_ime_pinyin_set_dict(ime_, const_cast<lv_pinyin_dict_t*>(detail::pinyin_dict()));
        lv_ime_pinyin_set_keyboard(ime_, kb_);
        // LVGL's IME puts the candidate row above the keyboard across the
        // whole parent (the screen) and leaves it transparent; it stays
        // inside the safe area here and covers what is under it, like the
        // keyboard.
        candidates_ = lv_ime_pinyin_get_cand_panel(ime_);
        lv_obj_set_align(candidates_, LV_ALIGN_TOP_LEFT);
        lv_obj_set_pos(candidates_, safe_.x1, top_y - kCandidateHeight);
        lv_obj_set_size(candidates_, width_of(safe_), kCandidateHeight);
        lv_obj_set_style_bg_color(candidates_, lv_obj_get_style_bg_color(kb_, LV_PART_MAIN), 0);
        lv_obj_set_style_bg_opa(candidates_, LV_OPA_COVER, 0);
        mark_opaque_overlay(candidates_);
    }
    mark_opaque_overlay(kb_);
    lv_obj_update_layout(kb_);
}

void SystemLayer::discard_keyboard(bool now) {
    // The IME deletes its keyboard and candidate row with itself.
    lv_obj_t* root = ime_ != nullptr ? ime_ : kb_;
    if (root == nullptr) return;
    if (now) {
        lv_obj_delete(root);
    } else {
        // Inside one of the keyboard's own events LVGL's handlers still
        // use it after ours return.
        lv_obj_add_flag(kb_, LV_OBJ_FLAG_HIDDEN);
        if (candidates_ != nullptr) lv_obj_add_flag(candidates_, LV_OBJ_FLAG_HIDDEN);
        lv_obj_delete_async(root);
    }
    kb_ = nullptr;
    ime_ = nullptr;
    candidates_ = nullptr;
}

void SystemLayer::key_cb(lv_event_t* e) {
    auto* self = static_cast<SystemLayer*>(lv_event_get_user_data(e));
    lv_obj_t* kb = static_cast<lv_obj_t*>(lv_event_get_current_target(e));
    const uint32_t id = lv_buttonmatrix_get_selected_button(kb);
    const char* text =
        id != LV_BUTTONMATRIX_BUTTON_NONE ? lv_buttonmatrix_get_button_text(kb, id) : nullptr;
    if (kb == self->kb_ && text != nullptr && std::strcmp(text, g_lower_map[kLangKeySlot]) == 0) {
        // A fresh keyboard in the other language: with or without the IME,
        // and no half-typed pinyin carried over.
        lv_obj_t* textarea = lv_keyboard_get_textarea(kb);
        self->pinyin_ = !self->pinyin_;
        self->discard_keyboard(false);
        self->build_keyboard(textarea);
        self->raise();
        return;
    }
    ++self->kb_event_depth_;
    lv_keyboard_def_event_cb(e);
    --self->kb_event_depth_;
}

void SystemLayer::kb_done_cb(lv_event_t* e) {
    auto* self = static_cast<SystemLayer*>(lv_event_get_user_data(e));
    if (lv_event_get_current_target(e) == self->kb_) self->discard_keyboard(false);
}

// --- Confirm dialogs ---

lv_obj_t* SystemLayer::open_confirm(StrId title, const std::string& text,
                                    std::function<void(bool yes)> on_answer) {
    if (confirm_box_ != nullptr) return nullptr;
    lv_obj_t* box = lv_msgbox_create(nullptr);
    // lv_msgbox_class's default width is a fixed LV_DPI_DEF * 2,
    // unrelated to the screen; clamped so a small round screen does not
    // clip it.
    lv_obj_set_width(box, std::min<int32_t>(LV_DPI_DEF * 2, width_of(safe_)));
    lv_msgbox_add_title(box, tr(title));
    lv_msgbox_add_text(box, text.c_str());
    lv_obj_t* yes = lv_msgbox_add_footer_button(box, tr(StrId::Yes));
    lv_obj_t* no = lv_msgbox_add_footer_button(box, tr(StrId::No));
    lv_obj_add_event_cb(yes, &SystemLayer::confirm_btn_cb, LV_EVENT_CLICKED, this);
    lv_obj_add_event_cb(no, &SystemLayer::confirm_btn_cb, LV_EVENT_CLICKED, this);
    // Laid out now rather than at the next refresh: a touch read in
    // between would otherwise hit-test the box at zero size and land on
    // the backdrop.
    lv_obj_update_layout(box);
    confirm_box_ = box;
    confirm_yes_ = yes;
    on_answer_ = std::move(on_answer);
    return box;
}

void SystemLayer::close_confirm(lv_obj_t* box) {
    if (box == nullptr || box != confirm_box_) return;
    lv_msgbox_close(box);
    confirm_box_ = nullptr;
    confirm_yes_ = nullptr;
    on_answer_ = nullptr;
}

void SystemLayer::confirm_btn_cb(lv_event_t* e) {
    auto* self = static_cast<SystemLayer*>(lv_event_get_user_data(e));
    const bool yes = lv_event_get_target(e) == self->confirm_yes_;
    // Closed first, so the answer may open the next dialog.
    std::function<void(bool)> on_answer = std::move(self->on_answer_);
    self->close_confirm(self->confirm_box_);
    if (on_answer) on_answer(yes);
}

// --- Stacking ---

void SystemLayer::raise() {
    for (lv_obj_t* piece : {error_panel_, kb_, candidates_, strip_}) {
        if (piece != nullptr) lv_obj_move_foreground(piece);
    }
    // The dialog's backdrop holds the box.
    if (confirm_box_ != nullptr) lv_obj_move_foreground(lv_obj_get_parent(confirm_box_));
}

}  // namespace wisp::ui
