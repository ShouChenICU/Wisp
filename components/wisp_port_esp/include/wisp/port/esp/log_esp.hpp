#pragma once

// ESP-IDF sink for wisp::log: forwards every accepted line to
// esp_log_write() (which the IDF console/UART-JTAG log backend then
// renders), so lines show up on the serial monitor the same way any other
// esp_log_write() caller's output does. See
// wisp/port/sim/log_stdout.hpp for the simulator's equivalent.

#include "wisp/core/log.hpp"

namespace wisp::port::esp {

class EspLogSink : public wisp::LogSink {
public:
    void write(wisp::LogLevel level, const char* tag, const char* msg) override;
};

}  // namespace wisp::port::esp
