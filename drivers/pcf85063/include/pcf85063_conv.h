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

// True if both nibbles of a BCD byte are valid decimal digits (0-9). A
// corrupted/uninitialized register (e.g. 0xFF, or a stray high nibble
// like the 0x75 in a botched minutes byte) fails this even though the
// OS flag might not be set. Not from MyOS -- see pcf85063_decode()'s
// comment below.
static inline bool pcf85063_bcd_valid(uint8_t v) {
    return (v & 0x0F) <= 9 && ((v >> 4) & 0x0F) <= 9;
}

// Decodes the 7 PCF85063 time registers into `t` (UTC). Returns false
// (leaving `*t` untouched) if the oscillator-stop flag is set -- the
// chip has no valid time since it last lost power -- or if any register
// decodes to an invalid BCD digit or an out-of-range calendar field
// (e.g. minute 75, hour 31): either indicates garbage registers, not a
// real time, and callers must not treat it as valid. Ported from MyOS's
// HalRtc::get_time(); the BCD/range validation is not from MyOS -- see
// task-17-report.md's "Fix round 1".
static inline bool pcf85063_decode(const uint8_t regs[7], struct tm* t) {
    if (regs[0] & PCF85063_SECONDS_OS) {
        return false;
    }
    for (int i = 0; i < 7; i++) {
        uint8_t masked = i == 6 ? regs[i] : (uint8_t)(regs[i] & 0x7F);
        if (!pcf85063_bcd_valid(masked)) {
            return false;
        }
    }

    uint8_t sec = pcf85063_bcd_to_dec(regs[0] & 0x7F);
    uint8_t min = pcf85063_bcd_to_dec(regs[1] & 0x7F);
    uint8_t hour = pcf85063_bcd_to_dec(regs[2] & 0x3F);
    uint8_t mday = pcf85063_bcd_to_dec(regs[3] & 0x3F);
    uint8_t wday = pcf85063_bcd_to_dec(regs[4] & 0x07);
    uint8_t mon = pcf85063_bcd_to_dec(regs[5] & 0x1F);
    if (sec > 59 || min > 59 || hour > 23 || mday < 1 || mday > 31 || wday > 6 || mon < 1 ||
        mon > 12) {
        return false;
    }

    t->tm_sec = sec;
    t->tm_min = min;
    t->tm_hour = hour;
    t->tm_mday = mday;
    t->tm_wday = wday;
    t->tm_mon = mon - 1;
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
