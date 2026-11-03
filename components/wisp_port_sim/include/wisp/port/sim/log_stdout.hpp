#pragma once

// Host simulator sink for wisp::log: writes every accepted line to
// stdout as "<ts_ms> <D|I|W|E> <tag>: <msg>" (the same text as
// log::ring_snapshot(), with the ring's own ts). Host-only (never built as part of an ESP-IDF
// component); does not pull in SDL.

#include "wisp/core/log.hpp"

namespace wisp::port::sim {

class StdoutLogSink : public wisp::LogSink {
public:
    void write(const wisp::LogRecord& rec) override;
};

}  // namespace wisp::port::sim
