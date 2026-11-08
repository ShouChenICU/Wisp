#pragma once

// Pure conversion helpers for the AXP2101 driver (drivers/axp2101/
// axp2101.c), unit-tested on the host without pulling in
// driver/i2c_master.h -- see tests/drivers/axp2101_conv_test.cpp.
//
// axp2101_normalize_battery_pct()/axp2101_combine_h5l8() are ported from
// MyOS (MIT, Copyright (c) 2026 ShouChen), src/hal/hal_power.cpp
// (HalPower) -- see docs/references.md. The charging-parameter encoders
// below (axp2101_icc_code/axp2101_iterm_code/axp2101_cv_code/
// axp2101_iprechg_code) are NOT from MyOS: MyOS's original encodings for
// REG 0x62/0x63/0x64 were wrong -- e.g. MyOS wrote 0x10 to REG 0x62
// meaning to request 400 mA, but 0x10 is actually the AXP2101's code
// for 1000 mA. These are rewritten from
// scratch against lewisxhe/XPowersLib's XPowersAXP2101.hpp/
// XPowersParams.hpp (the xpowers_axp2101_chg_curr_t/chg_iterm_t/chg_vol_t
// enums and setChargerConstantCurr()/setChargerTerminationCurr()/
// setChargeTargetVoltage()/setPrechargeCurr()), a public, widely used,
// read-only reference for this exact chip's register encodings.

#include <stdbool.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Normalizes a raw AXP2101 PMU battery-percent register reading (REG
// 0xA4) to the [0, 100] range. Some AXP2101 firmware revisions report
// values above 100 with an extra status bit set in the byte; masking to
// the low 7 bits recovers the real percentage. Ported from MyOS's
// normalize_pmu_battery_pct().
static inline uint8_t axp2101_normalize_battery_pct(uint8_t raw_pct) {
    if (raw_pct <= 100) {
        return raw_pct;
    }
    uint8_t masked_pct = (uint8_t)(raw_pct & 0x7F);
    return masked_pct <= 100 ? masked_pct : 100;
}

// Combines AXP2101's two battery-voltage ADC registers (5 valid bits in
// `high`, 8 in `low`) into the 13-bit raw reading, in millivolts (the
// ADC's LSB is 1 mV). Ported from MyOS's axp_read_reg_h5l8().
static inline uint16_t axp2101_combine_h5l8(uint8_t high, uint8_t low) {
    return (uint16_t)(((uint16_t)(high & 0x1F) << 8) | low);
}

// REG 0x62 (ICC_CHG_SET) bits[4:0]: constant-current charge current.
// Per XPowersLib's xpowers_axp2101_chg_curr_t: code 0 = 0 mA; codes 1-3
// are unused/reserved; codes 4-8 step by 25 mA from 100 mA to 200 mA
// (code == ma/25, since 4*25=100 .. 8*25=200); codes 9-16 step by 100 mA
// from 300 mA to 1000 mA (code == 9 + (ma-300)/100). 400 mA -> code 10
// (0x0A); 1000 mA -> code 16 (0x10). `ma` is clamped to [0, 1000] (the
// chip's documented/enum range); values strictly between 200 and 300 mA
// (no exact code exists) round up to the 300 mA code.
static inline uint8_t axp2101_icc_code(uint16_t ma) {
    if (ma == 0) {
        return 0;
    }
    if (ma <= 200) {
        uint16_t code = (uint16_t)(ma / 25);
        if (code < 1) {
            code = 1;
        }
        return (uint8_t)code;
    }
    if (ma > 1000) {
        ma = 1000;
    }
    if (ma < 300) {
        ma = 300;
    }
    return (uint8_t)(9 + (ma - 300) / 100);
}

// REG 0x63 (ITERM_CHG_SET) bits[3:0]: charge termination current, 25 mA
// per code (code == ma/25), per XPowersLib's xpowers_axp2101_chg_iterm_t
// (0=0mA, 1=25mA, ..., 8=200mA). Termination *enable* is a separate bit
// (bit 4 of the same register -- see XPowersLib's
// enableChargerTerminationLimit()), not part of this code.
static inline uint8_t axp2101_iterm_code(uint16_t ma) {
    uint16_t code = (uint16_t)(ma / 25);
    return (uint8_t)(code > 8 ? 8 : code);
}

