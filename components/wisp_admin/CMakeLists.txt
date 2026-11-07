# Self-contained include -- see cmake/wisp_component.cmake's header
# comment.
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/wisp_component.cmake)

wisp_component(
    NAME wisp_admin
    SRCS src/admin_server.cpp src/filters.cpp src/routes_pair.cpp src/routes_device.cpp
         src/routes_wifi.cpp src/routes_ai.cpp src/routes_system.cpp src/routes_firmware.cpp
         src/routes_scenes.cpp src/routes_power.cpp src/routes_scene_files.cpp
         src/routes_agent.cpp src/routes_memory.cpp src/ws_hub.cpp
    INCLUDE_DIRS include
    REQUIRES wisp_base wisp_os wisp_hal wisp_core wisp_net
)
