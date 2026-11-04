# lua: the official Lua 5.5.1 sources, vendored verbatim by
# tools/vendor_lua.sh (scene-runtime-core spec 2.1). Never edit anything
# under lua-5.5.1/; build-time adjustments belong here.
#
# Only the core VM, the auxiliary library and the base, coroutine, math,
# string, table and utf8 libraries are compiled. io, os, package (dynamic
# loading) and debug are left out on purpose: scripts reach files, time
# and the network only through wisp_scene's own bindings. lua.c/luac.c
# are the standalone interpreter and compiler; linit.c would open every
# standard library at once, which wisp_scene does selectively instead.
#
# Built as C, so Lua raises errors with longjmp (see wisp_scene's
# bindings for what that means on the C++ side).
#
# Self-contained include -- see cmake/wisp_component.cmake's header
# comment.
include(${CMAKE_CURRENT_LIST_DIR}/../../cmake/wisp_component.cmake)

set(WISP_LUA_DIR lua-5.5.1/src)
set(WISP_LUA_SRCS)
foreach(
    _wisp_lua_src
    lapi lcode lctype ldebug ldo ldump lfunc lgc llex lmem lobject lopcodes lparser lstate
    lstring ltable ltm lundump lvm lzio lauxlib lbaselib lcorolib lmathlib lstrlib ltablib
    lutf8lib
)
    list(APPEND WISP_LUA_SRCS ${WISP_LUA_DIR}/${_wisp_lua_src}.c)
endforeach()

wisp_component(
    NAME lua
    SRCS ${WISP_LUA_SRCS}
    INCLUDE_DIRS ${WISP_LUA_DIR}
)

if(ESP_PLATFORM)
    set(_wisp_lua_target ${COMPONENT_LIB})
else()
    set(_wisp_lua_target lua)
endif()

# Caps the nesting of C calls and of the parser (default 200), so deep
# recursion through C (pcall, string.gsub callbacks, metamethods,
# coroutines, nested expressions) is a Lua error ("C stack overflow")
# instead of a stack overflow. Each level costs C stack; the deepest per
# level is string.gsub, about 1.3 KB of Xtensa frames. 48 pairs with the
# scene task's 96 KB stack; see wisp/scene/vm.hpp before changing either.
target_compile_definitions(${_wisp_lua_target} PRIVATE LUAI_MAXCCALLS=48)

# luaconf.h picks 64-bit lua_Integer only if <limits.h> defines LLONG_MAX
# and #errors otherwise. The ESP-IDF toolchain's <limits.h> defines it
# for C (keyed on __STDC_VERSION__) but not for C++, so every C++ file
# that includes lua.hpp would fail on the device while compiling fine on
# the host. libstdc++'s <climits> supplies the missing C99 limits, so it
# is force-included ahead of each C++ consumer's own code; the C sources
# above don't need it. SHELL: keeps "-include climits" together (CMake
# would otherwise de-duplicate a lone "-include" against another one).
target_compile_options(
    ${_wisp_lua_target} PUBLIC "$<$<COMPILE_LANGUAGE:CXX>:SHELL:-include climits>"
)
