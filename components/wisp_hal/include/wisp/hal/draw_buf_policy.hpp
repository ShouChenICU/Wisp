#pragma once

// How the UI's two LVGL draw buffers (wisp::ui::UiRuntime) are sized and
// placed, from the Display caps (porting guide, "能力字段" table;
// foundation spec 6.4):
//
//   draw_buf_lines  int, lines per buffer (default 24, clamped to the
//                   display's height)
//   draw_buf_mem    "dma" (default): DMA-capable internal RAM (os::Mem::Dma);
//                   "large": DMA-capable PSRAM (os::Mem::DmaLarge), for a
//                   chip whose DMA reads PSRAM and a display driver that
//                   sends from it
//   render_mode     "partial" (default); "full" and "direct" are reserved
//                   names, not implemented yet
//
// A display declaring none of them gets two 24-line partial buffers in
// os::Mem::Dma. An unusable value degrades to the default with a warning
// instead of failing start-up (AGENTS.md section 3, device model).

#include <cstdint>
#include <vector>

#include "wisp/base/value.hpp"

namespace wisp::hal {

inline constexpr uint32_t kDefaultDrawBufLines = 24;

enum class DrawBufMem { Dma, Large };

struct DrawBufPolicy {
    uint32_t lines = kDefaultDrawBufLines;
    DrawBufMem mem = DrawBufMem::Dma;
    // One static message per rejected or adjusted declaration, for the
    // caller to log.
    std::vector<const char*> warnings;
};

DrawBufPolicy draw_buf_policy(const Properties& caps);

}  // namespace wisp::hal
