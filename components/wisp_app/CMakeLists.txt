# wisp_app: boot orchestration (wisp::app::Board/Platform/App) over every
# wisp_core service and the scene runtime, plus the self-check UI hook (see
# wisp/app/app.hpp, src/self_check.hpp).
#
# Self-contained include -- see cmake/wisp_component.cmake's header
# comment.
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/wisp_component.cmake)

wisp_component(
    NAME wisp_app
    SRCS src/app.cpp src/self_check_screen.cpp src/self_check_model.cpp
         src/self_check_layout.cpp src/self_check_overlays.cpp src/self_check_header.cpp
         src/self_check_devices.cpp src/self_check_sensors.cpp src/self_check_readouts.cpp
         src/self_check_tests.cpp src/self_check_sampler.cpp src/loopback.cpp src/power_key_controller.cpp
         src/power_manager.cpp src/format.cpp src/net_wiring.cpp src/net_section.cpp
         src/net_cards.cpp src/factory_reset.cpp src/firmware_confirm.cpp src/console.cpp
         src/ram_budget.cpp src/scene_wiring.cpp src/scene_section.cpp src/bench.cpp
    INCLUDE_DIRS include
    REQUIRES wisp_base wisp_os wisp_hal wisp_core wisp_net wisp_admin wisp_ui wisp_scene
)
