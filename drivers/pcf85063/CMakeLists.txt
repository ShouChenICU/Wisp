# ESP-IDF component: a C PCF85063 RTC driver, ported from MyOS (MIT,
# Copyright (c) 2026 ShouChen) -- see docs/references.md and
# pcf85063.h's file header. See drivers/axp2101/CMakeLists.txt's comment
# for why this uses idf_component_register() directly instead of
# cmake/wisp_component.cmake's wisp_component() macro.
idf_component_register(
    SRCS "pcf85063.c" "wisp/pcf85063_clock.cpp"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_i2c wisp_hal wisp_os wisp_core
)

# The component also carries the chip's Wisp adapter (wisp/pcf85063_clock.cpp,
# header wisp/drivers/pcf85063_clock.hpp); the C driver stays usable on its
# own. The adapter is C++20 like every wisp_* component; firmware/main
# pins the standard the same way.
target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_20)
