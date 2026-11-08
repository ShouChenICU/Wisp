#pragma once

// AXP2101 power-management IC driver (I2C, 7-bit address 0x34). Ported
// from MyOS (MIT, Copyright (c) 2026 ShouChen): register sequences and
// conversion formulas taken from src/hal/hal_power.cpp (HalPower) -- see
// docs/references.md. MyOS-specific logic is dropped: power-rail
// (BLDO1/OLED) control, MyOS's PWRON IRQ handling, esp_pm
// DFS/light-sleep, and deep-sleep. A board with a GPIO that follows the
// PWRON key reads it with EspGpioButton; a board without one polls the
// PWRON edge IRQ bits instead (axp2101_pwrkey_irq_enable()/_take() below,
// newly written against XPowersLib's register map, not from MyOS).
//
// axp2101_init() never changes any power-rail voltage or on/off state
// (AGENTS.md's "Board independence" rule plus the hardware doc's AXP2101 erratum
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
    bool battery_present;     // False if no battery is wired to this PMU,
                              // *or* if reading REG_BAT_PCT failed (see
                              // axp2101_read_status()'s doc comment) --
                              // in that second case battery_mv below may
                              // still hold a real reading.
    uint8_t battery_percent;  // 0-100; meaningless (0) if !battery_present.
    uint16_t battery_mv;      // Battery voltage, millivolts; 0 if no
                              // battery is wired, but may still be a
                              // real (nonzero) reading when
                              // battery_present was forced false purely
                              // because the REG_BAT_PCT read failed.
    bool charging;
    bool vbus_present;  // USB/VBUS currently connected.
} axp2101_status_t;

// Adds the AXP2101 device at address 0x34 on `bus`, verifies the chip ID
// register, and sets the PWRON cold-boot press-on threshold to 2s. Never
// writes to any power-rail enable/voltage register. On success `*out` is
// a heap-allocated handle owned by the caller; there is no corresponding
// deinit (matches the rest of Wisp's ESP HAL devices, which live for the
// process lifetime).
//
// Also attempts to configure charging parameters (disables TS-pin
// temperature detection -- required on boards without a battery
// thermistor, see the Waveshare reference note MyOS carried over -- and
// sets pre-charge/constant-current/termination-current/cutoff-voltage).
// This step is best-effort: if it fails partway (e.g. an I2C read
// fails), axp2101_init() still succeeds and `*out` is still usable --
// the device must register either way (AGENTS.md's device model rule) --
// but the failure is recorded; see axp2101_charging_config_status().
esp_err_t axp2101_init(i2c_master_bus_handle_t bus, axp2101_handle_t* out);

// Reads battery percentage/voltage and charging/VBUS status.
// `out->battery_present` distinguishes "no battery wired to this PMU"
// (battery_percent is then 0 and not meaningful) from an actual reading;
// see EspPower::status() (wisp/hal/power.hpp's PowerStatus::
// battery_percent contract: -1 when unknown/no battery). A failed
// REG_BAT_PCT read is also reported as !battery_present (not as
// battery_percent == 0) -- see axp2101_conv.h's
// axp2101_battery_present_after_pct_read() -- but unlike the "no battery
// wired" case, battery_mv is not forced to 0 here: it may still hold a
// real voltage reading even though battery_present is false.
esp_err_t axp2101_read_status(axp2101_handle_t handle, axp2101_status_t* out);

// ESP_OK if axp2101_init()'s charging-parameter configuration (the
// best-effort step described above) succeeded, otherwise the esp_err_t
// it failed with. Always ESP_OK for a handle axp2101_init() never
// returned (i.e. callers only need this after a successful init). The
// device is registered either way; this is purely diagnostic -- the
// caller (wisp::port::esp::EspPower, then the board) decides whether and
// how to surface it, e.g. via a WLOG_W instead of failing the device.
esp_err_t axp2101_charging_config_status(axp2101_handle_t handle);

// Requests an immediate shutdown (COMM_CFG register, bit 0): the AXP2101
// cuts every rail it controls. Only a PWRON cold-boot press can power the
// board back on afterwards.
esp_err_t axp2101_power_off(axp2101_handle_t handle);

// Enables the four PWRON key IRQs in REG 0x41 (read-modify-write; other
// IRQ enables are left as they are) and clears their latched status, so
// that axp2101_pwrkey_irq_take() starts from a clean slate. Only the
// status bits are used -- polled over I2C -- so the chip's IRQ output pin
// does not need to be wired. Touches no power rail.
esp_err_t axp2101_pwrkey_irq_enable(axp2101_handle_t handle);

// Reads the latched PWRON key bits of REG 0x49 into `*bits`
// (AXP2101_IRQ2_PKEY_* in axp2101_conv.h, other bits masked off) and
// clears exactly those bits. Feed the result to axp2101_pwrkey_update().
esp_err_t axp2101_pwrkey_irq_take(axp2101_handle_t handle, uint8_t* bits);

#ifdef __cplusplus
}
#endif
