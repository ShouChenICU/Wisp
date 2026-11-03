#pragma once

// The "wisp.console" task (connectivity spec 8.2; wisp/app/console.hpp):
// reads Platform::read_line, runs each line through run_command() and
// prints the result with Platform::emit_line. Commands may block for a
// while (persisting a network, opening the hotspot), so they run here,
// never on the UI thread.
//
// start_console() does nothing without Platform::read_line. stop_console()
// sets a flag and joins: read_line waits at most a few hundred ms per call,
// so the task notices within one call. The task also ends by itself when
// read_line reports Closed. App::stop() calls stop_console() before
// anything the commands reach is torn down.

#include "wisp/app/app.hpp"

namespace wisp::app {

void start_console(App& app);
// Idempotent.
void stop_console(App& app);

}  // namespace wisp::app
