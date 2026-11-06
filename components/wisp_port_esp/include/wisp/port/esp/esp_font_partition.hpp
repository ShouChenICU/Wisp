#pragma once

// The firmware's side of app::Platform::font_image (vector-font spec 2.2,
// 2.3) and glyph_image (glyph-partition spec 4.2): the "fonts" and
// "glyphs" data partitions, mapped into the data address space so
// wisp_ui reads the fonts straight from flash. Found by name only; their
// offsets and sizes live in the board's partitions.csv. The images are
// not parsed here: wisp_ui checks them when the UI starts.

#include <cstdint>
#include <span>

namespace wisp::port::esp {

// Maps the whole "fonts" data partition read-only and keeps it mapped for
// good. Maps flash, so only on a task whose stack is in internal RAM
// (app_main). Empty, with a warning, when there is no such partition or
// the mapping fails.
std::span<const uint8_t> map_font_partition();

// The same for the "glyphs" data partition, the 16 px body font
// (glyph-partition spec 4.2), which ui::open_glyph_image() checks.
std::span<const uint8_t> map_glyph_partition();

}  // namespace wisp::port::esp
