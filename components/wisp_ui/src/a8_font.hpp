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
// The cache holds at most kA8CacheBytes for all fonts together and
// never evicts: glyphs first seen after that are drawn the usual way. A
// glyph is cached the first time its metrics are asked for, which
// includes measuring text that is never drawn; the fonts are static, so
// this only costs memory, never correctness. UI thread only, like every
// lv_* call.

#include <cstddef>

#include "lvgl.h"

namespace wisp::ui {

inline constexpr size_t kA8CacheBytes = 512 * 1024;

// The A8 wrapper of `base`, with `base`'s fallbacks wrapped likewise.
// Fonts that are not uncompressed, unpadded 1/2/4 bpp lv_font_fmt_txt
// fonts are returned as they are (with their own fallbacks). `base` must
// outlive the result; wrappers are created once and never freed.
const lv_font_t* a8_font(const lv_font_t* base);

// Points the wrapper `wrapper` (a pointer a8_font() returned) at a new
// fallback, wrapped like a8_font() wraps `base`'s; nullptr: none. For a
// font whose fallback is decided after the wrapper was handed out, such
// as the body font's CJK fallback (fonts.cpp). Does nothing when
// `wrapper` is not a wrapper. Only while nothing draws with it.
void a8_set_fallback(const lv_font_t* wrapper, const lv_font_t* fallback);

// Empties the cache of every wrapper, freeing its memory, so a test sees
// the same cache whatever ran before it. Only while nothing draws with
// a wrapper (no UiRuntime running).
void reset_a8_cache_for_test();

}  // namespace wisp::ui
