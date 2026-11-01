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
# LVGL — declared now, but only usable once components/wisp_ui/config/lv_conf.h
# exists (added in Task 10). Skip fetching it until then so earlier tasks are
# not blocked on a config file that doesn't exist yet.
# ---------------------------------------------------------------------------
set(WISP_LV_CONF_PATH "${WISP_ROOT}/components/wisp_ui/config/lv_conf.h")
if(EXISTS ${WISP_LV_CONF_PATH})
    set(LV_CONF_PATH ${WISP_LV_CONF_PATH} CACHE PATH "Path to lv_conf.h" FORCE)

    FetchContent_Declare(
        lvgl
        GIT_REPOSITORY https://github.com/lvgl/lvgl.git
        GIT_TAG v9.5.0
        GIT_SHALLOW TRUE
    )
    FetchContent_MakeAvailable(lvgl)
endif()
