# ESP-IDF-only platform port: implements wisp::os on FreeRTOS/esp_timer/
# heap_caps (os/os_esp.cpp), wisp::port::esp::EspLogSink (log_esp.cpp) and
# the placeholder wisp::port::esp::RamSettingsBackend (ram_settings_backend.cpp
# -- see that header's comment; replaced by an NVS-backed implementation in
# Task 18). Like wisp_port_sim, this component is never built on the host
# (ESP_PLATFORM is never defined for it there, and no host CMakeLists adds
# it) -- only firmware/main pulls it in.
#
# Self-contained include -- see cmake/wisp_component.cmake's header
# comment.
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/wisp_component.cmake)

wisp_component(
    NAME wisp_port_esp
    SRCS os/os_esp.cpp log_esp.cpp ram_settings_backend.cpp
    INCLUDE_DIRS include
    REQUIRES wisp_base wisp_os wisp_core
    IDF_REQUIRES freertos esp_timer heap log
)
