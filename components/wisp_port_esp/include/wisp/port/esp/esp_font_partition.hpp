#pragma once

// The firmware's side of app::Platform::font_image (vector-font spec 2.2,
// 2.3): the "fonts" data partition, mapped into the data address space so
// wisp_ui reads the font straight from flash. Found by name only; its
// offset and size live in the board's partitions.csv. The image is not
// parsed here: ui::open_font_image() checks it when the UI starts.

#include <cstdint>
#include <span>

namespace wisp::port::esp {

// Maps the whole "fonts" data partition read-only and keeps it mapped for
// good. Maps flash, so only on a task whose stack is in internal RAM
// (app_main). Empty, with a warning, when there is no such partition or
// the mapping fails.
std::span<const uint8_t> map_font_partition();

}  // namespace wisp::port::esp
