# wisp_agent: the on-device agent (agent-core spec 2): the persistent
# conversation, the tool registry and, on top of them, the agent loop.
# It sits between wisp_scene and wisp_app (enforced by
# tools/check_layering.py).
#
# Self-contained include -- see cmake/wisp_component.cmake's header
# comment.
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/wisp_component.cmake)

wisp_component(
    NAME wisp_agent
    SRCS src/conversation.cpp src/tool_registry.cpp src/args_schema.cpp src/agent_service.cpp
    INCLUDE_DIRS include
    REQUIRES wisp_base wisp_os wisp_hal wisp_core wisp_net wisp_admin wisp_ui wisp_scene
)
