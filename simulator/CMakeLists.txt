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

# Needed by tests/CMakeLists.txt to run the sim_smoke ctest case
# (tests/smoke/sim_smoke.py) via ${Python3_EXECUTABLE}.
find_package(Python3 COMPONENTS Interpreter REQUIRED)

add_subdirectory(${WISP_ROOT}/components/wisp_base wisp_base)
add_subdirectory(${WISP_ROOT}/components/wisp_os wisp_os)
add_subdirectory(${WISP_ROOT}/components/wisp_hal wisp_hal)
add_subdirectory(${WISP_ROOT}/components/wisp_core wisp_core)
add_subdirectory(${WISP_ROOT}/components/wisp_ui wisp_ui)
add_subdirectory(${WISP_ROOT}/components/wisp_app wisp_app)
add_subdirectory(${WISP_ROOT}/components/wisp_port_sim wisp_port_sim)

# The simulator executable itself (Task 13): parses SimOptions
# (simulator/sim_options.{hpp,cpp}) and runs the SDL main loop
# (simulator/main.cpp) over a SimBoard from wisp_port_sim_dev.
add_executable(wisp_sim main.cpp sim_options.cpp)
target_compile_features(wisp_sim PRIVATE cxx_std_20)
target_link_libraries(wisp_sim PRIVATE
    wisp_app wisp_core wisp_hal wisp_os wisp_base wisp_ui
    wisp_port_sim_os wisp_port_sim_settings wisp_port_sim_dev
    SDL2::SDL2
)

option(WISP_BUILD_TESTS "Build the PC unit test suite" ON)
if(WISP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(${WISP_ROOT}/tests tests)
endif()

# Later tasks append further add_subdirectory(${WISP_ROOT}/components/<name> <name>)
# calls here as new portable components are added.
