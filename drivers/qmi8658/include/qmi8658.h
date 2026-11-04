#pragma once

// QMI8658 6-axis IMU driver (I2C, 7-bit address 0x6B; no magnetometer on
// this chip). Ported from MyOS (MIT, Copyright (c) 2026 ShouChen):
// chip-ID verification, soft-reset and accelerometer register sequence
// taken from src/hal/hal_imu.cpp (HalImu) -- see docs/references.md.
//
// MyOS only ever drove the accelerometer at +/-2g/62.5Hz for a level
// (tilt) indicator; Wisp additionally enables the gyroscope and uses
// +/-4g/+/-512dps at a default ODR of 125Hz (spec 5.5's imu.main caps).
// The gyroscope register sequence and the ODR table in qmi8658_conv.h
// are new, not ported -- see that header's comment, and
// docs/hardware/waveshare_esp32_s3_touch_amoled_2_16.md's acceptance table (row 12) for
// the real-hardware check this needs.
//
// Not a wisp_* portable component: this is a pure-C ESP-IDF component
// (drivers/), consumed only by wisp::port::esp::EspImu
// (components/wisp_port_esp/dev/esp_imu.cpp).

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct qmi8658_dev* qmi8658_handle_t;

typedef struct {
    float ax_g;
    float ay_g;
    float az_g;
    float gx_dps;
    float gy_dps;
    float gz_dps;
} qmi8658_sample_t;

// Adds the QMI8658 device at address 0x6B on `bus`, verifies WHO_AM_I,
// soft-resets the chip, and configures the accelerometer (+/-4g) and
// gyroscope (+/-512dps) at a default output data rate of 125Hz, with
// polling (DRDY interrupt disabled). On success `*out` is a
// heap-allocated handle owned by the caller.
esp_err_t qmi8658_init(i2c_master_bus_handle_t bus, qmi8658_handle_t* out);

// Reads the latest accelerometer + gyroscope sample (units g / dps).
esp_err_t qmi8658_read(qmi8658_handle_t handle, qmi8658_sample_t* out);

// Reconfigures both the accelerometer's and the gyroscope's output data
// rate to the QMI8658 register code nearest to `hz` (see
// qmi8658_conv.h's qmi8658_odr_code_for_hz()); full-scale ranges are
// unchanged.
esp_err_t qmi8658_set_odr(qmi8658_handle_t handle, uint32_t hz);

// IMU power-down stopgap (spec 6.7 standby): `on == false` clears
// CTRL7's sensor-enable bits
// (accelerometer + gyroscope off, lowest-power state short of a full
// reset/power rail cut); `on == true` re-runs the full sensor
// configuration sequence at the currently-selected output data rate
// (whatever qmi8658_set_odr() last set it to, or the init-time default
// if it was never called) -- not just re-enabling CTRL7, so the FS/ODR
// registers are guaranteed correct even if anything about being
// "powered off" ever changes to also affect them. Does not touch
// WHO_AM_I/soft-reset; the handle itself stays valid and reusable
// across a power() off/on cycle.
esp_err_t qmi8658_set_power(qmi8658_handle_t handle, bool on);

#ifdef __cplusplus
}
#endif
