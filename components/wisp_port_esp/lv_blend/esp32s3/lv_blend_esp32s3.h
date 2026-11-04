/*
 * SPDX-FileCopyrightText: 2024-2025 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 */

/*
 * LVGL software-renderer hooks for the ESP32-S3's SIMD instructions
 * (LV_USE_DRAW_SW_ASM == LV_DRAW_SW_ASM_CUSTOM; see lv_conf.h).
 *
 * Adapted from esp_lvgl_port 2.9.0 (espressif/esp-bsp commit
 * f0ef9497efce684997ce391edd19733483e250a5),
 * components/esp_lvgl_port/include/esp_lvgl_port_lv_blend.h, Apache-2.0;
 * see docs/references.md and THIRD_PARTY_NOTICES.md. Changes: only the
 * RGB565 fill and copy are hooked; the descriptors are LVGL 9.5's; the
 * RGB565_SWAPPED fill and copy reuse the RGB565 routines. The masked
 * RGB565_SWAPPED fill is Wisp's own plain C (../lv_blend_mask_rgb565.h).
 *
 * Only firmware/CMakeLists.txt names this header, and only for an
 * ESP32-S3 build; every other target compiles LVGL's plain C path.
 *
 * Each routine only handles the case LVGL calls the hook for: a fill
 * with no mask at full opacity, or a copy of an image with no mask at
 * full opacity.
 *
 * The copy, as upstream, may read up to 32 bytes past the end of a
 * source row (it loads whole 16-byte vectors). Sources are LVGL draw
 * buffers and images in RAM or mapped flash, where such a read cannot
 * fault on the S3, and the extra bytes are not stored.
 */

#pragma once

#include <stdint.h>

#include "src/draw/sw/blend/lv_draw_sw_blend_private.h"

// Plain C, not from esp_lvgl_port.
#include "../lv_blend_mask_rgb565.h"

#ifdef __cplusplus
extern "C" {
#endif

/* The argument block the assembly routines read (by byte offset). */
typedef struct {
    uint32_t opa;
    void* dst_buf;
    uint32_t dst_w;
    uint32_t dst_h;
    uint32_t dst_stride;
    const void* src_buf;
    uint32_t src_stride;
    const lv_opa_t* mask_buf;
    uint32_t mask_stride;
} wisp_lv_asm_dsc_t;

/* lv_color_blend_to_rgb565_esp32s3.S: src_buf points at an lv_color_t. */
extern int lv_color_blend_to_rgb565_esp(wisp_lv_asm_dsc_t* asm_dsc);
/* lv_rgb565_blend_normal_to_rgb565_esp32s3.S: a 16-bit copy. */
extern int lv_rgb565_blend_normal_to_rgb565_esp(wisp_lv_asm_dsc_t* asm_dsc);

static inline lv_result_t wisp_lv_fill_rgb565(lv_draw_sw_blend_fill_dsc_t* dsc, int swapped) {
    lv_color_t color = dsc->color;
    if (swapped) {
        /* The routine stores ((r & 0xf8) << 8) | ((g & 0xfc) << 3) | (b >> 3);
         * hand it the colour whose RGB565 value is the swapped one. */
        uint16_t v = lv_color_to_u16(dsc->color);
        v = (uint16_t)((v >> 8) | (v << 8));
        color.red = (uint8_t)((v >> 11) << 3);
        color.green = (uint8_t)(((v >> 5) & 0x3f) << 2);
        color.blue = (uint8_t)((v & 0x1f) << 3);
    }
    wisp_lv_asm_dsc_t asm_dsc = {
        .opa = LV_OPA_COVER,
        .dst_buf = dsc->dest_buf,
        .dst_w = (uint32_t)dsc->dest_w,
        .dst_h = (uint32_t)dsc->dest_h,
        .dst_stride = (uint32_t)dsc->dest_stride,
        .src_buf = &color,
        .src_stride = 0,
        .mask_buf = NULL,
        .mask_stride = 0,
    };
    return (lv_result_t)lv_color_blend_to_rgb565_esp(&asm_dsc);
}

static inline lv_result_t wisp_lv_copy_16bit(lv_draw_sw_blend_image_dsc_t* dsc) {
    wisp_lv_asm_dsc_t asm_dsc = {
        .opa = LV_OPA_COVER,
        .dst_buf = dsc->dest_buf,
        .dst_w = (uint32_t)dsc->dest_w,
        .dst_h = (uint32_t)dsc->dest_h,
        .dst_stride = (uint32_t)dsc->dest_stride,
        .src_buf = dsc->src_buf,
        .src_stride = (uint32_t)dsc->src_stride,
        .mask_buf = NULL,
        .mask_stride = 0,
    };
    return (lv_result_t)lv_rgb565_blend_normal_to_rgb565_esp(&asm_dsc);
}

#define LV_DRAW_SW_COLOR_BLEND_TO_RGB565(dsc) wisp_lv_fill_rgb565(dsc, 0)
#define LV_DRAW_SW_COLOR_BLEND_TO_RGB565_SWAPPED(dsc) wisp_lv_fill_rgb565(dsc, 1)
#define LV_DRAW_SW_RGB565_BLEND_NORMAL_TO_RGB565(dsc) wisp_lv_copy_16bit(dsc)
#define LV_DRAW_SW_RGB565_SWAPPED_BLEND_NORMAL_TO_RGB565_SWAPPED(dsc) wisp_lv_copy_16bit(dsc)
#define LV_DRAW_SW_COLOR_BLEND_TO_RGB565_SWAPPED_WITH_MASK(dsc) \
    wisp_lv_fill_mask_rgb565_swapped(dsc)

#ifdef __cplusplus
} /*extern "C"*/
#endif
