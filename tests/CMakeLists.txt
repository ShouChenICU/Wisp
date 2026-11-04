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
wisp_test(NAME draw_buf_policy_test SRCS hal/draw_buf_policy_test.cpp LIBS wisp_hal)
wisp_test(NAME wifi_types_test SRCS hal/wifi_types_test.cpp LIBS wisp_hal)
wisp_test(NAME value_json_test SRCS base/value_json_test.cpp LIBS wisp_base)
wisp_test(NAME value_test SRCS base/value_test.cpp LIBS wisp_base)
wisp_test(NAME sha256_test SRCS base/sha256_test.cpp LIBS wisp_base)
wisp_test(NAME base64_test SRCS base/base64_test.cpp LIBS wisp_base)
wisp_test(NAME crc32_test SRCS base/crc32_test.cpp LIBS wisp_base)
# zlib only produces the reference streams; wisp_base itself never links it.
wisp_test(NAME inflate_test SRCS base/inflate_test.cpp LIBS wisp_base ZLIB::ZLIB)
wisp_test(NAME os_test SRCS os/os_test.cpp LIBS wisp_os wisp_port_sim_os TIMEOUT 10)
wisp_test(NAME large_test SRCS os/large_test.cpp LIBS wisp_os wisp_port_sim_os TIMEOUT 10)
wisp_test(NAME log_test SRCS core/log_test.cpp LIBS wisp_core wisp_port_sim_os)
wisp_test(NAME event_bus_test SRCS core/event_bus_test.cpp LIBS wisp_core wisp_port_sim_os TIMEOUT 20)
wisp_test(NAME device_registry_test SRCS core/device_registry_test.cpp LIBS wisp_core wisp_port_sim_os)
wisp_test(NAME button_service_test SRCS core/button_service_test.cpp LIBS wisp_core wisp_port_sim_os TIMEOUT 20)
wisp_test(NAME settings_test SRCS core/settings_test.cpp LIBS wisp_core wisp_port_sim_os wisp_port_sim_settings)
wisp_test(NAME model_profiles_test SRCS core/model_profiles_test.cpp LIBS wisp_core wisp_port_sim_os
          TIMEOUT 20)
wisp_test(NAME system_settings_test SRCS core/system_settings_test.cpp
          LIBS wisp_core wisp_port_sim_os wisp_port_sim_settings)
wisp_test(NAME firmware_image_test SRCS core/firmware_image_test.cpp LIBS wisp_core)
wisp_test(NAME firmware_update_test SRCS core/firmware_update_test.cpp
          LIBS wisp_core wisp_port_sim_os)
wisp_test(NAME storage_manager_test SRCS core/storage_manager_test.cpp LIBS wisp_core wisp_port_sim_os TIMEOUT 20)
wisp_test(NAME storage_worker_test SRCS core/storage_worker_test.cpp LIBS wisp_core wisp_port_sim_os TIMEOUT 20)
# zip_test reads archives that tests/core/fixtures/make_zips.py writes at
# build time (stdlib zipfile for the valid ones, hand-laid records for the
# hostile ones), the same way fixture.wpak is produced below.
set(WISP_FIXTURE_ZIP_DIR ${CMAKE_BINARY_DIR}/zip_fixtures)
set(WISP_FIXTURE_ZIP_STAMP ${WISP_FIXTURE_ZIP_DIR}/ok.zip)
add_custom_command(
    OUTPUT ${WISP_FIXTURE_ZIP_STAMP}
    COMMAND ${Python3_EXECUTABLE} ${CMAKE_CURRENT_SOURCE_DIR}/core/fixtures/make_zips.py
            ${WISP_FIXTURE_ZIP_DIR}
    DEPENDS ${CMAKE_CURRENT_SOURCE_DIR}/core/fixtures/make_zips.py
    COMMENT "Generating the zip_test archives"
    VERBATIM
)
add_custom_target(wisp_fixture_zips DEPENDS ${WISP_FIXTURE_ZIP_STAMP})
wisp_test(NAME zip_test SRCS core/zip_test.cpp LIBS wisp_core wisp_port_sim_os TIMEOUT 60)
add_dependencies(zip_test wisp_fixture_zips)
target_compile_definitions(zip_test PRIVATE WISP_FIXTURE_ZIP_DIR="${WISP_FIXTURE_ZIP_DIR}")
wisp_test(NAME scene_manifest_test SRCS core/scene_manifest_test.cpp LIBS wisp_core wisp_port_sim_os)
wisp_test(NAME scene_repository_test SRCS core/scene_repository_test.cpp
          LIBS wisp_core wisp_port_sim_os TIMEOUT 30)
