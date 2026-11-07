#include "page_frame.hpp"

#include <algorithm>

#include "wisp/ui/fonts.hpp"
#include "wisp/ui/system_layer.hpp"
#include "wisp/ui/theme_tokens.hpp"
#include "wisp/ui/ui_runtime.hpp"
#include "wisp/ui/wisp_view.hpp"

namespace wisp::shell::detail {

namespace {
// Between the title and the list.
constexpr int32_t kTitleGap = ui::theme::kGapSm;
// Between the list's rows.
constexpr int32_t kRowGap = ui::theme::kGapSm;
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

    // Centred at the top of the safe area, below the status capsule.
    const hal::Area safe = hal::safe_rect(shape);
    const hal::Area capsule = ui::SystemLayer::status_area(shape);
    f.title = lv_label_create(scr);
    lv_obj_set_style_text_font(f.title, ui::font(kTitleFontPx), 0);
    lv_obj_set_style_text_align(f.title, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_long_mode(f.title, LV_LABEL_LONG_DOT);
    lv_label_set_text(f.title, ui::tr(title));
    const int32_t title_y = capsule.y2 + 1 + ui::theme::kGapSm;
    lv_obj_set_pos(f.title, safe.x1, title_y);
    lv_obj_set_width(f.title, safe.x2 - safe.x1 + 1);
    lv_obj_update_layout(f.title);

    const int32_t list_y = title_y + lv_obj_get_height(f.title) + kTitleGap;
    f.list = lv_obj_create(scr);
    lv_obj_t* list = f.list;
    lv_obj_remove_style_all(list);
    lv_obj_set_pos(list, 0, list_y);
    lv_obj_set_size(list, shape.width, std::max<int32_t>(shape.height - list_y, 1));
    lv_obj_set_style_pad_left(list, safe.x1, 0);
    lv_obj_set_style_pad_right(list, shape.width - 1 - safe.x2, 0);
    lv_obj_set_style_pad_top(list, 0, 0);
    lv_obj_set_style_pad_bottom(list, shape.height - 1 - safe.y2, 0);
    lv_obj_set_style_pad_row(list, kRowGap, 0);
    lv_obj_set_flex_flow(list, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(list, LV_DIR_VER);
    lv_obj_set_scrollbar_mode(list, LV_SCROLLBAR_MODE_OFF);
    // No overscroll: at an end the list stays put, so an up swipe at the
    // history's bottom end reads as a swipe and not as a scroll.
    lv_obj_remove_flag(list, LV_OBJ_FLAG_SCROLL_ELASTIC);
    lv_obj_add_flag(list, LV_OBJ_FLAG_EVENT_BUBBLE);
    return f;
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

}  // namespace wisp::shell::detail
