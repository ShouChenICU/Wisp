// The simulator's JPEG decoder (see wisp/port/sim/image_decoders.hpp),
// built on TJpgDec (ChaN, BSD-style licence), which LVGL bundles as
// src/libs/tjpgd; cmake/wisp_host_deps.cmake turns LV_USE_TJPGD on for
// it. LVGL's own lv_tjpgd decoder reads a variable source through a
// memory file system the simulator does not have and leaves the pixels
// in RGB888, so it is replaced by this one.

#include "wisp/port/sim/image_decoders.hpp"

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "lvgl.h"
#include "lvgl_private.h"  // lv_image_decoder_t, lv_image_decoder_dsc_t
#include "src/libs/tjpgd/lv_tjpgd.h"
#include "src/libs/tjpgd/tjpgd.h"
#include "wisp/os/os.hpp"

namespace wisp::port::sim {

namespace {

// What TJpgDec needs for its tables and one MCU (its documentation:
// 3100 bytes at the most).
constexpr size_t kWorkBytes = 4096;

struct Session {
    const uint8_t* data = nullptr;
    size_t size = 0;
    size_t at = 0;
    // Output; nullptr while only the header is read.
    uint8_t* out = nullptr;
    uint32_t stride = 0;
    bool swapped = false;
};

size_t input(JDEC* jd, uint8_t* buf, size_t n) {
    auto* s = static_cast<Session*>(jd->device);
    n = std::min(n, s->size - s->at);
    if (buf != nullptr) std::memcpy(buf, s->data + s->at, n);
    s->at += n;
    return n;
}

// TJpgDec hands out RGB888 in LVGL's byte order (B, G, R).
int output(JDEC* jd, void* bitmap, JRECT* rect) {
    auto* s = static_cast<Session*>(jd->device);
    const auto* px = static_cast<const uint8_t*>(bitmap);
    for (uint32_t y = rect->top; y <= rect->bottom; ++y) {
        auto* row = reinterpret_cast<uint16_t*>(s->out + y * s->stride);
        for (uint32_t x = rect->left; x <= rect->right; ++x, px += 3) {
            auto c =
                static_cast<uint16_t>(((px[2] & 0xF8) << 8) | ((px[1] & 0xFC) << 3) | (px[0] >> 3));
            if (s->swapped) c = static_cast<uint16_t>((c >> 8) | (c << 8));
            row[x] = c;
        }
    }
    return 1;
}

bool is_jpeg(const lv_image_decoder_dsc_t* dsc, const lv_image_dsc_t*& img) {
    if (dsc->src_type != LV_IMAGE_SRC_VARIABLE) return false;
    img = static_cast<const lv_image_dsc_t*>(dsc->src);
    return img->data_size >= 2 && img->data[0] == 0xFF && img->data[1] == 0xD8;
}

// Reads the header into `jd`; false for anything TJpgDec cannot decode
// (progressive JPEG among them).
bool prepare(JDEC& jd, Session& s, void* work) {
    return jd_prepare(&jd, input, work, kWorkBytes, &s) == JDR_OK;
}

lv_color_format_t display_format() {
    return lv_display_get_color_format(nullptr) == LV_COLOR_FORMAT_RGB565_SWAPPED
               ? LV_COLOR_FORMAT_RGB565_SWAPPED
               : LV_COLOR_FORMAT_RGB565;
}

lv_result_t info_cb(lv_image_decoder_t*, lv_image_decoder_dsc_t* dsc, lv_image_header_t* header) {
    const lv_image_dsc_t* img = nullptr;
    if (!is_jpeg(dsc, img)) return LV_RESULT_INVALID;
    void* work = os::alloc(kWorkBytes, os::Mem::Large);
    if (work == nullptr) return LV_RESULT_INVALID;
    Session s{img->data, img->data_size};
    JDEC jd;
    const bool ok = prepare(jd, s, work);
    os::dealloc(work);
    if (!ok) return LV_RESULT_INVALID;
    header->cf = display_format();
    header->w = jd.width;
    header->h = jd.height;
    header->stride = static_cast<uint32_t>(jd.width) * 2;
    return LV_RESULT_OK;
}

void free_buf(lv_draw_buf_t* buf) {
    if (buf == nullptr) return;
    os::dealloc(buf->unaligned_data);
    lv_free(buf);
}

lv_result_t open_cb(lv_image_decoder_t*, lv_image_decoder_dsc_t* dsc) {
    const lv_image_dsc_t* img = nullptr;
    if (!is_jpeg(dsc, img)) return LV_RESULT_INVALID;
    void* work = os::alloc(kWorkBytes, os::Mem::Large);
    if (work == nullptr) return LV_RESULT_INVALID;
    Session s{img->data, img->data_size};
    JDEC jd;
    lv_draw_buf_t* buf = nullptr;
    lv_result_t res = LV_RESULT_INVALID;
    if (prepare(jd, s, work)) {
        const lv_color_format_t cf = display_format();
        const uint32_t stride = static_cast<uint32_t>(jd.width) * 2;
        const size_t size = static_cast<size_t>(stride) * jd.height;
        auto* data = static_cast<uint8_t*>(os::alloc(size, os::Mem::Large));
        buf = static_cast<lv_draw_buf_t*>(lv_malloc(sizeof(lv_draw_buf_t)));
        if (data != nullptr && buf != nullptr &&
            lv_draw_buf_init(buf, jd.width, jd.height, cf, stride, data, size) == LV_RESULT_OK) {
            s.out = data;
            s.stride = stride;
            s.swapped = cf == LV_COLOR_FORMAT_RGB565_SWAPPED;
            if (jd_decomp(&jd, output, 0) == JDR_OK) res = LV_RESULT_OK;
        } else {
            os::dealloc(data);
            lv_free(buf);
            buf = nullptr;
        }
    }
    os::dealloc(work);
    if (res != LV_RESULT_OK) {
        free_buf(buf);
        return res;
    }
    dsc->decoded = buf;
    return LV_RESULT_OK;
}

void close_cb(lv_image_decoder_t*, lv_image_decoder_dsc_t* dsc) {
    free_buf(const_cast<lv_draw_buf_t*>(dsc->decoded));
    dsc->decoded = nullptr;
}

}  // namespace

void install_image_decoders() {
    lv_tjpgd_deinit();
    lv_image_decoder_t* dec = lv_image_decoder_create();
    lv_image_decoder_set_info_cb(dec, info_cb);
    lv_image_decoder_set_open_cb(dec, open_cb);
    lv_image_decoder_set_close_cb(dec, close_cb);
    dec->name = "wisp_tjpgd";
}

}  // namespace wisp::port::sim
