#pragma once

// ESP-IDF sink for wisp::log: formats each line as
// "<ts_ms> <D|I|W|E> <tag>: <msg>" (the same text as log::ring_snapshot()
// and wisp_port_sim's StdoutLogSink, with the ring's own ts) and writes it
// directly to the console (std::printf(), same transport as
// firmware/main.cpp's boot-report output) rather than through
// esp_log_write(). See log_esp.cpp for why: esp_log applies its own,
// independent runtime per-tag level filter that would otherwise
// re-filter lines wisp::log already decided to deliver, on top of (and
// inconsistently with) wisp::log's own filter -- bypassing esp_log
// avoids that without having to raise esp_log's own levels (which would
// also make unrelated IDF-internal logging visible). See
// wisp/port/sim/log_stdout.hpp for the simulator's equivalent.

#include "wisp/core/log.hpp"

namespace wisp::port::esp {

class EspLogSink : public wisp::LogSink {
public:
    void write(const wisp::LogRecord& rec) override;
};

}  // namespace wisp::port::esp
