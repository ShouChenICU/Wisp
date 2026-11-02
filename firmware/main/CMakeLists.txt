# Firmware entry point: app_main() (main.cpp) plus the one board.cpp
# selected by firmware/CMakeLists.txt's WISP_BOARD cache variable (see
# wisp::board::create_board(), components/wisp_app/include/wisp/app/board.hpp).
# board.cpp drives esp_board_manager (bmgr) and the esp_lcd/esp_lcd_touch
# handles it creates (see boards/waveshare_amoled_216/board.cpp).
idf_component_register(
    SRCS main.cpp ${CMAKE_CURRENT_LIST_DIR}/../../boards/${WISP_BOARD}/board.cpp
    REQUIRES wisp_app wisp_port_esp esp_board_manager esp_lcd esp_lcd_touch esp_driver_i2c
             esp_driver_gpio esp_pm esp_app_format
)

# Every wisp_* component already pins itself to C++20/-fno-exceptions/
# -fno-rtti via cmake/wisp_component.cmake (see AGENTS.md section 5); this
# component is registered directly with idf_component_register() instead
# (it isn't a wisp_* component, just the firmware's own main), so pin the
# language standard here too rather than relying on IDF's own default
# (gnu++26 as of this ESP-IDF version -- see tools/cmake/build.cmake).
# -fno-exceptions/-fno-rtti don't need repeating: sdkconfig.defaults'
# CONFIG_COMPILER_CXX_EXCEPTIONS=n/CONFIG_COMPILER_CXX_RTTI=n already apply
# those flags build-wide.
target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_20)
