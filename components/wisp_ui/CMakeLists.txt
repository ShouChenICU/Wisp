# wisp_ui: the LVGL 9.5 runtime over wisp::hal::Display/Touch (see
# wisp/ui/ui_runtime.hpp). lv_font_cjk_16.c is ~6MB of generated glyph data
# (ported from MyOS, ultimately from Noto Sans CJK SC); expect the first
# build of this component to take a while.
wisp_component(
    NAME wisp_ui
    SRCS src/align.cpp src/ui_runtime.cpp fonts/lv_font_cjk_16.c
    INCLUDE_DIRS include
    REQUIRES wisp_base wisp_os wisp_hal wisp_core
    HOST_LIBS lvgl
)
