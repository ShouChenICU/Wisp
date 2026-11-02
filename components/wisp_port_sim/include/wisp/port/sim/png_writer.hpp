#pragma once

// Minimal, self-contained PNG encoder for the simulator's --screenshot
// flag (spec 8.1). Writes an 8-bit RGB PNG whose IDAT stream is a zlib-
// wrapped, *uncompressed* ("stored" block type) deflate stream -- no
// third-party image/zlib dependency: --screenshot is a host-only
// developer convenience, not worth taking on an external image/zlib
// dependency for (AGENTS.md section 6: don't fork or hard-depend on
// upstream projects when a narrowly-scoped reimplementation suffices).
// Host-only (built into wisp_port_sim_dev); never used by any portable
// layer.

#include <cstdint>
#include <string>

#include "wisp/base/result.hpp"

namespace wisp::port::sim {

// Writes `w`*`h` pixels (row-major, 3 bytes/pixel: R, G, B, no padding)
// from `rgb` to a new PNG file at `path`, overwriting any existing file.
// The file is PNG signature + IHDR (8-bit, color type 2/RGB) + one IDAT
// chunk + IEND, each chunk carrying a CRC-32. IDAT's zlib stream encodes
// each scanline as a filter-type-0 byte followed by `w`*3 raw bytes, cut
// into deflate "stored" blocks of at most 65535 bytes each (deflate's
// own per-block limit, independent of PNG chunk size), with a trailing
// Adler-32 checksum. Fails with ErrorCode::IoError if `path` can't be
// opened for writing.
Result<void> write_png_rgb888(const std::string& path, const uint8_t* rgb, uint32_t w, uint32_t h);

}  // namespace wisp::port::sim
