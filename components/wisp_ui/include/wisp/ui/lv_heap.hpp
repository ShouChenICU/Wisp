#pragma once

// LVGL's heap. lv_conf.h selects LV_STDLIB_CUSTOM, and
// components/wisp_ui/src/lv_mem_wisp.cpp backs lv_malloc()/lv_realloc()/
// lv_free() with os::alloc(Mem::Large): without it every LVGL object, style
// and string (all well under 16 KB) would land in internal RAM
// (AGENTS.md section 8), and scene widgets would exhaust it. The DMA flush
// buffers are not LVGL heap; UiRuntime allocates them as Mem::Dma itself.
//
// No LVGL or platform headers here (see wisp/os/os.hpp for the rule).

#include <cstddef>

namespace wisp::ui {

// Bytes LVGL currently holds from its heap: the sum of the sizes it asked
// for and has not yet freed, not counting per-block bookkeeping. For
// diagnostics and tests; safe to call from any thread.
size_t lv_heap_bytes();

}  // namespace wisp::ui
