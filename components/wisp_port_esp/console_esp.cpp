#include "wisp/port/esp/console_esp.hpp"

#include <utility>

#include "esp_err.h"
#include "esp_timer.h"
#include "freertos/FreeRTOS.h"
#include "sdkconfig.h"
#include "wisp/core/log.hpp"

#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG
#include "driver/usb_serial_jtag.h"
#include "driver/usb_serial_jtag_vfs.h"
#elif CONFIG_ESP_CONSOLE_UART
#include "driver/uart.h"
#include "driver/uart_vfs.h"
#endif

namespace wisp::port::esp {

namespace {

constexpr const char* kTag = "console";

#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG

esp_err_t install_driver() {
    if (!usb_serial_jtag_is_driver_installed()) {
        usb_serial_jtag_driver_config_t cfg = USB_SERIAL_JTAG_DRIVER_CONFIG_DEFAULT();
        if (esp_err_t err = usb_serial_jtag_driver_install(&cfg); err != ESP_OK) return err;
    }
    usb_serial_jtag_vfs_use_driver();
    return ESP_OK;
}

int read_byte(uint8_t& c, TickType_t ticks) { return usb_serial_jtag_read_bytes(&c, 1, ticks); }

#elif CONFIG_ESP_CONSOLE_UART

esp_err_t install_driver() {
    if (!uart_is_driver_installed(CONFIG_ESP_CONSOLE_UART_NUM)) {
        // RX buffer only; TX stays unbuffered so log output is not delayed.
        esp_err_t err = uart_driver_install(CONFIG_ESP_CONSOLE_UART_NUM, 256, 0, 0, nullptr, 0);
        if (err != ESP_OK) return err;
    }
    uart_vfs_dev_use_driver(CONFIG_ESP_CONSOLE_UART_NUM);
    return ESP_OK;
}

int read_byte(uint8_t& c, TickType_t ticks) {
    return uart_read_bytes(CONFIG_ESP_CONSOLE_UART_NUM, &c, 1, ticks);
}

#endif

}  // namespace

std::unique_ptr<EspConsole> EspConsole::create() {
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG || CONFIG_ESP_CONSOLE_UART
    if (esp_err_t err = install_driver(); err != ESP_OK) {
        WLOG_W(kTag, "console driver install failed: %s; no serial console", esp_err_to_name(err));
        return nullptr;
    }
    return std::unique_ptr<EspConsole>(new EspConsole());
#else
    WLOG_I(kTag, "no USB-Serial-JTAG or UART console configured; no serial console");
    return nullptr;
#endif
}

bool EspConsole::read_line(std::string& line, uint32_t timeout_ms) {
#if CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG || CONFIG_ESP_CONSOLE_UART
    const int64_t deadline_us = esp_timer_get_time() + static_cast<int64_t>(timeout_ms) * 1000;
    while (true) {
        const int64_t left_us = deadline_us - esp_timer_get_time();
        if (left_us <= 0) return false;
        TickType_t ticks = pdMS_TO_TICKS(static_cast<uint32_t>(left_us / 1000));
        if (ticks == 0) ticks = 1;
        uint8_t c = 0;
        if (read_byte(c, ticks) <= 0) continue;  // timed out; the deadline check ends it
        if (c == '\r' || c == '\n') {
            const bool complete = !overlong_ && !pending_.empty();
            overlong_ = false;
            if (complete) {
                line = std::move(pending_);
                pending_.clear();
                return true;
            }
            pending_.clear();
        } else if (c == 0x08 || c == 0x7f) {
            if (!pending_.empty()) pending_.pop_back();
        } else if (c >= 0x20 || c == '\t') {
            if (overlong_) continue;
            pending_.push_back(static_cast<char>(c));
            if (pending_.size() > kMaxLine) {
                pending_.clear();
                overlong_ = true;
            }
        }
    }
#else
    (void)line;
    (void)timeout_ms;
    return false;
#endif
}

}  // namespace wisp::port::esp
