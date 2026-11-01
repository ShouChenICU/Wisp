include(GoogleTest)

# wisp_test(NAME <name> SRCS <src files...> LIBS <libs to link...>)
#
# Declares a GoogleTest executable, links it against GTest::gtest_main plus
# LIBS, and registers its cases with ctest via gtest_discover_tests().
function(wisp_test)
    cmake_parse_arguments(WISP_TEST "" "NAME" "SRCS;LIBS" ${ARGN})

    if(NOT WISP_TEST_NAME)
        message(FATAL_ERROR "wisp_test() requires NAME")
    endif()

    add_executable(${WISP_TEST_NAME} ${WISP_TEST_SRCS})
    target_link_libraries(${WISP_TEST_NAME} PRIVATE GTest::gtest_main ${WISP_TEST_LIBS})
    target_compile_features(${WISP_TEST_NAME} PRIVATE cxx_std_20)
    gtest_discover_tests(${WISP_TEST_NAME})
endfunction()

# Later tasks append further wisp_test(...) calls here as new test binaries
# are added.
wisp_test(NAME result_test SRCS base/result_test.cpp LIBS wisp_base)
wisp_test(NAME value_json_test SRCS base/value_json_test.cpp LIBS wisp_base)
wisp_test(NAME os_test SRCS os/os_test.cpp LIBS wisp_os wisp_port_sim_os)
