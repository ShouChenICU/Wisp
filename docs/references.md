# 外部参考

> 记录移植或参考过的外部代码来源；许可证全文登记在 `THIRD_PARTY_NOTICES.md`。

| 文件/组件 | 来源 | 说明 |
|---|---|---|
| `components/wisp_ui/fonts/lv_font_cjk_16.c` | `MyOS` 项目 `src/config/lv_font_cjk_16.c`（原样复制） | GB2312 全集、16px、4bpp 的 LVGL 字体数据，字形来自 Noto Sans CJK SC（通过 LVGL 官方字体转换工具生成），用作 `lv_font_montserrat_16` 的 CJK 回退字体。许可证见 `THIRD_PARTY_NOTICES.md`。 |
| LVGL 9.5.0 | https://github.com/lvgl/lvgl（`cmake/wisp_host_deps.cmake` 通过 FetchContent 锁定版本，未 fork） | `wisp_ui` 的 UI 运行时直接基于 LVGL；`components/wisp_ui/config/lv_conf.h` 是在官方 `lv_conf_template.h` 上按本项目要求改写的配置文件。许可证见 `THIRD_PARTY_NOTICES.md`。 |
| `boards/waveshare_amoled_216/{board_info,board_peripherals,board_devices}.yaml`、`boards/waveshare_amoled_216/setup_device.c` | ESP-Claw（https://github.com/espressif/esp-claw ，提交 `36b377f0a46688916e4f6458a87dd92bee42aa92`）的 `application/edge_agent/boards/waveshare/waveshare_ESP32_S3_Touch_AMOLED_2_16/`，Apache-2.0 | 同款板的 esp_board_manager 板级定义。改动：`board` 改名为 `waveshare_amoled_216`；去掉 `imu_sensor`、`axp2101_power_manager` 两个 `custom` 设备（由 Wisp 自己的驱动处理）；`setup_device.c` 保留 Espressif 版权头、TAG 改为 `"wisp_board"`、文件头注明 "Adapted for Wisp"。许可证见 `THIRD_PARTY_NOTICES.md`。 |
| esp_board_manager 0.7.5、esp_lcd_co5300 2.2.0、esp_lcd_touch 1.2.1、esp_lcd_touch_cst9217 2.0.0 | ESP 组件仓库（https://components.espressif.com ，`firmware/main/idf_component.yml` 锁定精确版本，`firmware/dependencies.lock` 已提交，未 fork） | 固件的板级初始化（总线与设备）、CO5300 屏幕驱动、触摸抽象与 CST9217 触摸驱动。`components/wisp_port_esp/dev/esp_display.cpp`、`esp_touch.cpp` 在它们创建的句柄上实现 `hal::Display`/`hal::Touch`。许可证见 `THIRD_PARTY_NOTICES.md`。 |
