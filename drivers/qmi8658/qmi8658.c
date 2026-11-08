// Ported from MyOS (MIT, Copyright (c) 2026 ShouChen): WHO_AM_I/soft-reset
// handling and the accelerometer register sequence are taken from
// src/hal/hal_imu.cpp (HalImu) -- see docs/references.md. The gyroscope
// sequence and the ODR table (qmi8658_conv.h) are new; see this
// component's qmi8658.h file header.

#include "qmi8658.h"

#include <stdlib.h>
#include <string.h>

#include "esp_check.h"
#include "esp_log.h"
#include "freertos/FreeRTOS.h"
#include "qmi8658_conv.h"

static const char* TAG = "qmi8658";

#define QMI8658_I2C_FREQ_HZ 400000
/* Named _TICKS, not _MS: this already holds a tick count --
 * pdMS_TO_TICKS() converts once, at this macro's expansion site. */
#define QMI8658_I2C_TIMEOUT_TICKS pdMS_TO_TICKS(20)

/* qmi8658_set_odr()/the HAL's EspImu::set_rate() clamp requested rates to
 * this; see qmi8658_conv.h's ODR table comment for why the nominal code
 * doesn't always mean an exact Hz in this driver's always-6DOF mode. */
#define QMI8658_MAX_ODR_HZ 1000

/* ==================== QMI8658 registers (see hal_imu.cpp) ==================== */
#define REG_WHO_AM_I 0x00
#define REG_REVISION_ID 0x01
#define REG_CTRL1 0x02
#define REG_CTRL2 0x03 /* accelerometer: FS[6:4] / ODR[3:0] */
#define REG_CTRL3 0x04 /* gyroscope: FS[6:4] / ODR[3:0] -- not exercised by MyOS */
#define REG_CTRL5 0x06 /* low-pass filter config */
#define REG_CTRL7 0x08 /* sensor enable */
#define REG_FIFO_CTRL 0x14
#define REG_STATUS0 0x2E
#define REG_AX_L 0x35
#define REG_RESET 0x60

#define WHO_AM_I_VALUE 0x05
#define RESET_CMD 0xB0

/* CTRL1: ADDR_AI=1 enables auto-incrementing multi-byte reads/writes. */
#define CTRL1_ADDR_AUTO_INC (1U << 6)

/* Full-scale codes, CTRL2/CTRL3 bits[6:4] (QMI8658 register map). */
#define ACCEL_FS_4G_CODE 0x01
#define GYRO_FS_512DPS_CODE 0x05

/* CTRL5: aLPF_EN=bit0, aLPF_MODE=bits[2:1], gLPF_EN=bit4, gLPF_MODE=bits[6:5].
 * aLPF part (mode 3) ported from MyOS; gLPF part added to match (not
 * exercised by MyOS, which never enabled the gyroscope). */
#define CTRL5_LPF_ACCEL_GYRO_MODE3 0x77

/* CTRL7: DRDY_DIS=bit5 (polling, no interrupt pulse -- ported from MyOS),
 * gEN=bit1 (added), aEN=bit0 (ported from MyOS). */
#define CTRL7_ENABLE_ACCEL_GYRO_POLLING ((1U << 5) | (1U << 1) | (1U << 0))

#define ACCEL_FS_G 4
#define GYRO_FS_DPS 512
#define DEFAULT_ODR_HZ 125

struct qmi8658_dev {
    i2c_master_dev_handle_t dev;
    /* The ODR register code configure_sensors() was last called with --
     * updated by qmi8658_init() (the default) and qmi8658_set_odr(), and
     * read back by qmi8658_set_power(true) so waking restores the
     * currently-selected rate, not the default. */
    uint8_t odr_code;
};

static esp_err_t qmi_read_reg(qmi8658_handle_t h, uint8_t reg, uint8_t* data, size_t len) {
    return i2c_master_transmit_receive(h->dev, &reg, 1, data, len, QMI8658_I2C_TIMEOUT_TICKS);
}

static esp_err_t qmi_write_reg(qmi8658_handle_t h, uint8_t reg, uint8_t val) {
    uint8_t buf[2] = {reg, val};
    return i2c_master_transmit(h->dev, buf, sizeof(buf), QMI8658_I2C_TIMEOUT_TICKS);
}

static esp_err_t configure_sensors(qmi8658_handle_t h, uint8_t odr_code) {
    /* Disable sensors first so FS/ODR writes never land on a half-updated
     * output state (ported from MyOS's configure_accelerometer()). */
    ESP_RETURN_ON_ERROR(qmi_write_reg(h, REG_CTRL7, 0x00), TAG, "disable sensors failed");
    ESP_RETURN_ON_ERROR(qmi_write_reg(h, REG_CTRL1, CTRL1_ADDR_AUTO_INC), TAG, "set ctrl1 failed");
    ESP_RETURN_ON_ERROR(qmi_write_reg(h, REG_CTRL2, (uint8_t)((ACCEL_FS_4G_CODE << 4) | odr_code)),
                        TAG, "set accel config failed");
    ESP_RETURN_ON_ERROR(
        qmi_write_reg(h, REG_CTRL3, (uint8_t)((GYRO_FS_512DPS_CODE << 4) | odr_code)), TAG,
        "set gyro config failed");
    ESP_RETURN_ON_ERROR(qmi_write_reg(h, REG_CTRL5, CTRL5_LPF_ACCEL_GYRO_MODE3), TAG,
                        "set lpf failed");
    ESP_RETURN_ON_ERROR(qmi_write_reg(h, REG_FIFO_CTRL, 0x00), TAG, "disable fifo failed");
    ESP_RETURN_ON_ERROR(qmi_write_reg(h, REG_CTRL7, CTRL7_ENABLE_ACCEL_GYRO_POLLING), TAG,
                        "enable sensors failed");

    h->odr_code = odr_code; /* remembered for qmi8658_set_power(true). */
    vTaskDelay(pdMS_TO_TICKS(20));
    return ESP_OK;
}

