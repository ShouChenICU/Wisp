#include "page_frame.hpp"

#include <algorithm>

#include "wisp/ui/fonts.hpp"
#include "wisp/ui/system_layer.hpp"
#include "wisp/ui/theme_tokens.hpp"
#include "wisp/ui/ui_runtime.hpp"
#include "wisp/ui/wisp_view.hpp"

namespace wisp::shell::detail {

namespace {
// Between the list's rows, the title and the first row too.
constexpr int32_t kRowGap = ui::theme::kGapSm;
// A row card's rows: their padding, and the width of their icons.
constexpr int32_t kRowPadX = ui::theme::kGapMd;
constexpr int32_t kRowPadY = ui::theme::kGapSm;
constexpr int32_t kIconPx = 20;
// Above a group's heading, besides the list's row gap.
constexpr int32_t kGroupGap = ui::theme::kGapSm;
}  // namespace

bool colour_panel(lv_obj_t* obj) {
    return hal::panel_is_colour(ui::display_panel(lv_obj_get_display(obj)));
}

lv_color_t text_colour(lv_obj_t* obj) {
    const hal::PanelKind panel = ui::display_panel(lv_obj_get_display(obj));
    return hal::panel_is_colour(panel) ? lv_color_hex(ui::theme::kText) : ui::wisp_colour(panel);
}

lv_color_t dim_colour(lv_obj_t* obj) {
    const hal::PanelKind panel = ui::display_panel(lv_obj_get_display(obj));
    return hal::panel_is_colour(panel) ? lv_color_hex(ui::theme::kTextDim) : ui::wisp_colour(panel);
}

lv_obj_t* make_root(lv_obj_t* parent, const hal::ScreenShape& shape) {
    lv_obj_t* root = lv_obj_create(parent);
    lv_obj_remove_style_all(root);
    lv_obj_set_pos(root, 0, 0);
    lv_obj_set_size(root, shape.width, shape.height);
    // Presses on nothing in particular fall through to the screen.
    lv_obj_remove_flag(root, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(root, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(root, LV_OBJ_FLAG_EVENT_BUBBLE);
    return root;
}

PageFrame make_page(const hal::ScreenShape& shape, ui::StrId title) {
    PageFrame f;
    f.screen = lv_obj_create(nullptr);
    lv_obj_t* scr = f.screen;
    lv_obj_remove_style_all(scr);
    const hal::PanelKind panel = ui::display_panel(lv_obj_get_display(scr));
    lv_obj_set_style_bg_color(scr, ui::wisp_background(panel), 0);
    lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
    // lv_obj_remove_style_all() drops the theme's font too; the default
    // one has the CJK glyphs.
    lv_obj_set_style_text_font(scr, ui::default_font(), 0);
    lv_obj_set_style_text_color(scr, text_colour(scr), 0);
    lv_obj_remove_flag(scr, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(scr, LV_OBJ_FLAG_PRESS_LOCK);

    // The list covers the screen and the title is its first item, so the
    // title scrolls away with the rows instead of keeping a band at the top
    // (on a round screen the capsule's room and the circle's top above it
    // took almost a third of the height). The top padding puts the title,
    // scrolled to the top, below the status capsule; the side and bottom
    // padding keep the content inside the safe area's columns and end it
    // at the safe area's bottom row.
    const hal::Area safe = hal::safe_rect(shape);
    const hal::Area capsule = ui::SystemLayer::status_area(shape);
    const int32_t title_y = std::max<int32_t>(capsule.y2 + 1 + ui::theme::kGapSm, safe.y1);
    f.root = make_root(scr, shape);
    f.list = lv_obj_create(f.root);
    lv_obj_t* list = f.list;
    lv_obj_remove_style_all(list);
    lv_obj_set_pos(list, 0, 0);
    lv_obj_set_size(list, shape.width, shape.height);
    lv_obj_set_style_pad_left(list, safe.x1, 0);
    lv_obj_set_style_pad_right(list, shape.width - 1 - safe.x2, 0);
    lv_obj_set_style_pad_top(list, title_y, 0);
    lv_obj_set_style_pad_bottom(list, shape.height - 1 - safe.y2, 0);
    lv_obj_set_style_pad_row(list, kRowGap, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    // No overscroll: at an end the list stays put while an up drag at the
    // history's bottom end takes the page home.
    lv_obj_remove_flag(list, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_add_flag(list, LV_OBJ_FLAG_EVENT_BUBBLE);

    f.title = lv_label_create(list);
    lv_obj_set_width(f.title, LV_PCT(100));
    lv_obj_set_style_text_font(f.title, ui::font(kTitleFontPx), 0);
    lv_obj_set_style_text_align(f.title, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(f.title, LV_LABEL_LONG_DOT);
    lv_label_set_text(f.title, ui::tr(title));
    lv_obj_add_flag(f.title, LV_OBJ_FLAG_EVENT_BUBBLE);
    return f;
}

void clear_rows(lv_obj_t* list) {
    while (lv_obj_get_child_count(list) > static_cast<uint32_t>(kFirstRow))
        lv_obj_delete(lv_obj_get_child(list, -1));
}

void bubble_tree(lv_obj_t* obj) {
    lv_obj_add_flag(obj, LV_OBJ_FLAG_EVENT_BUBBLE);
    const uint32_t n = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < n; ++i) bubble_tree(lv_obj_get_child(obj, static_cast<int32_t>(i)));
}

lv_obj_t* make_box(lv_obj_t* parent, int32_t width) {
    lv_obj_t* box = lv_obj_create(parent);
    lv_obj_remove_style_all(box);
    lv_obj_set_size(box, width, LV_SIZE_CONTENT);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_CLICKABLE);
    return box;
}

lv_obj_t* make_card(lv_obj_t* parent) {
    lv_obj_t* card = make_box(parent);
    if (colour_panel(parent)) {
        lv_obj_set_style_bg_color(card, lv_color_hex(ui::theme::kSurface), 0);
        lv_obj_set_style_bg_opa(card, LV_OPA_COVER, 0);
    } else {
        lv_obj_set_style_border_color(card, text_colour(parent), 0);
        lv_obj_set_style_border_width(card, ui::theme::kBorderWidth, 0);
    }
    lv_obj_set_style_radius(card, ui::theme::kRadius, 0);
    lv_obj_set_style_pad_all(card, ui::theme::kGapMd, 0);
    lv_obj_set_style_pad_row(card, ui::theme::kGapSm, 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    return card;
}

lv_obj_t* make_text(lv_obj_t* parent, const char* text, bool dim) {
    lv_obj_t* l = lv_label_create(parent);
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_long_mode(l, LV_LABEL_LONG_WRAP);
    lv_label_set_text(l, text);
    if (dim) lv_obj_set_style_text_color(l, dim_colour(parent), 0);
    return l;
}

lv_color_t accent_colour(lv_obj_t* obj) {
    return colour_panel(obj) ? lv_color_hex(ui::theme::kAccent) : text_colour(obj);
}

int32_t line_height(lv_obj_t* obj) {
    return lv_font_get_line_height(lv_obj_get_style_text_font(obj, LV_PART_MAIN));
}

lv_obj_t* make_group_heading(lv_obj_t* parent, const char* text) {
    lv_obj_t* l = make_text(parent, text, true);
    // In line with the rows' text, a little apart from the card above.
    lv_obj_set_style_pad_left(l, kRowPadX, 0);
    lv_obj_set_style_pad_top(l, kGroupGap, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_DOT);
    lv_obj_set_height(l, line_height(parent) + kGroupGap);
    return l;
}

lv_obj_t* make_row_card(lv_obj_t* parent) {
    lv_obj_t* card = make_card(parent);
    lv_obj_set_style_pad_all(card, 0, 0);
    lv_obj_set_style_pad_row(card, 0, 0);
    return card;
}

lv_obj_t* make_row_box(lv_obj_t* card) {
    lv_obj_t* box = make_box(card);
    lv_obj_set_style_pad_hor(box, kRowPadX, 0);
    lv_obj_set_style_pad_ver(box, kRowPadY, 0);
    return box;
}

void add_divider(lv_obj_t* obj) {
    if (colour_panel(obj)) {
        lv_obj_set_style_border_side(obj, LV_BORDER_SIDE_TOP, 0);
        lv_obj_set_style_border_width(obj, 1, 0);
        lv_obj_set_style_border_color(obj, lv_color_hex(ui::theme::kBorder), 0);
    } else {
        lv_obj_set_style_margin_top(obj, ui::theme::kGapXs, 0);
    }
}

void fit_row(lv_obj_t* row, int32_t content_h) {
    // Padding rather than only a least height: LVGL centres a flex row's
    // items in its content height, so a row stretched by min_height alone
    // would show them at its top.
    lv_obj_set_style_pad_ver(
        row, std::max<int32_t>(ui::theme::kGapXs, (kMinTouchPx - content_h) / 2), 0);
}

ListRow make_list_row(lv_obj_t* card, const char* icon, const char* name, bool clickable) {
    ListRow r;
    const bool first = lv_obj_get_child_count(card) == 0;
    r.row = make_box(card);
    lv_obj_t* row = r.row;
    lv_obj_set_style_min_height(row, kMinTouchPx, 0);
    lv_obj_set_style_pad_hor(row, kRowPadX, 0);
    fit_row(row, line_height(card));
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, ui::theme::kGapSm, 0);
    if (!first) add_divider(row);
    if (clickable) {
        lv_obj_add_flag(row, LV_OBJ_FLAG_CLICKABLE);
        // Two-tone panels have no grey to press with.
        if (colour_panel(card)) {
            lv_obj_set_style_radius(row, ui::theme::kRadius, 0);
            lv_obj_set_style_bg_color(row, lv_color_hex(ui::theme::kSurface3), LV_STATE_PRESSED);
            lv_obj_set_style_bg_opa(row, LV_OPA_COVER, LV_STATE_PRESSED);
        }
    }
    if (icon != nullptr) {
        lv_obj_t* i = lv_label_create(row);
        lv_label_set_text(i, icon);
        lv_obj_set_width(i, kIconPx);
        lv_obj_set_style_text_align(i, LV_TEXT_ALIGN_CENTER, 0);
        lv_obj_set_style_text_color(i, dim_colour(card), 0);
    }
    r.name = lv_label_create(row);
    lv_label_set_text(r.name, name);
    return r;
}

lv_obj_t* add_row_value(lv_obj_t* row, const char* text, bool chevron) {
    lv_obj_t* v = lv_label_create(row);
    // Whatever room the name leaves, one line, the text at its right end.
    lv_obj_set_flex_grow(v, 1);
    lv_obj_set_height(v, line_height(row));
    lv_label_set_long_mode(v, LV_LABEL_LONG_DOT);
    lv_obj_set_style_text_align(v, LV_TEXT_ALIGN_RIGHT, 0);
    lv_obj_set_style_text_color(v, dim_colour(row), 0);
    lv_label_set_text(v, text);
    if (chevron) {
        lv_obj_t* c = lv_label_create(row);
        lv_label_set_text(c, LV_SYMBOL_RIGHT);
        lv_obj_set_style_text_color(c, dim_colour(row), 0);
    }
    return v;
}

}  // namespace wisp::shell::detail
