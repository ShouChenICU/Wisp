#pragma once

// PCF85063 real-time clock driver (I2C, 7-bit address 0x51). Ported from
// MyOS (MIT, Copyright (c) 2026 ShouChen): register sequences and BCD
// conversion taken from src/hal/hal_rtc.cpp (HalRtc) -- see
// docs/references.md. Unlike MyOS (which read/wrote the device's local
// time via mktime()/localtime_r()), this driver always works in UTC --
// get/set take a Unix timestamp directly, and the board decides whether
// and how to convert to a local display time.
//
// Not a wisp_* portable component: this is a pure-C ESP-IDF component
// (drivers/), consumed only by wisp::port::esp::EspClock
// (components/wisp_port_esp/dev/esp_clock.cpp).

#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct pcf85063_dev* pcf85063_handle_t;

// Adds the PCF85063 device at address 0x51 on `bus`, verifies
// communication, restarts the oscillator if the STOP bit (control
// register 1) is set, and clears the 12/24-hour mode bit (Wisp always
// uses 24-hour fields). On success `*out` is a heap-allocated handle
// owned by the caller.
//
// If the STOP bit was found set, the handle is marked so that every
// pcf85063_get_unix() call fails with ESP_ERR_INVALID_STATE until
// pcf85063_set_unix() is called -- the registers held a time from before
// the oscillator stopped (power loss), which is not valid even though
// restarting the oscillator clears the STOP bit itself.
esp_err_t pcf85063_init(i2c_master_bus_handle_t bus, pcf85063_handle_t* out);

// Reads the current time and converts it (as UTC) to a Unix timestamp.
// Fails with ESP_ERR_INVALID_STATE if: the handle was marked invalid by
// pcf85063_init() (oscillator had stopped, see above, and
// pcf85063_set_unix() has not been called since); the chip's
// oscillator-stop flag is set on this read; any register fails to
// decode (invalid BCD digit, or a BCD-valid but out-of-range calendar
// field -- see pcf85063_conv.h's pcf85063_decode()); or the decoded year
// is implausible (see pcf85063_year_plausible()). In every failure case
// `*out` is left untouched and callers must not treat it as valid.
esp_err_t pcf85063_get_unix(pcf85063_handle_t handle, int64_t* out);

// Converts `unix_time` (UTC) to calendar fields and writes them to the
// chip, restarting the oscillator first if needed. On success, clears
// the "time invalid" mark described above. Fails with
// ESP_ERR_INVALID_ARG if `unix_time` falls outside PCF85063's
// representable range (years 2000-2099).
esp_err_t pcf85063_set_unix(pcf85063_handle_t handle, int64_t unix_time);

#ifdef __cplusplus
}
#endif
