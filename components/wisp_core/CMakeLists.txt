wisp_component(
    NAME wisp_core
    SRCS src/log.cpp src/event_bus.cpp src/device_registry.cpp src/boot_report.cpp
    INCLUDE_DIRS include
    REQUIRES wisp_base wisp_os wisp_hal
)
