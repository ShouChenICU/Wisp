cmake_minimum_required(VERSION 3.24)
project(wisp_host CXX C)

set(WISP_ROOT ${CMAKE_CURRENT_LIST_DIR}/..)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(${WISP_ROOT}/cmake/wisp_component.cmake)
include(${WISP_ROOT}/cmake/wisp_host_deps.cmake)

# Needed by wisp_sim's own render loop (simulator/main.cpp) -- see that
# file's header and AGENTS.md's layering rule: only wisp_port_sim and
# simulator/ may include SDL*.
find_package(SDL2 REQUIRED)

# Needed to pack web/ (tools/pack_assets.py, below) and by
# tests/CMakeLists.txt to run the sim_smoke ctest case
# (tests/smoke/sim_smoke.py) via ${Python3_EXECUTABLE}.
find_package(Python3 COMPONENTS Interpreter REQUIRED)

add_subdirectory(${WISP_ROOT}/components/wisp_base wisp_base)
add_subdirectory(${WISP_ROOT}/components/wisp_os wisp_os)
add_subdirectory(${WISP_ROOT}/components/wisp_hal wisp_hal)
add_subdirectory(${WISP_ROOT}/components/wisp_core wisp_core)
add_subdirectory(${WISP_ROOT}/components/wisp_net wisp_net)
add_subdirectory(${WISP_ROOT}/components/wisp_admin wisp_admin)
add_subdirectory(${WISP_ROOT}/components/wisp_ui wisp_ui)
# Vendored Lua (third_party/lua), used only by wisp_scene.
add_subdirectory(${WISP_ROOT}/third_party/lua lua)
add_subdirectory(${WISP_ROOT}/components/wisp_scene wisp_scene)
add_subdirectory(${WISP_ROOT}/components/wisp_agent wisp_agent)
add_subdirectory(${WISP_ROOT}/components/wisp_app wisp_app)
add_subdirectory(${WISP_ROOT}/components/wisp_port_sim wisp_port_sim)

# lv_conf.h selects LV_STDLIB_CUSTOM, so `lvgl` calls lv_malloc_core() and
# friends, which wisp_ui's src/lv_mem_wisp.cpp provides on os::alloc().
# Compiled into `lvgl` itself here, not into wisp_ui: both are static
# libraries and lvgl -> wisp_ui -> lvgl would be a cycle. That puts the
# counter behind wisp::ui::lv_heap_bytes() in `lvgl` too, which every
# wisp_ui user links anyway. The ESP-IDF build keeps the file in wisp_ui
# (see components/wisp_ui/CMakeLists.txt).
target_sources(lvgl PRIVATE ${WISP_ROOT}/components/wisp_ui/src/lv_mem_wisp.cpp)
target_include_directories(lvgl PRIVATE ${WISP_ROOT}/components/wisp_ui/include)
set_source_files_properties(
    ${WISP_ROOT}/components/wisp_ui/src/lv_mem_wisp.cpp
    TARGET_DIRECTORY lvgl
    PROPERTIES COMPILE_OPTIONS "-fno-exceptions;-fno-rtti"
)
target_link_libraries(lvgl PUBLIC wisp_os wisp_port_sim_os)

# The simulator executable itself: parses SimOptions
# (simulator/sim_options.{hpp,cpp}) and runs the SDL main loop
# (simulator/main.cpp) over a SimBoard from wisp_port_sim_dev.
add_executable(wisp_sim main.cpp sim_options.cpp sim_stdin.cpp)
target_compile_features(wisp_sim PRIVATE cxx_std_20)
# Simulator-only console commands ("debug pair-code"; see main.cpp's
# header). Defined on this target alone, so no portable component and no
# firmware build ever compiles them in.
target_compile_definitions(wisp_sim PRIVATE WISP_SIM_DEBUG_COMMANDS)
# The same version string the firmware build stamps into its image.
include(${WISP_ROOT}/cmake/wisp_version.cmake)
wisp_version(WISP_SIM_VERSION)
target_compile_definitions(wisp_sim PRIVATE WISP_VERSION="${WISP_SIM_VERSION}")
target_link_libraries(wisp_sim PRIVATE
    wisp_app wisp_core wisp_hal wisp_os wisp_base wisp_ui
    wisp_port_sim_os wisp_port_sim_settings wisp_port_sim_dev wisp_port_sim_net
    wisp_port_sim_ui
    SDL2::SDL2
)

