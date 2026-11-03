#pragma once

// Serial console input on the device (connectivity spec 8.2): backs
// wisp::app::Platform::read_line with the IDF console transport the build
// selects -- USB-Serial-JTAG or a UART.
//
// create() installs that transport's interrupt-driven driver and switches
// the stdio VFS over to it, so log and WISP_* output keep going to the same
// port (now through the driver) while this reads incoming bytes from the
// driver's RX buffer. A read waits on the driver with a timeout, never
// spinning, and bounds how long App::stop() waits for the console task.
//
// Nothing read is echoed back (the console never echoes, so a typed
// password does not appear); whether the terminal program echoes locally
// is up to the terminal. Backspace/DEL remove the last byte, CR or LF ends
// a line, empty lines are skipped, and a line longer than kMaxLine bytes
// is dropped whole.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

namespace wisp::port::esp {

class EspConsole {
public:
    static constexpr size_t kMaxLine = 256;

    // nullptr when the build has no USB-Serial-JTAG or UART console, or its
    // driver could not be installed (logged).
    static std::unique_ptr<EspConsole> create();

    // Waits up to timeout_ms for a complete line; true with `line` set, or
    // false on timeout (the console port never closes). A partial line
    // carries over to the next call. Single reader only.
    bool read_line(std::string& line, uint32_t timeout_ms);

private:
    EspConsole() = default;

    std::string pending_;
    bool overlong_ = false;
};

}  // namespace wisp::port::esp
