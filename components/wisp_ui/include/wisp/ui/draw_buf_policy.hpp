#pragma once

// How UiRuntime's two LVGL draw buffers are sized and placed, from the
// Display caps (porting guide, "能力字段" table; foundation spec 6.4):
//
//   draw_buf_lines  int, lines per buffer (default 24, clamped to the
//                   display's height)
//   draw_buf_mem    "dma" (default): DMA-capable internal RAM, Mem::Dma;
//                   "large": DMA-capable PSRAM, Mem::DmaLarge, for a chip
//                   whose DMA reads PSRAM and a display driver that sends
//                   from it
//   render_mode     "partial" (default); "full" and "direct" are reserved
//                   names, not implemented yet
//
// A display declaring none of them gets two 24-line partial buffers in
// Mem::Dma. An unusable value degrades to the default with a warning
// instead of failing start-up (AGENTS.md section 3, device model).

#include <cstdint>
#include <string>
#include <vector>

#include "wisp/base/value.hpp"
#include "wisp/os/os.hpp"

namespace wisp::ui {

inline constexpr uint32_t kDefaultDrawBufLines = 24;

struct DrawBufPolicy {
    uint32_t lines = kDefaultDrawBufLines;
    os::Mem mem = os::Mem::Dma;
    // One line per rejected or adjusted declaration, for the caller to log.
    std::vector<std::string> warnings;
};

DrawBufPolicy draw_buf_policy(const Properties& caps);

}  // namespace wisp::ui
