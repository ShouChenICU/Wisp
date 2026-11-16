# stb_image_write: stb_image_write.h v1.16 from nothings/stb at commit
# aeb6907, vendored verbatim by tools/vendor_stb.sh into stb-aeb6907/.
# Never edit anything under stb-aeb6907/; build-time adjustments belong
# here or in stb_image_write.c (Wisp's own wrapper that compiles the
# implementation once, as C).
#
# Only wisp_base may require this component or include its header
# (tools/check_layering.py); everything else encodes through
# wisp::encode_jpeg() (spec 3.6).
#
# Self-contained include -- see cmake/wisp_component.cmake's header
# comment.
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/wisp_component.cmake)

wisp_component(
    NAME stb_image_write
    SRCS stb_image_write.c
    INCLUDE_DIRS stb-aeb6907
)

if(ESP_PLATFORM)
    set(_wisp_stb_target ${COMPONENT_LIB})
else()
    set(_wisp_stb_target stb_image_write)
endif()

# The file-writing half (stbi_write_jpg() and friends) needs stdio and a
# file system the device's tasks may not touch (AGENTS.md section 5,
# flash rule); only the *_to_func() writers are used. PUBLIC, so every
# includer sees the same declarations as the compiled implementation.
target_compile_definitions(${_wisp_stb_target} PUBLIC STBI_WRITE_NO_STDIO)
