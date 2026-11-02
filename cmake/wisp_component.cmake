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
#
# Must be a macro(), not a function(): ESP-IDF's build extracts each component's
# REQUIRES/PRIV_REQUIRES by running a throwaway `cmake -P` *subprocess*
# (tools/cmake/scripts/component_get_requirements.cmake) that defines only
# a handful of stub macros (idf_component_register among them) and then
# include()s every component's CMakeLists.txt directly into one of its
# own function's scope. Two consequences:
#   1. wisp_component() itself must be defined inside that very
#      subprocess too -- it has no knowledge of anything the *including*
#      project (firmware/CMakeLists.txt) did -- so every component's
#      CMakeLists.txt include()s this file itself (see each
#      components/wisp_*/CMakeLists.txt's first line) rather than relying
#      on some top-level CMakeLists.txt having done it first (which is
#      all the host build needs, since simulator/CMakeLists.txt includes
#      this file once before any add_subdirectory()).
#   2. The early pass's idf_component_register() stub communicates the
#      REQUIRES it parsed back to its caller via plain set() (no
#      PARENT_SCOPE) followed by a return() -- it assumes it is being
#      invoked directly at the top level of the included CMakeLists.txt,
#      i.e. directly inside that caller's own function scope. If
#      wisp_component() were a function(), invoking idf_component_register()
#      from inside it would trap those set() results in wisp_component()'s
#      own scope instead (silently losing every component's REQUIRES list
#      -- a much worse failure than the first one, since it doesn't error,
#      it just silently drops dependency information) and the return()
#      would unwind only out of wisp_component(), not out of the
#      including caller. A macro() has no scope of its own: invoking it
#      is equivalent to pasting its body in place, so idf_component_register()
#      (itself a macro, in both the early-pass stub and the real one in
#      tools/cmake/component.cmake) ends up running in exactly the scope
#      it expects either way -- the real one's target_compile_options()/
#      target_compile_features() calls after it see the same COMPONENT_LIB
#      it just defined via PARENT_SCOPE, one level up from wherever
#      wisp_component() itself was invoked.
#
# Being a macro() (not a function()) cuts both ways -- macro()s have
# no scope of their own, so every variable this body sets
# (the cmake_parse_arguments() outputs: WISP_COMPONENT_NAME,
# WISP_COMPONENT_SRCS, WISP_COMPONENT_INCLUDE_DIRS, WISP_COMPONENT_REQUIRES,
# WISP_COMPONENT_IDF_REQUIRES, WISP_COMPONENT_HOST_LIBS, plus the local
# _wisp_component_scope/_wisp_component_iface_scope) leaks into whichever
# scope *called* wisp_component() -- typically a component's own
# CMakeLists.txt, i.e. effectively global for that file. Callers must
# not read or rely on any of these after calling wisp_component(); they
# are this macro's own working variables, not part of its interface, and
# a second wisp_component() call (nothing currently does this, but
# nothing stops it either) would silently overwrite them with its own
# arguments.
include_guard()

macro(wisp_component)
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
        # idf_component_register() itself mirrors the same STATIC-vs-
        # INTERFACE split as the host branch below (see
        # tools/cmake/component.cmake's __component_add_target(): SRCS
        # empty -> COMPONENT_LIB is an INTERFACE library, which only
        # accepts the INTERFACE usage-requirement scope, not PRIVATE/PUBLIC.
        if(WISP_COMPONENT_SRCS)
            set(_wisp_component_scope PRIVATE)
            set(_wisp_component_iface_scope PUBLIC)
        else()
            set(_wisp_component_scope INTERFACE)
            set(_wisp_component_iface_scope INTERFACE)
        endif()
        # -fno-exceptions/-fno-rtti are C++-only flags: GCC hard-errors
        # under -Werror (part of the ESP-IDF toolchain's default flags) if
        # they reach a .c file, which any component mixing C and C++ SRCS
        # (e.g. wisp_ui's lv_font_cjk_16.c) does. Gate them to CXX via a
        # generator expression rather than relying on language-specific
        # scope the way the plain PRIVATE/PUBLIC/INTERFACE keywords above
        # do.
        target_compile_options(
            ${COMPONENT_LIB} ${_wisp_component_scope}
            "$<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions;-fno-rtti>"
        )
        target_compile_features(${COMPONENT_LIB} ${_wisp_component_iface_scope} cxx_std_20)
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
        # Same C-vs-C++ concern as the ESP_PLATFORM branch above (gcc/clang
        # reject -fno-exceptions/-fno-rtti on C sources); gate to CXX so a
        # component mixing C and C++ SRCS on the host build is safe too.
        target_compile_options(
            ${WISP_COMPONENT_NAME} ${_wisp_component_scope}
            "$<$<COMPILE_LANGUAGE:CXX>:-fno-exceptions;-fno-rtti>"
        )
    endif()
endmacro()
