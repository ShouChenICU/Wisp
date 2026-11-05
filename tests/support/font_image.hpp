#pragma once

// The vector font image the host build packs (simulator/CMakeLists.txt's
// wisp_sim_fonts target), for tests that need the real font. The test
// target defines WISP_FONT_IMAGE as the path to fonts.bin and depends on
// wisp_sim_fonts. Read once and kept for the rest of the process.

#include <gtest/gtest.h>

#include <cstdint>
#include <fstream>
#include <iterator>
#include <span>
#include <vector>

namespace wisp::test {

// The packed image; an empty span (with a test failure) when it can't be
// read.
inline std::span<const uint8_t> test_font_image() {
    static const std::vector<uint8_t> bytes = [] {
        std::ifstream in(WISP_FONT_IMAGE, std::ios::binary);
        return std::vector<uint8_t>(std::istreambuf_iterator<char>(in),
                                    std::istreambuf_iterator<char>());
    }();
    if (bytes.empty()) {
        ADD_FAILURE() << "cannot read the font image " << WISP_FONT_IMAGE;
        return {};
    }
    return bytes;
}

}  // namespace wisp::test
