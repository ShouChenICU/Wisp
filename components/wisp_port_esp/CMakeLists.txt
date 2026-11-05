# ESP-IDF-only platform port: implements wisp::os on FreeRTOS/esp_timer/
# heap_caps (os/os_esp.cpp), wisp::port::esp::EspLogSink (log_esp.cpp) and
# wisp::port::esp::NvsSettingsBackend (settings_nvs.cpp, backed by NVS),
# plus the HAL devices boards construct from esp_board_manager's handles
# and drivers/'s AXP2101/PCF85063/QMI8658 handles: wisp::port::esp::
# EspDisplay (dev/esp_display.cpp), EspTouch (dev/esp_touch.cpp), EspPower
# (dev/esp_power.cpp), EspClock (dev/esp_clock.cpp), EspImu
# (dev/esp_imu.cpp), EspGpioButton (dev/esp_button.cpp), EspAudioOut/
# EspAudioIn (dev/esp_audio.cpp) and EspInternalStorage/
# EspSdStorage (dev/esp_storage.cpp), plus the serial console input
# wisp::port::esp::EspConsole (console_esp.cpp) and the networking
# platform services: EspWifi (net/esp_wifi.cpp, with the hotspot's
# captive DNS in net/captive_dns.cpp and mDNS), EspHttpServer
# (net/esp_http_server.cpp), EspHttpClient (net/esp_http_client.cpp) and
# EspSntp (net/esp_sntp.cpp, the network time query),
# plus EspFirmwareUpdater (esp_firmware.cpp, the OTA writer behind the
# admin page's firmware upload), mem_report() (mem_report.cpp, the
# firmware's `mem` console command) and the LVGL JPEG decoder for scene
# images (ui/esp_jpeg_decoder.cpp, on esp_new_jpeg), and the mapped
# "fonts" partition behind the vector font (ui/esp_font_partition.cpp).
# Like wisp_port_sim, this component is never built on the host
# (ESP_PLATFORM is never defined for it there, and no host CMakeLists adds
# it) -- only firmware/main pulls it in. (A handful of pure, platform-free headers factored out of the audio/
# storage/networking code -- audio_downmix.hpp, sd_retry.hpp,
# wifi_reason.hpp, net_iface.hpp, dns_reply.hpp, sntp_packet.hpp -- *are*
# unit-tested on the host; see tests/CMakeLists.txt's port_esp/ tests, which add this
# component's include/ directly rather than depending on this whole
# component.)
#
# Self-contained include -- see cmake/wisp_component.cmake's header
# comment.
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/wisp_component.cmake)

wisp_component(
    NAME wisp_port_esp
    SRCS os/os_esp.cpp log_esp.cpp settings_nvs.cpp dev/esp_display.cpp dev/esp_touch.cpp
         dev/esp_power.cpp dev/esp_clock.cpp dev/esp_imu.cpp dev/esp_button.cpp dev/esp_audio.cpp
         dev/esp_storage.cpp console_esp.cpp net/esp_wifi.cpp net/captive_dns.cpp
         net/esp_http_server.cpp net/esp_http_client.cpp net/esp_sntp.cpp esp_firmware.cpp
         mem_report.cpp
         ui/esp_jpeg_decoder.cpp ui/esp_font_partition.cpp
    INCLUDE_DIRS include
    REQUIRES wisp_base wisp_os wisp_hal wisp_core wisp_net
    IDF_REQUIRES freertos esp_timer heap log esp_lcd esp_lcd_touch esp_driver_i2c esp_driver_gpio
                 axp2101 pcf85063 qmi8658 esp_codec_dev esp_driver_i2s nvs_flash fatfs sdmmc esp_driver_sdmmc
                 littlefs esp_board_manager esp_driver_usb_serial_jtag esp_driver_uart
                 esp_wifi esp_netif esp_event lwip esp_http_server esp_http_client esp-tls
                 mbedtls mdns app_update esp_partition lvgl__lvgl espressif__esp_new_jpeg
)

# Rendering path: the display flush and touch read are always -O2,
# whatever the global level (connectivity spec 3.2; see
# firmware/CMakeLists.txt). Source options come after the target's on the
# compiler command line, so they win.
set_source_files_properties(dev/esp_display.cpp dev/esp_touch.cpp PROPERTIES COMPILE_OPTIONS -O2)

# LVGL's ESP32-S3 SIMD fill and copy (lv_blend/esp32s3/, from esp_lvgl_port;
# firmware/CMakeLists.txt points lv_conf.h at their hook header for this
# target only). Nothing in this component calls them, so the linker is
# told to keep them for LVGL's blend code.
if(CONFIG_IDF_TARGET_ESP32S3)
    target_sources(${COMPONENT_LIB} PRIVATE lv_blend/esp32s3/lv_color_blend_to_rgb565_esp32s3.S
                                            lv_blend/esp32s3/lv_rgb565_blend_normal_to_rgb565_esp32s3.S)
    target_link_libraries(${COMPONENT_LIB} INTERFACE "-u lv_color_blend_to_rgb565_esp"
                                                     "-u lv_rgb565_blend_normal_to_rgb565_esp")
endif()
