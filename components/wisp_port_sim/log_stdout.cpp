// PC simulator sink for wisp::log: formats each line the same way as the
// in-memory ring buffer (see wisp/core/log.hpp) and writes it to stdout.
// Host-only; must not pull in SDL.

#include "wisp/port/sim/log_stdout.hpp"

#include <cstdio>

#include "wisp/os/os.hpp"

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

void StdoutLogSink::write(wisp::LogLevel level, const char* tag, const char* msg) {
    std::printf("%llu %c %s: %s\n", static_cast<unsigned long long>(wisp::os::now_ms()),
                level_char(level), tag, msg);
}

}  // namespace wisp::port::sim
