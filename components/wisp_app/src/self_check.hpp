#pragma once

// Internal (not under include/, so nothing outside wisp_app's own .cpp
// files can see this) hook for the self-check UI shown right after boot
// on any board with a display (spec 8). App::start() posts
// start_self_check(*this, impl_->self_check) onto the UiRuntime as soon
// as it starts one (see app.cpp), which builds the screen and stores its
// teardown handle into App::Impl::self_check; App::stop() calls
// stop_self_check() on that same member before stopping UiRuntime/
// EventBus -- see app_impl.hpp's Lifetime note and app.hpp's file header.
//
// SelfCheck itself is only forward-declared here (its full definition,
// and everything it owns -- the "wisp.sample" task, the 100ms lv_timer,
// its "button."/"power.state"/"system.settings" bus subscriptions, the
// 5000ms WISP_PERF timer -- lives in
// self_check_screen.cpp) so that app.cpp and app_impl.hpp, which only
// need to hold/forward a `std::unique_ptr<SelfCheck>` without touching
// its insides, don't need to include any LVGL/HAL headers at all.

#include <memory>

namespace wisp::ui {
class SystemLayer;
class UiRuntime;
}  // namespace wisp::ui

namespace wisp::app {

class App;
class SelfCheck;

// Builds the self-check screen (spec 8's device list, IMU/power/RTC/
// storage readouts, touch test area, button hints, "录音回放" button and
// performance row) on the UI thread, and stores the owner object for its
// non-LVGL resources into `slot` (see stop_self_check() below). Callers
// must already be inside a ui()->post()'d callback (see app.cpp).
// Equivalent to `slot = build_self_check(app)`, except that assignment
// itself (destroying `slot`'s previous value, if any, which needs
// SelfCheck's full definition) is done here, in the one TU where that's
// available -- app.cpp only ever forward-declares SelfCheck. The screen
// opens its confirm dialogs on `system`, which must outlive it.
void start_self_check(App& app, ui::SystemLayer& system, std::unique_ptr<SelfCheck>& slot);

// Tears down everything `self_check` owns (see the class-level comment
// above) and resets the pointer to null. A no-op if `self_check` is
// already null. Safe to call from any thread (not just the UI thread) --
// see self_check_screen.cpp's ~SelfCheck() for how it marshals the LVGL-
// specific teardown (deleting the lv_timer, closing any open confirm
// msgbox) onto the UI thread regardless of which thread calls this.
void stop_self_check(std::unique_ptr<SelfCheck>& self_check);

// Re-renders the header's "启动 .. ms" text (spec 8) from BootReport's
// current boot_ms. A no-op if `self_check` is null (headless boot).
// Posts the refresh onto `ui` and returns immediately, without waiting
// for it to run. boot_ms is only known once start_self_check() above has
// already run (spec 6.1's boot sequence ends with the self-check screen
// on screen), so the header it built shows a stale value until this
// corrects it.
void refresh_self_check_header(ui::UiRuntime& ui, std::unique_ptr<SelfCheck>& self_check);

}  // namespace wisp::app
