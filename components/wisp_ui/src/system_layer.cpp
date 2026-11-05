#include "wisp/ui/system_layer.hpp"

#include <algorithm>
#include <array>
#include <cstdlib>
#include <cstring>
#include <string>
#include <utility>
#include <vector>

#include "lvgl_private.h"  // lv_indev_t::pointer.act_obj, lv_buttonmatrix_t
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
// close key, one pair per input language; the other layouts stay LVGL's.
// lv_keyboard_set_map() stores the maps for every keyboard and
// lv_buttonmatrix keeps the pointer, so they live for the whole program:
// constant tables, in flash.
using KeyMap = std::array<const char*, 45>;
constexpr size_t kLangKeySlot = 39;
// clang-format off
constexpr KeyMap lower_map(const char* lang_key) {
    return {"1#", "q", "w", "e", "r", "t", "y", "u", "i", "o", "p", LV_SYMBOL_BACKSPACE, "\n",
            "ABC", "a", "s", "d", "f", "g", "h", "j", "k", "l", LV_SYMBOL_NEW_LINE, "\n",
            "_", "-", "z", "x", "c", "v", "b", "n", "m", ".", ",", ":", "\n",
            LV_SYMBOL_KEYBOARD, lang_key, LV_SYMBOL_LEFT, " ", LV_SYMBOL_RIGHT, LV_SYMBOL_OK, ""};
}
constexpr KeyMap upper_map(const char* lang_key) {
    return {"1#", "Q", "W", "E", "R", "T", "Y", "U", "I", "O", "P", LV_SYMBOL_BACKSPACE, "\n",
            "abc", "A", "S", "D", "F", "G", "H", "J", "K", "L", LV_SYMBOL_NEW_LINE, "\n",
            "_", "-", "Z", "X", "C", "V", "B", "N", "M", ".", ",", ":", "\n",
            LV_SYMBOL_CLOSE, lang_key, LV_SYMBOL_LEFT, " ", LV_SYMBOL_RIGHT, LV_SYMBOL_OK, ""};
}
// clang-format on
constexpr KeyMap kPinyinLower = lower_map(SystemLayer::kPinyinKey);
constexpr KeyMap kPinyinUpper = upper_map(SystemLayer::kPinyinKey);
constexpr KeyMap kLatinLower = lower_map(SystemLayer::kLatinKey);
constexpr KeyMap kLatinUpper = upper_map(SystemLayer::kLatinKey);
// Every slot filled: a short initializer list would leave null keys and
// no "" terminator.
static_assert(kPinyinLower.back()[0] == '\0' && kPinyinUpper.back()[0] == '\0');
static_assert(kPinyinLower[kLangKeySlot] == SystemLayer::kPinyinKey);
static_assert(kPinyinUpper[kLangKeySlot] == SystemLayer::kPinyinKey);
// clang-format off
// One entry per key (the "\n" row breaks have none). Both layouts share
// it, as LVGL's own lower and upper case control maps are equal.
const lv_buttonmatrix_ctrl_t kCtrlMap[] = {
    control(5), key(4), key(4), key(4), key(4), key(4), key(4), key(4), key(4), key(4), key(4),
    checked(width(7)),
    control(6), key(3), key(3), key(3), key(3), key(3), key(3), key(3), key(3), key(3),
    checked(width(7)),
    checked(key(1)), checked(key(1)), key(1), key(1), key(1), key(1), key(1), key(1), key(1),
    checked(key(1)), checked(key(1)), checked(key(1)),
    control(2), control(2), checked(width(2)), width(4), checked(width(2)), control(2)};
// clang-format on

int32_t width_of(const hal::Area& a) { return a.x2 - a.x1 + 1; }
int32_t height_of(const hal::Area& a) { return a.y2 - a.y1 + 1; }