wisp_test(NAME scene_install_test SRCS core/scene_install_test.cpp
          LIBS wisp_core wisp_port_sim_os TIMEOUT 60)
wisp_test(NAME scene_store_test SRCS core/scene_store_test.cpp
          LIBS wisp_core wisp_port_sim_os TIMEOUT 60)
wisp_test(NAME power_monitor_test SRCS core/power_monitor_test.cpp
          LIBS wisp_core wisp_port_sim_os wisp_port_sim_settings TIMEOUT 20)
wisp_test(NAME align_test SRCS ui/align_test.cpp LIBS wisp_ui wisp_port_sim_os)
wisp_test(NAME lv_heap_test SRCS ui/lv_heap_test.cpp LIBS wisp_ui wisp_port_sim_os TIMEOUT 30)
wisp_test(NAME strings_test SRCS ui/strings_test.cpp LIBS wisp_ui wisp_port_sim_os)
wisp_test(NAME ui_runtime_test SRCS ui/ui_runtime_test.cpp LIBS wisp_ui wisp_port_sim_os TIMEOUT 30)
wisp_test(NAME overlay_test SRCS ui/overlay_test.cpp LIBS wisp_ui wisp_port_sim_os TIMEOUT 30)
wisp_test(NAME a8_font_test SRCS ui/a8_font_test.cpp LIBS wisp_ui wisp_port_sim_os TIMEOUT 60)
wisp_test(NAME system_layer_test SRCS ui/system_layer_test.cpp LIBS wisp_ui wisp_port_sim_os TIMEOUT 30)
target_include_directories(a8_font_test PRIVATE ${WISP_ROOT}/components/wisp_ui/src)
wisp_test(NAME app_boot_test SRCS app/app_boot_test.cpp
          LIBS wisp_app wisp_port_sim_os wisp_port_sim_settings TIMEOUT 30)
wisp_test(NAME power_key_controller_test SRCS app/power_key_controller_test.cpp LIBS wisp_app)
wisp_test(NAME power_manager_test SRCS app/power_manager_test.cpp
          LIBS wisp_app wisp_ui wisp_core wisp_hal wisp_port_sim_os wisp_port_sim_settings
          TIMEOUT 30)
wisp_test(NAME format_test SRCS app/format_test.cpp LIBS wisp_app wisp_core)
wisp_test(NAME ram_budget_test SRCS app/ram_budget_test.cpp LIBS wisp_app wisp_base)
wisp_test(NAME self_check_screen_test SRCS app/self_check_screen_test.cpp
          LIBS wisp_app wisp_port_sim_os wisp_port_sim_settings TIMEOUT 60)
target_include_directories(self_check_screen_test PRIVATE ${WISP_ROOT}/components/wisp_app/src)
wisp_test(NAME self_check_model_test SRCS app/self_check_model_test.cpp LIBS wisp_app)
target_include_directories(self_check_model_test PRIVATE ${WISP_ROOT}/components/wisp_app/src)
wisp_test(NAME app_net_test SRCS app/app_net_test.cpp
          LIBS wisp_app wisp_port_sim_os wisp_port_sim_settings TIMEOUT 30)
wisp_test(NAME console_test SRCS app/console_test.cpp
          LIBS wisp_app wisp_port_sim_os wisp_port_sim_settings TIMEOUT 30)
wisp_test(NAME loopback_test SRCS app/loopback_test.cpp
          LIBS wisp_app wisp_port_sim_os wisp_port_sim_settings TIMEOUT 30)
target_include_directories(loopback_test PRIVATE ${WISP_ROOT}/components/wisp_app/src)
foreach(app_ui_test net_ui_test factory_reset_test firmware_confirm_test)
    wisp_test(NAME ${app_ui_test} SRCS app/${app_ui_test}.cpp
              LIBS wisp_app wisp_port_sim_os wisp_port_sim_settings TIMEOUT 60)
    target_include_directories(${app_ui_test} PRIVATE ${WISP_ROOT}/components/wisp_app/src)
endforeach()
wisp_test(NAME png_writer_test SRCS sim/png_writer_test.cpp LIBS wisp_port_sim_dev)
wisp_test(NAME sim_options_test SRCS sim/sim_options_test.cpp ${WISP_ROOT}/simulator/sim_options.cpp
          LIBS wisp_base)
target_include_directories(sim_options_test PRIVATE ${WISP_ROOT}/simulator)
wisp_test(NAME sim_stdin_test SRCS sim/sim_stdin_test.cpp ${WISP_ROOT}/simulator/sim_stdin.cpp
          LIBS wisp_app TIMEOUT 20)
