#pragma once

// Light-sleep statistics for standby on battery: how often and how long
// the chip actually sleeps in deep standby, what wakes it, and how much
// battery a session cost. Backs the firmware's `sleep` console command;
// the measuring procedure is in
// docs/hardware/waveshare_esp32_s3_touch_amoled_1_75c.md "Measuring
// standby".
//
// A session runs from sleep_stats_begin() to sleep_stats_end(), which
// firmware/main/main.cpp calls around turning automatic light sleep on and
// off (the only place it is toggled). Each light sleep is counted by
// ESP-IDF's light-sleep exit callback (CONFIG_PM_LIGHT_SLEEP_CALLBACKS;
// without it sessions still record their length and battery, but no
// sleeps). The record is kept in RTC_NOINIT memory, so it survives the
// chip reset that opening the USB serial port can cause, but not power
// loss (see wisp/port/esp/sleep_record.hpp).
//
// Battery figures come from `power` (power.main; may be null), read over
// I2C on the caller's thread.

#include <string>

#include "wisp/hal/power.hpp"

namespace wisp::port::esp {

// Once at boot, before light sleep can first be enabled: checks the RTC
// record (closing a session a reset interrupted, and logging it) and
// registers the light-sleep callback.
void sleep_stats_init();

// A session starts; one already running ends first.
void sleep_stats_begin(hal::Power* power);

// The running session ends and is logged as one WLOG_I line; nothing
// happens when none is running.
void sleep_stats_end(hal::Power* power);

// The running session, the last finished one and esp_pm_dump_locks()
// (each PM lock's holders and, with CONFIG_PM_PROFILING, held times and
// the time in each PM mode). Only the console task calls it.
std::string sleep_stats_report();

// Forgets the last session; a running one restarts now, with no start
// battery.
void sleep_stats_reset();

}  // namespace wisp::port::esp
