#pragma once

// The simulator's scripted IMU (--imu-script <file>): one step per line,
// times in ms from simulator start (os::now_ms()), in order:
//
//   <ms> tilt <x> <y> <z>   the acceleration from then on, in g
//   <ms> shake              three 3 g peaks of 40 ms, 100 ms apart
//                           (300 ms in all), along the current direction
//   <ms> flip               z changes sign and stays so (face up to face
//                           down, or back)
//
// Blank lines and lines starting with '#' are skipped. Before the first
// step the keyboard's tilt applies; from it on the script decides
// (SimImu::read()). The gyroscope always reads 0.

#include <array>
#include <cstdint>
#include <optional>
#include <span>
#include <string_view>
#include <vector>

#include "wisp/base/result.hpp"

namespace wisp::port::sim {

struct ImuScriptStep {
    uint64_t at_ms;
    enum class Kind { Tilt, Shake, Flip } kind;
    float x = 0, y = 0, z = 0;
};

// InvalidArgument naming the line ("line 3: ...") for a malformed step or
// one earlier than the step before it.
Result<std::vector<ImuScriptStep>> parse_imu_script(std::string_view text);

// The acceleration (x, y, z in g) `steps` give at `t_ms`; nullopt before
// the first step.
std::optional<std::array<float, 3>> imu_script_accel(std::span<const ImuScriptStep> steps,
                                                     uint64_t t_ms);

}  // namespace wisp::port::sim
