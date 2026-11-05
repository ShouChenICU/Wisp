#pragma once

// The scene theme (scene-runtime-core spec 4.4): the colour tokens and
// font sizes scene properties name. Sub-project 7's theme system extends
// this set of tokens.

#include <cstdint>
#include <string_view>

#include "lvgl.h"

namespace wisp::scene {

// A token (accent #4F9DFF, text #FFFFFF, muted #8E8E93, surface #1C1C1E,
// bg #000000, danger #FF453A, ok #32D74B, warn #FFD60A) or "#RRGGBB" in
// either case. The schema only lets those through; anything else is the
// `text` colour, so a bad value can never leave a widget unreadable.
lv_color_t theme_color(std::string_view token_or_hex);
// The same colour as 0xRRGGBB; any thread.
uint32_t theme_rgb(std::string_view token_or_hex);

// The pixel sizes a scene's `font` may give as a number (spec 5.3 of the
// vector-font design). ui::font() rounds them down to its steps.
inline constexpr int kFontPxMin = 16;
inline constexpr int kFontPxMax = 128;

// xs, sm and md are 16 pixels, lg 24, xl 32, xxl 48; anything else 16.
// Any thread.
int theme_font_px(std::string_view token);

// ui::font(theme_font_px(token)). UI thread only, like ui::font().
const lv_font_t* theme_font(std::string_view token);
// ui::font(px) for a size the schema already held to kFontPxMin..
// kFontPxMax. UI thread only.
const lv_font_t* theme_font(int px);

}  // namespace wisp::scene
