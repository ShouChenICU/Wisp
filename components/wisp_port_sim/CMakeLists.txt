# Host-only PC simulator port. This is never built as an ESP-IDF component
# (ESP_PLATFORM is never defined for it); it is only ever pulled in by
# simulator/CMakeLists.txt. It is split into:
#
#   - wisp_port_sim_os: implements wisp::os on top of the host's threading
#     and clock facilities (std::thread/mutex/condition_variable, no SDL),
#     plus the stdout wisp::log sink (wisp::port::sim::StdoutLogSink), so
#     the unit test suite can link it without pulling in display/audio.
#   - wisp_port_sim_settings: JsonFileSettingsBackend, a
#     wisp::core::SettingsBackend that persists to a JSON file, and
#     SimFirmwareUpdater, which writes uploaded firmware to a file -- also
#     host-only and SDL-free, so unit tests can link it on its own.
#   - wisp_port_sim_dev: SDL-backed device port.
#   - wisp_port_sim_net: SimHttpServer, the loopback HTTP/1.1 + WebSocket
#     net::HttpServer on POSIX sockets; SimHttpClient (libcurl); SimSntp
#     (the host clock as every time server); SimWifi,
#     the simulated hal::Wifi; and the --web-dir in-memory WispPack packer
#     (zlib). SDL-free, so their tests link it on its own.

# ESP-IDF's component discovery (EXTRA_COMPONENT_DIRS in
# firmware/CMakeLists.txt) still finds this directory and -- during the
# early, isolated requirements-extraction pass described in
# cmake/wisp_component.cmake's header comment -- include()s this whole
# file, even though nothing in firmware/ ever REQUIRES any of the
# targets below. Guarding the entire file behind `if(NOT ESP_PLATFORM)`
# (rather than relying on wisp_component()'s own per-target ESP_PLATFORM
# branch, as every other wisp_* component does) means this file simply
# never calls idf_component_register()/register_component() for any of
# these targets when ESP_PLATFORM is set; they are then silently
# absent from the dependency graph (see tools/cmake/build.cmake's
# __build_expand_requirements(), which just skips an unregistered
# component rather than erroring -- it only errors if something actually
# REQUIRES one, which nothing here does) instead of attempting
# find_package(Threads)/SDL2 in that minimal cross-compiling environment.
if(NOT ESP_PLATFORM)

find_package(Threads REQUIRED)

wisp_component(
    NAME wisp_port_sim_os
    SRCS os/os_sim.cpp log_stdout.cpp
    INCLUDE_DIRS include
    REQUIRES wisp_os wisp_core
    HOST_LIBS Threads::Threads
)

wisp_component(
    NAME wisp_port_sim_settings
    SRCS settings_json_file.cpp sim_firmware.cpp
    INCLUDE_DIRS include
    REQUIRES wisp_base wisp_core wisp_os
)

wisp_component(
    NAME wisp_port_sim_net
    SRCS net/sim_http_server.cpp net/sha1.cpp net/sim_http_client.cpp net/sim_wifi.cpp
         net/sim_web_pack.cpp net/sim_sntp.cpp
    INCLUDE_DIRS include
    REQUIRES wisp_base wisp_os wisp_hal wisp_core wisp_net
    HOST_LIBS Threads::Threads CURL::libcurl ZLIB::ZLIB
)

# The simulator's image decoders for ui::UiConfig::install_image_decoders
# (baseline JPEG through LVGL's bundled TJPGD, which simulator/CMakeLists.txt
# turns on with LV_USE_TJPGD). SDL-free, so the scene tests link it.
wisp_component(
    NAME wisp_port_sim_ui
    SRCS ui/image_decoders.cpp
    INCLUDE_DIRS include
    REQUIRES wisp_os wisp_core
    HOST_LIBS lvgl
)

# Simulated HAL devices: SimDisplay/SimTouch/SimImu/SimPower/
# SimClock/SimButton/SimAudioOut/SimAudioIn/SimStorage, SimBoard (the
# wisp::app::Board that constructs them) and the --screenshot PNG
# encoder. SimDisplay itself still never touches SDL (see wisp/port/sim/
# sim_board.hpp's file header: only simulator/main.cpp's SDL render loop
# reads its framebuffer), but SimAudioOut/SimAudioIn are backed by real
# SDL playback/capture devices and SimBoard::init() probes
# SDL_GetNumAudioDevices() -- both only ever in .cpp files, never in a
# header -- so this library does need SDL2.
# Only *portable* layers (wisp_base/os/hal/core/ui/app) are forbidden
# from linking it; wisp_port_sim_dev is platform code (see AGENTS.md).
wisp_component(
    NAME wisp_port_sim_dev
    SRCS dev/sim_display.cpp dev/sim_touch.cpp dev/sim_imu.cpp dev/sim_power.cpp
         dev/sim_clock.cpp dev/sim_button.cpp dev/sim_audio.cpp dev/sim_storage.cpp
         sim_board.cpp png_writer.cpp imu_script.cpp
    INCLUDE_DIRS include
    REQUIRES wisp_base wisp_os wisp_hal wisp_core wisp_app wisp_port_sim_net
    HOST_LIBS SDL2::SDL2
)

endif()