// How far keys stay inside the visible width of their rows, like
// hal::safe_rect()'s corner margin: panel outlines are rarely known to the
// pixel. Also the keyboard's own side padding, so on a rectangle every row
// is the keyboard's full content width.
constexpr int32_t kKeyInset = 4;
// The narrowest a key may get before the keyboard rises on a round
// screen to find wider rows, and how far it may rise: at most this share
// of the screen height (percent).
constexpr int32_t kMinKeyWidth = 28;
constexpr int32_t kMaxKeyboardPercent = 55;
// The step the keyboard rises by while looking for rows that fit.
constexpr int32_t kRiseStep = 4;

// The visible columns common to screen rows y1..y2, `inset` further in
// from each side; empty if some row shows nothing.
hal::Span visible_extent(const hal::ScreenShape& shape, int32_t y1, int32_t y2, int32_t inset) {
    hal::Span ext{0, shape.width - 1};
    for (int32_t y = y1; y <= y2; ++y) {
        const hal::Span s = hal::row_span(shape, y);
        if (s.empty()) return {};
        ext.x1 = std::max(ext.x1, s.x1);
        ext.x2 = std::min(ext.x2, s.x2);
    }
    return {ext.x1 + inset, ext.x2 - inset};
}

uint32_t key_units(lv_buttonmatrix_ctrl_t ctrl) {
    // As lv_buttonmatrix: a width of 0 counts as 1.
    const uint32_t w = ctrl & LV_BUTTONMATRIX_CTRL_WIDTH_15;  // the width bits
    return w != 0 ? w : 1;
}

// One row of a key map: how many keys, how many width units in all, and
// the narrowest key's units.
struct KeyRow {
    uint32_t keys = 0;
    uint32_t units = 0;
    uint32_t min_units = 0;
};

std::vector<KeyRow> key_rows(const char* const* map, const lv_buttonmatrix_ctrl_t* ctrl) {
    std::vector<KeyRow> rows;
    KeyRow row;
    for (size_t i = 0, k = 0;; ++i) {
        const char* t = map[i];
        const bool end = t == nullptr || t[0] == '\0';
        if (end || std::strcmp(t, "\n") == 0) {
            if (row.keys > 0) rows.push_back(row);
            row = {};
            if (end) break;
            continue;
        }
        const uint32_t u = key_units(ctrl[k++]);
        row.keys += 1;
        row.units += u;
        row.min_units = row.min_units == 0 ? u : std::min(row.min_units, u);
    }
    return rows;
}

// Spreads the keys of each row of `matrix` across the visible width of
// that row's pixel rows (visible_extent() with kKeyInset) instead of the
// matrix's whole content width. lv_buttonmatrix lays every row out over
// the same width whenever its map, size or style changes; this redoes only
// the x positions, with the same unit arithmetic, so the keys drawn and
// the keys hit stay the same rectangles.
void fit_rows(lv_obj_t* matrix, const hal::ScreenShape& shape) {
    auto* m = reinterpret_cast<lv_buttonmatrix_t*>(matrix);
    if (m->button_areas == nullptr || m->map_p == nullptr || m->ctrl_bits == nullptr) return;
    lv_area_t box;
    lv_obj_get_coords(matrix, &box);
    const int32_t content_x1 = lv_obj_get_style_space_left(matrix, LV_PART_MAIN);
    const int32_t content_x2 =
        lv_area_get_width(&box) - 1 - lv_obj_get_style_space_right(matrix, LV_PART_MAIN);
    const int32_t pcol = lv_obj_get_style_pad_column(matrix, LV_PART_MAIN);
    const char* const* map = m->map_p;
    uint32_t first = 0;  // the row's first button
    while (first < m->btn_cnt) {
        uint32_t n = 0;
        uint32_t units = 0;
        while (map[n] != nullptr && map[n][0] != '\0' && std::strcmp(map[n], "\n") != 0) {
            units += key_units(m->ctrl_bits[first + n]);
            ++n;
        }
        if (n > 0) {
            const lv_area_t& head = m->button_areas[first];
            const hal::Span ext =
                visible_extent(shape, box.y1 + head.y1, box.y1 + head.y2, kKeyInset);
            int32_t x1 = std::max(content_x1, ext.x1 - box.x1);
            int32_t x2 = std::min(content_x2, ext.x2 - box.x1);
            if (x1 > x2) {
                x1 = content_x1;
                x2 = content_x2;
            }
            const int32_t w =
                std::max(int32_t{0}, x2 - x1 + 1 - pcol * static_cast<int32_t>(n - 1));
            uint32_t at = 0;
            for (uint32_t b = 0; b < n; ++b) {
                lv_area_t& a = m->button_areas[first + b];
                const uint32_t u = key_units(m->ctrl_bits[first + b]);
                const auto gap = static_cast<int32_t>(b) * pcol;
                a.x1 = x1 + w * static_cast<int32_t>(at) / static_cast<int32_t>(units) + gap;
                a.x2 =
                    x1 + w * static_cast<int32_t>(at + u) / static_cast<int32_t>(units) + gap - 1;
                at += u;
            }
        }
        first += n;
        map += n;
        if (*map == nullptr || (*map)[0] == '\0') break;
        ++map;  // the "\n"
    }
    lv_obj_invalidate(matrix);
}

