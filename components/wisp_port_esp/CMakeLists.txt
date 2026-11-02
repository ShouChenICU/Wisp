# ESP-IDF-only platform port: implements wisp::os on FreeRTOS/esp_timer/
# heap_caps (os/os_esp.cpp), wisp::port::esp::EspLogSink (log_esp.cpp) and
# the placeholder wisp::port::esp::RamSettingsBackend (ram_settings_backend.cpp
# -- see that header's comment; replaced by an NVS-backed implementation in
# Task 18), plus the HAL devices boards construct from esp_board_manager's
# handles and (Task 17) drivers/'s AXP2101/PCF85063/QMI8658 handles:
# wisp::port::esp::EspDisplay (dev/esp_display.cpp), EspTouch
# (dev/esp_touch.cpp), EspPower (dev/esp_power.cpp), EspClock
# (dev/esp_clock.cpp), EspImu (dev/esp_imu.cpp) and EspGpioButton
# (dev/esp_button.cpp). Like wisp_port_sim, this component is never built
# on the host (ESP_PLATFORM is never defined for it there, and no host
# CMakeLists adds it) -- only firmware/main pulls it in.
#
# Self-contained include -- see cmake/wisp_component.cmake's header
# comment.
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/wisp_component.cmake)

wisp_component(
    NAME wisp_port_esp
    SRCS os/os_esp.cpp log_esp.cpp ram_settings_backend.cpp dev/esp_display.cpp dev/esp_touch.cpp
         dev/esp_power.cpp dev/esp_clock.cpp dev/esp_imu.cpp dev/esp_button.cpp
    INCLUDE_DIRS include
    REQUIRES wisp_base wisp_os wisp_hal wisp_core
    IDF_REQUIRES freertos esp_timer heap log esp_lcd esp_lcd_touch esp_driver_i2c esp_driver_gpio
                 axp2101 pcf85063 qmi8658
)
