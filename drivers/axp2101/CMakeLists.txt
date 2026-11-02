# Pure C ESP-IDF component: AXP2101 power-management IC driver, ported
# from MyOS (MIT, Copyright (c) 2026 ShouChen) -- see docs/references.md
# and axp2101.h's file header. Registered directly with
# idf_component_register() rather than cmake/wisp_component.cmake's
# wisp_component() macro: that macro exists so the *portable* layers
# (wisp_base/os/hal/core/ui/app) build both under idf.py and as plain
# CMake targets in the host simulator (AGENTS.md section 5); this
# component uses driver/i2c_master.h directly and only ever builds under
# idf.py, picked up via firmware/CMakeLists.txt's EXTRA_COMPONENT_DIRS.
idf_component_register(
    SRCS "axp2101.c"
    INCLUDE_DIRS "include"
    REQUIRES esp_driver_i2c
)