target_include_directories(sim_stdin_test PRIVATE ${WISP_ROOT}/simulator)
wisp_test(NAME sim_audio_test SRCS sim/sim_audio_test.cpp
          LIBS wisp_port_sim_dev wisp_os wisp_port_sim_os TIMEOUT 20)
target_include_directories(sim_audio_test PRIVATE ${WISP_ROOT}/components/wisp_port_sim)
wisp_test(NAME sim_board_test SRCS sim/sim_board_test.cpp
          LIBS wisp_port_sim_dev wisp_core wisp_os wisp_port_sim_os TIMEOUT 20)
# Real loopback sockets; net/sha1.hpp is private to wisp_port_sim.
wisp_test(NAME sim_http_server_test SRCS sim/sim_http_server_test.cpp
          LIBS wisp_port_sim_net TIMEOUT 30)
target_include_directories(sim_http_server_test PRIVATE ${WISP_ROOT}/components/wisp_port_sim)
wisp_test(NAME sim_wifi_test SRCS sim/sim_wifi_test.cpp LIBS wisp_port_sim_net)
wisp_test(NAME sim_firmware_test SRCS sim/sim_firmware_test.cpp
          LIBS wisp_port_sim_settings wisp_core wisp_port_sim_os)
# Talks to a real SimHttpServer over loopback; includes curl/curl.h for the
# error constants.
wisp_test(NAME sim_http_client_test SRCS sim/sim_http_client_test.cpp
          LIBS wisp_port_sim_net wisp_port_sim_os CURL::libcurl TIMEOUT 30)
wisp_test(NAME sim_web_pack_test SRCS sim/sim_web_pack_test.cpp
          LIBS wisp_port_sim_net wisp_net ZLIB::ZLIB)
target_compile_definitions(sim_web_pack_test PRIVATE
    WISP_FIXTURE_WEB_DIR="${CMAKE_CURRENT_SOURCE_DIR}/net/fixtures/web")

# Host-testable conversion helpers ported from MyOS into drivers/*/include/
# *_conv.h (see docs/references.md and each header's own comment). The
# drivers themselves are pure ESP-IDF components (driver/
# i2c_master.h) and only ever build under idf.py; these targets compile
# only the header-only, platform-free conversion math, so each needs its
# driver's include/ added explicitly (wisp_test() only adds tests/ itself).
wisp_test(NAME axp2101_conv_test SRCS drivers/axp2101_conv_test.cpp)
target_include_directories(axp2101_conv_test PRIVATE ${WISP_ROOT}/drivers/axp2101/include)
wisp_test(NAME pcf85063_conv_test SRCS drivers/pcf85063_conv_test.cpp)
target_include_directories(pcf85063_conv_test PRIVATE ${WISP_ROOT}/drivers/pcf85063/include)
wisp_test(NAME qmi8658_conv_test SRCS drivers/qmi8658_conv_test.cpp)
target_include_directories(qmi8658_conv_test PRIVATE ${WISP_ROOT}/drivers/qmi8658/include)

# Host-testable pure logic factored out of wisp_port_esp's ESP-only audio/
# SD-storage code: the stereo->mono downmix and the SD-card retry/backoff
# policy live in plain headers under
# components/wisp_port_esp/include/ with no ESP-IDF include, so (like the
# drivers/*_conv.h tests above) only that include dir is needed here, not
# the whole wisp_port_esp component (which never builds on the host).
# The masked-fill blend hook is plain C over LVGL's types; LVGL's own C
# path (the host build has no hooks) is the reference.
wisp_test(NAME lv_blend_mask_test SRCS port_esp/lv_blend_mask_test.cpp LIBS wisp_ui wisp_port_sim_os)
target_include_directories(lv_blend_mask_test PRIVATE ${WISP_ROOT}/components/wisp_port_esp)
wisp_test(NAME audio_downmix_test SRCS port_esp/audio_downmix_test.cpp)
target_include_directories(audio_downmix_test PRIVATE ${WISP_ROOT}/components/wisp_port_esp/include)
wisp_test(NAME jpeg_header_test SRCS port_esp/jpeg_header_test.cpp)
target_include_directories(jpeg_header_test PRIVATE ${WISP_ROOT}/components/wisp_port_esp/include)
wisp_test(NAME sd_retry_test SRCS port_esp/sd_retry_test.cpp)
target_include_directories(sd_retry_test PRIVATE ${WISP_ROOT}/components/wisp_port_esp/include)
# The networking port's pure pieces: Wi-Fi disconnect-reason mapping,
# request interface classification and the hotspot's DNS answers.
wisp_test(NAME wifi_reason_test SRCS port_esp/wifi_reason_test.cpp LIBS wisp_hal)
target_include_directories(wifi_reason_test PRIVATE ${WISP_ROOT}/components/wisp_port_esp/include)
wisp_test(NAME net_iface_test SRCS port_esp/net_iface_test.cpp LIBS wisp_net)
target_include_directories(net_iface_test PRIVATE ${WISP_ROOT}/components/wisp_port_esp/include)
wisp_test(NAME dns_reply_test SRCS port_esp/dns_reply_test.cpp)
target_include_directories(dns_reply_test PRIVATE ${WISP_ROOT}/components/wisp_port_esp/include)
# Where a task's stack goes, from its flash-access declaration.
wisp_test(NAME stack_placement_test SRCS port_esp/stack_placement_test.cpp LIBS wisp_os)
target_include_directories(stack_placement_test PRIVATE ${WISP_ROOT}/components/wisp_port_esp/include)

