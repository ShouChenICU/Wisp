cmake_minimum_required(VERSION 3.24)
project(wisp_host CXX C)

set(WISP_ROOT ${CMAKE_CURRENT_LIST_DIR}/..)
set(CMAKE_EXPORT_COMPILE_COMMANDS ON)
set(CMAKE_CXX_STANDARD 20)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

include(${WISP_ROOT}/cmake/wisp_component.cmake)
include(${WISP_ROOT}/cmake/wisp_host_deps.cmake)

add_subdirectory(${WISP_ROOT}/components/wisp_base wisp_base)
add_subdirectory(${WISP_ROOT}/components/wisp_os wisp_os)
add_subdirectory(${WISP_ROOT}/components/wisp_core wisp_core)
add_subdirectory(${WISP_ROOT}/components/wisp_port_sim wisp_port_sim)

option(WISP_BUILD_TESTS "Build the PC unit test suite" ON)
if(WISP_BUILD_TESTS)
    enable_testing()
    add_subdirectory(${WISP_ROOT}/tests tests)
endif()

# Later tasks append further add_subdirectory(${WISP_ROOT}/components/<name> <name>)
# calls here as new portable components are added.
