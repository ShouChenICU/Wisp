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
         src/reactive.cpp src/schema.cpp src/node.cpp src/constructors.cpp
         src/collect.cpp src/patch_queue.cpp src/scene_view.cpp src/theme.cpp
         src/widgets.cpp src/scene_view_list.cpp src/scene_view_canvas.cpp
         src/scene_view_media.cpp src/scene_message.cpp src/scene_instance.cpp
         src/scene_runtime.cpp src/timer_set.cpp src/minute_ticker.cpp
         src/bind_timer.cpp src/bind_device.cpp src/bind_log.cpp src/bind_scene.cpp
         src/bind_store.cpp src/bind_http.cpp src/bind_power.cpp src/lua_json.cpp
         src/dir_watcher.cpp
    INCLUDE_DIRS include
    REQUIRES wisp_base wisp_os wisp_hal wisp_core wisp_net wisp_ui lua
)
