#include "screenshot.hpp"

#include <algorithm>
#include <cstring>
#include <utility>

#include "wisp/base/image.hpp"
#include "wisp/os/large.hpp"

namespace wisp::agent {

hal::Area screenshot_area(const hal::ScreenShape& shape) {
    hal::Area a{.x1 = shape.width, .y1 = shape.height, .x2 = -1, .y2 = -1};
    for (int32_t y = 0; y < shape.height; ++y) {
        const hal::Span s = hal::row_span(shape, y);
        if (s.empty()) continue;
        a.x1 = std::min(a.x1, s.x1);
        a.x2 = std::max(a.x2, s.x2);
        a.y1 = std::min(a.y1, y);
        a.y2 = y;
    }
    return a;
}

Result<std::shared_ptr<ToolImage>> make_screenshot(std::vector<uint8_t>&& rgb,
                                                   const hal::ScreenShape& shape,
                                                   const ScreenshotOptions& opts) {
    using R = Result<std::shared_ptr<ToolImage>>;
    if (shape.width <= 0 || shape.height <= 0 ||
        rgb.size() != static_cast<size_t>(shape.width) * shape.height * 3) {
        return R::err(ErrorCode::InvalidArgument, "the snapshot does not match the panel");
    }
    const hal::Area area = screenshot_area(shape);
    if (area.x1 > area.x2 || area.y1 > area.y2 || opts.width == 0) {
        return R::err(ErrorCode::InvalidArgument, "nothing to show");
    }
    const size_t stride = static_cast<size_t>(shape.width) * 3;

    // What the panel cannot show is black, as the user sees it.
    for (int32_t y = 0; y < shape.height; ++y) {
        uint8_t* row = rgb.data() + y * stride;
        const hal::Span s = hal::row_span(shape, y);
        if (s.empty()) {
            std::memset(row, 0, stride);
            continue;
        }
        std::memset(row, 0, static_cast<size_t>(s.x1) * 3);
        std::memset(row + (static_cast<size_t>(s.x2) + 1) * 3, 0,
                    static_cast<size_t>(shape.width - 1 - s.x2) * 3);
    }

    const uint32_t cw = static_cast<uint32_t>(area.x2 - area.x1 + 1);
    const uint32_t ch = static_cast<uint32_t>(area.y2 - area.y1 + 1);
    const uint32_t ow = std::min(opts.width, cw);
    const uint32_t oh = std::max<uint32_t>(
        1, static_cast<uint32_t>((static_cast<uint64_t>(ch) * ow + cw / 2) / cw));

    // Area average: each output pixel is the mean of its source box. With
    // ow <= cw and oh <= ch no box is empty.
    os::LargeBuffer scaled = os::LargeBuffer::allocate(static_cast<size_t>(ow) * oh * 3);
    if (!scaled.ok()) return R::err(ErrorCode::NoMemory, "no memory for the scaled image");
    for (uint32_t oy = 0; oy < oh; ++oy) {
        const uint32_t y0 = static_cast<uint32_t>(static_cast<uint64_t>(oy) * ch / oh);
        const uint32_t y1 = static_cast<uint32_t>(static_cast<uint64_t>(oy + 1) * ch / oh);
        for (uint32_t ox = 0; ox < ow; ++ox) {
            const uint32_t x0 = static_cast<uint32_t>(static_cast<uint64_t>(ox) * cw / ow);
            const uint32_t x1 = static_cast<uint32_t>(static_cast<uint64_t>(ox + 1) * cw / ow);
            uint32_t sum[3] = {0, 0, 0};
            for (uint32_t y = y0; y < y1; ++y) {
                const uint8_t* p = rgb.data() + (area.y1 + y) * stride + (area.x1 + x0) * 3;
                for (uint32_t x = x0; x < x1; ++x, p += 3) {
                    sum[0] += p[0];
                    sum[1] += p[1];
                    sum[2] += p[2];
                }
            }
            const uint32_t n = (y1 - y0) * (x1 - x0);
            uint8_t* d = scaled.data() + (static_cast<size_t>(oy) * ow + ox) * 3;
            for (int c = 0; c < 3; ++c) d[c] = static_cast<uint8_t>((sum[c] + n / 2) / n);
        }
    }
    // The snapshot (651 KiB on a 466 px panel) goes before the JPEG buffer
    // is taken (spec 3.6).
    rgb = std::vector<uint8_t>();

    // Never above kImageMaxBytes, whatever the caller asked (spec 3.5).
    auto img = make_tool_image("image/jpeg", std::min(opts.max_bytes, kImageMaxBytes));
    if (!img) return R::err(ErrorCode::NoMemory, "no memory for the JPEG");
    auto n = encode_jpeg(scaled.span(), ow, oh, opts.quality, img->bytes.span());
    if (!n) return R::err(n.error().code, n.error().message);
    img->bytes.truncate(n.value());
    img->width = ow;
    img->height = oh;
    return R::ok(std::move(img));
}

}  // namespace wisp::agent
