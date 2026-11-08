/*
 * SPDX-FileCopyrightText: 2026 Espressif Systems (Shanghai) CO LTD
 *
 * SPDX-License-Identifier: Apache-2.0
 *
 * The factory entries are adapted from
 * boards/waveshare_esp32_s3_touch_amoled_2_16/setup_device.c, itself
 * adapted from ESP-Claw (https://github.com/espressif/esp-claw, Apache-2.0).
 * The CO5300 init table s_lcd_init_cmds is copied verbatim from Waveshare's
 * ESP-IDF BSP for this board (waveshare/esp32_s3_touch_amoled_1_75c 3.0.0,
 * esp32_s3_touch_amoled_1_75c.c, lcd_init_cmds; Waveshare-ESP32-components
 * commit 010c5fb39ea7f0e996efd1f8e162cc70e569e57f, Apache-2.0; the BSP
 * ships the license text without a copyright line), as are the
 * esp_lcd_panel_set_gap(6, 0) column offset and the touch flips. See
 * docs/references.md and THIRD_PARTY_NOTICES.md.
 *
 * esp_board_manager calls these while bringing up the display_lcd /
 * lcd_touch devices declared in board_devices.yaml.
 */

#include <string.h>

#include "driver/gpio.h"
#include "esp_err.h"
#include "esp_lcd_co5300.h"
#include "esp_lcd_panel_ops.h"
#include "esp_lcd_touch_cst9217.h"
#include "esp_log.h"

static const char* TAG = "wisp_board";

/* The panel's 466 visible columns start at controller column 6. The
 * driver adds this to every draw_bitmap() window. */
#define LCD_X_GAP 6
#define LCD_Y_GAP 0

/* Touch orientation, from the BSP's bsp_display_start() defaults. To be
 * confirmed on the device (the hardware doc's acceptance checklist: the
 * touch point follows the finger in all four quadrants); flip a 1/0 here
 * if one axis comes out mirrored. */
#define TOUCH_SWAP_XY 0
#define TOUCH_MIRROR_X 1
#define TOUCH_MIRROR_Y 1

#define TOUCH_RST_GPIO GPIO_NUM_2
#define TOUCH_INT_GPIO GPIO_NUM_11

static const co5300_lcd_init_cmd_t s_lcd_init_cmds[] = {
    {0xFE, (uint8_t[]){0x20}, 1, 0},
    {0x19, (uint8_t[]){0x10}, 1, 0},
    {0x1C, (uint8_t[]){0xA0}, 1, 0},

    {0xFE, (uint8_t[]){0x00}, 1, 0},
    {0xC4, (uint8_t[]){0x80}, 1, 0},
    {0x3A, (uint8_t[]){0x55}, 1, 0},
    {0x35, (uint8_t[]){0x00}, 1, 0},
    {0x53, (uint8_t[]){0x20}, 1, 0},
    {0x51, (uint8_t[]){0xFF}, 1, 0},
    {0x63, (uint8_t[]){0xFF}, 1, 0},
    {0x2A, (uint8_t[]){0x00, 0x06, 0x01, 0xD7}, 4, 0},
    {0x2B, (uint8_t[]){0x00, 0x00, 0x01, 0xD1}, 4, 600},
    {0x11, NULL, 0, 600},
    {0x29, NULL, 0, 0},
};

static const co5300_vendor_config_t s_lcd_vendor_config = {
    .init_cmds = s_lcd_init_cmds,
    .init_cmds_size = sizeof(s_lcd_init_cmds) / sizeof(s_lcd_init_cmds[0]),
    .flags =
        {
            .use_qspi_interface = 1,
        },
};

esp_err_t lcd_panel_factory_entry_t(esp_lcd_panel_io_handle_t io,
                                    const esp_lcd_panel_dev_config_t* panel_dev_config,
                                    esp_lcd_panel_handle_t* ret_panel) {
    esp_lcd_panel_dev_config_t panel_dev_cfg = {0};
    memcpy(&panel_dev_cfg, panel_dev_config, sizeof(panel_dev_cfg));
    panel_dev_cfg.vendor_config = (void*)&s_lcd_vendor_config;

    esp_err_t ret = esp_lcd_new_panel_co5300(io, &panel_dev_cfg, ret_panel);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create CO5300 panel: %s", esp_err_to_name(ret));
        return ret;
    }
    /* Only stored by the driver, so it is safe before bmgr's reset/init. */
    ret = esp_lcd_panel_set_gap(*ret_panel, LCD_X_GAP, LCD_Y_GAP);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to set the CO5300 column offset: %s", esp_err_to_name(ret));
    }
    return ret;
}

esp_err_t lcd_touch_factory_entry_t(esp_lcd_panel_io_handle_t io,
                                    const esp_lcd_touch_config_t* touch_dev_config,
                                    esp_lcd_touch_handle_t* ret_touch) {
    esp_lcd_touch_config_t touch_cfg = {0};
    memcpy(&touch_cfg, touch_dev_config, sizeof(touch_cfg));

    touch_cfg.rst_gpio_num = TOUCH_RST_GPIO;
    touch_cfg.int_gpio_num = TOUCH_INT_GPIO;
    touch_cfg.levels.reset = 0;
    touch_cfg.levels.interrupt = 0;
    touch_cfg.flags.swap_xy = TOUCH_SWAP_XY;
    touch_cfg.flags.mirror_x = TOUCH_MIRROR_X;
    touch_cfg.flags.mirror_y = TOUCH_MIRROR_Y;

    esp_err_t ret = esp_lcd_touch_new_i2c_cst9217(io, &touch_cfg, ret_touch);
    if (ret != ESP_OK) {
        ESP_LOGE(TAG, "Failed to create CST9217 touch: %s", esp_err_to_name(ret));
    }
    return ret;
}