// REG 0x64 (CV_CHG_VOL_SET) bits[2:0]: constant-voltage charge cutoff.
// Per XPowersLib's xpowers_axp2101_chg_vol_t: 1=4.0V, 2=4.1V, 3=4.2V,
// 4=4.35V, 5=4.4V (there is no code 0). `mv` below each threshold rounds
// up to it; 4.4V and above all map to code 5.
static inline uint8_t axp2101_cv_code(uint16_t mv) {
    if (mv <= 4000) {
        return 1;
    }
    if (mv <= 4100) {
        return 2;
    }
    if (mv <= 4200) {
        return 3;
    }
    if (mv <= 4350) {
        return 4;
    }
    return 5;
}

// REG 0x61 (IPRECHG_SET) bits[3:0]: pre-charge current, 25 mA per code
// (code == ma/25), per XPowersLib's setPrechargeCurr() doc comment
// ("Precharge current limit 25*N mA"). This encoding was already correct
// in the original port; kept here alongside the others it's configured
// with so every charging-parameter encoder lives in one place.
static inline uint8_t axp2101_iprechg_code(uint16_t ma) {
    uint16_t code = (uint16_t)(ma / 25);
    return (uint8_t)(code > 15 ? 15 : code);
}

// Decides axp2101_read_status()'s final axp2101_status_t::battery_present:
// `preliminary_present` is whatever status1's battery bit or a nonzero
// battery-voltage ADC reading already indicated, and `pct_read_ok` is
// whether the REG_BAT_PCT (0xA4) read that follows actually succeeded. A
// failed PCT read must be reported the same way as "no battery wired" --
// wisp::port::esp::EspPower::status() maps !battery_present to
// PowerStatus::battery_percent == -1 (unknown) -- rather than leaving
// battery_percent at 0, which looks like a real empty battery and would
// spuriously fire wisp::core::PowerMonitor's power.battery.low.
static inline bool axp2101_battery_present_after_pct_read(bool preliminary_present,
                                                          bool pct_read_ok) {
    return preliminary_present && pct_read_ok;
}

// REG 0x49 (INTSTS2) PWRON key bits, per XPowersLib's
// xpowers_axp2101_irq_t (XPOWERS_AXP2101_PKEY_*_IRQ, bits 8-11 of its
// 24-bit IRQ word, i.e. bits 0-3 of the second status register). The
// same bit positions enable each IRQ in REG 0x41 (INTEN2). Each status
// bit latches until written back as 1. PWRON is pulled low while the key
// is held, so the negative edge is the press and the positive edge the
// release.
#define AXP2101_IRQ2_PKEY_POSITIVE 0x01
#define AXP2101_IRQ2_PKEY_NEGATIVE 0x02
#define AXP2101_IRQ2_PKEY_LONG 0x04
#define AXP2101_IRQ2_PKEY_SHORT 0x08
#define AXP2101_IRQ2_PKEY_ALL 0x0F

// The PWRON key's level, rebuilt from the latched edge bits for boards
// where no GPIO follows the key (wisp::port::esp::EspPmuPowerKey).
// Zero-initialise; `down` is the level the last update left behind.
typedef struct {
    bool down;
    // Set once any edge bit has been seen: from then on the edges alone
    // drive the level and the short-press bit is ignored, since it may
    // latch in a later read than the release edge it belongs to.
    bool edges_seen;
} axp2101_pwrkey_tracker_t;

// Feeds one read of REG 0x49 (only the AXP2101_IRQ2_PKEY_* bits matter)
// and returns the level to report for this poll:
//   - press edge only: held;
//   - release edge only: released (a release with no press before it is
//     the power-on press, which was still held when init cleared the
//     edges, and is not a press);
//   - both edges in one read: they alternate, so the level before says
//     which came first. Released before: a press shorter than the poll
//     period, reported held for this one poll and released on the next.
//     Held before: released and pressed again, still held;
//   - no edge: the previous level, except that until an edge has ever
//     been seen a short-press bit alone counts as a one-poll press, so
//     the key still works if this chip revision latches no edge bits.
//     The long-press bit is never used: ButtonService times long presses
//     from the level itself.
static inline bool axp2101_pwrkey_update(axp2101_pwrkey_tracker_t* t, uint8_t intsts2) {
    const bool press = (intsts2 & AXP2101_IRQ2_PKEY_NEGATIVE) != 0;
    const bool release = (intsts2 & AXP2101_IRQ2_PKEY_POSITIVE) != 0;
    if (press || release) {
        t->edges_seen = true;
    }
    if (press && release) {
        return true;  // t->down is already the level after both edges.
    }
    if (press) {
        t->down = true;
        return true;
    }
    if (release) {
        t->down = false;
        return false;
    }
    if (!t->edges_seen && !t->down && (intsts2 & AXP2101_IRQ2_PKEY_SHORT) != 0) {
        return true;
    }
    return t->down;
}

#ifdef __cplusplus
}
#endif
