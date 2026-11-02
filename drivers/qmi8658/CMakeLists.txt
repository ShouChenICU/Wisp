# Pure C ESP-IDF component: QMI8658 IMU driver, ported from MyOS (MIT,
# Copyright (c) 2026 ShouChen) -- see docs/references.md and
# qmi8658.h's file header. See drivers/axp2101/CMakeLists.txt's comment
# for why this uses idf_component_register() directly instead of
# cmake/wisp_component.cmake's wisp_component() macro.
idf_component_register(
    SRCS "qmi8658.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_i2c
)
