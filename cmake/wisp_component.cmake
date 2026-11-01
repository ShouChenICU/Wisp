# wisp_component(NAME <name>
#                SRCS <src files...>
#                INCLUDE_DIRS <include dirs...>
#                [REQUIRES <wisp components...>]
#                [IDF_REQUIRES <idf components...>]
#                [HOST_LIBS <host-only libs...>])
#
# Declares a portable Wisp component so the same CMakeLists.txt works both
# as an ESP-IDF component (idf.py build) and as a plain CMake target in the
# host simulator build.
#
#   - Under ESP-IDF (ESP_PLATFORM defined): registers the component via
#     idf_component_register(), using REQUIRES + IDF_REQUIRES as the IDF
#     component dependency list. HOST_LIBS is ignored (host-only deps make
#     no sense on the firmware side).
#   - On the host (ESP_PLATFORM not defined): creates a CMake library target
#     named NAME. If SRCS is non-empty it is a STATIC library; if SRCS is
#     empty it is an INTERFACE (header-only) library. REQUIRES (other wisp_*
#     components) and HOST_LIBS (e.g. GTest, ArduinoJson) are linked
#     PUBLIC/INTERFACE as appropriate. The target always gets C++20 and
#     -fno-exceptions -fno-rtti.
function(wisp_component)
    cmake_parse_arguments(
        WISP_COMPONENT
        ""
        "NAME"
        "SRCS;INCLUDE_DIRS;REQUIRES;IDF_REQUIRES;HOST_LIBS"
        ${ARGN}
    )

    if(NOT WISP_COMPONENT_NAME)
        message(FATAL_ERROR "wisp_component() requires NAME")
    endif()

    if(ESP_PLATFORM)
        idf_component_register(
            SRCS ${WISP_COMPONENT_SRCS}
            INCLUDE_DIRS ${WISP_COMPONENT_INCLUDE_DIRS}
            REQUIRES ${WISP_COMPONENT_REQUIRES} ${WISP_COMPONENT_IDF_REQUIRES}
        )
        target_compile_options(${COMPONENT_LIB} PRIVATE -fno-exceptions -fno-rtti)
        target_compile_features(${COMPONENT_LIB} PUBLIC cxx_std_20)
    else()
        if(WISP_COMPONENT_SRCS)
            add_library(${WISP_COMPONENT_NAME} STATIC ${WISP_COMPONENT_SRCS})
            set(_wisp_component_scope PUBLIC)
        else()
            add_library(${WISP_COMPONENT_NAME} INTERFACE)
            set(_wisp_component_scope INTERFACE)
        endif()

        target_include_directories(
            ${WISP_COMPONENT_NAME} ${_wisp_component_scope} ${WISP_COMPONENT_INCLUDE_DIRS}
        )
        target_link_libraries(
            ${WISP_COMPONENT_NAME} ${_wisp_component_scope}
            ${WISP_COMPONENT_REQUIRES}
            ${WISP_COMPONENT_HOST_LIBS}
        )
        target_compile_features(${WISP_COMPONENT_NAME} ${_wisp_component_scope} cxx_std_20)
        target_compile_options(
            ${WISP_COMPONENT_NAME} ${_wisp_component_scope} -fno-exceptions -fno-rtti
        )
    endif()
endfunction()
