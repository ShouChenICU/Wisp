#pragma once

// Factory reset (connectivity spec 6.1): erases Settings, the saved Wi-Fi
// networks, every paired browser and every model profile (keys included),
// then restarts. Library and Backup data stay.
//
// Flow: POST /system/factory-reset only asks for it (request_factory_reset()
// publishes system.factory_reset {state:"requested"}); the system duties
// (system_duties.hpp) show a confirmation dialog over whatever screen is
// up. "No" publishes {state:"cancelled"}; "yes" calls
// start_factory_reset(), which runs factory_reset() on a one-shot
// "wisp.reset" task -- never on the UI thread, since the reset waits for
// the network and writes flash.

#include <cstdint>
#include <functional>
#include <memory>

#include "wisp/os/os.hpp"

namespace wisp::app {

class App;

using TaskSpawner =
    std::function<std::unique_ptr<os::Task>(const os::TaskConfig&, std::function<void()>)>;

// Asks the device to confirm a factory reset on its screen: marks the
// request pending, then publishes system.factory_reset
// {state:"requested"}. Any thread.
void request_factory_reset(App& app);

// Clears a pending request and returns whether there was one. The screen
// calls it on the "requested" event, when it is built and on its periodic
// resync, and shows the dialog when it returns true -- so a request is
// shown once even if the screen did not exist yet or the bus dropped the
// event. Any thread.
bool take_factory_reset_request(App& app);

// Clears any pending request and publishes system.factory_reset
// {state:"cancelled"}. Any thread.
void cancel_factory_reset(App& app);

// The reset itself. Everything from step 1 to 3 is one job on the flash
// thread (spec 3.7 of the flash-thread design), retried for up to 30 s
// while the storage queue is full:
//   1. publishes system.factory_reset {state:"done"} -- while the browser
//      that asked still has a token, hence a socket to hear it on;
//   2. waits (at most 500 ms in all) for the event bus to deliver it and
//      the admin WebSocket sender to put it on the wire;
//   3. erases Settings, forgets the Wi-Fi networks, revokes every paired
//      browser (closing their sockets), clears the model profiles and the
//      agent's conversations;
//   4. writes what still waits for flash (App::flush_storage());
//   5. calls Platform::reboot (if any).
// If that job cannot start within the 30 s, nothing is erased: it
// publishes {state:"failed"} instead, does not restart, and a later
// start_factory_reset() may try again.
// Blocks; must not run on the UI thread.
void factory_reset(App& app);

// Runs factory_reset() on the "wisp.reset" task. Only the first call per
// start() does anything (the reset ends in a restart), unless that reset
// failed to start erasing; true when the task is running. If it cannot be created (out of memory),
// nothing is erased, system.factory_reset {state:"cancelled"} is published -- the browser's
// "nothing was erased" is then still true -- and false is returned so the
// caller can let a later request ask again. Any thread.
bool start_factory_reset(App& app);

// Waits for a "wisp.reset" task to finish. App::stop() calls it before
// tearing down the services the reset uses.
void join_factory_reset(App& app);

// How long factory_reset() keeps trying to start its erase job (30 s by
// default). For tests that need a full storage queue to give up quickly.
void set_reset_erase_budget(App& app, uint32_t ms);

// Replaces os::Task::spawn for the "wisp.reset" task; empty restores it.
// For tests that need the spawn to fail. Call before the reset starts.
void set_reset_spawner(App& app, TaskSpawner spawner);

}  // namespace wisp::app
