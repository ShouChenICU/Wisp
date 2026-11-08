#pragma once

// A wall-clock time as the device shows it to the user (mvp spec 6.4):
// every place that writes one (the pull-down panel's header, the
// self-check's time sync row) goes through here, so they all follow the
// Settings time format (sys/time_24h).
//   24-hour: "HH:MM" in every language ("08:05", "23:59").
//   12-hour: the day half from the string table (ClockAm / ClockPm) around
//            "H:MM": "下午 3:05", "3:05 PM", "午後3:05". Midnight and noon
//            are 12 in Chinese and English ("上午 12:05", "12:30 PM") and 0
//            in Japanese ("午前0:05", "午後0:30"), as each language's clocks
//            write them.
//
// Thread-safe; no LVGL.

#include <cstdint>
#include <string>

#include "wisp/ui/strings.hpp"

namespace wisp::ui {

// `hour` and `minute` wrap into 0-23 and 0-59.
std::string format_clock(int hour, int minute, bool h24, Lang lang);

// The local time of `unix_s` (seconds since the epoch) in the time zone
// `utc_offset_min` minutes east of UTC.
std::string format_local_clock(int64_t unix_s, int utc_offset_min, bool h24, Lang lang);

}  // namespace wisp::ui
