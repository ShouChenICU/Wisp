// Ported from MyOS (MIT, Copyright (c) 2026 ShouChen): register sequences
// taken from src/hal/hal_rtc.cpp (HalRtc) -- see docs/references.md and
// this file's header (pcf85063.h) for what changed (UTC instead of local
// time; no sync_system_time()/sync_from_system_time(), which is the
// board's job, not the driver's).

#include "pcf85063.h"

#include <stdatomic.h>
#include <stdbool.h>
#include <stdlib.h>
#include <string.h>

#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "pcf85063_conv.h"

static const char* TAG = "pcf85063";

#define PCF85063_I2C_ADDR 0x51
#define PCF85063_I2C_FREQ_HZ 400000
#define PCF85063_I2C_TIMEOUT_MS 50

#define REG_CTRL1 0x00
#define REG_SECONDS 0x04
#define CTRL1_STOP 0x20
/* CTRL1 bit1: 12-hour mode when set. Wisp always decodes/encodes 24-hour
 * fields (pcf85063_conv.h), so this is cleared unconditionally whenever
 * CTRL1 needs rewriting -- not from MyOS, which never touched this bit
 * (it apparently powered up in 24h mode on every unit MyOS ran on). */
#define CTRL1_12_24 0x02

struct pcf85063_dev {
    i2c_master_dev_handle_t dev;
    // Set when pcf85063_init() found the oscillator had been stopped
    // (no battery backup, or a dead backup cell): there is no valid time
    // until pcf85063_set_unix() is called. See pcf85063_get_unix().
    // Atomic: set_unix may run on one task (the network time sync) while
    // get_unix runs on others.
    atomic_bool time_invalid;
};

static esp_err_t rtc_read(pcf85063_handle_t h, uint8_t reg, uint8_t* data, size_t len) {
    return i2c_master_transmit_receive(h->dev, &reg, 1, data, len,
                                       pdMS_TO_TICKS(PCF85063_I2C_TIMEOUT_MS));
}

static esp_err_t rtc_write_bytes(pcf85063_handle_t h, uint8_t start_reg, const uint8_t* data,
                                 size_t len) {
    uint8_t buf[1 + 7];
    if (len > 7) {
        return ESP_ERR_INVALID_ARG;
    }
    buf[0] = start_reg;
    memcpy(&buf[1], data, len);
    return i2c_master_transmit(h->dev, buf, len + 1, pdMS_TO_TICKS(PCF85063_I2C_TIMEOUT_MS));
}

static esp_err_t rtc_write_byte(pcf85063_handle_t h, uint8_t reg, uint8_t value) {
    return rtc_write_bytes(h, reg, &value, 1);
}

// Restarts the oscillator if the STOP bit is set and clears the 12/24
// bit (Wisp's pcf85063_conv.h always decodes/encodes 24-hour fields), in
// a single read-modify-write when either needs clearing. `was_stopped`
// (if non-NULL) reports whether STOP was set, so callers can track that
// the time read before this call (or any read until the next
// pcf85063_set_unix()) was not valid.
static esp_err_t rtc_ensure_running(pcf85063_handle_t h, bool* was_stopped) {
    uint8_t ctrl1 = 0;
    esp_err_t ret = rtc_read(h, REG_CTRL1, &ctrl1, 1);
    if (ret != ESP_OK) {
        return ret;
    }
    bool stopped = (ctrl1 & CTRL1_STOP) != 0;
    bool is_12h = (ctrl1 & CTRL1_12_24) != 0;
    if (was_stopped != NULL) {
        *was_stopped = stopped;
    }
    if (!stopped && !is_12h) {
        return ESP_OK;
    }
    ctrl1 &= (uint8_t)~(CTRL1_STOP | CTRL1_12_24);
    ret = rtc_write_byte(h, REG_CTRL1, ctrl1);
    if (ret == ESP_OK) {
        if (stopped) {
            ESP_LOGW(TAG, "RTC STOP flag was set; oscillator restarted");
        }
        if (is_12h) {
            ESP_LOGI(TAG, "RTC 12-hour mode bit cleared (Wisp always uses 24h fields)");
        }
    }
    return ret;
}