# End-to-end smoke test: runs the actual wisp_sim executable
# through a handful of --headless scenarios and checks its
# WISP_BOOT_REPORT/WISP_MANIFEST stdout and screenshot. See
# tests/smoke/sim_smoke.py's file header.
add_test(NAME sim_smoke COMMAND ${Python3_EXECUTABLE} ${WISP_ROOT}/tests/smoke/sim_smoke.py
         $<TARGET_FILE:wisp_sim>)

# End-to-end admin API smoke test: pairs over HTTP with the simulator-only
# "debug pair-code" console command, exercises the routes, the model probe
# (against tools/fake_openai.py) and the WebSocket log, then stops wisp_sim
# with SIGTERM. See tests/smoke/admin_smoke.py's file header.
add_test(NAME admin_smoke COMMAND ${Python3_EXECUTABLE} ${WISP_ROOT}/tests/smoke/admin_smoke.py
         $<TARGET_FILE:wisp_sim>)
set_tests_properties(admin_smoke PROPERTIES TIMEOUT 120)

# wisp_net. asset_pack_test reads a WispPack that tools/pack_assets.py
# builds from the committed tests/net/fixtures/web tree at build time, so
# the test always exercises the real packer's output, not a checked-in
# binary that could drift from it.
set(WISP_FIXTURE_WEB_DIR ${CMAKE_CURRENT_SOURCE_DIR}/net/fixtures/web)
set(WISP_FIXTURE_WPAK ${CMAKE_BINARY_DIR}/fixture.wpak)
file(GLOB_RECURSE WISP_FIXTURE_WEB_FILES CONFIGURE_DEPENDS ${WISP_FIXTURE_WEB_DIR}/*)
add_custom_command(
    OUTPUT ${WISP_FIXTURE_WPAK}
    COMMAND ${Python3_EXECUTABLE} ${WISP_ROOT}/tools/pack_assets.py ${WISP_FIXTURE_WEB_DIR}
            -o ${WISP_FIXTURE_WPAK}
    DEPENDS ${WISP_ROOT}/tools/pack_assets.py ${WISP_FIXTURE_WEB_FILES}
    COMMENT "Packing tests/net/fixtures/web into fixture.wpak"
    VERBATIM
)
add_custom_target(wisp_fixture_wpak DEPENDS ${WISP_FIXTURE_WPAK})
wisp_test(NAME http_test SRCS net/http_test.cpp LIBS wisp_net)
wisp_test(NAME asset_pack_test SRCS net/asset_pack_test.cpp LIBS wisp_net)
add_dependencies(asset_pack_test wisp_fixture_wpak)
target_compile_definitions(asset_pack_test PRIVATE
    WISP_FIXTURE_WPAK="${WISP_FIXTURE_WPAK}"
    WISP_FIXTURE_WEB_DIR="${WISP_FIXTURE_WEB_DIR}"
)
wisp_test(NAME wifi_service_test SRCS net/wifi_service_test.cpp
          LIBS wisp_net wisp_port_sim_os wisp_port_sim_settings TIMEOUT 30)
wisp_test(NAME pairing_service_test SRCS net/pairing_service_test.cpp
          LIBS wisp_net wisp_port_sim_os TIMEOUT 30)
wisp_test(NAME model_probe_test SRCS net/model_probe_test.cpp LIBS wisp_net wisp_core wisp_port_sim_os)
wisp_test(NAME scene_http_test SRCS net/scene_http_test.cpp LIBS wisp_net wisp_port_sim_os TIMEOUT 30)
wisp_test(NAME tls_gate_test SRCS net/tls_gate_test.cpp LIBS wisp_net wisp_port_sim_os TIMEOUT 30)
wisp_test(NAME request_watch_test SRCS net/request_watch_test.cpp LIBS wisp_net wisp_port_sim_os TIMEOUT 30)

# wisp_admin: the router is driven with in-memory requests
# (support/fake_http.hpp) over real services and the fixture WispPack.
foreach(admin_test filters_test pair_routes_test routes_test routes_ai_test routes_firmware_test
                   routes_scenes_test ws_hub_test)
    wisp_test(NAME ${admin_test} SRCS admin/${admin_test}.cpp
              LIBS wisp_admin wisp_port_sim_os wisp_port_sim_settings TIMEOUT 30)
    add_dependencies(${admin_test} wisp_fixture_wpak)
    target_compile_definitions(${admin_test} PRIVATE WISP_FIXTURE_WPAK="${WISP_FIXTURE_WPAK}")
endforeach()

# wisp_scene: the Lua scene runtime (scene-runtime-core spec).
wisp_test(NAME lua_vendor_test SRCS scene/lua_vendor_test.cpp LIBS wisp_scene)
wisp_test(NAME vm_test SRCS scene/vm_test.cpp LIBS wisp_scene wisp_port_sim_os TIMEOUT 60)
wisp_test(NAME patch_batch_test SRCS scene/patch_batch_test.cpp LIBS wisp_scene wisp_port_sim_os)
wisp_test(NAME reactive_test SRCS scene/reactive_test.cpp LIBS wisp_scene wisp_port_sim_os)
wisp_test(NAME schema_test SRCS scene/schema_test.cpp LIBS wisp_scene wisp_port_sim_os)
wisp_test(NAME patch_queue_test SRCS scene/patch_queue_test.cpp LIBS wisp_scene wisp_port_sim_os
          TIMEOUT 30)
wisp_test(NAME scene_view_test SRCS scene/scene_view_test.cpp LIBS wisp_scene wisp_port_sim_os
          TIMEOUT 30)
# list_window.hpp and widget_data.hpp are private to wisp_scene.
wisp_test(NAME list_window_test SRCS scene/list_window_test.cpp LIBS wisp_scene)
wisp_test(NAME scene_view_list_test SRCS scene/scene_view_list_test.cpp
          LIBS wisp_scene wisp_port_sim_os TIMEOUT 30)
wisp_test(NAME canvas_test SRCS scene/canvas_test.cpp LIBS wisp_scene wisp_port_sim_os TIMEOUT 30)
# The simulator's JPEG decoder comes from wisp_port_sim_ui, the platform
# side of the image-decoder hook (ui::UiConfig::install_image_decoders).
wisp_test(NAME scene_view_media_test SRCS scene/scene_view_media_test.cpp
          LIBS wisp_scene wisp_port_sim_os wisp_port_sim_ui TIMEOUT 30)
# scene_message.hpp is private to wisp_scene.
wisp_test(NAME scene_runtime_test SRCS scene/scene_runtime_test.cpp
          LIBS wisp_scene wisp_port_sim_os TIMEOUT 60)
target_include_directories(scene_runtime_test PRIVATE ${WISP_ROOT}/components/wisp_scene/src)
wisp_test(NAME lua_json_test SRCS scene/lua_json_test.cpp LIBS wisp_scene wisp_port_sim_os)
wisp_test(NAME store_binding_test SRCS scene/store_binding_test.cpp
          LIBS wisp_scene wisp_port_sim_os TIMEOUT 60)
wisp_test(NAME http_binding_test SRCS scene/http_binding_test.cpp
          LIBS wisp_scene wisp_port_sim_os TIMEOUT 60)
wisp_test(NAME timer_set_test SRCS scene/timer_set_test.cpp LIBS wisp_scene)
wisp_test(NAME minute_ticker_test SRCS scene/minute_ticker_test.cpp LIBS wisp_scene)
wisp_test(NAME scene_api_test SRCS scene/scene_api_test.cpp
          LIBS wisp_scene wisp_port_sim_os TIMEOUT 60)
foreach(t list_window_test scene_view_list_test canvas_test)
    target_include_directories(${t} PRIVATE ${WISP_ROOT}/components/wisp_scene/src)
endforeach()
# DocIsUpToDate compares docs/scene-api.md with the schema and writes the
# expected text into the build directory when they differ.
target_compile_definitions(schema_test PRIVATE WISP_ROOT="${WISP_ROOT}"
                                               WISP_BUILD_DIR="${CMAKE_BINARY_DIR}")
