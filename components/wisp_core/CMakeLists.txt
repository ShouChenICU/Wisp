# Self-contained include -- see cmake/wisp_component.cmake's header
# comment.
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/wisp_component.cmake)

wisp_component(
    NAME wisp_core
    SRCS src/log.cpp src/event_bus.cpp src/device_registry.cpp src/boot_report.cpp
         src/button_service.cpp src/settings.cpp src/cached_settings.cpp src/latest_save.cpp
         src/storage_manager.cpp
         src/storage_worker.cpp
         src/power_monitor.cpp src/wakelock.cpp src/notification_service.cpp src/tone_player.cpp
         src/model_profiles.cpp src/model_presets.cpp src/system_settings.cpp
         src/firmware_image.cpp src/firmware_update.cpp src/zip.cpp
         src/scene_manifest.cpp src/scene_repository.cpp src/scene_install.cpp src/scene_store.cpp
         src/scene_host.cpp src/scene_files.cpp src/occupancy.cpp src/gesture.cpp src/sensor_service.cpp
         src/memory_store.cpp
    INCLUDE_DIRS include
    REQUIRES wisp_base wisp_os wisp_hal
)
