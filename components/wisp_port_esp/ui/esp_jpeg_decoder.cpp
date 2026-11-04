// The firmware's JPEG decoder (see wisp/port/esp/esp_jpeg_decoder.hpp).

#include "wisp/port/esp/esp_jpeg_decoder.hpp"

#include <cstddef>
#include <cstdint>

#include "esp_jpeg_dec.h"
#include "lvgl.h"
#include "lvgl_private.h"  // lv_image_decoder_t, lv_image_decoder_dsc_t
#include "wisp/os/os.hpp"
#include "wisp/port/esp/jpeg_header.hpp"

namespace wisp::port::esp {

namespace {

// esp_new_jpeg writes its output with aligned vector stores.
constexpr uintptr_t kOutAlign = 16;

JpegHeader header_of(const lv_image_decoder_dsc_t* dsc, const lv_image_dsc_t*& img) {
    if (dsc->src_type != LV_IMAGE_SRC_VARIABLE) return {};
    img = static_cast<const lv_image_dsc_t*>(dsc->src);
    return read_jpeg_header(img->data, img->data_size);
}

// The display's RGB565 byte order; esp_new_jpeg writes either.
bool swapped() { return lv_display_get_color_format(nullptr) == LV_COLOR_FORMAT_RGB565_SWAPPED; }

lv_result_t info_cb(lv_image_decoder_t*, lv_image_decoder_dsc_t* dsc, lv_image_header_t* header) {
    const lv_image_dsc_t* img = nullptr;
    const JpegHeader h = header_of(dsc, img);
    if (h.kind != JpegKind::Baseline) return LV_RESULT_INVALID;
    header->cf = swapped() ? LV_COLOR_FORMAT_RGB565_SWAPPED : LV_COLOR_FORMAT_RGB565;
    header->w = h.width;
    header->h = h.height;
    header->stride = static_cast<uint32_t>(h.width) * 2;
    return LV_RESULT_OK;
}

void free_buf(lv_draw_buf_t* buf) {
    if (buf == nullptr) return;
    os::dealloc(buf->unaligned_data);
    lv_free(buf);
}

// Decodes `img` into a new draw buffer, or nullptr.
lv_draw_buf_t* decode(const lv_image_dsc_t* img, const JpegHeader& h) {
    const bool be = swapped();
    jpeg_dec_config_t config = DEFAULT_JPEG_DEC_CONFIG();
    config.output_type = be ? JPEG_PIXEL_FORMAT_RGB565_BE : JPEG_PIXEL_FORMAT_RGB565_LE;
    jpeg_dec_handle_t dec = nullptr;
    if (jpeg_dec_open(&config, &dec) != JPEG_ERR_OK) return nullptr;
    jpeg_dec_io_t io{};
    // Only read.
    io.inbuf = const_cast<uint8_t*>(img->data);
    io.inbuf_len = static_cast<int>(img->data_size);
    jpeg_dec_header_info_t info{};
    int out_len = 0;
    lv_draw_buf_t* buf = nullptr;
    void* base = nullptr;
    const uint32_t stride = static_cast<uint32_t>(h.width) * 2;
    const size_t size = static_cast<size_t>(stride) * h.height;
    bool ok = jpeg_dec_parse_header(dec, &io, &info) == JPEG_ERR_OK && info.width == h.width &&
              info.height == h.height && jpeg_dec_get_outbuf_len(dec, &out_len) == JPEG_ERR_OK &&
              out_len > 0 && static_cast<size_t>(out_len) >= size;
    if (ok) {
        base = os::alloc(static_cast<size_t>(out_len) + kOutAlign - 1, os::Mem::Large);
        buf = static_cast<lv_draw_buf_t*>(lv_malloc(sizeof(lv_draw_buf_t)));
        ok = base != nullptr && buf != nullptr;
    }
    if (ok) {
        auto* out = reinterpret_cast<uint8_t*>((reinterpret_cast<uintptr_t>(base) + kOutAlign - 1) &
                                               ~(kOutAlign - 1));
        io.outbuf = out;
        ok = jpeg_dec_process(dec, &io) == JPEG_ERR_OK &&
             lv_draw_buf_init(buf, h.width, h.height,
                              be ? LV_COLOR_FORMAT_RGB565_SWAPPED : LV_COLOR_FORMAT_RGB565, stride,
                              out, static_cast<uint32_t>(size)) == LV_RESULT_OK;
        // What free_buf() releases.
        if (ok) buf->unaligned_data = base;
    }
    jpeg_dec_close(dec);
    if (!ok) {
        os::dealloc(base);
        lv_free(buf);
        return nullptr;
    }
    return buf;
}

lv_result_t open_cb(lv_image_decoder_t*, lv_image_decoder_dsc_t* dsc) {
    const lv_image_dsc_t* img = nullptr;
    const JpegHeader h = header_of(dsc, img);
    if (h.kind != JpegKind::Baseline) return LV_RESULT_INVALID;
    lv_draw_buf_t* buf = decode(img, h);
    if (buf == nullptr) return LV_RESULT_INVALID;
    dsc->decoded = buf;
    return LV_RESULT_OK;
}

void close_cb(lv_image_decoder_t*, lv_image_decoder_dsc_t* dsc) {
    free_buf(const_cast<lv_draw_buf_t*>(dsc->decoded));
    dsc->decoded = nullptr;
}

}  // namespace

void install_image_decoders() {
    lv_image_decoder_t* dec = lv_image_decoder_create();
    lv_image_decoder_set_info_cb(dec, info_cb);
    lv_image_decoder_set_open_cb(dec, open_cb);
    lv_image_decoder_set_close_cb(dec, close_cb);
    dec->name = "wisp_esp_jpeg";
}

}  // namespace wisp::port::esp
