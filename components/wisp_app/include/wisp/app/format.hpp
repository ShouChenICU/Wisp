#pragma once

// Portable formatting helpers used by the self-check screen (spec 8) --
// kept in their own pure, dependency-light header/cpp pair so they're
// trivially unit-testable (see tests/app/format_test.cpp) without needing
// a UiRuntime or any wisp::hal device at all.

#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

#include "wisp/core/boot_report.hpp"
#include "wisp/ui/strings.hpp"

namespace wisp::app {

// Formats `bytes` 1024-based, one decimal place, e.g.:
//   0            -> "0 B"       (always an integer below 1024; no ".0")
//   1536         -> "1.5 KB"
//   6160384      -> "5.9 MB"
//   34359738368  -> "32.0 GB"
std::string format_bytes(uint64_t bytes);

// Label for a device's boot-report state in `lang`; in Chinese: "正常"
// (Ok), "降级" (Degraded), "缺失" (Missing), "失败" (Failed).
const char* state_label(core::DeviceState state, ui::Lang lang);

// Formats a Unix timestamp (seconds since the epoch) as "YYYY-MM-DD
// HH:MM:SS" in UTC.
std::string format_time(int64_t unix_s);

// Length of the well-formed UTF-8 sequence starting at s[i] (RFC 3629: no
// overlongs, no surrogates, nothing above U+10FFFF), or 0 if there is none.
size_t utf8_sequence(std::string_view s, size_t i);

// An SSID as the device screen can draw it. SSIDs are raw bytes and GBK
// names are common, while labels expect UTF-8: each byte of an ill-formed
// sequence, and each control byte, becomes "?".
std::string displayable_ssid(std::string_view ssid);

}  // namespace wisp::app