void fit_rows_cb(lv_event_t* e) {
    auto* shape = static_cast<const hal::ScreenShape*>(lv_event_get_user_data(e));
    fit_rows(static_cast<lv_obj_t*>(lv_event_get_current_target(e)), *shape);
}

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
    unhook_pointer();
    if (arrow_ != nullptr) {
        lv_obj_delete(arrow_);
        arrow_ = nullptr;
    }
}

// --- Back gesture ---

namespace {

// The layer whose observe() the wrapped read callback calls. LVGL is one
// instance per process (UiRuntime refuses a second), and so is the
// pointer it wraps.
SystemLayer* g_pointer_watcher = nullptr;

// The arrow's distance from the visible edge once fully in, like
// hal::safe_rect()'s corner margin: panel corners are rarely known to the
// pixel.
constexpr int32_t kArrowEdgeInset = 4;

bool point_in(lv_obj_t* obj, lv_point_t p) {
    if (obj == nullptr || lv_obj_has_flag(obj, LV_OBJ_FLAG_HIDDEN)) return false;
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    return lv_area_is_point_on(&a, &p, 0);
}

}  // namespace

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
    return dx >= kBackTriggerDx && dx > 2 * std::abs(dy);
}

void SystemLayer::set_back_handler(std::function<void()> on_back) {
    on_back_ = std::move(on_back);
    if (on_back_) {
        hook_pointer();
    } else {
        end_swipe();
        unhook_pointer();
    }
}

lv_obj_t* SystemLayer::back_arrow() const {
    if (arrow_ == nullptr || lv_obj_has_flag(arrow_, LV_OBJ_FLAG_HIDDEN)) return nullptr;
    return arrow_;
}

// LVGL tells an input device's event callbacks about PRESSED and RELEASED
// only when a widget is under the finger, and never about PRESSING; the
// read callback sees every read, so the gesture is followed from there.
void SystemLayer::hook_pointer() {
    if (indev_ != nullptr) return;
    for (lv_indev_t* i = lv_indev_get_next(nullptr); i != nullptr; i = lv_indev_get_next(i)) {
        if (lv_indev_get_type(i) == LV_INDEV_TYPE_POINTER) {
            indev_ = i;
            break;
        }
    }
    if (indev_ == nullptr) return;
    prev_read_cb_ = lv_indev_get_read_cb(indev_);
    lv_indev_set_read_cb(indev_, &SystemLayer::pointer_read_cb);
    g_pointer_watcher = this;
    // A press already under way when the handler is set is not a swipe.
    pressed_ = lv_indev_get_state(indev_) == LV_INDEV_STATE_PRESSED;
    candidate_ = false;
    taken_ = false;
}

