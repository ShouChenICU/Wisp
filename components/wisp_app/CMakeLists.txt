# wisp_app: boot orchestration (wisp::app::Board/Platform/App) over every
# wisp_core service, plus the self-check UI hook (see
# wisp/app/app.hpp, src/self_check.hpp).
#
# Self-contained include -- see cmake/wisp_component.cmake's header
# comment.
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/wisp_component.cmake)

wisp_component(
    NAME wisp_app
    SRCS src/app.cpp src/self_check_screen.cpp src/loopback.cpp src/power_key_controller.cpp
         src/power_manager.cpp src/format.cpp src/net_wiring.cpp src/net_section.cpp
         src/net_cards.cpp src/factory_reset.cpp src/console.cpp
    INCLUDE_DIRS include
    REQUIRES wisp_base wisp_os wisp_hal wisp_core wisp_net wisp_admin wisp_ui
)
