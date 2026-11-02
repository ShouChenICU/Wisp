// Ported from MyOS (MIT, Copyright (c) 2026 ShouChen): register sequences
// and conversion formulas taken from src/hal/hal_power.cpp (HalPower) --
// see docs/references.md and this file's header (axp2101.h) for exactly
// what was dropped (rail control, PWRON IRQ handling, sleep).

#include "axp2101.h"

#include <stdlib.h>
#include <string.h>

#include "axp2101_conv.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"

static const char* TAG = "axp2101";

#define AXP2101_I2C_ADDR 0x34
#define AXP2101_I2C_FREQ_HZ 400000
#define AXP2101_I2C_TIMEOUT_MS 50

/* ==================== AXP2101 registers (see hal_power.cpp) ==================== */
#define REG_STATUS1 0x00
#define REG_STATUS2 0x01
#define REG_CHIP_ID 0x03
#define REG_COMM_CFG 0x10 /* bit0: write 1 to request shutdown */
#define REG_IRQ_OFF_ON_LEVEL_CTRL 0x27
#define REG_ADC_CHANNEL_CTRL 0x30 /* ADC channel enable */
#define REG_BAT_VOLT_H 0x34
#define REG_BAT_VOLT_L 0x35
#define REG_TS_PIN_CTRL 0x50    /* TS pin function control */
#define REG_IPRECHG_SET 0x61    /* Pre-charge current */
#define REG_ICC_CHG_SET 0x62    /* Constant-current charge current */
#define REG_ITERM_CHG_SET 0x63  /* Charge termination current */
#define REG_CV_CHG_VOL_SET 0x64 /* Constant-voltage charge cutoff */
#define REG_BAT_PCT 0xA4        /* PMU-computed battery percentage */

/* REG 0x27: ONLEVEL select, 0=128ms 1=512ms 2=1s 3=2s. */
#define PWRON_LEVEL_2S 0x03

/* Charging parameters configure_charging() sets (see axp2101_conv.h for
 * why these aren't MyOS's original values -- it had the wrong register
 * encodings for REG 0x62/0x63/0x64). */
#define CHG_IPRECHG_MA 50
#define CHG_ICC_MA 400
#define CHG_ITERM_MA 25
#define CHG_CV_MV 4200

struct axp2101_dev {
    i2c_master_dev_handle_t dev;
    esp_err_t charging_cfg_err;  // See axp2101_charging_config_status().
};

static esp_err_t axp_read_reg(axp2101_handle_t h, uint8_t reg, uint8_t* data, size_t len) {
    return i2c_master_transmit_receive(h->dev, &reg, 1, data, len,
                                       pdMS_TO_TICKS(AXP2101_I2C_TIMEOUT_MS));
}

static esp_err_t axp_write_reg(axp2101_handle_t h, uint8_t reg, uint8_t val) {
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(h->dev, buf, sizeof(buf), pdMS_TO_TICKS(AXP2101_I2C_TIMEOUT_MS));
}

static void configure_power_key_press_on_time(axp2101_handle_t h) {
    uint8_t reg_val = 0;
    esp_err_t ret = axp_read_reg(h, REG_IRQ_OFF_ON_LEVEL_CTRL, &reg_val, 1);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "read PWRON press-on threshold failed: %s", esp_err_to_name(ret));
        return;
    }
    reg_val = (uint8_t)((reg_val & 0xFC) | PWRON_LEVEL_2S);
    ret = axp_write_reg(h, REG_IRQ_OFF_ON_LEVEL_CTRL, reg_val);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "set PWRON press-on threshold failed: %s", esp_err_to_name(ret));
        return;
    }
    ESP_LOGI(TAG, "PWRON cold-boot threshold set to 2s press");
}

