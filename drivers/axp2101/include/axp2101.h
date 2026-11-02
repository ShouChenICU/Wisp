#pragma once

// AXP2101 power-management IC driver (I2C, 7-bit address 0x34). Ported
// from MyOS (MIT, Copyright (c) 2026 ShouChen): register sequences and
// conversion formulas taken from src/hal/hal_power.cpp (HalPower) -- see
// docs/references.md. MyOS-specific logic is dropped: power-rail
// (BLDO1/OLED) control, PWRON IRQ handling (Wisp's EspGpioButton polls
// the PWRON detect pin directly instead -- see
// components/wisp_port_esp/dev/esp_button.cpp), esp_pm DFS/light-sleep,
// and deep-sleep.
//
// axp2101_init() never changes any power-rail voltage or on/off state
// (AGENTS.md's "板子无关" rule plus the hardware doc's AXP2101 erratum
// note) -- it only configures charging parameters and the PWRON
// cold-boot press-on threshold, both of which MyOS's init() already
// verified work without touching rails.
//
// Not a wisp_* portable component: this is a pure-C ESP-IDF component
// (drivers/), consumed only by wisp::port::esp::EspPower
// (components/wisp_port_esp/dev/esp_power.cpp).

#include <stdbool.h>
#include <stdint.h>

#include "driver/i2c_master.h"
#include "esp_err.h"

#ifdef __cplusplus
extern "C" {
#endif

typedef struct axp2101_dev* axp2101_handle_t;

typedef struct {
    uint8_t battery_percent;  // 0-100; 0 if no battery is detected.
    uint16_t battery_mv;      // Battery voltage, millivolts.
    bool charging;
    bool vbus_present;  // USB/VBUS currently connected.
} axp2101_status_t;

// Adds the AXP2101 device at address 0x34 on `bus`, verifies the chip ID
// register, disables TS-pin temperature detection (required on boards
// without a battery thermistor -- see the Waveshare reference note MyOS
// carried over), sets MyOS's verified safe charging current/voltage
// parameters, and sets the PWRON cold-boot press-on threshold to 2s.
// Never writes to any power-rail enable/voltage register. On success
// `*out` is a heap-allocated handle owned by the caller; there is no
// corresponding deinit (matches the rest of Wisp's ESP HAL devices,
// which live for the process lifetime).
esp_err_t axp2101_init(i2c_master_bus_handle_t bus, axp2101_handle_t* out);

// Reads battery percentage/voltage and charging/VBUS status.
esp_err_t axp2101_read_status(axp2101_handle_t handle, axp2101_status_t* out);

// Requests an immediate shutdown (COMM_CFG register, bit 0): the AXP2101
// cuts every rail it controls. Only a PWRON cold-boot press can power the
// board back on afterwards.
esp_err_t axp2101_power_off(axp2101_handle_t handle);

#ifdef __cplusplus
}
#endif
