// MPEG audio Layer III stream info (media spec 3.3, 4.3): the first frame
// header, then the duration from a Xing/Info or VBRI header, else a
// constant-bit-rate estimate.

#include <algorithm>
#include <cstring>

#include "wisp/media/tags.hpp"

namespace wisp::media {
namespace {

struct FrameHeader {
    uint8_t version;  // 3 = MPEG-1, 2 = MPEG-2, 0 = MPEG-2.5 (the header's bits)
    uint32_t sample_rate;
    uint32_t kbps;
    uint8_t channels;
    uint32_t length;  // bytes, header included
    uint32_t samples_per_frame;
    uint32_t side_info;  // bytes between the header and a Xing tag
};

constexpr uint16_t kKbpsV1[16] = {0,   32,  40,  48,  56,  64,  80,  96,
                                  112, 128, 160, 192, 224, 256, 320, 0};
constexpr uint16_t kKbpsV2[16] = {0, 8, 16, 24, 32, 40, 48, 56, 64, 80, 96, 112, 128, 144, 160, 0};
constexpr uint32_t kRateV1[3] = {44100, 48000, 32000};

uint32_t be32(const uint8_t* p) {
    return (uint32_t{p[0]} << 24) | (uint32_t{p[1]} << 16) | (uint32_t{p[2]} << 8) | p[3];
}

// Only Layer III is accepted: the player has no Layer I/II decoder, and
// the stricter match makes a false sync in non-MP3 data less likely.
// Free-format bit rates (index 0) are rejected since the frame length is
// then unknown.
bool parse_header(const uint8_t* p, FrameHeader& h) {
    if (p[0] != 0xFF || (p[1] & 0xE0) != 0xE0) return false;
    const uint8_t version = (p[1] >> 3) & 3;
    const uint8_t layer = (p[1] >> 1) & 3;
    const uint8_t br = p[2] >> 4;
    const uint8_t sr = (p[2] >> 2) & 3;
    if (version == 1 || layer != 1 || br == 0 || br == 15 || sr == 3 || (p[3] & 3) == 2) {
        return false;
    }
    const bool v1 = version == 3;
    h.version = version;
    h.sample_rate = kRateV1[sr] >> (v1 ? 0 : version == 2 ? 1 : 2);
    h.kbps = v1 ? kKbpsV1[br] : kKbpsV2[br];
    h.channels = (p[3] >> 6) == 3 ? 1 : 2;
    const uint32_t padding = (p[2] >> 1) & 1;
    h.length = (v1 ? 144000 : 72000) * h.kbps / h.sample_rate + padding;
    h.samples_per_frame = v1 ? 1152 : 576;
    h.side_info = v1 ? (h.channels == 1 ? 17 : 32) : (h.channels == 1 ? 9 : 17);
    return true;
}

// Offset in `s` of the first frame whose successor (when it lies within
// `s`) is a matching frame header. A frame whose successor is past the
// span counts only when the span reaches the end of the file, so a stray
// sync near the end of a bounded head read is not taken for the stream.
bool find_first_frame(std::span<const uint8_t> s, bool span_reaches_eof, size_t& at,
                      FrameHeader& h) {
    for (size_t i = 0; s.size() >= 4 && i <= s.size() - 4; ++i) {
        if (!parse_header(s.data() + i, h)) continue;
        const uint64_t next = uint64_t{i} + h.length;
        if (next + 4 <= s.size()) {
            FrameHeader n;
            if (!parse_header(s.data() + next, n) || n.version != h.version ||
                n.sample_rate != h.sample_rate) {
                continue;
            }
        } else if (!span_reaches_eof) {
            continue;
        }
        at = i;
        return true;
    }
    return false;
}

uint32_t clamp_ms(uint64_t ms) { return static_cast<uint32_t>(std::min<uint64_t>(ms, UINT32_MAX)); }

}  // namespace

Result<Mp3Info> parse_mp3_info(std::span<const uint8_t> after_id3, uint64_t file_size,
                               uint64_t audio_offset) {
    const bool reaches_eof = audio_offset + after_id3.size() >= file_size;
    size_t at = 0;
    FrameHeader h{};
    if (!find_first_frame(after_id3, reaches_eof, at, h)) {
        return Result<Mp3Info>::err(ErrorCode::Corrupt, "no MPEG Layer III frame");
    }

    Mp3Info info;
    info.sample_rate = h.sample_rate;
    info.channels = h.channels;
    info.audio_offset = audio_offset + at;
    const uint64_t remaining = file_size > info.audio_offset ? file_size - info.audio_offset : 0;
    info.audio_bytes = remaining;

    // The first frame as far as it lies within the span.
    const auto frame = after_id3.subspan(at, std::min<size_t>(h.length, after_id3.size() - at));
    uint64_t frames = 0;
    uint64_t bytes = 0;

    const size_t xing = 4 + h.side_info;
    constexpr size_t kVbri = 4 + 32;  // fixed, whatever the channel mode
    if (frame.size() >= xing + 8 && (std::memcmp(frame.data() + xing, "Xing", 4) == 0 ||
                                     std::memcmp(frame.data() + xing, "Info", 4) == 0)) {
        const uint32_t flags = be32(frame.data() + xing + 4);
        size_t pos = xing + 8;
        // A field cut off by the span ends the reading; what came before
        // is kept and the rest falls back to the estimate.
        bool whole = true;
        auto field32 = [&](uint32_t bit, uint64_t& out) {
            if (!whole || !(flags & bit)) return;
            if (frame.size() - pos < 4) {
                whole = false;
                return;
            }
            out = be32(frame.data() + pos);
            pos += 4;
        };
        field32(1, frames);
        field32(2, bytes);
        if (whole && (flags & 4) && frame.size() - pos >= info.toc.size()) {
            std::memcpy(info.toc.data(), frame.data() + pos, info.toc.size());
            info.has_toc = true;
        }
    } else if (frame.size() >= kVbri + 18 && std::memcmp(frame.data() + kVbri, "VBRI", 4) == 0) {
        // "VBRI", version, delay, quality (2 bytes each), then bytes and
        // frames. Its own TOC has a different layout and is not kept.
        bytes = be32(frame.data() + kVbri + 10);
        frames = be32(frame.data() + kVbri + 14);
    }

    if (bytes > 0 && bytes <= remaining) info.audio_bytes = bytes;
    if (frames > 0) {
        info.duration_ms = clamp_ms(frames * h.samples_per_frame * 1000 / h.sample_rate);
    } else {
        // bits / (kbit/s) = ms
        info.duration_ms = clamp_ms(info.audio_bytes * 8 / h.kbps);
    }
    return Result<Mp3Info>::ok(info);
}

}  // namespace wisp::media
