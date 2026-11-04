# Host-only third-party dependencies for the simulator build, fetched and
# version-locked via FetchContent. This file is included only from
# simulator/CMakeLists.txt (never under ESP_PLATFORM); WISP_ROOT must already
# be set by the including file.
include(FetchContent)

# ---------------------------------------------------------------------------
# googletest
# ---------------------------------------------------------------------------
FetchContent_Declare(
    googletest
    GIT_REPOSITORY https://github.com/google/googletest.git
    GIT_TAG v1.17.0
    GIT_SHALLOW TRUE
)
set(INSTALL_GTEST OFF CACHE BOOL "" FORCE)
set(BUILD_GMOCK OFF CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(googletest)

# ---------------------------------------------------------------------------
# ArduinoJson (provides the `ArduinoJson` target)
# ---------------------------------------------------------------------------
FetchContent_Declare(
    ArduinoJson
    GIT_REPOSITORY https://github.com/bblanchon/ArduinoJson.git
    GIT_TAG v7.4.3
    GIT_SHALLOW TRUE
)
FetchContent_MakeAvailable(ArduinoJson)

# ---------------------------------------------------------------------------
# LVGL -- guarded on components/wisp_ui/config/lv_conf.h actually existing
# (checked below) so this file degrades instead of hard-failing if that
# config header is ever missing, since LV_BUILD_CONF_PATH pointing at a
# nonexistent file would otherwise break every target that links `lvgl`,
# not just wisp_ui.
#
# LVGL 9.5's own CMakeLists.txt (env_support/cmake/os_desktop.cmake) reads the
# cache variable `LV_BUILD_CONF_PATH` -- NOT `LV_CONF_PATH` -- to decide which
# lv_conf.h to use; setting LV_CONF_PATH directly (as an earlier draft of this
# file did) is silently ignored, and LVGL falls back to looking for a
# `lv_conf.h` next to the top-level CMakeLists.txt, which doesn't exist here.
# When LV_BUILD_CONF_PATH is set, LVGL's CMakeLists.txt does
# `target_compile_definitions(lvgl PUBLIC LV_CONF_PATH="<abs path>")`, and it
# is that PUBLIC compile definition (propagated to every target that links
# `lvgl`) which makes `lv_conf_internal.h` do `#include LV_CONF_PATH` and pick
# up our file (see src/lv_conf_internal.h in the LVGL source).
# ---------------------------------------------------------------------------
set(WISP_LV_CONF_PATH "${WISP_ROOT}/components/wisp_ui/config/lv_conf.h")
if(EXISTS ${WISP_LV_CONF_PATH})
    set(LV_BUILD_CONF_PATH ${WISP_LV_CONF_PATH} CACHE PATH "Path to lv_conf.h" FORCE)

    # LVGL's env_support/cmake/os_desktop.cmake defaults all three of these
    # to ON, which (a) is a lot of extra build time we don't need and (b)
    # builds the demos/examples against *our* lv_conf.h, which doesn't
    # enable everything every demo assumes. wisp_ui only needs the core
    # `lvgl` target.
    set(CONFIG_LV_BUILD_DEMOS OFF CACHE BOOL "" FORCE)
    set(CONFIG_LV_BUILD_EXAMPLES OFF CACHE BOOL "" FORCE)
    set(CONFIG_LV_USE_THORVG_INTERNAL OFF CACHE BOOL "" FORCE)

    FetchContent_Declare(
        lvgl
        GIT_REPOSITORY https://github.com/lvgl/lvgl.git
        GIT_TAG v9.5.0
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(lvgl)
    # The simulator's JPEG decoder (components/wisp_port_sim/ui/
    # image_decoders.cpp) is built on the TJpgDec library LVGL bundles,
    # which LVGL compiles only with LV_USE_TJPGD. lv_conf.h leaves it off
    # for the firmware, which decodes JPEG with esp_new_jpeg.
    target_compile_definitions(lvgl PUBLIC LV_USE_TJPGD=1)
endif()

# ---------------------------------------------------------------------------
# libcurl and zlib (system packages, not fetched): only the simulator's
# platform layer uses them -- wisp::port::sim::SimHttpClient (libcurl,
# 7.80 or newer for curl_easy_* timeouts in ms and the CURLE_* set the
# error mapping relies on) and the --web-dir in-memory packer (zlib's
# gzip writer). Portable components never include curl/ (enforced by
# tools/check_layering.py).
# ---------------------------------------------------------------------------
find_package(CURL 7.80 REQUIRED)
find_package(ZLIB REQUIRED)