void SystemLayer::unhook_pointer() {
    if (indev_ == nullptr) return;
    // The device may be gone already when the UI stops first.
    for (lv_indev_t* i = lv_indev_get_next(nullptr); i != nullptr; i = lv_indev_get_next(i)) {
        if (i == indev_ && lv_indev_get_read_cb(i) == &SystemLayer::pointer_read_cb) {
            lv_indev_set_read_cb(i, prev_read_cb_);
        }
    }
    if (g_pointer_watcher == this) g_pointer_watcher = nullptr;
    indev_ = nullptr;
    prev_read_cb_ = nullptr;
    candidate_ = false;
    taken_ = false;
}

void SystemLayer::pointer_read_cb(lv_indev_t* indev, lv_indev_data_t* data) {
    SystemLayer* self = g_pointer_watcher;
    if (self == nullptr || self->indev_ != indev) return;
    if (self->prev_read_cb_ != nullptr) self->prev_read_cb_(indev, data);
    self->observe(indev, *data);
}

void SystemLayer::observe(lv_indev_t* indev, const lv_indev_data_t& data) {
    // LVGL rotates the raw point after this callback; the zone is in
    // display coordinates.
    lv_point_t p = data.point;
    lv_display_rotate_point(lv_indev_get_display(indev), &p);

    if (data.state != LV_INDEV_STATE_PRESSED) {
        // A release (LVGL fills in the last pressed point).
        const bool was_pressed = pressed_;
        pressed_ = false;
        if (!was_pressed || !candidate_) return;
        const bool fire = taken_ && is_back_swipe(shape_, start_, p) && on_back_;
        end_swipe();
        if (fire) {
            // A copy: the handler may replace or clear itself.
            std::function<void()> on_back = on_back_;
            on_back();
        }
        return;
    }

    if (!pressed_) {
        pressed_ = true;
        start_ = p;
        taken_ = false;
        candidate_ = on_back_ && confirm_box_ == nullptr && !point_in(kb_, p) &&
                     !point_in(candidates_, p) && in_back_zone(shape_, p.x, p.y);
        return;
    }
    if (!candidate_) return;

    const int32_t dx = p.x - start_.x;
    const int32_t dy = std::abs(p.y - start_.y);
    const bool past = is_back_swipe(shape_, start_, p);
    if (!taken_) {
        if (past) {
            taken_ = true;
            cancel_press(indev);
        } else if (2 * dy >= kBackTriggerDx && 2 * dy >= dx) {
            // Clearly up or down: a scroll, no longer a swipe.
            end_swipe();
            return;
        }
    }
    if (taken_ || dx > 0) {
        show_arrow(p, dx, past);
    } else if (arrow_ != nullptr) {
        lv_obj_add_flag(arrow_, LV_OBJ_FLAG_HIDDEN);
    }
}

// Runs inside the read callback, before LVGL processes this read. The
// widget holding the press gets PRESS_LOST (it drops its pressed look, a
// slider its drag); the reset sends LV_EVENT_INDEV_RESET to it and to the
// object being scrolled (clearing PRESSED and SCROLLED) and makes LVGL
// forget both right after this callback returns. Waiting for the release
// then keeps LVGL from finding a new widget under the moving finger: the
// following pressed reads are skipped, and the release sends no RELEASED
// or CLICKED, since no widget has the press any more.
void SystemLayer::cancel_press(lv_indev_t* indev) {
    lv_obj_t* act = indev->pointer.act_obj;
    if (act != nullptr) lv_obj_send_event(act, LV_EVENT_PRESS_LOST, indev);
    lv_indev_reset(indev, nullptr);
    lv_indev_wait_release(indev);
}

