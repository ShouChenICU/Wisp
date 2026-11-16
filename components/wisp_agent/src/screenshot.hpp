#pragma once

// ui.screenshot's image (spec 3.6): from the UI's RGB888 snapshot to a JPEG
// a vision model is shown. Internal to wisp_agent; tools_ui.cpp registers the
// tool.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <vector>

#include "wisp/agent/limits.hpp"
#include "wisp/agent/tool_registry.hpp"
#include "wisp/base/result.hpp"
#include "wisp/hal/display_geometry.hpp"

namespace wisp::agent {

struct ScreenshotOptions {
    uint32_t width = kScreenshotWidth;  // never scaled up (plan decision 10)
    int quality = kScreenshotQuality;
    size_t max_bytes = kImageMaxBytes;  // at most kImageMaxBytes
};

// The crop: the bounding box of the pixels hal::visible() accepts, not
// safe_rect(), which would cut away visible content on a round panel (plan
// decision 2). The full panel for every shape today; empty (x1 > x2) when no
// pixel is visible.
hal::Area screenshot_area(const hal::ScreenShape& shape);

// Blacks out, in place, every pixel the panel cannot show (each row outside
// its row_span()); crops to screenshot_area(); scales by area average into an
// os::LargeBuffer; frees `rgb`; encodes into a buffer of max_bytes, truncated
// to the JPEG. `rgb` is a snapshot of the whole panel (R, G, B per pixel, no
// row padding). Errors: InvalidArgument (rgb size != width * height * 3, or
// nothing visible), NoMemory, QuotaExceeded (the JPEG does not fit: too_large).
Result<std::shared_ptr<ToolImage>> make_screenshot(std::vector<uint8_t>&& rgb,
                                                   const hal::ScreenShape& shape,
                                                   const ScreenshotOptions& opts = {});

}  // namespace wisp::agent
