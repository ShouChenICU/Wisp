#pragma once

// The per-board device bring-up hook that wisp::app::App::start() calls
// during boot (spec 6.1 step ②). A concrete Board (one per supported
// board, e.g. boards/waveshare_amoled_216/ for the ESP port, or a
// simulator board for the PC port -- see wisp::board::create_board()
// below) constructs every device it knows how to bring up and:
//   - on success, add()s it to `registry` so the rest of the system can
//     find it;
//   - on failure, records a DeviceReport with DeviceState::Failed (and a
//     reason) into `report` instead -- the device is simply absent from
//     `registry`, and boot continues (see AGENTS.md's device model rule:
//     a missing/failed device degrades, it never aborts boot);
//   - for a device that is present but currently offline (e.g. an SD card
//     slot with no card inserted), still add()s it to `registry` (so
//     StorageManager etc. can probe it later) but records
//     DeviceState::Missing instead of leaving it unreported.
//
// App::start() itself fills in a DeviceState::Ok entry for every
// registered device that init() didn't already record an entry for (see
// wisp/app/app.hpp) -- a Board only needs to record the non-Ok cases
// above.

#include <memory>
#include <string>

#include "wisp/core/boot_report.hpp"
#include "wisp/core/device_registry.hpp"

namespace wisp::app {

class Board {
public:
    virtual ~Board() = default;

    // The board identifier surfaced as "board" in WISP_BOOT_REPORT and
    // WISP_MANIFEST (e.g. "waveshare_amoled_216", or a simulator board
    // name).
    virtual std::string name() const = 0;

    // Constructs this board's devices, registering the ones that came up
    // successfully into `registry` and recording the rest into `report`
    // -- see the class-level comment above for the exact rules.
    virtual void init(core::DeviceRegistry& registry, core::BootReport& report) = 0;
};

}  // namespace wisp::app

namespace wisp::board {

// Constructs the Board for whichever single board this binary was built
// for. Defined by the concrete board/platform target (e.g.
// firmware/main/main.cpp for the ESP port), never by wisp_app itself --
// the PC simulator instead builds its own SimBoard directly (see
// simulator/main.cpp), since it isn't tied to any one physical board.
std::unique_ptr<wisp::app::Board> create_board();

}  // namespace wisp::board
