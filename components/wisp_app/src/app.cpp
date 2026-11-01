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
#include "wisp/hal/touch.hpp"
#include "wisp/os/os.hpp"
#include "wisp/ui/ui_runtime.hpp"

namespace wisp::app {

namespace {

constexpr uint32_t kStorageRefreshMs = 2000;

}  // namespace

App::App(Platform platform) : impl_(std::make_unique<Impl>(std::move(platform))) {}

App::~App() { stop(); }

Result<void> App::start() {
    if (impl_->started) {
        return Result<void>::err(ErrorCode::Busy, "App already started");
    }

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
    if (display) {
        core::Settings display_settings(*impl_->platform.settings, "display");
        int64_t brightness = display_settings.get_int("brightness", 200);
        brightness = std::clamp<int64_t>(brightness, 0, 255);
        display->set_brightness(static_cast<uint8_t>(brightness));

        auto touch = impl_->registry.get<hal::Touch>("touch.main");
        impl_->ui = std::make_unique<ui::UiRuntime>(display, touch);
        if (impl_->ui->start()) {
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
        } else {
            WLOG_E("app", "UiRuntime failed to start; continuing headless");
            impl_->ui.reset();
            impl_->report.set_note("mode", "headless");
        }
    } else {
        impl_->report.set_note("mode", "headless");
    }

    // Step 5: finalize and report.
    impl_->report.set_boot_ms(os::now_ms() - boot_t0);
    const std::string board_name = impl_->platform.board->name();
    impl_->platform.emit_line("WISP_BOOT_REPORT " + to_json(impl_->report.to_value(board_name)));
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
ui::UiRuntime* App::ui() { return impl_->ui.get(); }
const Platform& App::platform() const { return impl_->platform; }

}  // namespace wisp::app
