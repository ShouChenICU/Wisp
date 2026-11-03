// PC simulator sink for wisp::log: formats each line the way
// log::ring_snapshot() renders the ring (see wisp/core/log.hpp) and
// writes it to stdout.
// Host-only; must not pull in SDL.

#include "wisp/port/sim/log_stdout.hpp"

#include <cstdio>

namespace wisp::port::sim {

namespace {

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

void StdoutLogSink::write(const wisp::LogRecord& rec) {
    // The ring's own timestamp, so the console and the admin log viewer
    // agree on when a line was logged.
    std::printf("%llu %c %s: %s\n", static_cast<unsigned long long>(rec.ts_ms),
                level_char(rec.level), rec.tag, rec.msg);
}

}  // namespace wisp::port::sim
