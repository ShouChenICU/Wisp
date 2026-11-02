#pragma once

// Ported from MyOS (MIT, Copyright (c) 2026 ShouChen): pure BCD/register
// conversion helpers extracted from src/hal/hal_rtc.cpp (HalRtc), so they
// can be unit-tested on the host without pulling in driver/i2c_master.h
// -- see tests/drivers/pcf85063_conv_test.cpp and docs/references.md.
//
// Wisp keeps PCF85063 time in UTC (unlike MyOS, which used the device's
// local time via mktime()/localtime_r()) -- see pcf85063.h.

#include <stdbool.h>
#include <stdint.h>
#include <time.h>

#ifdef __cplusplus
extern "C" {
#endif

// Oscillator-stop flag, seconds register (REG 0x04) bit 7: set whenever
// the chip has lost power and so no longer has a valid time.
#define PCF85063_SECONDS_OS 0x80

static inline uint8_t pcf85063_bcd_to_dec(uint8_t value) {
    return (uint8_t)(((value >> 4) * 10) + (value & 0x0F));
}

static inline uint8_t pcf85063_dec_to_bcd(uint8_t value) {
    return (uint8_t)(((value / 10) << 4) | (value % 10));
}

// Encodes `t` (interpreted as UTC) into the 7 PCF85063 time registers, in
// register order (seconds, minutes, hours, days, weekdays, months,
// years). `t->tm_year` must already be in [100, 199] (2000-2099);
// callers validate this before calling (pcf85063_set_unix() does).
// Ported from MyOS's HalRtc::set_time().
static inline void pcf85063_encode(const struct tm* t, uint8_t regs[7]) {
    regs[0] = pcf85063_dec_to_bcd((uint8_t)t->tm_sec);
    regs[1] = pcf85063_dec_to_bcd((uint8_t)t->tm_min);
    regs[2] = pcf85063_dec_to_bcd((uint8_t)t->tm_hour);
    regs[3] = pcf85063_dec_to_bcd((uint8_t)t->tm_mday);
    regs[4] = pcf85063_dec_to_bcd((uint8_t)t->tm_wday);
    regs[5] = pcf85063_dec_to_bcd((uint8_t)(t->tm_mon + 1));
    regs[6] = pcf85063_dec_to_bcd((uint8_t)(t->tm_year - 100));
}

// Decodes the 7 PCF85063 time registers into `t` (UTC). Returns false
// (leaving `*t` untouched) if the oscillator-stop flag is set -- the
// chip has no valid time since it last lost power. Ported from MyOS's
// HalRtc::get_time().
static inline bool pcf85063_decode(const uint8_t regs[7], struct tm* t) {
    if (regs[0] & PCF85063_SECONDS_OS) {
        return false;
    }
    t->tm_sec = pcf85063_bcd_to_dec(regs[0] & 0x7F);
    t->tm_min = pcf85063_bcd_to_dec(regs[1] & 0x7F);
    t->tm_hour = pcf85063_bcd_to_dec(regs[2] & 0x3F);
    t->tm_mday = pcf85063_bcd_to_dec(regs[3] & 0x3F);
    t->tm_wday = pcf85063_bcd_to_dec(regs[4] & 0x07);
    t->tm_mon = pcf85063_bcd_to_dec(regs[5] & 0x1F) - 1;
    t->tm_year = 100 + pcf85063_bcd_to_dec(regs[6]);
    t->tm_isdst = 0;
    return true;
}

// Sanity bound for a decoded year (full year, e.g. 2026): guards against
// an RTC that has never been set (powers up with a plausible-looking but
// wrong date on some PCF85063 units) even when the OS flag somehow isn't
// set. Not from MyOS -- MyOS never validated the decoded year beyond the
// OS flag; see task-17-report.md.
static inline bool pcf85063_year_plausible(int full_year) { return full_year >= 2024; }

#ifdef __cplusplus
}
#endif
