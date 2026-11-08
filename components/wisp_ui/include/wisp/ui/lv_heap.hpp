#pragma once

// LVGL's heap. lv_conf.h selects LV_STDLIB_CUSTOM, and
// components/wisp_ui/src/lv_mem_wisp.cpp backs lv_malloc()/lv_realloc()/
// lv_free() with os::alloc(Mem::Large): without it every LVGL object, style
// and string (all well under 16 KB) would land in internal RAM
// (AGENTS.md section 8), and scene widgets would exhaust it. The DMA flush
// buffers are not LVGL heap; UiRuntime allocates them as Mem::Dma itself.
//
// When PSRAM is missing or exhausted, a block falls back to Mem::Fast
// (internal RAM) so the UI keeps working. That fallback eats into the
// internal-RAM budget (wisp/app/ram_budget.hpp) that DMA, Wi-Fi and the
// httpd stacks need, so it is counted separately: lv_heap_internal_bytes().
// On a board without PSRAM every allocation first tries PSRAM and fails;
// that costs a heap-list walk per call, which is negligible.
//
// LVGL's own TLSF heap (LV_STDLIB_BUILTIN) with its pool in PSRAM, and
// keeping small or render-time blocks in internal RAM, were measured
// against this and were no faster; see
// docs/hardware/waveshare_esp32_s3_touch_amoled_2_16.md, "LVGL heap".
//
// No LVGL or platform headers here (see wisp/os/os.hpp for the rule).

#include <cstddef>

namespace wisp::ui {

// Bytes LVGL currently holds from its heap: the sum of the sizes it asked
// for and has not yet freed, not counting per-block bookkeeping. For
// diagnostics and tests; safe to call from any thread.
size_t lv_heap_bytes();

// The part of lv_heap_bytes() that fell back to internal RAM. Zero while
// PSRAM has room. Same thread rules as lv_heap_bytes().
size_t lv_heap_internal_bytes();

}  // namespace wisp::ui