void SystemLayer::show_arrow(lv_point_t at, int32_t dx, bool armed) {
    if (arrow_ == nullptr) {
        arrow_ = lv_obj_create(lv_layer_top());
        lv_obj_remove_style_all(arrow_);
        lv_obj_remove_flag(arrow_, LV_OBJ_FLAG_CLICKABLE);
        lv_obj_remove_flag(arrow_, LV_OBJ_FLAG_SCROLLABLE);
        lv_obj_set_size(arrow_, kBackArrowSize, kBackArrowSize);
        lv_obj_set_style_radius(arrow_, LV_RADIUS_CIRCLE, 0);
        lv_obj_set_style_bg_color(arrow_, lv_palette_main(LV_PALETTE_BLUE), 0);
        lv_obj_set_style_bg_opa(arrow_, LV_OPA_COVER, 0);
        lv_obj_t* icon = lv_label_create(arrow_);
        lv_label_set_text(icon, LV_SYMBOL_LEFT);
        lv_obj_set_style_text_color(icon, lv_color_white(), 0);
        lv_obj_center(icon);
        lv_obj_add_flag(arrow_, LV_OBJ_FLAG_HIDDEN);
    }
    if (lv_obj_has_flag(arrow_, LV_OBJ_FLAG_HIDDEN)) {
        lv_obj_remove_flag(arrow_, LV_OBJ_FLAG_HIDDEN);
        raise();
    }
    // At the finger's row, kept within the safe area's rows, so that its
    // box lies on rows that are visible.
    const int32_t half = kBackArrowSize / 2;
    const int32_t y1 =
        std::clamp(at.y - half, safe_.y1, std::max(safe_.y1, safe_.y2 + 1 - kBackArrowSize));
    // Fully in, its box clears the visible left edge on each of its rows.
    int32_t x_in = 0;
    for (int32_t y = y1; y < y1 + kBackArrowSize; ++y) {
        const hal::Span span = hal::row_span(shape_, y);
        if (!span.empty()) x_in = std::max(x_in, span.x1);
    }
    x_in += kArrowEdgeInset;
    // Below the trigger it is partly in and see-through, in step with dx.
    const int32_t d = std::clamp(dx, int32_t{0}, kBackTriggerDx);
    const int32_t x = armed ? x_in : x_in - half * (kBackTriggerDx - d) / kBackTriggerDx;
    const int32_t faint = LV_OPA_30;
    const int32_t strong = LV_OPA_80;
    const auto opa = static_cast<lv_opa_t>(armed ? int32_t{LV_OPA_COVER}
                                                 : faint + (strong - faint) * d / kBackTriggerDx);
    lv_obj_set_pos(arrow_, x, y1);
    lv_obj_set_style_opa(arrow_, opa, 0);
}

void SystemLayer::end_swipe() {
    candidate_ = false;
    taken_ = false;
    if (arrow_ != nullptr) lv_obj_add_flag(arrow_, LV_OBJ_FLAG_HIDDEN);
}

// --- Error panel ---

