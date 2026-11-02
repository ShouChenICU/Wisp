#pragma once

// ESP-IDF sink for wisp::log: formats each line the same way as the
// in-memory ring buffer (see wisp/core/log.hpp) and wisp_port_sim's
// StdoutLogSink -- "<ts_ms> <D|I|W|E> <tag>: <msg>" -- and hands it to
// esp_log_write() (which the IDF console/UART-JTAG log backend then
// renders over the serial monitor). See log_esp.cpp for why the line is
// formatted here rather than left to esp_log_write() itself (it adds no
// formatting when called this way, not even the tag) and for the
// interaction between esp_log's own runtime level filter and
// wisp::log's. See wisp/port/sim/log_stdout.hpp for the simulator's
// equivalent.

#include "wisp/core/log.hpp"

namespace wisp::port::esp {

class EspLogSink : public wisp::LogSink {
public:
    void write(wisp::LogLevel level, const char* tag, const char* msg) override;
};

}  // namespace wisp::port::esp
