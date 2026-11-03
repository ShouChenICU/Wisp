# Self-contained include -- see cmake/wisp_component.cmake's header
# comment.
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/wisp_component.cmake)

wisp_component(
    NAME wisp_core
    SRCS src/log.cpp src/event_bus.cpp src/device_registry.cpp src/boot_report.cpp
         src/button_service.cpp src/settings.cpp src/storage_manager.cpp src/power_monitor.cpp
         src/model_profiles.cpp src/model_presets.cpp src/system_settings.cpp
    INCLUDE_DIRS include
    REQUIRES wisp_base wisp_os wisp_hal
)
