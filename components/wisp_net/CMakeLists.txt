# Self-contained include -- see cmake/wisp_component.cmake's header
# comment.
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/wisp_component.cmake)

wisp_component(
    NAME wisp_net
    SRCS src/http.cpp src/asset_pack.cpp src/wifi_service.cpp
         src/pairing_service.cpp src/llm_probe.cpp
    INCLUDE_DIRS include
    REQUIRES wisp_base wisp_os wisp_hal wisp_core
)