// Disables TS-pin temperature detection (required on boards with no
// battery thermistor wired to TS, otherwise charging behaves abnormally
// -- MyOS carried this note over from Waveshare's own reference example)
// and sets safe charging current/voltage parameters. Never touches any
// rail-enable or rail-voltage register.
//
// Every read is checked: on failure this aborts immediately (does not
// fall through to a write using a zeroed/garbage `reg_val`) and returns
// that error -- see axp2101_init()'s caller-facing contract
// (axp2101_charging_config_status()) for why a failure here doesn't stop
// the device from registering.
static esp_err_t configure_charging(axp2101_handle_t h) {
    uint8_t reg_val = 0;
    esp_err_t ret;

    /* Disable TS-pin temperature detection: REG 0x50 = (v & 0xF0) | 0x10,
     * REG 0x30 bit1 cleared -- XPowersLib's disableTSPinMeasure(). */
    ret = axp_read_reg(h, REG_TS_PIN_CTRL, &reg_val, 1);
    if (ret != ESP_OK) {
        return ret;
    }
    reg_val = (uint8_t)((reg_val & 0xF0) | 0x10);
    ret = axp_write_reg(h, REG_TS_PIN_CTRL, reg_val);
    if (ret != ESP_OK) {
        return ret;
    }

    ret = axp_read_reg(h, REG_ADC_CHANNEL_CTRL, &reg_val, 1);
    if (ret != ESP_OK) {
        return ret;
    }
    reg_val &= (uint8_t)~(1U << 1);              /* disable TS ADC channel */
    reg_val |= (uint8_t)((1U << 0) | (1U << 2)); /* enable battery-voltage + VBUS ADC */
    ret = axp_write_reg(h, REG_ADC_CHANNEL_CTRL, reg_val);
    if (ret != ESP_OK) {
        return ret;
    }

    /* Pre-charge current: 50mA (REG 0x61 bits[3:0], 25mA/code). */
    ret = axp_read_reg(h, REG_IPRECHG_SET, &reg_val, 1);
    if (ret != ESP_OK) {
        return ret;
    }
    reg_val = (uint8_t)((reg_val & 0xF0) | axp2101_iprechg_code(CHG_IPRECHG_MA));
    ret = axp_write_reg(h, REG_IPRECHG_SET, reg_val);
    if (ret != ESP_OK) {
        return ret;
    }

    /* Constant-current charge current: 400mA (REG 0x62 bits[4:0] -- code
     * 10 / 0x0A; see axp2101_icc_code()'s comment for the encoding MyOS
     * got wrong, 0x10, which is actually 1000mA). */
    ret = axp_read_reg(h, REG_ICC_CHG_SET, &reg_val, 1);
    if (ret != ESP_OK) {
        return ret;
    }
    reg_val = (uint8_t)((reg_val & 0xE0) | axp2101_icc_code(CHG_ICC_MA));
    ret = axp_write_reg(h, REG_ICC_CHG_SET, reg_val);
    if (ret != ESP_OK) {
        return ret;
    }

    /* Termination current: 25mA (REG 0x63 bits[3:0] -- code 1) plus
     * termination enable (bit4), forced on regardless of its previous
     * state. */
    ret = axp_read_reg(h, REG_ITERM_CHG_SET, &reg_val, 1);
    if (ret != ESP_OK) {
        return ret;
    }
    reg_val = (uint8_t)((reg_val & 0xE0) | axp2101_iterm_code(CHG_ITERM_MA) | 0x10);
    ret = axp_write_reg(h, REG_ITERM_CHG_SET, reg_val);
    if (ret != ESP_OK) {
        return ret;
    }

    /* Constant-voltage cutoff: 4.2V (REG 0x64 bits[2:0] -- code 3). */
    ret = axp_read_reg(h, REG_CV_CHG_VOL_SET, &reg_val, 1);
    if (ret != ESP_OK) {
        return ret;
    }
    reg_val = (uint8_t)((reg_val & 0xF8) | axp2101_cv_code(CHG_CV_MV));
    ret = axp_write_reg(h, REG_CV_CHG_VOL_SET, reg_val);
    if (ret != ESP_OK) {
        return ret;
    }

    ESP_LOGI(TAG, "charging configured: pre-chg %dmA, CC %dmA, term %dmA, cutoff %dmV",
             CHG_IPRECHG_MA, CHG_ICC_MA, CHG_ITERM_MA, CHG_CV_MV);
    return ESP_OK;
}

