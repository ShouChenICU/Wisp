# ESP-IDF-only platform port: implements wisp::os on FreeRTOS/esp_timer/
# heap_caps (os/os_esp.cpp), wisp::port::esp::EspLogSink (log_esp.cpp) and
# wisp::port::esp::NvsSettingsBackend (settings_nvs.cpp, backed by NVS),
# plus the HAL devices boards construct from esp_board_manager's handles
# and drivers/'s AXP2101/PCF85063/QMI8658 handles: wisp::port::esp::
# EspDisplay (dev/esp_display.cpp), EspTouch (dev/esp_touch.cpp), EspPower
# (dev/esp_power.cpp), EspClock (dev/esp_clock.cpp), EspImu
# (dev/esp_imu.cpp), EspGpioButton (dev/esp_button.cpp), EspAudioOut/
# EspAudioIn (dev/esp_audio.cpp) and EspInternalStorage/
# EspSdStorage (dev/esp_storage.cpp). Like wisp_port_sim, this
# component is never built on the host (ESP_PLATFORM is never defined for
# it there, and no host CMakeLists adds it) -- only firmware/main pulls it
# in. (A handful of pure, platform-free headers factored out of the audio/
# storage code -- audio_downmix.hpp, sd_retry.hpp -- *are* unit-tested on
# the host; see tests/CMakeLists.txt's audio_downmix_test/sd_retry_test,
# which add this component's include/ directly rather than depending on
# this whole component.)
#
# Self-contained include -- see cmake/wisp_component.cmake's header
# comment.
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/wisp_component.cmake)

wisp_component(
    NAME wisp_port_esp
    SRCS os/os_esp.cpp log_esp.cpp settings_nvs.cpp dev/esp_display.cpp dev/esp_touch.cpp
         dev/esp_power.cpp dev/esp_clock.cpp dev/esp_imu.cpp dev/esp_button.cpp dev/esp_audio.cpp
         dev/esp_storage.cpp
    INCLUDE_DIRS include
    REQUIRES wisp_base wisp_os wisp_hal wisp_core
    IDF_REQUIRES freertos esp_timer heap log esp_lcd esp_lcd_touch esp_driver_i2c esp_driver_gpio
                 axp2101 pcf85063 qmi8658 esp_codec_dev nvs_flash fatfs sdmmc esp_driver_sdmmc
                 littlefs esp_board_manager
)
