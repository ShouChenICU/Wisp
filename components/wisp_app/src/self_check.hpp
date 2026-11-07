#pragma once

// Internal (not under include/, so nothing outside wisp_app's own .cpp
// files can see this) hook for the self-check screen (spec 8), the
// hardware diagnostics (mvp spec 6.4, 6.6). It is not built at boot: the
// shell's open_diagnostics() has App build it (start_self_check(), on the
// UI thread) into App::Impl::self_check, and a back swipe has App tear it
// down again (stop_self_check(), also on the UI thread, after the shell
// loaded the home screen). App::stop() tears down whatever is still open
// before stopping UiRuntime/EventBus -- see app_impl.hpp.
//
// SelfCheck itself is only forward-declared here (its full definition,
// and everything it owns -- the "wisp.sample" task, the 100ms lv_timer,
// its "button."/"power.state"/"net."/"scene.state" bus subscriptions --
// lives in self_check_screen.cpp) so that app.cpp and app_impl.hpp, which
// only need to hold/forward a `std::unique_ptr<SelfCheck>` without
// touching its insides, don't need to include any of its headers.

#include <memory>

namespace wisp::ui {
class SystemLayer;
}  // namespace wisp::ui

namespace wisp::app {

class App;
class SceneWiring;
class SelfCheck;

// Builds the self-check screen (spec 8's device list, IMU/power/RTC/
// storage readouts, touch test area, button hints, "Mic test" button and
// performance row) on a new screen, loads that screen, and stores the
// owner object for its non-LVGL resources into `slot` (see
// stop_self_check() below). UI thread only. Equivalent to
// `slot = build_self_check(app)`, except that assignment itself
// (destroying `slot`'s previous value, if any, which needs SelfCheck's
// full definition) is done here, in the one TU where that's available --
// app.cpp only ever forward-declares SelfCheck. The page is laid out in
// `system`'s shape; `system` must outlive it. `scenes` (may be null)
// feeds the "Scenes" card and must outlive the screen.
void start_self_check(App& app, ui::SystemLayer& system, SceneWiring* scenes,
                      std::unique_ptr<SelfCheck>& slot);

// Tears down everything `self_check` owns (see the class-level comment
// above), its screen included, and resets the pointer to null. A no-op if
// `self_check` is already null. The screen must not be the active one any
// more. Safe to call from any thread -- see self_check_screen.cpp's
// ~SelfCheck() for how it runs the LVGL-specific teardown on the UI
// thread (inline when already on it).
void stop_self_check(std::unique_ptr<SelfCheck>& self_check);

}  // namespace wisp::app
