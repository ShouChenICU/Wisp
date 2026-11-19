// RIFF/WAVE header parsing (media spec 3.3, 4.3): PCM 16-bit, 1 or 2
// channels.

#include <cstring>

#include "wisp/media/tags.hpp"

namespace wisp::media {
namespace {

constexpr uint16_t kFormatPcm = 1;
constexpr uint16_t kFormatExtensible = 0xFFFE;

uint16_t le16(const uint8_t* p) { return static_cast<uint16_t>(p[0] | (p[1] << 8)); }

uint32_t le32(const uint8_t* p) {
    return uint32_t{p[0]} | (uint32_t{p[1]} << 8) | (uint32_t{p[2]} << 16) | (uint32_t{p[3]} << 24);
}

Result<WavInfo> corrupt(const char* why) { return Result<WavInfo>::err(ErrorCode::Corrupt, why); }

}  // namespace

Result<WavInfo> parse_wav(std::span<const uint8_t> head, uint64_t file_size) {
    if (head.size() < 12 || std::memcmp(head.data(), "RIFF", 4) != 0 ||
        std::memcmp(head.data() + 8, "WAVE", 4) != 0) {
        return corrupt("not a RIFF/WAVE file");
    }
    // The RIFF size is not trusted: writers that stream leave it 0 or
    // 0xFFFFFFFF, so the chunk walk is bounded by the span and the data
    // chunk by the file size instead.
    WavInfo info;
    bool have_fmt = false;
    size_t pos = 12;
    while (head.size() - pos >= 8) {
        const uint8_t* c = head.data() + pos;
        const uint32_t size = le32(c + 4);
        const size_t body = pos + 8;

        if (std::memcmp(c, "fmt ", 4) == 0) {
            if (size < 16 || size > head.size() - body) return corrupt("short fmt chunk");
            const uint8_t* f = head.data() + body;
            uint16_t format = le16(f);
            const uint16_t channels = le16(f + 2);
            const uint32_t rate = le32(f + 4);
            const uint16_t bits = le16(f + 14);
            if (format == kFormatExtensible) {
                // The sub-format GUID starts with the plain format code.
                if (size < 40) return corrupt("short extensible fmt chunk");
                format = le16(f + 24);
            }
            if (format != kFormatPcm) {
                return Result<WavInfo>::err(ErrorCode::Unsupported, "WAV is not integer PCM");
            }
            if (bits != 16) {
                return Result<WavInfo>::err(ErrorCode::Unsupported, "WAV is not 16-bit");
            }
            if (channels == 0 || rate == 0) return corrupt("WAV without channels or rate");
            if (channels > 2) {
                return Result<WavInfo>::err(ErrorCode::Unsupported, "WAV has more than 2 channels");
            }
            info.sample_rate = rate;
            info.channels = static_cast<uint8_t>(channels);
            have_fmt = true;
        } else if (std::memcmp(c, "data", 4) == 0) {
            if (!have_fmt) return corrupt("data chunk before fmt");
            const uint64_t frame_bytes = uint64_t{info.channels} * 2;
            info.data_offset = body;
            const uint64_t in_file = file_size > body ? file_size - body : 0;
            const uint64_t bytes = size < in_file ? size : in_file;
            info.data_bytes = bytes - bytes % frame_bytes;
            const uint64_t ms = info.data_bytes / frame_bytes * 1000 / info.sample_rate;
            info.duration_ms = ms > UINT32_MAX ? UINT32_MAX : static_cast<uint32_t>(ms);
            return Result<WavInfo>::ok(info);
        }

        // Chunks are padded to an even size.
        const uint64_t next = uint64_t{body} + size + (size & 1);
        if (next > head.size()) break;
        pos = static_cast<size_t>(next);
    }
    return corrupt("no data chunk in the header");
}

}  // namespace wisp::media