void SystemLayer::show_error(ErrorPanelInfo info, std::function<void()> on_reload,
                             std::function<void()> on_back) {
    hide_error();
    // The panel replaces the scene's UI, the textarea included.
    hide_keyboard();
    on_reload_ = std::move(on_reload);
    on_error_back_ = std::move(on_back);

    lv_obj_t* panel = lv_obj_create(lv_layer_top());
    lv_obj_set_pos(panel, safe_.x1, safe_.y1);
    lv_obj_set_size(panel, width_of(safe_), height_of(safe_));
    lv_obj_set_style_bg_opa(panel, LV_OPA_COVER, 0);
    lv_obj_set_style_shadow_width(panel, 0, 0);
    // Clickable, so a touch the buttons do not take stops here instead of
    // reaching what the panel covers.
    lv_obj_remove_flag(panel, LV_OBJ_FLAG_CLICK_FOCUSABLE);
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
        if (ta_ == textarea) return;
        // A fresh keyboard: the IME's half-typed pinyin belongs to the old
        // textarea, and picking a candidate would replace that many
        // characters of the new one.
        hide_keyboard();
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
    lv_keyboard_set_map(kb_, LV_KEYBOARD_MODE_TEXT_LOWER,
                        (pinyin_ ? kPinyinLower : kLatinLower).data(), kCtrlMap);
    lv_keyboard_set_map(kb_, LV_KEYBOARD_MODE_TEXT_UPPER,
                        (pinyin_ ? kPinyinUpper : kLatinUpper).data(), kCtrlMap);

    // The background runs to the left, right and bottom edges; the
    // screen's outline clips its corners. The key rows are laid out below.
    lv_obj_set_align(kb_, LV_ALIGN_TOP_LEFT);
    lv_obj_set_style_radius(kb_, 0, 0);
    lv_obj_set_style_pad_left(kb_, kKeyInset, 0);
    lv_obj_set_style_pad_right(kb_, kKeyInset, 0);
    const KeyboardPlace place = place_keyboard(textarea);
    const int32_t top_y = place.top;
    lv_obj_set_pos(kb_, 0, top_y);
    lv_obj_set_size(kb_, shape_.width, shape_.height - top_y);
    lv_obj_set_style_pad_bottom(kb_, place.pad_bottom, 0);
    lv_obj_set_style_bg_opa(kb_, LV_OPA_COVER, 0);
    lv_keyboard_set_textarea(kb_, textarea);
    // lv_keyboard does not watch its textarea; a key typed after the
    // scene deleted it would write into freed memory.
    ta_ = textarea;
    lv_obj_add_event_cb(ta_, &SystemLayer::ta_delete_cb, LV_EVENT_DELETE, this);

    if (pinyin_) {
        ime_ = lv_ime_pinyin_create(top);
        // LVGL only reads the dictionary; the setter is just not const.
        lv_ime_pinyin_set_dict(ime_, const_cast<lv_pinyin_dict_t*>(detail::pinyin_dict()));
        lv_ime_pinyin_set_keyboard(ime_, kb_);
        // LVGL's IME puts the candidate row above the keyboard across the
        // whole parent (the screen) and leaves it transparent; here it
        // spans the screen's width and covers what is under it, like the
        // keyboard.
        candidates_ = lv_ime_pinyin_get_cand_panel(ime_);
        lv_obj_set_align(candidates_, LV_ALIGN_TOP_LEFT);
        const int32_t cand_y = top_y - kCandidateHeight;
        lv_obj_set_pos(candidates_, 0, cand_y);
        lv_obj_set_size(candidates_, shape_.width, kCandidateHeight);
        // One row: its padding alone keeps the candidates within the
        // visible width of its rows.
        hal::Span ext = visible_extent(shape_, cand_y, top_y - 1, kKeyInset);
        if (ext.empty()) ext = {safe_.x1, safe_.x2};
        lv_obj_set_style_pad_left(candidates_, ext.x1, 0);
        lv_obj_set_style_pad_right(candidates_, shape_.width - 1 - ext.x2, 0);
        lv_obj_set_style_radius(candidates_, 0, 0);
        lv_obj_set_style_bg_color(candidates_, lv_obj_get_style_bg_color(kb_, LV_PART_MAIN), 0);
        lv_obj_set_style_bg_opa(candidates_, LV_OPA_COVER, 0);
        mark_opaque_overlay(candidates_);
    }
    mark_opaque_overlay(kb_);
    // After the IME's own handler, which may switch the map: every map,
    // size or style change lays the rows out over the full width again.
    for (lv_event_code_t code :
         {LV_EVENT_VALUE_CHANGED, LV_EVENT_SIZE_CHANGED, LV_EVENT_STYLE_CHANGED}) {
        lv_obj_add_event_cb(kb_, fit_rows_cb, code, &shape_);
    }
    lv_obj_update_layout(kb_);
    fit_rows(kb_, shape_);
}

