// Board definition for the Waveshare ESP32-S3-Touch-AMOLED-2.16 (see
// docs/hardware/waveshare_amoled_216.md). This task (Task 15) only brings
// the firmware up headless: zero devices are constructed here, so
// App::start() takes the "mode": "headless" path (see app.cpp) and the
// device bring-up (display/touch/power/imu/rtc/audio/sd/buttons -- see the
// hardware doc's pinout table) is added by later tasks via
// esp_board_manager (bmgr) plus Wisp's own I2C-device drivers in
// drivers/.
//
// wisp::board::create_board() (declared in
// components/wisp_app/include/wisp/app/board.hpp) is defined here, once,
// for whichever single board a given firmware binary is built for --
// selected at CMake configure time by firmware/CMakeLists.txt's
// WISP_BOARD cache variable, which picks this very file as one of
// firmware/main's sources.

#include "wisp/app/board.hpp"

#include <memory>

namespace {

class WaveshareAmoled216Board : public wisp::app::Board {
public:
    std::string name() const override { return "waveshare_amoled_216"; }

    void init(wisp::core::DeviceRegistry& /*registry*/,
              wisp::core::BootReport& /*report*/) override {
        // No devices yet (Task 15 only boots the firmware headless). A
        // board with zero devices registers nothing and records nothing
        // -- App::start() already handles both "no display.main" (mode:
        // headless) and an otherwise-empty registry/report without any
        // special casing on its side (see AGENTS.md's device model
        // rule).
    }
};

}  // namespace

namespace wisp::board {

std::unique_ptr<wisp::app::Board> create_board() {
    return std::make_unique<WaveshareAmoled216Board>();
}

}  // namespace wisp::board