# The admin web UI, packed by tools/pack_assets.py into assets.wpak next to
# wisp_sim, which loads it at startup (--web-dir overrides it). Until web/
# exists an empty directory is packed instead, so the simulator still
# serves the API. The CONFIGURE_DEPENDS glob notices web/ appearing and
# files changing. The pack also depends on web/ and every directory under
# it: deleting or renaming a file bumps only its directory's mtime, which
# is what makes the build re-pack instead of keeping a stale entry.
set(WISP_WEB_DIR ${WISP_ROOT}/web)
set(WISP_SIM_WPAK ${CMAKE_CURRENT_BINARY_DIR}/assets.wpak)
file(GLOB_RECURSE WISP_WEB_FILES CONFIGURE_DEPENDS ${WISP_WEB_DIR}/*)
file(GLOB_RECURSE WISP_WEB_ENTRIES LIST_DIRECTORIES true CONFIGURE_DEPENDS ${WISP_WEB_DIR}/*)
if(IS_DIRECTORY ${WISP_WEB_DIR})
    set(WISP_WEB_PACK_SRC ${WISP_WEB_DIR})
    list(APPEND WISP_WEB_FILES ${WISP_WEB_DIR})
    foreach(entry IN LISTS WISP_WEB_ENTRIES)
        if(IS_DIRECTORY ${entry})
            list(APPEND WISP_WEB_FILES ${entry})
        endif()
    endforeach()
else()
    set(WISP_WEB_PACK_SRC ${CMAKE_CURRENT_BINARY_DIR}/empty-web)
    file(MAKE_DIRECTORY ${WISP_WEB_PACK_SRC})
endif()
add_custom_command(
    OUTPUT ${WISP_SIM_WPAK}
    COMMAND ${Python3_EXECUTABLE} ${WISP_ROOT}/tools/pack_assets.py ${WISP_WEB_PACK_SRC}
            -o ${WISP_SIM_WPAK}
    DEPENDS ${WISP_ROOT}/tools/pack_assets.py ${WISP_WEB_FILES}
    COMMENT "Packing web/ into assets.wpak"
    VERBATIM
)
add_custom_target(wisp_sim_assets DEPENDS ${WISP_SIM_WPAK})
add_dependencies(wisp_sim wisp_sim_assets)

# The vector font image (vector-font spec 3.3), packed from the committed
# TTF exactly as the firmware packs it, next to wisp_sim. The simulator has
# no partition, so there is no --max-bytes; the reference board's limit is
# checked by tools/pack_font_image_test.py.
set(WISP_SIM_FONT_TTF ${WISP_ROOT}/components/wisp_ui/fonts/noto_sans_cjk_sc_subset.ttf)
set(WISP_SIM_FONTS ${CMAKE_CURRENT_BINARY_DIR}/fonts.bin)
add_custom_command(
    OUTPUT ${WISP_SIM_FONTS}
    COMMAND ${Python3_EXECUTABLE} ${WISP_ROOT}/tools/pack_font_image.py ${WISP_SIM_FONT_TTF}
            -o ${WISP_SIM_FONTS}
    DEPENDS ${WISP_SIM_FONT_TTF} ${WISP_ROOT}/tools/pack_font_image.py
    COMMENT "Packing the vector font into fonts.bin"
    VERBATIM
)
add_custom_target(wisp_sim_fonts DEPENDS ${WISP_SIM_FONTS})
add_dependencies(wisp_sim wisp_sim_fonts)

option(WISP_BUILD_TESTS "Build the PC unit test suite" ON)
if(WISP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(${WISP_ROOT}/tests tests)
endif()

# Later tasks append further add_subdirectory(${WISP_ROOT}/components/<name> <name>)
# calls here as new portable components are added.
