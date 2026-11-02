#pragma once

// Ported from MyOS (MIT, Copyright (c) 2026 ShouChen): the raw-to-g
// accelerometer conversion is taken from src/hal/hal_imu.cpp (HalImu),
// generalized from its hardcoded +/-2g scale to the configurable full-
// scale Wisp uses (+/-4g accel, +/-512dps gyro -- see qmi8658.h). The
// gyroscope conversion and the ODR-code lookup table are NOT from MyOS,
// which only ever drove the accelerometer: they follow the QMI8658
// register map's publicly documented encoding, and should be checked
// against real hardware (see task-17-report.md's manual verification
// steps) since MyOS never exercised them.
//
// Kept header-only/pure so these can be unit-tested on the host without
// pulling in driver/i2c_master.h -- see tests/drivers/qmi8658_conv_test.cpp.

#include <stddef.h>
#include <stdint.h>

#ifdef __cplusplus
extern "C" {
#endif

// Little-endian 16-bit sample, as the QMI8658 output registers are laid
// out (low byte first). Ported from MyOS's le_i16().
static inline int16_t qmi8658_le_i16(const uint8_t* data) {
    return (int16_t)((uint16_t)data[0] | ((uint16_t)data[1] << 8));
}

// Converts a raw 16-bit accelerometer sample to g, given the configured
// full-scale range in g (4 for Wisp's +/-4g setting). The QMI8658's
// 16-bit output always spans the full +/-fs_g range, so 1g = 32768/fs_g
// LSB. Generalizes MyOS's fixed `QMI_ACC_LSB_PER_G = 16384.0f` (its
// +/-2g setting: 32768/2).
static inline float qmi8658_raw_to_g(int16_t raw, int fs_g) {
    return (float)raw / (32768.0f / (float)fs_g);
}

// Converts a raw 16-bit gyroscope sample to degrees/second, given the
// configured full-scale range in dps (512 for Wisp's +/-512dps setting).
// Not from MyOS -- see this file's header comment.
static inline float qmi8658_raw_to_dps(int16_t raw, int fs_dps) {
    return (float)raw / (32768.0f / (float)fs_dps);
}

// Maps a requested output data rate (Hz) to QMI8658's nearest normal-mode
// ODR register code (CTRL2/CTRL3 bits[3:0] -- the encoding is shared by
// the accelerometer and gyroscope). Not from MyOS, which never changed
// the ODR after init; see this file's header comment.
static inline uint8_t qmi8658_odr_code_for_hz(uint32_t hz) {
    static const struct {
        uint32_t hz;
        uint8_t code;
    } kTable[] = {
        {8000, 0}, {4000, 1}, {2000, 2}, {1000, 3}, {500, 4}, {250, 5}, {125, 6}, {63, 7}, {31, 8},
    };
    size_t best = 0;
    uint32_t best_diff = 0xFFFFFFFFu;
    for (size_t i = 0; i < sizeof(kTable) / sizeof(kTable[0]); i++) {
        uint32_t diff = hz > kTable[i].hz ? hz - kTable[i].hz : kTable[i].hz - hz;
        if (diff < best_diff) {
            best_diff = diff;
            best = i;
        }
    }
    return kTable[best].code;
}

#ifdef __cplusplus
}
#endif
