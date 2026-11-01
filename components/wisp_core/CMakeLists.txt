wisp_component(
    NAME wisp_core
    SRCS src/log.cpp
    INCLUDE_DIRS include
    REQUIRES wisp_base wisp_os
)
