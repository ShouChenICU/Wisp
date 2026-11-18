# ESP-IDF component: a C AXP2101 power-management IC driver, ported
# from MyOS (MIT, Copyright (c) 2026 ShouChen) -- see docs/references.md
# and axp2101.h's file header. Registered directly with
# idf_component_register() rather than cmake/wisp_component.cmake's
# wisp_component() macro: that macro exists so the *portable* layers
# (wisp_base/os/hal/core/ui/app) build both under idf.py and as plain
# CMake targets in the host simulator (AGENTS.md section 5); this
# component uses driver/i2c_master.h directly and only ever builds under
# idf.py, picked up via firmware/CMakeLists.txt's EXTRA_COMPONENT_DIRS.
idf_component_register(
    SRCS "axp2101.c" "wisp/axp2101_power.cpp"
    INCLUDE_DIRS "include"
    # wisp_port_esp: the power key shares EspGpioButton's standby read
    # interval (wisp/port/esp/button_input.hpp). The dependency points from
    # the driver to the port, never back (tools/check_layering.py).
    REQUIRES esp_driver_i2c wisp_hal wisp_os wisp_core wisp_port_esp
)

# The component also carries the chip's Wisp adapter (wisp/axp2101_power.cpp,
# header wisp/drivers/axp2101_power.hpp); the C driver stays usable on its
# own. The adapter is C++20 like every wisp_* component; firmware/main
# pins the standard the same way.
target_compile_features(${COMPONENT_LIB} PRIVATE cxx_std_20)
