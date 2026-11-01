#pragma once

// Internal (not under include/, so nothing outside wisp_app's own .cpp
// files can see this) hook for the self-check UI shown right after boot
// on any board with a display. App::start() posts build_self_check(*this)
// onto the UiRuntime as soon as it starts one (see app.cpp). Task 11
// leaves this an empty placeholder; Task 12 replaces its implementation
// with the real screen (device list, storage status, perf readout).

namespace wisp::app {

class App;

void build_self_check(App& app);

}  // namespace wisp::app
