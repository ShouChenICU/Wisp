#pragma once

// The device side of a firmware upload (connectivity spec 6.5). App owns
// the core::FirmwareUpdateService (App::Impl::firmware) over
// Platform::firmware; the admin route streams the image into it.
//
// Flow: once an image is written and verified, the service calls
// request_firmware_confirm(), which marks the request pending; the
// self-check screen takes it (take_firmware_request(), on the
// system.firmware "received" event and on its periodic resync) and shows
// "<current> → <new>" with Yes/No. "Yes" runs start_firmware_install(),
// which confirms on a one-shot "wisp.fwup" task -- never on the UI thread,
// as the platform re-verifies the image before switching the boot slot --
// and the service then restarts through restart_for_firmware(). "No"
// calls decline_firmware(). An unanswered request times out in the
// service (two minutes), which publishes "rejected"; the screen then
// closes its dialog because pending_firmware() is empty.

#include <optional>

#include "wisp/core/firmware_update.hpp"

namespace wisp::app {

class App;

// FirmwareUpdateService::Deps::request_confirm. Any thread.
void request_firmware_confirm(App& app);

// Clears a pending request and returns whether there was one. Any thread.
bool take_firmware_request(App& app);

// What awaits the user's answer, if anything. Any thread.
std::optional<core::FirmwarePending> pending_firmware(App& app);

// Confirms the pending image on the "wisp.fwup" task. True when the task
// runs; false (the image is declined) when it cannot start. Any thread.
bool start_firmware_install(App& app);

// Drops the pending image. Any thread.
void decline_firmware(App& app);

// FirmwareUpdateService::Deps::restart: lets the "confirmed" event reach
// the browser (at most 500 ms), then calls Platform::reboot.
void restart_for_firmware(App& app);

// Waits for a "wisp.fwup" task to finish; App::stop() calls it before
// tearing down the services it uses.
void join_firmware_install(App& app);

}  // namespace wisp::app