esp_err_t pcf85063_init(i2c_master_bus_handle_t bus, pcf85063_handle_t* out) {
    if (bus == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    pcf85063_handle_t h = calloc(1, sizeof(*h));
    if (h == NULL) {
        return ESP_ERR_NO_MEM;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = PCF85063_I2C_ADDR,
        .scl_speed_hz = PCF85063_I2C_FREQ_HZ,
    };
    esp_err_t ret = i2c_master_bus_add_device(bus, &dev_cfg, &h->dev);
    if (ret != ESP_OK) {
        free(h);
        return ret;
    }

    uint8_t ctrl1 = 0;
    ret = rtc_read(h, REG_CTRL1, &ctrl1, 1);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "PCF85063 communication failed: %s", esp_err_to_name(ret));
        i2c_master_bus_rm_device(h->dev);
        free(h);
        return ret;
    }

    bool was_stopped = false;
    ret = rtc_ensure_running(h, &was_stopped);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "PCF85063 oscillator restart failed: %s", esp_err_to_name(ret));
        i2c_master_bus_rm_device(h->dev);
        free(h);
        return ret;
    }
    // The oscillator was stopped (dead/missing backup battery): whatever
    // time is in the registers predates that, so it's not valid until
    // pcf85063_set_unix() is called. See pcf85063_get_unix().
    atomic_init(&h->time_invalid, was_stopped);

    *out = h;
    ESP_LOGI(TAG, "PCF85063 init done");
    return ESP_OK;
}

esp_err_t pcf85063_get_unix(pcf85063_handle_t handle, int64_t* out) {
    if (handle == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (atomic_load(&handle->time_invalid)) {
        return ESP_ERR_INVALID_STATE;
    }

    uint8_t regs[7] = {0};
    esp_err_t ret = rtc_read(handle, REG_SECONDS, regs, sizeof(regs));
    if (ret != ESP_OK) {
        return ret;
    }

    struct tm t;
    memset(&t, 0, sizeof(t));
    if (!pcf85063_decode(regs, &t)) {
        /* Oscillator-stop flag set, or a register decoded to an invalid
         * BCD digit/out-of-range calendar field -- see pcf85063_conv.h. */
        return ESP_ERR_INVALID_STATE;
    }
    if (!pcf85063_year_plausible(t.tm_year + 1900)) {
        return ESP_ERR_INVALID_STATE;
    }

    time_t epoch = timegm(&t);
    if (epoch == (time_t)-1) {
        return ESP_ERR_INVALID_STATE;
    }
    *out = (int64_t)epoch;
    return ESP_OK;
}

esp_err_t pcf85063_set_unix(pcf85063_handle_t handle, int64_t unix_time) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    time_t epoch = (time_t)unix_time;
    struct tm t;
    memset(&t, 0, sizeof(t));
    if (gmtime_r(&epoch, &t) == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (t.tm_year < 100 || t.tm_year > 199) {
        return ESP_ERR_INVALID_ARG; /* outside PCF85063's 2000-2099 range */
    }

    esp_err_t ret = rtc_ensure_running(handle, NULL);
    if (ret != ESP_OK) {
        return ret;
    }

    uint8_t regs[7];
    pcf85063_encode(&t, regs);
    ret = rtc_write_bytes(handle, REG_SECONDS, regs, sizeof(regs));
    if (ret == ESP_OK) {
        atomic_store(&handle->time_invalid, false);
        ESP_LOGI(TAG, "RTC time set: %04d-%02d-%02d %02d:%02d:%02d UTC", t.tm_year + 1900,
                 t.tm_mon + 1, t.tm_mday, t.tm_hour, t.tm_min, t.tm_sec);
    } else {
        ESP_LOGE(TAG, "RTC write time failed: %s", esp_err_to_name(ret));
    }
    return ret;
}