SystemLayer::KeyboardPlace SystemLayer::place_keyboard(lv_obj_t* textarea) const {
    // Where it always sat: from the middle of the safe area down, the rows
    // ending kKeyInset above the bottom edge.
    const int32_t default_top = safe_.y1 + height_of(safe_) / 2;
    const std::vector<KeyRow> rows = key_rows(kLatinLower.data(), kCtrlMap);
    const int32_t space_top = lv_obj_get_style_space_top(kb_, LV_PART_MAIN);
    const int32_t border = lv_obj_get_style_border_width(kb_, LV_PART_MAIN);
    const int32_t prow = lv_obj_get_style_pad_row(kb_, LV_PART_MAIN);
    const int32_t pcol = lv_obj_get_style_pad_column(kb_, LV_PART_MAIN);
    const auto row_cnt = static_cast<int32_t>(rows.size());

    // Whether every row, laid out as lv_buttonmatrix does in a band from
    // `top` whose rows keep their usual height, has keys at least
    // kMinKeyWidth wide.
    const int32_t rows_h = shape_.height - kKeyInset - border - default_top - space_top;
    auto fits = [&](int32_t top) {
        const int32_t y0 = top + space_top;
        const int32_t h_no_gap = rows_h - prow * (row_cnt - 1);
        for (int32_t r = 0; r < row_cnt; ++r) {
            const KeyRow& row = rows[static_cast<size_t>(r)];
            const int32_t y1 = y0 + h_no_gap * r / row_cnt + r * prow;
            const int32_t y2 = y0 + h_no_gap * (r + 1) / row_cnt + r * prow - 1;
            const hal::Span ext = visible_extent(shape_, y1, y2, kKeyInset);
            if (ext.empty()) return false;
            const int32_t w = ext.x2 - ext.x1 + 1 - pcol * static_cast<int32_t>(row.keys - 1);
            if (w * static_cast<int32_t>(row.min_units) / static_cast<int32_t>(row.units) <
                kMinKeyWidth) {
                return false;
            }
        }
        return true;
    };

    // Too narrow there (on a round screen): rise, the band growing taller
    // and the rows keeping their height, until they fit, the band reaches
    // its share of the screen, or it would cover the textarea (with the
    // candidate row above it).
    int32_t highest = shape_.height - shape_.height * kMaxKeyboardPercent / 100;
    if (textarea != nullptr) {
        lv_obj_update_layout(textarea);
        lv_area_t ta;
        lv_obj_get_coords(textarea, &ta);
        highest = std::max(highest, ta.y2 + 1 + (pinyin_ ? kCandidateHeight : 0));
    }
    int32_t top = default_top;
    while (!fits(top) && top - kRiseStep >= highest) top -= kRiseStep;
    return {top, kKeyInset + (default_top - top)};
}

void SystemLayer::discard_keyboard(bool now) {
    if (ta_ != nullptr) {
        lv_obj_remove_event_cb_with_user_data(ta_, &SystemLayer::ta_delete_cb, this);
        ta_ = nullptr;
    }
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
    const char* lang_key = self->pinyin_ ? kPinyinKey : kLatinKey;
    if (kb == self->kb_ && text != nullptr && std::strcmp(text, lang_key) == 0) {
        // A fresh keyboard in the other language: with or without the IME,
        // and no half-typed pinyin carried over.
        lv_obj_t* textarea = self->ta_;
        self->pinyin_ = !self->pinyin_;
        self->discard_keyboard(false);
        if (textarea != nullptr) {
            self->build_keyboard(textarea);
            self->raise();
        }
        return;
    }
    ++self->kb_event_depth_;
    lv_keyboard_def_event_cb(e);
    --self->kb_event_depth_;
}

void SystemLayer::ta_delete_cb(lv_event_t* e) {
    auto* self = static_cast<SystemLayer*>(lv_event_get_user_data(e));
    // LVGL drops the textarea's callbacks itself once this event is over.
    self->ta_ = nullptr;
    // A keyboard that has to outlive one of its own events must not
    // reach the textarea after it is gone either.
    if (self->kb_ != nullptr) lv_keyboard_set_textarea(self->kb_, nullptr);
    self->hide_keyboard();
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
    for (lv_obj_t* piece : {error_panel_, kb_, candidates_, arrow_}) {
        if (piece != nullptr) lv_obj_move_foreground(piece);
    }
    // The dialog's backdrop holds the box.
    if (confirm_box_ != nullptr) lv_obj_move_foreground(lv_obj_get_parent(confirm_box_));
}

}  // namespace wisp::ui
