#include "wisp/base/image.hpp"

#include <cstring>

#include "stb_image_write.h"

namespace wisp {

namespace {

// stb_image_write hands the encoded stream to a callback in small pieces.
// The sink copies them into the caller's span; past its end it only
// records the overflow, so a too-small span costs no allocation and the
// encoder still runs to completion (it has no way to be stopped early).
struct JpegSink {
    std::span<uint8_t> out;
    size_t written = 0;
    bool overflow = false;
};

void jpeg_sink(void* context, void* data, int size) {
    auto* sink = static_cast<JpegSink*>(context);
    if (sink->overflow || size <= 0) return;
    const size_t n = size_t(size);
    if (n > sink->out.size() - sink->written) {
        sink->overflow = true;
        return;
    }
    std::memcpy(sink->out.data() + sink->written, data, n);
    sink->written += n;
}

uint32_t be16(const uint8_t* p) { return (uint32_t(p[0]) << 8) | p[1]; }

uint32_t be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}

// Walks the marker segments after SOI by their length fields up to the
// first frame header (SOFn). Stops, size unknown, at the scan (SOS), the
// end (EOI), anything that is not a marker, or a segment that runs past
// `b`: the bytes are untrusted (http.fetch), so nothing is read out of
// bounds.
void jpeg_size(std::span<const uint8_t> b, ImageInfo& info) {
    size_t i = 2;
    while (i + 1 < b.size()) {
        if (b[i] != 0xFF) return;
        const uint8_t m = b[i + 1];
        if (m == 0xFF) {  // fill byte before a marker
            ++i;
            continue;
        }
        i += 2;
        // Markers without a length: TEM, RSTn, SOI.
        if (m == 0x01 || (m >= 0xD0 && m <= 0xD8)) continue;
        if (m == 0xD9 || m == 0xDA) return;
        if (i + 2 > b.size()) return;
        const uint32_t len = be16(&b[i]);
        if (len < 2 || i + len > b.size()) return;
        const bool sof = m >= 0xC0 && m <= 0xCF && m != 0xC4 && m != 0xC8 && m != 0xCC;
        if (sof) {
            // length(2) precision(1) height(2) width(2)
            if (len < 7) return;
            info.height = be16(&b[i + 3]);
            info.width = be16(&b[i + 5]);
            return;
        }
        i += len;
    }
}

}  // namespace

Result<size_t> encode_jpeg(std::span<const uint8_t> rgb, uint32_t width, uint32_t height,
                           int quality, std::span<uint8_t> out) {
    if (width == 0 || height == 0 || width > 65535 || height > 65535) {
        return Result<size_t>::err(ErrorCode::InvalidArgument, "image size out of range");
    }
    if (quality < 1 || quality > 100) {
        return Result<size_t>::err(ErrorCode::InvalidArgument, "quality outside 1-100");
    }
    if (rgb.size() != size_t(width) * height * 3) {
        return Result<size_t>::err(ErrorCode::InvalidArgument, "rgb size != width * height * 3");
    }
    JpegSink sink{out};
    const int ok =
        stbi_write_jpg_to_func(jpeg_sink, &sink, int(width), int(height), 3, rgb.data(), quality);
    if (ok == 0) return Result<size_t>::err(ErrorCode::IoError, "jpeg encoder failed");
    if (sink.overflow) {
        return Result<size_t>::err(ErrorCode::QuotaExceeded, "jpeg larger than the buffer");
    }
    return Result<size_t>::ok(sink.written);
}

std::optional<ImageInfo> sniff_image(std::span<const uint8_t> b) {
    static constexpr uint8_t kPng[] = {0x89, 'P', 'N', 'G', '\r', '\n', 0x1A, '\n'};
    if (b.size() >= 3 && b[0] == 0xFF && b[1] == 0xD8 && b[2] == 0xFF) {
        ImageInfo info{"image/jpeg"};
        jpeg_size(b, info);
        return info;
    }
    if (b.size() >= sizeof(kPng) && std::memcmp(b.data(), kPng, sizeof(kPng)) == 0) {
        ImageInfo info{"image/png"};
        // The first chunk is IHDR: length(4) "IHDR" width(4) height(4).
        if (b.size() >= 24 && std::memcmp(&b[12], "IHDR", 4) == 0) {
            info.width = be32(&b[16]);
            info.height = be32(&b[20]);
        }
        return info;
    }
    return std::nullopt;
}

}  // namespace wisp
