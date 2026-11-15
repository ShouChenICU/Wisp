#pragma once

// Lets a test step PowerMonitor by hand instead of waiting for its timer.

namespace wisp::app {

class App;

// Takes one PowerMonitor sample now (PowerMonitor::sample() is safe next to
// its own timer). A no-op before start() or without a power device.
void sample_power_now(App& app);

}  // namespace wisp::app
