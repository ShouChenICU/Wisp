#pragma once

// Command-line options for the PC simulator executable (spec 8.1,
// task-13-brief.md). Declared at global scope -- not under any wisp::
// namespace -- because this struct and its parser belong to the
// simulator *executable* itself, not to a portable or platform library.
// In particular, wisp::port::sim::SimBoard (see
// components/wisp_port_sim/include/wisp/port/sim/sim_board.hpp) does not
// take a SimOptions directly: it takes a
// wisp::port::sim::SimBoardConfig, mirroring the handful of fields its
// device construction actually needs (shape, disabled, no_sd, data_dir),
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
};

// Parses argv[1..argc) (argv[0], the program name, is ignored) into a
// SimOptions. Recognized flags: --headless, --screenshot <path>,
// --exit-after-ms <n>, --disable <id-or-class> (repeatable, appended to
// `disabled` in order), --no-sd, --data-dir <path>, --shape
// <rect|rounded_rect|circle>.
//
// Fails with wisp::ErrorCode::InvalidArgument if:
//   - an argument is none of the flags above (the error message includes
//     the offending argument, e.g. "unknown argument: --bogus");
//   - a flag that takes a value is the last argument, so no value
//     follows it (the message includes the flag name, e.g. "missing
//     value for --screenshot");
//   - --shape's value is not one of "rect"/"rounded_rect"/"circle" (the
//     message includes the offending value).
wisp::Result<SimOptions> parse_sim_options(int argc, const char* const* argv);
