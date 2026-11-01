wisp_component(
    NAME wisp_hal
    SRCS src/device.cpp src/display_geometry.cpp
    INCLUDE_DIRS include
    REQUIRES wisp_base
)
