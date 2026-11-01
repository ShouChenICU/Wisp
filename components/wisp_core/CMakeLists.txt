wisp_component(
    NAME wisp_core
    SRCS src/log.cpp src/event_bus.cpp
    INCLUDE_DIRS include
    REQUIRES wisp_base wisp_os
)