esp_err_t axp2101_init(i2c_master_bus_handle_t bus, axp2101_handle_t* out) {
    if (bus == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    axp2101_handle_t h = calloc(1, sizeof(*h));
    if (h == NULL) {
        return ESP_ERR_NO_MEM;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = AXP2101_I2C_ADDR,
        .scl_speed_hz = AXP2101_I2C_FREQ_HZ,
    };
    esp_err_t ret = i2c_master_bus_add_device(bus, &dev_cfg, &h->dev);
    if (ret != ESP_OK) {
        free(h);
        return ret;
    }

    uint8_t chip_id = 0;
    ret = axp_read_reg(h, REG_CHIP_ID, &chip_id, 1);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "AXP2101 communication failed: %s", esp_err_to_name(ret));
        i2c_master_bus_rm_device(h->dev);
        free(h);
        return ret;
    }
    ESP_LOGI(TAG, "AXP2101 chip id: 0x%02X", chip_id);

    configure_power_key_press_on_time(h);

    h->charging_cfg_err = configure_charging(h);
    if (h->charging_cfg_err != ESP_OK) {
        /* Non-fatal: the device still registers (AGENTS.md's device-model
         * rule -- init failures degrade, they don't block boot). The
         * caller (EspPower, then board.cpp) surfaces this via
         * axp2101_charging_config_status(). */
        ESP_LOGE(TAG, "charging configuration failed: %s", esp_err_to_name(h->charging_cfg_err));
    }

    *out = h;
    ESP_LOGI(TAG, "AXP2101 init done (rails untouched)");
    return ESP_OK;
}

esp_err_t axp2101_charging_config_status(axp2101_handle_t handle) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    return handle->charging_cfg_err;
}

esp_err_t axp2101_read_status(axp2101_handle_t handle, axp2101_status_t* out) {
    if (handle == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    memset(out, 0, sizeof(*out));

    uint8_t status1 = 0, status2 = 0;
    esp_err_t ret = axp_read_reg(handle, REG_STATUS1, &status1, 1);
    if (ret != ESP_OK) {
        return ret;
    }
    ret = axp_read_reg(handle, REG_STATUS2, &status2, 1);
    if (ret != ESP_OK) {
        return ret;
    }

    const bool battery_connected = (status1 & 0x08) != 0;
    out->vbus_present = (status1 & 0x20) != 0;
    /* STATUS2[6:5]: 0=standby, 1=charging, 2=discharging. */
    out->charging = ((status2 >> 5) & 0x03) == 0x01;

    uint8_t high = 0, low = 0;
    if (axp_read_reg(handle, REG_BAT_VOLT_H, &high, 1) == ESP_OK &&
        axp_read_reg(handle, REG_BAT_VOLT_L, &low, 1) == ESP_OK) {
        out->battery_mv = axp2101_combine_h5l8(high, low);
    }

    /* No battery wired to this PMU (or the read otherwise failed to show
     * one): battery_percent/battery_mv stay 0 and are not meaningful --
     * EspPower::status() maps this to PowerStatus::battery_percent == -1
     * (wisp/hal/power.hpp's "unknown" contract). */
    out->battery_present = battery_connected || out->battery_mv != 0;
    if (!out->battery_present) {
        return ESP_OK;
    }

    uint8_t pct_reg = 0;
    if (axp_read_reg(handle, REG_BAT_PCT, &pct_reg, 1) == ESP_OK) {
        out->battery_percent = axp2101_normalize_battery_pct(pct_reg);
    }
    return ESP_OK;
}

esp_err_t axp2101_power_off(axp2101_handle_t handle) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    uint8_t cfg = 0;
    axp_read_reg(handle, REG_COMM_CFG, &cfg, 1);
    cfg |= 0x01;
    ESP_LOGI(TAG, "AXP2101 power off requested");
    return axp_write_reg(handle, REG_COMM_CFG, cfg);
}
