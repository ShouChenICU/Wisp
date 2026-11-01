#include "wisp/port/esp/log_esp.hpp"

#include "esp_log.h"

namespace wisp::port::esp {

namespace {

esp_log_level_t map_level(wisp::LogLevel level) {
    switch (level) {
        case wisp::LogLevel::Debug:
            return ESP_LOG_DEBUG;
        case wisp::LogLevel::Info:
            return ESP_LOG_INFO;
        case wisp::LogLevel::Warn:
            return ESP_LOG_WARN;
        case wisp::LogLevel::Error:
            return ESP_LOG_ERROR;
    }
    return ESP_LOG_INFO;
}

}  // namespace

void EspLogSink::write(wisp::LogLevel level, const char* tag, const char* msg) {
    // esp_log_write() does not add formatting or a trailing newline (see
    // esp_log_write.h); the "%s\n" format string supplies the newline
    // esp_log_write()'s eventual UART/USB-Serial-JTAG output needs.
    esp_log_write(map_level(level), tag, "%s\n", msg);
}

}  // namespace wisp::port::esp
