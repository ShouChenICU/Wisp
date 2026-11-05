#include "wisp/ui/theme.hpp"

#include "theme_internal.hpp"

namespace wisp::ui {

namespace {

using namespace theme;

lv_color_t col(uint32_t rgb) { return lv_color_hex(rgb); }

// A part in a state, as one style selector.
constexpr lv_style_selector_t sel(lv_part_t part, lv_state_t state) {
    return static_cast<lv_style_selector_t>(part) | static_cast<lv_style_selector_t>(state);
}

// The styles the Wisp theme lays over LVGL's default theme. Each is added
// after the default theme's own, so where both set a property this one
// wins; whatever these leave unset (paddings, sizes, transitions) stays
// the default theme's.
struct Styles {
    lv_style_t scr;
    lv_style_t card;
    lv_style_t scrollbar;
    // Buttons, by role and state.
    lv_style_t btn;
    lv_style_t btn_pressed;
    lv_style_t btn_checked;
    lv_style_t secondary;
    lv_style_t secondary_pressed;
    lv_style_t danger;
    lv_style_t danger_pressed;
    // Switches, sliders, bars and arcs.
    lv_style_t track;
    lv_style_t indicator;
    lv_style_t knob;
    lv_style_t arc_track;
    lv_style_t arc_indicator;
    // Text inputs, dropdowns and rollers.
    lv_style_t field;
    lv_style_t field_focused;
    lv_style_t placeholder;
    lv_style_t selected;
    lv_style_t cb_marker;
    lv_style_t cb_checked;
    // Message boxes.
    lv_style_t msgbox;
    lv_style_t msgbox_header;
    lv_style_t backdrop;
    // The keyboard and other button matrices.
    lv_style_t keys_bg;
    lv_style_t key;
    lv_style_t key_control;
    lv_style_t key_pressed;
    // Lists, tables and charts.
    lv_style_t row_divider;
    lv_style_t list_text;
    lv_style_t chart;
};

// The theme's state. lv_init() starts each UiRuntime run with no theme,
// and lv_deinit() frees LVGL's default theme, so this is built again on
// every start and released on every stop.
Styles g_styles;
lv_theme_t* g_theme = nullptr;

lv_style_t* all_styles(size_t* count) {
    *count = sizeof(Styles) / sizeof(lv_style_t);
    return reinterpret_cast<lv_style_t*>(&g_styles);
}
static_assert(sizeof(Styles) % sizeof(lv_style_t) == 0);

void init_styles(Styles& s) {
    size_t n = 0;
    lv_style_t* each = all_styles(&n);
    for (size_t i = 0; i < n; ++i) lv_style_init(&each[i]);

    lv_style_set_bg_color(&s.scr, col(kBg));
    lv_style_set_text_color(&s.scr, col(kText));

    lv_style_set_bg_color(&s.card, col(kSurface));
    lv_style_set_border_color(&s.card, col(kBorder));
    lv_style_set_border_width(&s.card, kBorderWidth);
    lv_style_set_radius(&s.card, kRadius);
    lv_style_set_text_color(&s.card, col(kText));
    lv_style_set_line_color(&s.card, col(kBorder));

    lv_style_set_bg_color(&s.scrollbar, col(kTextDim));

    // The default theme gives a light button a drop shadow and darkens a
    // pressed one with a black recolor; a press here is its fill colour
    // alone.
    lv_style_set_bg_color(&s.btn, col(kAccent));
    lv_style_set_text_color(&s.btn, col(kAccentInk));
    lv_style_set_radius(&s.btn, kRadiusSm);
    lv_style_set_shadow_width(&s.btn, 0);
    lv_style_set_bg_color(&s.btn_pressed, col(pressed(kAccent, kAccentInk)));
    lv_style_set_recolor_opa(&s.btn_pressed, LV_OPA_TRANSP);
    lv_style_set_bg_color(&s.btn_checked, col(kAccentStrong));
    lv_style_set_text_color(&s.btn_checked, col(kAccentInk));

    lv_style_set_bg_color(&s.secondary, col(kSurface3));
    lv_style_set_text_color(&s.secondary, col(kText));
    lv_style_set_bg_color(&s.secondary_pressed, col(pressed(kSurface3, kText)));
    lv_style_set_text_color(&s.secondary_pressed, col(kText));
    lv_style_set_recolor_opa(&s.secondary_pressed, LV_OPA_TRANSP);

    lv_style_set_bg_color(&s.danger, col(kErr));
    lv_style_set_text_color(&s.danger, col(kAccentInk));
    lv_style_set_bg_color(&s.danger_pressed, col(pressed(kErr, kAccentInk)));
    lv_style_set_text_color(&s.danger_pressed, col(kAccentInk));
    lv_style_set_recolor_opa(&s.danger_pressed, LV_OPA_TRANSP);

    lv_style_set_bg_color(&s.track, col(kSurface3));
    lv_style_set_bg_opa(&s.track, LV_OPA_COVER);
    lv_style_set_bg_color(&s.indicator, col(kAccent));
    lv_style_set_bg_opa(&s.indicator, LV_OPA_COVER);
    lv_style_set_bg_color(&s.knob, col(kText));
    lv_style_set_bg_opa(&s.knob, LV_OPA_COVER);
    lv_style_set_arc_color(&s.arc_track, col(kSurface3));
    lv_style_set_arc_color(&s.arc_indicator, col(kAccent));

    lv_style_set_bg_color(&s.field, col(kSurface2));
    lv_style_set_bg_opa(&s.field, LV_OPA_COVER);
    lv_style_set_border_color(&s.field, col(kBorderStrong));
    lv_style_set_border_width(&s.field, kBorderWidth);
    lv_style_set_radius(&s.field, kRadiusSm);
    lv_style_set_text_color(&s.field, col(kText));
    lv_style_set_border_color(&s.field_focused, col(kAccent));
    lv_style_set_text_color(&s.placeholder, col(kTextFaint));
    lv_style_set_bg_color(&s.selected, col(kAccent));
    lv_style_set_bg_opa(&s.selected, LV_OPA_COVER);
    lv_style_set_text_color(&s.selected, col(kAccentInk));

    lv_style_set_bg_color(&s.cb_marker, col(kSurface2));
    lv_style_set_border_color(&s.cb_marker, col(kBorderStrong));
    lv_style_set_radius(&s.cb_marker, kRadiusSm / 2);
    lv_style_set_bg_color(&s.cb_checked, col(kAccent));
    lv_style_set_border_color(&s.cb_checked, col(kAccent));
    lv_style_set_text_color(&s.cb_checked, col(kAccentInk));

    lv_style_set_bg_color(&s.msgbox, col(kSurface));
    lv_style_set_border_color(&s.msgbox, col(kBorderStrong));
    lv_style_set_border_width(&s.msgbox, kBorderWidth);
    lv_style_set_radius(&s.msgbox, kRadiusLg);
    lv_style_set_text_color(&s.msgbox, col(kText));
    lv_style_set_bg_opa(&s.msgbox_header, LV_OPA_TRANSP);
    lv_style_set_bg_color(&s.backdrop, lv_color_black());
    lv_style_set_bg_opa(&s.backdrop, LV_OPA_60);

    lv_style_set_bg_color(&s.keys_bg, col(kBgRaised));
    lv_style_set_border_width(&s.keys_bg, 0);
    lv_style_set_bg_color(&s.key, col(kSurface3));
    lv_style_set_bg_opa(&s.key, LV_OPA_COVER);
    lv_style_set_text_color(&s.key, col(kText));
    lv_style_set_radius(&s.key, kRadiusSm);
    lv_style_set_shadow_width(&s.key, 0);
    lv_style_set_bg_color(&s.key_control, col(kSurface2));
    lv_style_set_text_color(&s.key_control, col(kText));
    lv_style_set_bg_color(&s.key_pressed, col(pressed(kSurface3, kText)));
    lv_style_set_recolor_opa(&s.key_pressed, LV_OPA_TRANSP);

    lv_style_set_bg_color(&s.row_divider, col(kSurface));
    lv_style_set_border_color(&s.row_divider, col(kBorder));
    lv_style_set_text_color(&s.row_divider, col(kText));
    lv_style_set_bg_color(&s.list_text, col(kSurface2));
    lv_style_set_text_color(&s.list_text, col(kTextDim));
    lv_style_set_line_color(&s.chart, col(kBorder));
}

void add_scrollbar(lv_obj_t* obj) { lv_obj_add_style(obj, &g_styles.scrollbar, LV_PART_SCROLLBAR); }

void style_as_button(lv_obj_t* obj) {
    lv_obj_add_style(obj, &g_styles.btn, 0);
    lv_obj_add_style(obj, &g_styles.btn_checked, LV_STATE_CHECKED);
    lv_obj_add_style(obj, &g_styles.btn_pressed, LV_STATE_PRESSED);
}

// Mirrors the class checks of LVGL's default theme (lv_theme_default.c)
// for the widgets the device uses; a class not handled here keeps the
// default theme's dark look.
void apply_cb(lv_theme_t*, lv_obj_t* obj) {
    Styles& s = g_styles;
    lv_obj_t* parent = lv_obj_get_parent(obj);
    if (parent == nullptr) {
        lv_obj_add_style(obj, &s.scr, 0);
        add_scrollbar(obj);
        return;
    }
    if (lv_obj_check_type(obj, &lv_obj_class)) {
        lv_obj_add_style(obj, &s.card, 0);
        add_scrollbar(obj);
    } else if (lv_obj_check_type(obj, &lv_button_class) ||
               lv_obj_check_type(obj, &lv_msgbox_footer_button_class) ||
               lv_obj_check_type(obj, &lv_msgbox_header_button_class)) {
        style_as_button(obj);
    } else if (lv_obj_check_type(obj, &lv_keyboard_class)) {
        lv_obj_add_style(obj, &s.keys_bg, 0);
        lv_obj_add_style(obj, &s.key, LV_PART_ITEMS);
        lv_obj_add_style(obj, &s.key_control, sel(LV_PART_ITEMS, LV_STATE_CHECKED));
        lv_obj_add_style(obj, &s.key_pressed, sel(LV_PART_ITEMS, LV_STATE_PRESSED));
    } else if (lv_obj_check_type(obj, &lv_buttonmatrix_class)) {
        lv_obj_add_style(obj, &s.card, 0);
        lv_obj_add_style(obj, &s.key, LV_PART_ITEMS);
        lv_obj_add_style(obj, &s.key_pressed, sel(LV_PART_ITEMS, LV_STATE_PRESSED));
        lv_obj_add_style(obj, &s.selected, sel(LV_PART_ITEMS, LV_STATE_CHECKED));
    } else if (lv_obj_check_type(obj, &lv_bar_class) || lv_obj_check_type(obj, &lv_slider_class)) {
        lv_obj_add_style(obj, &s.track, 0);
        lv_obj_add_style(obj, &s.indicator, LV_PART_INDICATOR);
        lv_obj_add_style(obj, &s.knob, LV_PART_KNOB);
    } else if (lv_obj_check_type(obj, &lv_switch_class)) {
        lv_obj_add_style(obj, &s.track, 0);
        lv_obj_add_style(obj, &s.indicator, sel(LV_PART_INDICATOR, LV_STATE_CHECKED));
        lv_obj_add_style(obj, &s.knob, LV_PART_KNOB);
    } else if (lv_obj_check_type(obj, &lv_arc_class) || lv_obj_check_type(obj, &lv_spinner_class)) {
        lv_obj_add_style(obj, &s.arc_track, 0);
        lv_obj_add_style(obj, &s.arc_indicator, LV_PART_INDICATOR);
        lv_obj_add_style(obj, &s.knob, LV_PART_KNOB);
    } else if (lv_obj_check_type(obj, &lv_checkbox_class)) {
        lv_obj_add_style(obj, &s.cb_marker, LV_PART_INDICATOR);
        lv_obj_add_style(obj, &s.cb_checked, sel(LV_PART_INDICATOR, LV_STATE_CHECKED));
    } else if (lv_obj_check_type(obj, &lv_textarea_class)) {
        lv_obj_add_style(obj, &s.field, 0);
        lv_obj_add_style(obj, &s.field_focused, LV_STATE_FOCUSED);
        lv_obj_add_style(obj, &s.placeholder, LV_PART_TEXTAREA_PLACEHOLDER);
        lv_obj_add_style(obj, &s.indicator, sel(LV_PART_CURSOR, LV_STATE_FOCUSED));
        add_scrollbar(obj);
    } else if (lv_obj_check_type(obj, &lv_dropdown_class)) {
        lv_obj_add_style(obj, &s.field, 0);
        lv_obj_add_style(obj, &s.field_focused, LV_STATE_PRESSED);
    } else if (lv_obj_check_type(obj, &lv_dropdownlist_class)) {
        lv_obj_add_style(obj, &s.field, 0);
        lv_obj_add_style(obj, &s.list_text, LV_PART_SELECTED);
        lv_obj_add_style(obj, &s.selected, sel(LV_PART_SELECTED, LV_STATE_CHECKED));
        add_scrollbar(obj);
    } else if (lv_obj_check_type(obj, &lv_roller_class)) {
        lv_obj_add_style(obj, &s.field, 0);
        lv_obj_add_style(obj, &s.selected, LV_PART_SELECTED);
    } else if (lv_obj_check_type(obj, &lv_msgbox_class)) {
        lv_obj_add_style(obj, &s.msgbox, 0);
    } else if (lv_obj_check_type(obj, &lv_msgbox_backdrop_class)) {
        lv_obj_add_style(obj, &s.backdrop, 0);
    } else if (lv_obj_check_type(obj, &lv_msgbox_header_class)) {
        lv_obj_add_style(obj, &s.msgbox_header, 0);
    } else if (lv_obj_check_type(obj, &lv_msgbox_content_class)) {
        add_scrollbar(obj);
    } else if (lv_obj_check_type(obj, &lv_list_class)) {
        lv_obj_add_style(obj, &s.card, 0);
        add_scrollbar(obj);
    } else if (lv_obj_check_type(obj, &lv_list_button_class)) {
        lv_obj_add_style(obj, &s.row_divider, 0);
        lv_obj_add_style(obj, &s.key_pressed, LV_STATE_PRESSED);
    } else if (lv_obj_check_type(obj, &lv_list_text_class)) {
        lv_obj_add_style(obj, &s.list_text, 0);
    } else if (lv_obj_check_type(obj, &lv_table_class)) {
        lv_obj_add_style(obj, &s.card, 0);
        lv_obj_add_style(obj, &s.row_divider, LV_PART_ITEMS);
        add_scrollbar(obj);
    } else if (lv_obj_check_type(obj, &lv_chart_class)) {
        lv_obj_add_style(obj, &s.card, 0);
        lv_obj_add_style(obj, &s.chart, 0);
        add_scrollbar(obj);
    }
}

}  // namespace

void set_button_role(lv_obj_t* btn, ButtonRole role) {
    Styles& s = g_styles;
    lv_obj_remove_style(btn, &s.secondary, 0);
    lv_obj_remove_style(btn, &s.secondary_pressed, LV_STATE_PRESSED);
    lv_obj_remove_style(btn, &s.danger, 0);
    lv_obj_remove_style(btn, &s.danger_pressed, LV_STATE_PRESSED);
    switch (role) {
        case ButtonRole::Primary:
            break;
        case ButtonRole::Secondary:
            lv_obj_add_style(btn, &s.secondary, 0);
            lv_obj_add_style(btn, &s.secondary_pressed, LV_STATE_PRESSED);
            break;
        case ButtonRole::Danger:
            lv_obj_add_style(btn, &s.danger, 0);
            lv_obj_add_style(btn, &s.danger_pressed, LV_STATE_PRESSED);
            break;
    }
}

namespace detail {

lv_theme_t* install_theme(lv_display_t* disp, const lv_font_t* font) {
    // The secondary colour is what the default theme gives checked
    // buttons and the "edited" outline: the brighter accent, not a
    // warning colour.
    lv_theme_t* base = lv_theme_default_init(disp, col(kAccent), col(kAccentStrong), true, font);
    if (g_theme == nullptr) {
        init_styles(g_styles);
        g_theme = lv_theme_create();
        if (g_theme == nullptr) return base;
        lv_theme_copy(g_theme, base);
        lv_theme_set_parent(g_theme, base);
        lv_theme_set_apply_cb(g_theme, &apply_cb);
    }
    return g_theme;
}

void release_theme() {
    if (g_theme == nullptr) return;
    size_t n = 0;
    lv_style_t* each = all_styles(&n);
    for (size_t i = 0; i < n; ++i) lv_style_reset(&each[i]);
    lv_theme_delete(g_theme);
    g_theme = nullptr;
}

}  // namespace detail

}  // namespace wisp::ui
