# wisp_ui: the LVGL 9.5 runtime over wisp::hal::Display/Touch (see
# wisp/ui/ui_runtime.hpp). lv_font_cjk_16.c is ~6MB of generated glyph data
# (ported from MyOS, ultimately from Noto Sans CJK SC); expect the first
# build of this component to take a while.
#
# Self-contained include -- see cmake/wisp_component.cmake's header
# comment.
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/wisp_component.cmake)

wisp_component(
    NAME wisp_ui
    SRCS src/align.cpp src/ui_runtime.cpp fonts/lv_font_cjk_16.c
    INCLUDE_DIRS include
    REQUIRES wisp_base wisp_os wisp_hal wisp_core
    IDF_REQUIRES lvgl__lvgl
    HOST_LIBS lvgl
)
