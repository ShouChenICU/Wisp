# minimp3: minimp3.h and minimp3_ex.h from lieff/minimp3 at commit
# 1428237, vendored verbatim by tools/vendor_minimp3.sh into
# minimp3-1428237/. Never edit anything under minimp3-1428237/;
# build-time adjustments belong here or in minimp3.c (Wisp's own wrapper
# that compiles the implementation once, as C).
#
# Host only: the simulator's MP3 decoder (wisp_port_sim) is the only user.
# The firmware decodes through esp_audio_codec, so firmware/CMakeLists.txt
# keeps this component out of the build (EXCLUDE_COMPONENTS), although
# EXTRA_COMPONENT_DIRS lists third_party/. Only wisp_port_sim and tests
# may include the header or require the component
# (tools/check_layering.py).
#
# Self-contained include -- see cmake/wisp_component.cmake's header
# comment.
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/wisp_component.cmake)

wisp_component(
    NAME minimp3
    SRCS minimp3.c
    INCLUDE_DIRS minimp3-1428237
)

if(ESP_PLATFORM)
    set(_wisp_minimp3_target ${COMPONENT_LIB})
else()
    set(_wisp_minimp3_target minimp3)
endif()

# Decoding goes through minimp3_ex.h's callback API (mp3dec_io_t): the
# file-opening half (fopen/mmap) would let a task other than wisp.sd.io
# touch the card (AGENTS.md section 5). PUBLIC, so every includer sees the
# same declarations as the compiled implementation.
target_compile_definitions(${_wisp_minimp3_target} PUBLIC MINIMP3_NO_STDIO)
