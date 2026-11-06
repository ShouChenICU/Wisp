#include "firmware_confirm.hpp"

#include <memory>
#include <mutex>

#include "app_impl.hpp"
#include "wisp/core/log.hpp"
#include "wisp/os/os.hpp"

namespace wisp::app {

namespace {

constexpr const char* kTag = "firmware";
// How long "confirmed" gets to reach the browser before the restart.
constexpr uint32_t kFlushMs = 500;
// activate() (esp_ota_set_boot_partition(), which re-reads and hashes
// the whole image) runs on "wisp.fs"; this task only blocks waiting for
// it. Should be resized from the measured device high-water mark (the
// "mem" command, AGENTS.md 5) rather than kept at this guess.
constexpr size_t kInstallStackBytes = 6144;

}  // namespace

void request_firmware_confirm(App& app) { app.impl_->firmware_pending.store(true); }

bool take_firmware_request(App& app) { return app.impl_->firmware_pending.exchange(false); }

std::optional<core::FirmwarePending> pending_firmware(App& app) {
    core::FirmwareUpdateService* fw = app.impl_->firmware.get();
    return fw != nullptr ? fw->pending() : std::nullopt;
}

bool start_firmware_install(App& app) {
    App::Impl& impl = *app.impl_;
    std::lock_guard<os::Mutex> lock(impl.firmware_mutex);
    impl.firmware_pending.store(false);
    if (!impl.firmware) return false;
    // A task left from an earlier install whose activation failed has
    // returned already: the service only had another image pending
    // after that confirm() came back.
    if (impl.firmware_task) {
        impl.firmware_task->join();
        impl.firmware_task.reset();
    }
    impl.firmware_task =
        os::Task::spawn({.name = "wisp.fwup", .stack_bytes = kInstallStackBytes}, [&app] {
            core::FirmwareOutcome r = app.impl_->firmware->confirm();
            if (!r.ok()) WLOG_E(kTag, "installing the firmware failed: %s", r.message.c_str());
        });
    if (!impl.firmware_task) {
        WLOG_E(kTag, "firmware install task could not start; image dropped");
        (void)impl.firmware->decline();
        return false;
    }
    return true;
}

void decline_firmware(App& app) {
    app.impl_->firmware_pending.store(false);
    if (core::FirmwareUpdateService* fw = app.impl_->firmware.get()) (void)fw->decline();
}

void restart_for_firmware(App& app) {
    const uint64_t deadline = os::now_ms() + kFlushMs;
    auto remaining = [deadline] {
        const uint64_t now = os::now_ms();
        return static_cast<uint32_t>(now < deadline ? deadline - now : 0);
    };
    (void)app.bus().wait_idle(remaining());
    if (app.impl_->net && !app.impl_->net->flush_admin_ws(remaining())) {
        WLOG_W(kTag, "admin WebSocket not flushed in time; restarting anyway");
    }
    app.flush_storage();
    if (app.impl_->platform.reboot) {
        app.impl_->platform.reboot();
    } else {
        WLOG_E(kTag, "new firmware is set to boot, but this platform cannot restart");
    }
}

void join_firmware_install(App& app) {
    std::unique_ptr<os::Task> task;
    {
        std::lock_guard<os::Mutex> lock(app.impl_->firmware_mutex);
        task = std::move(app.impl_->firmware_task);
    }
    if (task) task->join();
}

}  // namespace wisp::app
