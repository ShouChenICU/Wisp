#pragma once

// The scene theme (scene-runtime-core spec 4.4): the colour tokens and
// font sizes scene properties name. Sub-project 7's theme system extends
// this set of tokens.

#include <string_view>

#include "lvgl.h"

namespace wisp::scene {

// A token (accent #4F9DFF, text #FFFFFF, muted #8E8E93, surface #1C1C1E,
// bg #000000, danger #FF453A, ok #32D74B, warn #FFD60A) or "#RRGGBB" in
// either case. The schema only lets those through; anything else is the
// `text` colour, so a bad value can never leave a widget unreadable.
lv_color_t theme_color(std::string_view token_or_hex);

// xs, sm and md are ui::font(16), lg 24, xl 32, xxl 48; anything else
// md. UI thread only, like ui::font().
const lv_font_t* theme_font(std::string_view size);

}  // namespace wisp::scene
