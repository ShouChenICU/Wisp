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
