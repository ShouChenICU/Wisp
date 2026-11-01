#pragma once

// Host simulator sink for wisp::log: writes every accepted line to
// stdout. Host-only (never built as part of an ESP-IDF component); does
// not pull in SDL.

#include "wisp/core/log.hpp"

namespace wisp::port::sim {

class StdoutLogSink : public wisp::LogSink {
public:
    void write(wisp::LogLevel level, const char* tag, const char* msg) override;
};

}  // namespace wisp::port::sim
