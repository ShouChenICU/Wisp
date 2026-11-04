#pragma once

// Fonts whose glyphs reach LVGL already expanded to 8 bits per pixel.
//
// LVGL's built-in fonts (lv_font_fmt_txt) keep 1, 2 or 4 bits per pixel
// and expand a glyph into a scratch buffer every time it is drawn, which
// on the device was a tenth of a scrolling frame. The wrapper expands
// each glyph once, into a cache in Mem::Large memory, and declares the
// font a static-bitmap font so LVGL's software renderer blends straight
// from the cached A8 bitmap. Pixels are identical to the base font's:
// the expansion is the same table LVGL uses.
//
// The cache holds at most kA8CacheBytes for all fonts together; glyphs
// first seen after that are drawn the usual way. UI thread only, like
// every lv_* call.

#include <cstddef>

#include "lvgl.h"

namespace wisp::ui {

inline constexpr size_t kA8CacheBytes = 512 * 1024;

// The A8 wrapper of `base`, with `base`'s fallbacks wrapped likewise.
// Fonts that are not uncompressed, unpadded 1/2/4 bpp lv_font_fmt_txt
// fonts are returned as they are (with their own fallbacks). `base` must
// outlive the result; wrappers are created once and never freed.
const lv_font_t* a8_font(const lv_font_t* base);

}  // namespace wisp::ui
