#include "wisp/app/app.hpp"

#include <algorithm>
#include <memory>
#include <utility>

#include "app_impl.hpp"
#include "loopback.hpp"
#include "self_check.hpp"
#include "wisp/base/json.hpp"
#include "wisp/core/button_service.hpp"
#include "wisp/core/log.hpp"
#include "wisp/hal/display.hpp"
#include "wisp/hal/imu.hpp"
#include "wisp/hal/power.hpp"
#include "wisp/hal/touch.hpp"
#include "wisp/os/os.hpp"
#include "wisp/ui/ui_runtime.hpp"

namespace wisp::app {

namespace {

constexpr uint32_t kStorageRefreshMs = 2000;
// How often PowerManager::tick() is driven. Settings
// "power"/"screen_off_s" is specified (and set, default 60) in whole
// seconds, so sub-second resolution here would be pointless; 1s keeps
// the idle-timeout check's own overhead (and the "wisp.power" Timer
// callback's wake rate) negligible.
constexpr uint32_t kPowerTickMs = 1000;

}  // namespace

App::App(Platform platform) : impl_(std::make_unique<Impl>(std::move(platform))) {}

App::~App() { stop(); }

Result<void> App::start() {
    if (impl_->started) {
        return Result<void>::err(ErrorCode::Busy, "App already started");
    }

    // A previous start()/stop() cycle on this same App may have left
    // loopback permanently refusing new "wisp.loop" tasks (see
    // join_loopback() in loopback.cpp, called from stop()); clear that
    // now so "录音回放" works again after a restart.
    reset_loopback(*this);

    uint64_t boot_t0 = os::now_ms();

    // Step 1: logging.
    log::init();

    // Step 2: board bring-up, then fill in the implicit Ok entries for
    // whatever the board didn't already report on (see board.hpp).
    impl_->platform.board->init(impl_->registry, impl_->report);
    for (const auto& device : impl_->registry.all()) {
        if (!impl_->report.find(device->info().id)) {
            impl_->report.record(
                {device->info().id, device->info().cls, core::DeviceState::Ok, ""});
        }
    }

    // Step 3: core services.
    impl_->bus.start();
    if (!impl_->storage.init()) {
        impl_->report.set_note("storage", "readonly");
    }
    impl_->storage_timer = std::make_unique<os::Timer>(
        "wisp.storage", kStorageRefreshMs, /*periodic=*/true, [this] { impl_->storage.refresh(); });
    impl_->storage_timer->start();
    impl_->buttons.start();

    // Step 4: UI (or headless).
    auto display = impl_->registry.get<hal::Display>("display.main");
    std::shared_ptr<hal::Touch> touch;
    if (display) {
        core::Settings display_settings(*impl_->platform.settings, "display");
        int64_t brightness = display_settings.get_int("brightness", 200);
        brightness = std::clamp<int64_t>(brightness, 0, 255);
        display->set_brightness(static_cast<uint8_t>(brightness));

        touch = impl_->registry.get<hal::Touch>("touch.main");
        impl_->ui = std::make_unique<ui::UiRuntime>(display, touch);
        if (!impl_->ui->start()) {
            WLOG_E("app", "UiRuntime failed to start; continuing headless");
            impl_->ui.reset();
            impl_->report.set_note("mode", "headless");
        }
    } else {
        impl_->report.set_note("mode", "headless");
    }

    // PowerManager (spec 6.7) is constructed here -- once `ui`
    // is settled either way -- rather than inside the `if (display)`
    // branch above, so it also exists (minus the display/touch/UI
    // pieces, which all tolerate being null) in headless mode: the
    // idle-timeout/platform-power-hook half of its job is useful
    // regardless of whether there's a screen. It must exist before the
    // self-check screen is built below, since that screen's power-key
    // handling calls into it.
    core::Settings power_settings(*impl_->platform.settings, "power");
    auto imu = impl_->registry.get<hal::Imu>("imu.main");

    // PowerMonitor is constructed here too -- before
    // PowerManager, which holds a non-owning pointer into it -- and, like
    // PowerManager itself, regardless of headless mode: polling
    // "power.main" and publishing power.charging.changed/power.battery.
    // low has nothing to do with whether there's a screen. It's
    // completely inert if no "power.main" device is registered (see
    // power_monitor.hpp).
    auto power_dev = impl_->registry.get<hal::Power>("power.main");
    impl_->power_monitor = std::make_unique<core::PowerMonitor>(
        impl_->bus, power_dev, core::Settings(*impl_->platform.settings, "power"));
    impl_->power_monitor->start();

    impl_->power_manager = std::make_unique<PowerManager>(
        impl_->bus, display, touch, imu, &impl_->storage, impl_->power_monitor.get(),
        impl_->ui.get(), &impl_->buttons, impl_->platform.set_standby, std::move(power_settings));
    impl_->power_timer =
        std::make_unique<os::Timer>("wisp.power", kPowerTickMs, /*periodic=*/true,
                                    [this] { impl_->power_manager->tick(os::now_ms()); });
    impl_->power_timer->start();

    if (impl_->ui) {
        // Block until start_self_check() has actually run on the UI
        // thread (rather than merely queued) before start() returns:
        // otherwise a caller that calls stop() right after start()
        // returns could race a still-pending post() (see
        // app_impl.hpp's Lifetime note) -- stop() would find
        // impl_->self_check still null and skip tearing it down, and
        // the queued start_self_check() would then run during
        // UiRuntime::stop()'s own final post-queue drain, leaving a
        // freshly built screen (task, timer, subscription) with
        // nothing left to tear it down afterwards.
        os::Semaphore done(1, 0);
        bool posted = impl_->ui->post([this, &done] {
            start_self_check(*this, impl_->self_check);
            done.give();
        });
        if (posted) {
            done.take(os::kForever);
        } else {
            WLOG_E("app", "UI post queue full; self-check screen not built");
        }
    }

    // boot_ms is recorded here -- right after Step 4, once the self-check
    // screen (when there is one) has actually finished building -- per
    // spec 6.1's boot sequence, which only *ends* at step ⑤ (the
    // self-check screen going up). The screen's header already rendered
    // once with whatever set_boot_ms() had defaulted to (0) during
    // build(), so refresh_self_check_header() below re-renders it with
    // this now-final value. Setting boot_ms any earlier (e.g. right
    // before Step 4, so the header's *first* render already showed a
    // non-zero value) would under-count actual boot time by excluding
    // the screen's own build time.
    impl_->report.set_boot_ms(os::now_ms() - boot_t0);
    if (impl_->ui) {
        refresh_self_check_header(*impl_->ui, impl_->self_check);
    }

    // Step 5: finalize and report.
    const std::string board_name = impl_->platform.board->name();
    // spec 9.2 ("--no-sd 时 Library 只有内置根"): surfaces the actual
    // root list, rather than leaving it something only a unit test can
    // see, so a smoke test can assert on it directly without depending
    // on whether "storage.sd" happened to be registered.
    {
        wisp::List library_roots;
        for (const std::string& root : impl_->storage.roots(core::Area::Library)) {
            library_roots.emplace_back(root);
        }
        impl_->report.set_note("library_roots", Value(std::move(library_roots)));
    }
    impl_->platform.emit_line(
        "WISP_BOOT_REPORT " +
        to_json(impl_->report.to_value(board_name, impl_->platform.firmware_version)));
    impl_->platform.emit_line("WISP_MANIFEST " + to_json(impl_->registry.manifest(board_name)));

    impl_->bus.publish({"system.boot.completed", "app", 0, {}});

    impl_->started = true;
    return Result<void>::ok();
}

void App::stop() {
    // Reverse of start()'s step 3/4 order. Every call below is documented
    // as idempotent/safe even if the corresponding start() step never ran
    // (e.g. stop() called before start(), or called again after an
    // earlier stop()), so no `started` guard is needed here -- that flag
    // only gates start() itself.
    //
    // The self-check screen (and any in-flight "录音回放" loopback task)
    // is torn down first, before ButtonService/UiRuntime/EventBus -- see
    // app_impl.hpp's Lifetime note: it holds a bus subscription and a
    // reference to `ui`/`registry`, so it must release those before the
    // things it references go away. join_loopback() runs first and
    // specifically *before* stop_self_check(): the "wisp.loop" task's
    // on_status callback (see loopback.hpp) posts back onto the UI
    // thread through a SelfCheck-owned label, so that post must be fully
    // enqueued (join_loopback() waits for the task itself to return)
    // before SelfCheck's own destructor enqueues its UI-thread teardown
    // -- the post queue is FIFO, so ordering these two posts this way
    // guarantees the loopback callback's queued work runs (while
    // SelfCheck is still alive) strictly before SelfCheck's teardown
    // lambda does.
    join_loopback(*this);
    stop_self_check(impl_->self_check);

    // PowerManager is torn down next -- after the self-check screen
    // (which calls into it), before buttons/ui/bus (which it in turn
    // holds pointers/a reference into) are themselves stopped/reset.
    if (impl_->power_timer) {
        impl_->power_timer->stop();
        impl_->power_timer.reset();
    }
    // If stop() happens while in standby, leave standby first: buttons/
    // storage/display are members of Impl (buttons, storage) or held by
    // shared_ptr past this App's lifetime (display), so without this, a
    // later start() on the very same App instance (App supports
    // start()/stop()/start() -- see reset_loopback()'s own doc comment)
    // would resume with the screen still asleep, SD probing still
    // paused, and the button/power-monitor poll periods still widened.
    //
    // The standby() check below must run on the UI task, not here: the
    // power_timer tick that was in flight (or already queued onto the UI
    // task, via ui_->post([this] { enter_standby_if_still_idle(...); })
    // in power_manager.cpp) when power_timer->stop() above returned may
    // not have run yet, so standby() read from this (stop()'s own)
    // thread could still say false even though that queued tick is
    // about to flip it true. Checking it here and skipping the post
    // below when it reads false would race: that queued tick would then
    // run anyway (UiRuntime's post queue is FIFO, so strictly before
    // ~PowerManager()'s own post() a few lines down), leaving the device
    // in standby with nothing left to undo it, since power_manager.reset()
    // destroys the only thing that could. Posting unconditionally and
    // checking standby() from *inside* the posted job instead sidesteps
    // the race entirely: FIFO guarantees that job runs after any
    // already-queued enter_standby_if_still_idle(), so it always sees
    // the final state.
    if (impl_->power_manager) {
        PowerManager* pm = impl_->power_manager.get();
        if (impl_->ui && !impl_->ui->on_ui_thread()) {
            auto done = std::make_shared<os::Semaphore>(1, 0);
            while (!impl_->ui->post([pm, done] {
                if (pm->standby()) {
                    pm->exit_standby();
                }
                done->give();
            })) {
                os::sleep_ms(1);
            }
            done->take(os::kForever);
        } else if (pm->standby()) {
            pm->exit_standby();
        }
    }
    impl_->power_manager.reset();

    // power_monitor is torn down right after power_manager
    // (which points into it, like storage/storage_timer below) -- its
    // own stop() tears down its internal Timer; see power_monitor.hpp.
    impl_->power_monitor.reset();

    if (impl_->storage_timer) {
        impl_->storage_timer->stop();
        impl_->storage_timer.reset();
    }
    impl_->buttons.stop();
    if (impl_->ui) {
        impl_->ui->stop();
        impl_->ui.reset();
    }
    impl_->bus.stop();

    impl_->started = false;
}

core::DeviceRegistry& App::registry() { return impl_->registry; }
core::BootReport& App::report() { return impl_->report; }
core::EventBus& App::bus() { return impl_->bus; }
core::StorageManager& App::storage() { return impl_->storage; }
core::ButtonService& App::buttons() { return impl_->buttons; }
ui::UiRuntime* App::ui() { return impl_->ui.get(); }
PowerManager* App::power_manager() { return impl_->power_manager.get(); }
const Platform& App::platform() const { return impl_->platform; }

}  // namespace wisp::app
