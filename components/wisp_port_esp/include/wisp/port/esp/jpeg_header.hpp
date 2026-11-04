#pragma once

// Pure, platform-free JPEG header reader for the firmware's image decoder
// (ui/esp_jpeg_decoder.cpp): finds the frame header and tells baseline
// JPEG, which esp_new_jpeg decodes, from everything it does not, so a
// progressive file is refused before any decoding starts and
// ui.image reports it as an unsupported JPEG (scene-runtime-core spec
// 4.3). Header-only and free of IDF includes so it is unit-tested on the
// host (tests/port_esp/jpeg_header_test.cpp).

#include <cstddef>
#include <cstdint>

namespace wisp::port::esp {

enum class JpegKind {
    NotJpeg,      // no SOI, or the segments end or break before a frame header
    Baseline,     // SOF0 or SOF1 (sequential, Huffman)
    Unsupported,  // progressive, lossless, arithmetic coding, or a 0 size
};

struct JpegHeader {
    JpegKind kind = JpegKind::NotJpeg;
    uint16_t width = 0;
    uint16_t height = 0;
};

inline JpegHeader read_jpeg_header(const uint8_t* d, size_t size) {
    JpegHeader out;
    if (d == nullptr || size < 4 || d[0] != 0xFF || d[1] != 0xD8) return out;
    size_t i = 2;
    while (i + 4 <= size) {
        if (d[i] != 0xFF) return out;
        const uint8_t m = d[i + 1];
        if (m == 0xFF) {  // fill byte
            ++i;
            continue;
        }
        // Markers without a segment.
        if (m == 0x01 || (m >= 0xD0 && m <= 0xD8)) {
            i += 2;
            continue;
        }
        if (m == 0xD9 || m == 0xDA) return out;  // the image ends or its scan starts
        const size_t len = static_cast<size_t>(d[i + 2]) << 8 | d[i + 3];
        if (len < 2 || len > size - i - 2) return out;
        // A frame header (SOF0..SOF15): 0xC0..0xCF except DHT (0xC4), JPG
        // (0xC8) and DAC (0xCC).
        if (m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC) {
            if (len < 7) return out;
            out.height = static_cast<uint16_t>(d[i + 5] << 8 | d[i + 6]);
            out.width = static_cast<uint16_t>(d[i + 7] << 8 | d[i + 8]);
            const bool baseline = m == 0xC0 || m == 0xC1;
            out.kind = baseline && out.width > 0 && out.height > 0 ? JpegKind::Baseline
                                                                   : JpegKind::Unsupported;
            return out;
        }
        i += 2 + len;
    }
    return out;
}

}  // namespace wisp::port::esp
