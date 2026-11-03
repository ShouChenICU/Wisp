#pragma once

// Command-line options for the PC simulator executable (spec 8.1).
// Declared at global scope -- not under any wisp::
// namespace -- because this struct and its parser belong to the
// simulator *executable* itself, not to a portable or platform library.
// In particular, wisp::port::sim::SimBoard (see
// components/wisp_port_sim/include/wisp/port/sim/sim_board.hpp) does not
// take a SimOptions directly: it takes a
// wisp::port::sim::SimBoardConfig, mirroring the handful of fields its
// device construction actually needs (shape, disabled, no_sd, data_dir,
// wifi_fail),
// so that components/wisp_port_sim never has to #include a header from
// simulator/ to name its own constructor parameter type. main.cpp
// converts between the two when it constructs a SimBoard.

#include <cstdint>
#include <string>
#include <vector>

#include "wisp/base/result.hpp"

struct SimOptions {
    bool headless = false;
    std::string screenshot;
    uint32_t exit_after_ms = 0;
    std::vector<std::string> disabled;
    bool no_sd = false;
    std::string data_dir = "./sim-data";
    std::string shape = "rounded_rect";
    // Admin HTTP server port on 127.0.0.1; 0 leaves the server off.
    uint16_t http_port = 8080;
    // Serve the admin page from this directory (packed in memory at
    // startup) instead of the assets.wpak next to the executable.
    std::string web_dir;
    bool wifi_fail = false;
};

// Parses argv[1..argc) (argv[0], the program name, is ignored) into a
// SimOptions. Recognized flags: --headless, --screenshot <path>,
// --exit-after-ms <n>, --disable <id-or-class> (repeatable, appended to
// `disabled` in order), --no-sd, --data-dir <path>, --shape
// <rect|rounded_rect|circle>, --http-port <n> (0-65535; 0 = no admin
// server), --web-dir <path>, --wifi-fail.
//
// Fails with wisp::ErrorCode::InvalidArgument if:
//   - an argument is none of the flags above (the error message includes
//     the offending argument, e.g. "unknown argument: --bogus");
//   - a flag that takes a value is the last argument, so no value
//     follows it (the message includes the flag name, e.g. "missing
//     value for --screenshot");
//   - --shape's value is not one of "rect"/"rounded_rect"/"circle" (the
//     message includes the offending value);
//   - --exit-after-ms's value is not a plain non-negative base-10
//     integer that fits in uint32_t (the message includes the offending
//     value, e.g. "invalid --exit-after-ms value: -5");
//   - --http-port's value is not such an integer or exceeds 65535 (the
//     message includes the offending value).
wisp::Result<SimOptions> parse_sim_options(int argc, const char* const* argv);
