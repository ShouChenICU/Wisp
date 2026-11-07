# wisp_shell: the system shell on the device (MVP spec 6): the
# ConversationController that joins the voice chain to the agent, and the
# talk key. It sits between wisp_agent and wisp_app (MVP spec 1.4,
# enforced by tools/check_layering.py).
#
# Self-contained include -- see cmake/wisp_component.cmake's header
# comment.
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/wisp_component.cmake)

wisp_component(
    NAME wisp_shell
    SRCS src/conversation.cpp src/talk_key.cpp
    INCLUDE_DIRS include
    REQUIRES wisp_base wisp_os wisp_hal wisp_core wisp_net wisp_voice wisp_admin wisp_ui wisp_scene
             wisp_agent
)
