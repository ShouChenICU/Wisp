#pragma once

// The UI fonts (vector-font spec 4): the 16 px bitmap body font, and from
// 20 px up the vector font from the fonts partition image, drawn by LVGL's
// Tiny TTF. Callers ask for a pixel size and get a font; which font that
// is, and what it falls back to, is decided here.
//
// Portable: LVGL only, no platform headers.

#include <array>
#include <cstddef>

#include "lvgl.h"

namespace wisp::ui {

// The body font's size, and the smallest size drawn from the vector font.
inline constexpr int kBodyFontPx = 16;
inline constexpr int kVectorMinPx = 20;

// The sizes vector fonts are created at (spec 4.1). Any request lands on
// one of these, so the number of font objects and glyph caches, and with
// them the PSRAM they take, has a fixed bound (spec 4.4).
inline constexpr std::array<int, 10> kFontSteps = {20, 24, 28, 32, 40, 48, 64, 80, 96, 128};

// The default UI font: lv_font_montserrat_16, falling back glyph by
// glyph to a CJK font for Chinese and Japanese text (glyph-partition
// spec 4.2, 4.3). While a UiRuntime runs that is the 16 px body font
// (GB2312 plus Japanese kana and level-1 kanji, 4bpp; see
// tools/gen_cjk_font.py) read from UiConfig::glyph_image; failing that,
// the same font built into host builds, a 16 px vector font, or nothing.
// Outside a runtime it is the built-in font on the host and nothing on
// the device. Bitmap fonts are drawn from a cache of glyphs expanded to
// 8 bits per pixel (src/a8_font.hpp), with the same pixels. Lazily
// constructed on first call (thread-safe, C++11 magic statics); the
// pointer never changes, and it is safe to call from any thread, before
// or after any UiRuntime exists. UiRuntime::start() installs this as the
// default theme font.
const lv_font_t* default_font();

// The size font(px) draws at: kBodyFontPx below kVectorMinPx, otherwise
// the largest step in kFontSteps not above `px` (128 for anything larger).
int font_step(int px);

// Glyph cache entries for a vector font of `step` px: about 128 KiB of
// glyphs at the measured 0.6 * step^2 + 250 bytes each, between 8 and 256
// entries (spec 4.4).
size_t font_cache_entries(int step);

// The UI font for `px` pixels. Below kVectorMinPx: default_font().
// Otherwise the vector font at font_step(px), created on first use and
// kept until UiRuntime::stop(), so a size always returns the same
// pointer while the runtime runs. Characters the vector font lacks fall
// back glyph by glyph to a bitmap font and finally to default_font()
// (spec 4.2). Without a usable font image, or if Tiny TTF cannot create
// the font, the bitmap fallback is returned instead.
//
// UI thread only, between UiRuntime::start() and stop(): the fonts live
// in LVGL's heap and caches, which only the UI thread touches.
const lv_font_t* font(int px);

// UI thread: whether this UiRuntime::start() got a font image that passed
// open_font_image(), i.e. whether font() can return vector fonts. For
// tests and diagnostics.
bool vector_fonts_ready();

}  // namespace wisp::ui
