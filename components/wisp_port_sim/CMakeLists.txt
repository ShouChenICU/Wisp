# Host-only PC simulator port. This is never built as an ESP-IDF component
# (ESP_PLATFORM is never defined for it); it is only ever pulled in by
# simulator/CMakeLists.txt. It is split into:
#
#   - wisp_port_sim_os: implements wisp::os on top of the host's threading
#     and clock facilities (std::thread/mutex/condition_variable, no SDL),
#     plus the stdout wisp::log sink (wisp::port::sim::StdoutLogSink), so
#     the unit test suite can link it without pulling in display/audio.
#   - wisp_port_sim_settings: JsonFileSettingsBackend, a
#     wisp::core::SettingsBackend that persists to a JSON file -- also
#     host-only and SDL-free, so unit tests can link it on its own.
#   - wisp_port_sim_dev: SDL-backed device port, added in Task 13.

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
    SRCS settings_json_file.cpp
    INCLUDE_DIRS include
    REQUIRES wisp_base wisp_core wisp_os
)

# Simulated HAL devices (Task 13): SimDisplay/SimTouch/SimImu/SimPower/
# SimClock/SimButton/SimAudioOut/SimAudioIn/SimStorage, SimBoard (the
# wisp::app::Board that constructs them) and the --screenshot PNG
# encoder. SimDisplay itself still never touches SDL (see wisp/port/sim/
# sim_board.hpp's file header: only simulator/main.cpp's SDL render loop
# reads its framebuffer), but as of fix round 1 (task-13-report.md),
# SimAudioOut/SimAudioIn are backed by real SDL playback/capture devices
# and SimBoard::init() probes SDL_GetNumAudioDevices() -- both only ever
# in .cpp files, never in a header -- so this library does need SDL2.
# Only *portable* layers (wisp_base/os/hal/core/ui/app) are forbidden
# from linking it; wisp_port_sim_dev is platform code (see AGENTS.md).
wisp_component(
    NAME wisp_port_sim_dev
    SRCS dev/sim_display.cpp dev/sim_touch.cpp dev/sim_imu.cpp dev/sim_power.cpp
         dev/sim_clock.cpp dev/sim_button.cpp dev/sim_audio.cpp dev/sim_storage.cpp
         sim_board.cpp png_writer.cpp
    INCLUDE_DIRS include
    REQUIRES wisp_base wisp_os wisp_hal wisp_core wisp_app
    HOST_LIBS SDL2::SDL2
)
