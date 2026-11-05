#pragma once

// UiRuntime's side of the device theme (wisp/ui/theme.hpp). UI thread
// only.

#include "lvgl.h"

namespace wisp::ui::detail {

// Builds the Wisp theme for `disp` over LVGL's default theme (dark, with
// `font` as its body font) and returns it, for lv_display_set_theme().
// Builds it once per lv_init().
lv_theme_t* install_theme(lv_display_t* disp, const lv_font_t* font);

// Frees what install_theme() built. Called after lv_deinit(), once no
// object uses the theme's styles any more.
void release_theme();

}  // namespace wisp::ui::detail
