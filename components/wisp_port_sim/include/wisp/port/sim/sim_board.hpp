#pragma once

// The PC simulator's wisp::app::Board (spec 6.1/8.1): constructs every
// simulated HAL device and registers them into the
// DeviceRegistry wisp::app::App::start() hands to Board::init(). The
// other simulated devices (touch/imu/power/clock/button/audio/storage)
// are declared in a private header (dev/sim_devices.hpp) shared by their
// .cpp files and sim_board.cpp; see this header's own comment on why
// SimDisplay lives here instead. See AGENTS.md's device model rule: a
// device --disable'd (or that otherwise fails to come up) is recorded
// into the BootReport instead of being added, and boot degrades rather
// than aborting.
//
// SimDisplay is declared here, in this public header, rather than in
// dev/sim_devices.hpp, because simulator/main.cpp's SDL render loop
// needs it directly: that file is the one place in this project that
// actually touches SDL video (see its own file header and SimDisplay's
// doc comment below), and it needs copy_framebuffer() -- a simulator-
// only accessor, not part of wisp::hal::Display -- to read out what
// flush() wrote. wisp_* is built -fno-rtti, so there is no dynamic_cast
// available to recover SimDisplay from a plain wisp::hal::Display taken
// out of the DeviceRegistry; SimBoard::display() hands main.cpp the
// concrete type directly instead.
//
// SimBoardConfig mirrors the handful of SimOptions (simulator/
// sim_options.hpp) fields SimBoard's device construction actually needs
// (shape, disabled, no_sd, data_dir, wifi_fail) rather than SimBoard taking
// SimOptions itself: SimOptions belongs to the simulator *executable*
// (declared in the global namespace -- see that header), and this
// component must not have to #include a header from simulator/ just to
// name its own constructor parameter type. simulator/main.cpp converts
// between the two when it constructs a SimBoard.

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "wisp/app/board.hpp"
#include "wisp/base/result.hpp"
#include "wisp/core/boot_report.hpp"
#include "wisp/core/device_registry.hpp"
#include "wisp/hal/display.hpp"
#include "wisp/port/sim/sim_input.hpp"

namespace wisp::port::sim {

// Simulated 480x480 panel. flush() only ever copies into an internal
// RGB565 framebuffer under mutex_ -- it never touches SDL -- because
// flush() runs on wisp_ui's UI task, and only the main thread may touch
// SDL video; simulator/main.cpp's SDL render loop instead polls
// copy_framebuffer() at ~60 Hz from the main thread.
class SimDisplay : public wisp::hal::Display {
public:
    SimDisplay(std::string id, int32_t width, int32_t height, std::string shape,
               int32_t corner_radius, std::string panel = "amoled");

    const wisp::hal::DeviceInfo& info() const override { return info_; }
    wisp::Result<void> flush(const wisp::hal::Area& area, const uint16_t* rgb565) override;
    wisp::Result<void> set_brightness(uint8_t level) override;
    wisp::Result<void> set_power(wisp::hal::PowerLevel level) override;
    bool powered() const override;

    int32_t width() const { return width_; }
    int32_t height() const { return height_; }
    uint8_t brightness() const { return brightness_.load(); }

    // Copies the whole width()*height() RGB565 framebuffer (row-major,
    // same layout flush() writes into) into `out` (caller-owned, must
    // hold at least width()*height() elements). Safe to call
    // concurrently with flush() -- both take mutex_.
    void copy_framebuffer(uint16_t* out) const;

private:
    wisp::hal::DeviceInfo info_;
    int32_t width_;
    int32_t height_;

    mutable std::mutex mutex_;
    std::vector<uint16_t> framebuffer_;  // guarded by mutex_; size width_*height_

    std::atomic<uint8_t> brightness_{200};
    std::atomic<bool> powered_{true};
};

struct SimBoardConfig {
    std::string shape = "rounded_rect";
    // Display caps "panel" (wisp/hal/display_geometry.hpp's panel_kind()).
    std::string panel = "amoled";
    std::vector<std::string> disabled;
    bool no_sd = false;
    std::string data_dir = "./sim-data";
    // --wifi-fail: every net.wifi connection attempt fails (see sim_wifi.hpp).
    bool wifi_fail = false;
};

class SimBoard : public wisp::app::Board {
public:
    SimBoard(SimBoardConfig config, std::shared_ptr<SimInput> input,
             std::function<void()> on_power_off);

    std::string name() const override { return "simulator"; }
    void init(wisp::core::DeviceRegistry& registry, wisp::core::BootReport& report) override;

    // The concrete display device init() constructs (null only if
    // --disable matched "display"/"display.main"); see this header's
    // file comment for why simulator/main.cpp needs the concrete type.
    std::shared_ptr<SimDisplay> display() const { return display_; }

private:
    SimBoardConfig config_;
    std::shared_ptr<SimInput> input_;
    std::function<void()> on_power_off_;
    std::shared_ptr<SimDisplay> display_;
};

}  // namespace wisp::port::sim
