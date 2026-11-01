# 外部参考

> 记录移植或参考过的外部代码来源；许可证全文登记在 `THIRD_PARTY_NOTICES.md`。

| 文件/组件 | 来源 | 说明 |
|---|---|---|
| `components/wisp_ui/fonts/lv_font_cjk_16.c` | `MyOS` 项目 `src/config/lv_font_cjk_16.c`（原样复制） | GB2312 全集、16px、4bpp 的 LVGL 字体数据，字形来自 Noto Sans CJK SC（通过 LVGL 官方字体转换工具生成），用作 `lv_font_montserrat_16` 的 CJK 回退字体。许可证见 `THIRD_PARTY_NOTICES.md`。 |
| LVGL 9.5.0 | https://github.com/lvgl/lvgl（`cmake/wisp_host_deps.cmake` 通过 FetchContent 锁定版本，未 fork） | `wisp_ui` 的 UI 运行时直接基于 LVGL；`components/wisp_ui/config/lv_conf.h` 是在官方 `lv_conf_template.h` 上按本项目要求改写的配置文件。许可证见 `THIRD_PARTY_NOTICES.md`。 |
