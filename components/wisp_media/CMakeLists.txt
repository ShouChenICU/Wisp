# wisp_media: the media library and music player. It sits between
# wisp_voice and wisp_admin (media spec 3, enforced by
# tools/check_layering.py), so the admin routes can serve the library; it
# needs only the foundation layers.
#
# Self-contained include -- see cmake/wisp_component.cmake's header
# comment.
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/wisp_component.cmake)

wisp_component(
    NAME wisp_media
    SRCS src/path.cpp src/errors.cpp src/id3.cpp src/mp3_info.cpp src/wav.cpp src/decoder.cpp
         src/wav_decoder.cpp src/index_store.cpp src/library.cpp src/play_queue.cpp
    INCLUDE_DIRS include
    REQUIRES wisp_base wisp_os wisp_hal wisp_core
)
