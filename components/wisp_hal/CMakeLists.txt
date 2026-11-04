# Self-contained include -- see cmake/wisp_component.cmake's header
# comment.
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/wisp_component.cmake)

wisp_component(
    NAME wisp_hal
    SRCS src/device.cpp src/display_geometry.cpp src/draw_buf_policy.cpp
    INCLUDE_DIRS include
    REQUIRES wisp_base
)
