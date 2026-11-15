#pragma once

// The device side of a firmware upload (connectivity spec 6.5). App owns
// the core::FirmwareUpdateService (App::Impl::firmware) over
// Platform::firmware; the admin route streams the image into it.
//
// Flow: once an image is written and verified, the service moves its
// progress cell (firmware_progress()) to "received"; the system duties
// (system_duties.hpp) follow that cell and open the dialog for a
// "received" whose seq they have not handled -- also at their start, so
// an image received before is still asked -- showing
// "<current> → <new>" with Yes/No. "Yes" runs start_firmware_install(),
// which confirms on a one-shot "wisp.fwup" task -- never on the UI thread,
// as the platform re-verifies the image before switching the boot slot --
// and the service then restarts through restart_for_firmware(). "No"
// calls decline_firmware(). An unanswered request times out in the
// service (two minutes), which publishes "rejected"; any phase but
// "received" closes the dialog.

#include <cstdint>
#include <optional>

#include "wisp/core/firmware_update.hpp"

namespace wisp::app {

class App;

// What awaits the user's answer, if anything. Any thread.
std::optional<core::FirmwarePending> pending_firmware(App& app);

// The firmware service's progress cell (FirmwareUpdateService::
// progress_cell()). Only between App::start() and App::stop(), which build
// and destroy the service: followers release their subscription before
// App::stop() resets it. Any thread.
const core::StateCell<core::FirmwareProgress>& firmware_progress(App& app);

// Confirms the pending image on the "wisp.fwup" task. True when the task
// runs; false (the image is declined) when it cannot start. With `upload`
// (FirmwarePending::upload of the image the dialog showed) only that
// image: one declined or timed out meanwhile is not replaced by the next
// upload's, which gets a dialog of its own. Any thread.
bool start_firmware_install(App& app, std::optional<uint32_t> upload = std::nullopt);

// Drops the pending image (only `upload`'s, when given). Any thread.
void decline_firmware(App& app, std::optional<uint32_t> upload = std::nullopt);

// FirmwareUpdateService::Deps::restart: lets the "confirmed" event reach
// the browser (at most 500 ms), writes what waits for flash
// (App::flush_storage()), then calls Platform::reboot.
void restart_for_firmware(App& app);

// Waits for a "wisp.fwup" task to finish; App::stop() calls it before
// tearing down the services it uses.
void join_firmware_install(App& app);

}  // namespace wisp::app
