#pragma once

// Portable, locale-independent formatting helpers used by the self-check
// screen (spec 8) -- kept in their own pure, dependency-light header/cpp
// pair so they're trivially unit-testable (see tests/app/format_test.cpp)
// without needing a UiRuntime or any wisp::hal device at all.

#include <cstdint>
#include <string>

#include "wisp/core/boot_report.hpp"

namespace wisp::app {

// Formats `bytes` 1024-based, one decimal place, e.g.:
//   0            -> "0 B"       (always an integer below 1024; no ".0")
//   1536         -> "1.5 KB"
//   6160384      -> "5.9 MB"
//   34359738368  -> "32.0 GB"
std::string format_bytes(uint64_t bytes);

// Chinese label for a device's boot-report state: "正常" (Ok), "降级"
// (Degraded), "缺失" (Missing), "失败" (Failed).
const char* state_label(core::DeviceState state);

// Formats a Unix timestamp (seconds since the epoch) as "YYYY-MM-DD
// HH:MM:SS" in UTC.
std::string format_time(int64_t unix_s);

}  // namespace wisp::app
