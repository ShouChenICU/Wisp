# wisp_voice: the voice chain (MVP spec 4). It sits between wisp_net and
# wisp_admin (MVP spec 1.4, enforced by tools/check_layering.py), so the
# admin pages can test ASR and TTS; the conversation shell above
# wisp_agent wires it to the agent.
#
# Self-contained include -- see cmake/wisp_component.cmake's header
# comment.
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/wisp_component.cmake)

wisp_component(
    NAME wisp_voice
    SRCS src/say_parser.cpp src/vad.cpp src/sentences.cpp
    INCLUDE_DIRS include
    REQUIRES wisp_base wisp_os wisp_hal wisp_core wisp_net
)
