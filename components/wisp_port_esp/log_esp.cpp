#include "wisp/port/esp/log_esp.hpp"

#include "esp_log.h"
#include "wisp/os/os.hpp"

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

// Same one-letter convention as wisp::core::log's own ring buffer format
// ("<ts_ms> <D|I|W|E> <tag>: <msg>", see wisp/core/log.hpp's
// ring_snapshot() doc comment) and wisp_port_sim's StdoutLogSink -- kept
// consistent across every port rather than inventing a third scheme.
char level_char(wisp::LogLevel level) {
    switch (level) {
        case wisp::LogLevel::Debug:
            return 'D';
        case wisp::LogLevel::Info:
            return 'I';
        case wisp::LogLevel::Warn:
            return 'W';
        case wisp::LogLevel::Error:
            return 'E';
    }
    return '?';
}

}  // namespace

void EspLogSink::write(wisp::LogLevel level, const char* tag, const char* msg) {
    // Fix round 1, M6: esp_log_write() is documented (esp_log_write.h)
    // to add no formatting of its own -- no timestamp, no level letter,
    // and (confirmed by reading esp_log_va()/esp_log_format() in
    // components/log/src/log.c and log_format_text.c) when called this
    // way (as opposed to via the ESP_LOGx macros) it doesn't even print
    // `tag`, only using it internally for the runtime level check below.
    // Build the same "<ts_ms> <D|I|W|E> <tag>: <msg>" line the other
    // ports' sinks use instead of silently dropping the tag from the
    // actual serial output.
    //
    // esp_log_write()'s *own* runtime level check (is_level_loggable() /
    // esp_log_is_tag_loggable() in log.c) is independent of -- and would
    // otherwise re-filter on top of -- wisp::log's own level filter
    // (wisp::log::set_level(), the intended single source of truth here,
    // defaulting to LogLevel::Debug i.e. nothing filtered): see
    // firmware/sdkconfig.defaults's CONFIG_LOG_DEFAULT_LEVEL_DEBUG=y for
    // why that second filter is kept wide open instead.
    esp_log_write(map_level(level), tag, "%llu %c %s: %s\n",
                  static_cast<unsigned long long>(wisp::os::now_ms()), level_char(level), tag, msg);
}

}  // namespace wisp::port::esp
