#include "factory_reset.hpp"

#include <algorithm>
#include <memory>
#include <mutex>
#include <utility>

#include "app_impl.hpp"
#include "wisp/base/error.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/core/log.hpp"
#include "wisp/net/pairing_service.hpp"
#include "wisp/net/wifi_service.hpp"
#include "wisp/os/os.hpp"

namespace wisp::app {

namespace {

constexpr const char* kTag = "reset";
constexpr const char* kTopic = "system.factory_reset";
// How long the "done" event gets to reach the browser before the
// revocation closes its socket. Bounded: a stalled socket must not keep
// the device from resetting.
constexpr uint32_t kFlushMs = 500;
// How long the erase may take on the flash thread (an NVS erase of every
// namespace and a few file deletions; spec 3.7).
constexpr uint32_t kEraseMs = 30000;
// Waits for the network and the flash thread; touches no flash itself.
constexpr size_t kResetStackBytes = 6144;

void publish_state(App& app, const char* state) {
    app.bus().publish(core::Event{kTopic, "app", 0, Properties{{"state", Value(state)}}});
}

}  // namespace

void request_factory_reset(App& app) {
    // Set before the event goes out, so whoever handles the event finds it.
    app.impl_->reset_pending.store(true);
    publish_state(app, "requested");
}

bool take_factory_reset_request(App& app) { return app.impl_->reset_pending.exchange(false); }

void cancel_factory_reset(App& app) {
    WLOG_I(kTag, "factory reset cancelled on the device");
    app.impl_->reset_pending.store(false);
    publish_state(app, "cancelled");
}

void factory_reset(App& app) {
    WLOG_W(kTag,
           "factory reset: erasing settings, Wi-Fi, pairings, the model profiles and the "
           "agent's conversations");
    publish_state(app, "done");

    const uint64_t deadline = os::now_ms() + kFlushMs;
    auto remaining = [deadline] {
        const uint64_t now = os::now_ms();
        return static_cast<uint32_t>(now < deadline ? deadline - now : 0);
    };
    (void)app.bus().wait_idle(remaining());
    if (app.impl_->net && !app.impl_->net->flush_admin_ws(remaining())) {
        WLOG_W(kTag, "admin WebSocket not flushed in time; resetting anyway");
    }

    // The erase itself, on the flash thread. The services' own writes
    // queued from here run inline (App::Impl::submit_io() on the worker).
    auto erase_everything = [&app] {
        if (auto r = app.impl_->platform.settings->erase_all(); !r) {
            WLOG_E(kTag, "erasing settings failed: %s", r.error().message.c_str());
        } else if (auto f = app.impl_->settings_front.flush(kEraseMs); !f) {
            // The cache erases the platform in its save, run inline here.
            WLOG_E(kTag, "erasing settings failed: %s", f.error().message.c_str());
        }
        if (net::WifiService* wifi = app.wifi()) wifi->forget_all();
        if (net::PairingService* pairing = app.pairing()) pairing->revoke_all();
        app.models().clear();
        // The agent's conversation and archives (agent-core spec 3.5).
        if (auto root = app.storage().write_root(core::Area::System); root) {
            if (auto r = app.storage().remove_tree(root.value() + "/agent"); !r) {
                WLOG_E(kTag, "erasing the conversations failed: %s", r.error().message.c_str());
            }
        }
    };

    // Every erase touches flash, so all of it runs as one job on the flash
    // thread (spec 3.7). The job uses App's services: should the wait time
    // out, this task waits for the job after the restart call, which only
    // returns on a platform that cannot restart for real (the simulator),
    // so App::stop() never tears them down under it.
    auto finished = std::make_shared<os::Semaphore>(1, 0);
    Result<void> erased = app.impl_->run_io(
        [erase_everything, finished] {
            erase_everything();
            finished->give();
        },
        kEraseMs);
    const bool still_running = !erased && erased.error().code == ErrorCode::Timeout;
    if (!erased) {
        WLOG_E(kTag, "factory reset erase %s: %s",
               still_running ? "did not finish in time" : "could not start",
               erased.error().message.c_str());
    }

    // Whatever else still waits for flash is written before the restart.
    app.flush_storage();
    if (app.impl_->platform.reboot) {
        WLOG_W(kTag, "factory reset complete; restarting");
        app.impl_->platform.reboot();
    } else {
        WLOG_E(kTag, "factory reset complete but this platform cannot restart");
    }
    if (still_running) (void)finished->take(os::kForever);
}

bool start_factory_reset(App& app) {
    App::Impl& impl = *app.impl_;
    std::lock_guard<os::Mutex> lock(impl.reset_mutex);
    impl.reset_pending.store(false);
    if (impl.reset_task) {
        WLOG_W(kTag, "factory reset already running");
        return true;
    }
    const os::TaskConfig config{.name = "wisp.reset", .stack_bytes = kResetStackBytes};
    auto body = [&app] { factory_reset(app); };
    impl.reset_task =
        impl.reset_spawner ? impl.reset_spawner(config, body) : os::Task::spawn(config, body);
    if (!impl.reset_task) {
        WLOG_E(kTag, "factory reset task could not start; nothing was erased");
        publish_state(app, "cancelled");
        return false;
    }
    return true;
}

void set_reset_spawner(App& app, TaskSpawner spawner) {
    std::lock_guard<os::Mutex> lock(app.impl_->reset_mutex);
    app.impl_->reset_spawner = std::move(spawner);
}

void join_factory_reset(App& app) {
    std::unique_ptr<os::Task> task;
    {
        std::lock_guard<os::Mutex> lock(app.impl_->reset_mutex);
        task = std::move(app.impl_->reset_task);
    }
    if (task) task->join();
}

}  // namespace wisp::app
