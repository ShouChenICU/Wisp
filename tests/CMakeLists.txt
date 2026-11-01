include(GoogleTest)

# wisp_test(NAME <name> SRCS <src files...> LIBS <libs to link...> [TIMEOUT <seconds>])
#
# Declares a GoogleTest executable, links it against GTest::gtest_main plus
# LIBS, and registers its cases with ctest via gtest_discover_tests(). If
# TIMEOUT is given, every discovered test case gets that ctest TIMEOUT
# property, so a regression that deadlocks one case fails it (and lets the
# rest of the suite proceed) instead of hanging the whole ctest run.
function(wisp_test)
    cmake_parse_arguments(WISP_TEST "" "NAME;TIMEOUT" "SRCS;LIBS" ${ARGN})

    if(NOT WISP_TEST_NAME)
        message(FATAL_ERROR "wisp_test() requires NAME")
    endif()

    add_executable(${WISP_TEST_NAME} ${WISP_TEST_SRCS})
    target_link_libraries(${WISP_TEST_NAME} PRIVATE GTest::gtest_main ${WISP_TEST_LIBS})
    target_compile_features(${WISP_TEST_NAME} PRIVATE cxx_std_20)
    # Lets any test #include "support/fake_devices.hpp" (and similar shared
    # test-only headers) regardless of which subdirectory it lives in.
    target_include_directories(${WISP_TEST_NAME} PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
    if(WISP_TEST_TIMEOUT)
        gtest_discover_tests(${WISP_TEST_NAME} PROPERTIES TIMEOUT ${WISP_TEST_TIMEOUT})
    else()
        gtest_discover_tests(${WISP_TEST_NAME})
    endif()
endfunction()

# Later tasks append further wisp_test(...) calls here as new test binaries
# are added.
wisp_test(NAME result_test SRCS base/result_test.cpp LIBS wisp_base)
wisp_test(NAME display_geometry_test SRCS hal/display_geometry_test.cpp LIBS wisp_hal)
wisp_test(NAME value_json_test SRCS base/value_json_test.cpp LIBS wisp_base)
wisp_test(NAME os_test SRCS os/os_test.cpp LIBS wisp_os wisp_port_sim_os TIMEOUT 10)
wisp_test(NAME log_test SRCS core/log_test.cpp LIBS wisp_core wisp_port_sim_os)
wisp_test(NAME event_bus_test SRCS core/event_bus_test.cpp LIBS wisp_core wisp_port_sim_os TIMEOUT 20)
wisp_test(NAME device_registry_test SRCS core/device_registry_test.cpp LIBS wisp_core wisp_port_sim_os)
wisp_test(NAME button_service_test SRCS core/button_service_test.cpp LIBS wisp_core wisp_port_sim_os TIMEOUT 20)
wisp_test(NAME settings_test SRCS core/settings_test.cpp LIBS wisp_core wisp_port_sim_os wisp_port_sim_settings)
wisp_test(NAME storage_manager_test SRCS core/storage_manager_test.cpp LIBS wisp_core wisp_port_sim_os TIMEOUT 20)
wisp_test(NAME align_test SRCS ui/align_test.cpp LIBS wisp_ui)
wisp_test(NAME ui_runtime_test SRCS ui/ui_runtime_test.cpp LIBS wisp_ui wisp_port_sim_os TIMEOUT 30)
wisp_test(NAME app_boot_test SRCS app/app_boot_test.cpp
          LIBS wisp_app wisp_port_sim_os wisp_port_sim_settings TIMEOUT 30)
wisp_test(NAME power_key_controller_test SRCS app/power_key_controller_test.cpp LIBS wisp_app)
wisp_test(NAME format_test SRCS app/format_test.cpp LIBS wisp_app wisp_core)
wisp_test(NAME self_check_screen_test SRCS app/self_check_screen_test.cpp
          LIBS wisp_app wisp_port_sim_os wisp_port_sim_settings TIMEOUT 30)
wisp_test(NAME png_writer_test SRCS sim/png_writer_test.cpp LIBS wisp_port_sim_dev)
wisp_test(NAME sim_options_test SRCS sim/sim_options_test.cpp ${WISP_ROOT}/simulator/sim_options.cpp
          LIBS wisp_base)
target_include_directories(sim_options_test PRIVATE ${WISP_ROOT}/simulator)
wisp_test(NAME sim_audio_test SRCS sim/sim_audio_test.cpp
          LIBS wisp_port_sim_dev wisp_os wisp_port_sim_os TIMEOUT 20)
target_include_directories(sim_audio_test PRIVATE ${WISP_ROOT}/components/wisp_port_sim)
wisp_test(NAME sim_board_test SRCS sim/sim_board_test.cpp
          LIBS wisp_port_sim_dev wisp_core wisp_os wisp_port_sim_os TIMEOUT 20)

# End-to-end smoke test (Task 14): runs the actual wisp_sim executable
# through a handful of --headless scenarios and checks its
# WISP_BOOT_REPORT/WISP_MANIFEST stdout and screenshot. See
# tests/smoke/sim_smoke.py's file header.
add_test(NAME sim_smoke COMMAND ${Python3_EXECUTABLE} ${WISP_ROOT}/tests/smoke/sim_smoke.py
         $<TARGET_FILE:wisp_sim>)
