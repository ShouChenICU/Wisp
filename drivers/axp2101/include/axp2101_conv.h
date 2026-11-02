#pragma once

// Ported from MyOS (MIT, Copyright (c) 2026 ShouChen): pure conversion
// helpers extracted from src/hal/hal_power.cpp (HalPower), so they can be
// unit-tested on the host without pulling in driver/i2c_master.h -- see
// tests/drivers/axp2101_conv_test.cpp and docs/references.md.

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

#ifdef __cplusplus
}
#endif
