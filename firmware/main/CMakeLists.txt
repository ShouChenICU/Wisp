# Firmware entry point: app_main() (main.cpp) plus the one board.cpp
# selected by firmware/CMakeLists.txt's WISP_BOARD cache variable (see
# wisp::board::create_board(), components/wisp_app/include/wisp/app/board.hpp).
# board.cpp drives esp_board_manager (bmgr) and the esp_lcd/esp_lcd_touch
# handles it creates (see boards/waveshare_esp32_s3_touch_amoled_2_16/board.cpp).
idf_component_register(
    SRCS main.cpp board_desc.c ${CMAKE_CURRENT_LIST_DIR}/../../boards/${WISP_BOARD}/board.cpp
    REQUIRES wisp_app wisp_port_esp esp_board_manager esp_lcd esp_lcd_touch esp_driver_i2c
             esp_driver_gpio esp_pm esp_app_format
    # Moves esp_new_jpeg's IRAM code to flash (see the file).
    LDFRAGMENTS esp_new_jpeg.lf
)

# The board descriptor (board_desc.c) names the board this image is for,
# right after esp_app_desc_t, where the firmware upload reads it from an
# image's first bytes (connectivity spec 6.5, docs/porting-guide.md).
# Nothing references the symbol, so the linker is told to keep it.
target_compile_definitions(${COMPONENT_LIB} PRIVATE WISP_BOARD_NAME="${WISP_BOARD}")
target_link_libraries(${COMPONENT_LIB} INTERFACE "-u wisp_board_desc")
if(DEFINED CONFIG_ESP_BOARD_NAME AND NOT CONFIG_ESP_BOARD_NAME STREQUAL WISP_BOARD)
    message(FATAL_ERROR "idf.py bmgr selected board \"${CONFIG_ESP_BOARD_NAME}\" but WISP_BOARD is "
                        "\"${WISP_BOARD}\"; rerun bmgr with -b ${WISP_BOARD} or pass -DWISP_BOARD")
endif()

# The admin web UI (connectivity spec 2.3): tools/pack_assets.py packs web/
# into wisp_web.wpak (failing the build over the pack's 256 KiB or the
# 150 KiB gzip budget), which is linked into the app image, 4-byte aligned,
# as _binary_wisp_web_pack_start/_end. The web page and the firmware are
# thus always the same version, and switch OTA slots together. The pack
# depends on every file and directory under web/: deleting or renaming a
# file only bumps its directory's mtime (same scheme as
# simulator/CMakeLists.txt).
set(WISP_WEB_DIR ${CMAKE_CURRENT_LIST_DIR}/../../web)
set(WISP_WEB_WPAK ${CMAKE_CURRENT_BINARY_DIR}/wisp_web.wpak)
file(GLOB_RECURSE WISP_WEB_FILES CONFIGURE_DEPENDS ${WISP_WEB_DIR}/*)
file(GLOB_RECURSE WISP_WEB_ENTRIES LIST_DIRECTORIES true CONFIGURE_DEPENDS ${WISP_WEB_DIR}/*)
list(APPEND WISP_WEB_FILES ${WISP_WEB_DIR})
foreach(entry IN LISTS WISP_WEB_ENTRIES)
    if(IS_DIRECTORY ${entry})
        list(APPEND WISP_WEB_FILES ${entry})
    endif()
endforeach()
idf_build_get_property(_wisp_python PYTHON)
add_custom_command(
    OUTPUT ${WISP_WEB_WPAK}
    COMMAND ${_wisp_python} ${CMAKE_CURRENT_LIST_DIR}/../../tools/pack_assets.py ${WISP_WEB_DIR}
            -o ${WISP_WEB_WPAK} --budget-kb 150
    DEPENDS ${CMAKE_CURRENT_LIST_DIR}/../../tools/pack_assets.py ${WISP_WEB_FILES}
    COMMENT "Packing web/ into wisp_web.wpak"
    VERBATIM
)
target_add_binary_data(${COMPONENT_LIB} ${WISP_WEB_WPAK} BINARY RENAME_TO wisp_web_pack ALIGN 4)

# The vector font image (vector-font spec 3.3, 3.5): tools/pack_font_image.py
# wraps the committed TTF in its header as build/fonts.bin, failing the
# build when it outgrows the board's fonts partition. It is not linked into
# the app: idf.py flash writes it to the fonts partition alongside the app,
# idf.py fonts-flash writes only it, and OTA never touches it. A board
# whose partition table has no fonts partition gets neither the image nor
# the flash targets (it simply has no vector font).
partition_table_get_partition_info(_wisp_fonts_size "--partition-name fonts" "size")
if(_wisp_fonts_size)
    set(WISP_FONT_IMAGE ${CMAKE_BINARY_DIR}/fonts.bin)
    set(_wisp_font_ttf
        ${CMAKE_CURRENT_LIST_DIR}/../../components/wisp_ui/fonts/noto_sans_cjk_sc_subset.ttf)
    add_custom_command(
        OUTPUT ${WISP_FONT_IMAGE}
        COMMAND ${_wisp_python} ${CMAKE_CURRENT_LIST_DIR}/../../tools/pack_font_image.py
                ${_wisp_font_ttf} -o ${WISP_FONT_IMAGE} --max-bytes ${_wisp_fonts_size}
        DEPENDS ${_wisp_font_ttf} ${CMAKE_CURRENT_LIST_DIR}/../../tools/pack_font_image.py
        COMMENT "Packing the vector font into fonts.bin"
        VERBATIM
    )
    add_custom_target(wisp_fonts_bin ALL DEPENDS ${WISP_FONT_IMAGE})
    idf_component_get_property(_wisp_main_args esptool_py FLASH_ARGS)
    idf_component_get_property(_wisp_sub_args esptool_py FLASH_SUB_ARGS)
    esptool_py_flash_target(fonts-flash "${_wisp_main_args}" "${_wisp_sub_args}" ALWAYS_PLAINTEXT)
    esptool_py_flash_to_partition(fonts-flash fonts ${WISP_FONT_IMAGE})
    add_dependencies(fonts-flash wisp_fonts_bin)
    esptool_py_flash_to_partition(flash fonts ${WISP_FONT_IMAGE})
    add_dependencies(flash wisp_fonts_bin)
endif()

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