esp_err_t qmi8658_init(i2c_master_bus_handle_t bus, uint8_t addr, qmi8658_handle_t* out) {
    if (bus == NULL || out == NULL || !qmi8658_addr_valid(addr)) {
        return ESP_ERR_INVALID_ARG;
    }

    qmi8658_handle_t h = calloc(1, sizeof(*h));
    if (h == NULL) {
        return ESP_ERR_NO_MEM;
    }

    i2c_device_config_t dev_cfg = {
        .dev_addr_length = I2C_ADDR_BIT_LEN_7,
        .device_address = addr,
        .scl_speed_hz = QMI8658_I2C_FREQ_HZ,
    };
    esp_err_t ret = i2c_master_bus_add_device(bus, &dev_cfg, &h->dev);
    if (ret != ESP_OK) {
        free(h);
        return ret;
    }

    uint8_t id[2] = {0};
    ret = qmi_read_reg(h, REG_WHO_AM_I, id, sizeof(id));
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "QMI8658 communication failed: %s", esp_err_to_name(ret));
        goto fail;
    }
    if (id[0] != WHO_AM_I_VALUE) {
        ESP_LOGW(TAG, "QMI8658 WHO_AM_I mismatch: 0x%02X (expected 0x%02X)", id[0], WHO_AM_I_VALUE);
        ret = ESP_ERR_NOT_FOUND;
        goto fail;
    }

    ret = qmi_write_reg(h, REG_RESET, RESET_CMD);
    if (ret != ESP_OK) {
        ESP_LOGW(TAG, "QMI8658 soft reset failed: %s", esp_err_to_name(ret));
        goto fail;
    }
    vTaskDelay(pdMS_TO_TICKS(30));

    ret = configure_sensors(h, qmi8658_odr_code_for_hz(DEFAULT_ODR_HZ));
    if (ret != ESP_OK) {
        goto fail;
    }

    uint8_t status = 0;
    qmi_read_reg(h, REG_STATUS0, &status, 1);

    *out = h;
    ESP_LOGI(TAG,
             "QMI8658 init done at 0x%02X: WHO=0x%02X REV=0x%02X accel=+/-%dg gyro=+/-%ddps "
             "odr=%dHz STATUS0=0x%02X",
             addr, id[0], id[1], ACCEL_FS_G, GYRO_FS_DPS, DEFAULT_ODR_HZ, status);
    return ESP_OK;

fail:
    i2c_master_bus_rm_device(h->dev);
    free(h);
    return ret;
}

esp_err_t qmi8658_read(qmi8658_handle_t handle, qmi8658_sample_t* out) {
    if (handle == NULL || out == NULL) {
        return ESP_ERR_INVALID_ARG;
    }

    uint8_t raw[12] = {0};
    esp_err_t ret = qmi_read_reg(handle, REG_AX_L, raw, sizeof(raw));
    if (ret != ESP_OK) {
        return ret;
    }

    out->ax_g = qmi8658_raw_to_g(qmi8658_le_i16(&raw[0]), ACCEL_FS_G);
    out->ay_g = qmi8658_raw_to_g(qmi8658_le_i16(&raw[2]), ACCEL_FS_G);
    out->az_g = qmi8658_raw_to_g(qmi8658_le_i16(&raw[4]), ACCEL_FS_G);
    out->gx_dps = qmi8658_raw_to_dps(qmi8658_le_i16(&raw[6]), GYRO_FS_DPS);
    out->gy_dps = qmi8658_raw_to_dps(qmi8658_le_i16(&raw[8]), GYRO_FS_DPS);
    out->gz_dps = qmi8658_raw_to_dps(qmi8658_le_i16(&raw[10]), GYRO_FS_DPS);
    return ESP_OK;
}

esp_err_t qmi8658_set_odr(qmi8658_handle_t handle, uint32_t hz) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (hz > QMI8658_MAX_ODR_HZ) {
        hz = QMI8658_MAX_ODR_HZ;
    }
    return configure_sensors(handle, qmi8658_odr_code_for_hz(hz));
}

esp_err_t qmi8658_set_power(qmi8658_handle_t handle, bool on) {
    if (handle == NULL) {
        return ESP_ERR_INVALID_ARG;
    }
    if (!on) {
        /* CTRL7 = 0x00: disable both sensors, nothing else touched --
         * see qmi8658_set_power()'s doc comment. FS/ODR (CTRL2/CTRL3)
         * are left as-is; configure_sensors() below restores them from
         * h->odr_code on the way back on regardless, as extra insurance
         * against any future power state that *does* clear them. */
        return qmi_write_reg(handle, REG_CTRL7, 0x00);
    }
    /* Re-run the full sequence at the last-selected ODR (not necessarily
     * DEFAULT_ODR_HZ) -- see qmi8658_set_power()'s doc comment. */
    return configure_sensors(handle, handle->odr_code);
}
