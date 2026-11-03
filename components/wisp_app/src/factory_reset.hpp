#pragma once

// Factory reset (connectivity spec 6.1): erases Settings, the saved Wi-Fi
// networks, every paired browser and the LLM config (key included), then
// restarts. Library and Backup data stay.
//
// Flow: POST /system/factory-reset only asks for it (request_factory_reset()
// publishes system.factory_reset {state:"requested"}); the self-check
// screen shows a confirmation dialog. "No" publishes {state:"cancelled"};
// "yes" calls start_factory_reset(), which runs factory_reset() on a
// one-shot "wisp.reset" task -- never on the UI thread, since the reset
// waits for the network and writes flash.

#include <functional>
#include <memory>

#include "wisp/os/os.hpp"

namespace wisp::app {

class App;

using TaskSpawner =
    std::function<std::unique_ptr<os::Task>(const os::TaskConfig&, std::function<void()>)>;

// Asks the device to confirm a factory reset on its screen. Any thread.
void request_factory_reset(App& app);

// Publishes system.factory_reset {state:"cancelled"}. Any thread.
void cancel_factory_reset(App& app);

// The reset itself, in this order:
//   1. publishes system.factory_reset {state:"done"} -- while the browser
//      that asked still has a token, hence a socket to hear it on;
//   2. waits (at most 500 ms in all) for the event bus to deliver it and
//      the admin WebSocket sender to put it on the wire;
//   3. erases Settings, forgets the Wi-Fi networks, revokes every paired
//      browser (closing their sockets) and clears the LLM config;
//   4. calls Platform::reboot (if any).
// Blocks; must not run on the UI thread.
void factory_reset(App& app);

// Runs factory_reset() on the "wisp.reset" task. Only the first call per
// start() does anything (the reset ends in a restart); true when the task
// is running. If it cannot be created (out of memory), nothing is erased,
// system.factory_reset {state:"cancelled"} is published -- the browser's
// "nothing was erased" is then still true -- and false is returned so the
// caller can let a later request ask again. Any thread.
bool start_factory_reset(App& app);

// Waits for a "wisp.reset" task to finish. App::stop() calls it before
// tearing down the services the reset uses.
void join_factory_reset(App& app);

// Replaces os::Task::spawn for the "wisp.reset" task; empty restores it.
// For tests that need the spawn to fail. Call before the reset starts.
void set_reset_spawner(App& app, TaskSpawner spawner);

}  // namespace wisp::app
