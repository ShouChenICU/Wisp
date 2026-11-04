# wisp_scene: the Lua scene runtime (scene-runtime-core spec), sitting
# between wisp_ui and wisp_app. The only layer allowed to see Lua's
# headers or require the vendored `lua` component (third_party/lua;
# enforced by tools/check_layering.py).
#
# Self-contained include -- see cmake/wisp_component.cmake's header
# comment.
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/wisp_component.cmake)

wisp_component(
    NAME wisp_scene
    SRCS src/version.cpp src/vm.cpp src/patch_batch.cpp src/prelude.cpp
         src/reactive.cpp
    INCLUDE_DIRS include
    REQUIRES wisp_base wisp_os wisp_hal wisp_core wisp_net wisp_ui lua
)
