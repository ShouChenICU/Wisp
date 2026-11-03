#include "wisp/port/esp/log_esp.hpp"

#include <cstdio>

namespace wisp::port::esp {

namespace {

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

void EspLogSink::write(const wisp::LogRecord& rec) {
    // Build the same "<ts_ms> <D|I|W|E> <tag>: <msg>"
    // line the other ports' sinks use (wisp_port_sim's StdoutLogSink;
    // see wisp/core/log.hpp's ring_snapshot() doc comment) ourselves,
    // rather than relying on esp_log to render it.
    //
    // This is written directly via std::printf() -- the same
    // console transport firmware/main.cpp's boot-report output and
    // os_esp.cpp's fatal() already use, since this board's stdout is
    // already routed to the console (see
    // firmware/sdkconfig.defaults's CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG)
    // -- instead of going through esp_log_write(). esp_log_write()
    // applies esp_log's *own*, independent
    // runtime per-tag level filter (is_level_loggable() in
    // components/log/src/log.c) on top of wisp::log's own filter
    // (wisp::log::set_level(), the intended single source of truth
    // here, defaulting to LogLevel::Debug i.e. nothing filtered).
    // Working around that by raising esp_log's own default level also
    // made every IDF-internal component's unrelated ESP_LOGD() traffic
    // visible, not just wisp's own. Every call reaching this function
    // has, by construction, already passed wisp::log's own filter (see
    // wisp/core/log.hpp's LogSink doc comment: sinks only receive
    // already-accepted lines), so there is nothing left for a second,
    // independent filter to usefully decide here -- bypassing esp_log
    // entirely removes that redundant (and, for Debug, actively
    // unwanted) filtering without having to touch esp_log's per-tag
    // runtime levels at all. IDF's own ESP_LOGx call sites are
    // untouched and still go through esp_log's normal, independent
    // filtering (left at its Kconfig default, Info -- see
    // firmware/sdkconfig.defaults), so IDF's own Debug-level logging
    // stays quiet.
    // The ring's own timestamp, so the console and the admin log viewer
    // agree on when a line was logged.
    std::printf("%llu %c %s: %s\n", static_cast<unsigned long long>(rec.ts_ms),
                level_char(rec.level), rec.tag, rec.msg);
}

}  // namespace wisp::port::esp
