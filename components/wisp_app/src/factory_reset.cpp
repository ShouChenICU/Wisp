#include "factory_reset.hpp"

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include "app_impl.hpp"
#include "wisp/base/error.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/core/log.hpp"
#include "wisp/core/notification_service.hpp"
#include "wisp/core/reminders.hpp"
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
           "factory reset: erasing settings, Wi-Fi, pairings, the model profiles, the "
           "agent's conversations and memory, todos and notifications");

    // Pending -> Started (the job) or Abandoned (this task, once the wait
    // is over): whichever comes first wins, so a job still queued when we
    // give up never erases behind a device that reported failure.
    enum : int { kPending, kStarted, kAbandoned };
    auto stage = std::make_shared<std::atomic<int>>(kPending);
    auto finished = std::make_shared<os::Semaphore>(1, 0);

    // The whole reset that touches flash, as one job on the flash thread
    // (spec 3.7). "done" goes out from here, once the erase is certain to
    // happen, while the browser that asked still has a token -- hence a
    // socket to hear it on. The services' own writes queued from here run
    // inline (App::Impl::submit_io() on the worker).
    auto erase_everything = [&app] {
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

        if (auto r = app.impl_->platform.settings->erase_all(); !r) {
            WLOG_E(kTag, "erasing settings failed: %s", r.error().message.c_str());
        } else if (auto f = app.impl_->settings_front.flush(kEraseMs); !f) {
            // The cache erases the platform in its save, run inline here.
            WLOG_E(kTag, "erasing settings failed: %s", f.error().message.c_str());
        }
        if (net::WifiService* wifi = app.wifi()) wifi->forget_all();
        if (net::PairingService* pairing = app.pairing()) pairing->revoke_all();
        app.models().clear();
        // The todos go from memory before their file, so no timer pass,
        // tool call or reminder action still running can write them back
        // before the restart.
        if (app.impl_->reminders) app.impl_->reminders->forget();
        // Notices carry todo titles and scene text; the list goes, and its
        // file with it (a save after this writes what is left: nothing, or
        // what was posted since).
        if (core::NotificationService* notes = app.notifications()) notes->clear_all();
        // The agent's conversation, archives, memory notes, todos and the
        // notifications (agent-core spec 3.5; mvp spec 5.1, 5.2).
        if (auto root = app.storage().write_root(core::Area::System); root) {
            for (const char* suffix : {"", ".tmp", ".new"}) {
                const std::string file = root.value() + "/notifications.json" + suffix;
                if (auto r = app.storage().remove_tree(file); !r) {
                    WLOG_E(kTag, "erasing the notifications failed: %s", r.error().message.c_str());
                }
            }
            if (auto r = app.storage().remove_tree(root.value() + "/agent"); !r) {
                WLOG_E(kTag, "erasing the conversations failed: %s", r.error().message.c_str());
            }
            if (auto r = app.storage().remove_tree(root.value() + "/memory"); !r) {
                WLOG_E(kTag, "erasing the memory notes failed: %s", r.error().message.c_str());
            }
            if (auto r = app.storage().remove_tree(root.value() + "/todo"); !r) {
                WLOG_E(kTag, "erasing the todos failed: %s", r.error().message.c_str());
            }
        }
    };

    // A full queue is retried for the whole budget, not given up on.
    const uint32_t budget = app.impl_->reset_erase_ms;
    Result<void> erased = app.impl_->run_io(
        [erase_everything, stage, finished] {
            int expected = kPending;
            if (stage->compare_exchange_strong(expected, kStarted)) erase_everything();
            finished->give();
        },
        budget, budget);
    int expected = kPending;
    if (!erased && stage->compare_exchange_strong(expected, kAbandoned)) {
        // Nothing was erased or announced: the device keeps running, and
        // whoever asked hears so.
        WLOG_E(kTag, "factory reset could not start (%s); nothing erased, not restarting",
               erased.error().message.c_str());
        app.impl_->reset_failed.store(true);
        publish_state(app, "failed");
        return;
    }
    const bool still_running = !erased;
    if (still_running) {
        WLOG_E(kTag, "factory reset erase did not finish in time: %s",
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
    // The job uses App's services. The restart only returns on a platform
    // that cannot restart for real (the simulator); there App::stop() must
    // not tear them down under a job still running.
    if (still_running) (void)finished->take(os::kForever);
}

bool start_factory_reset(App& app) {
    App::Impl& impl = *app.impl_;
    std::lock_guard<os::Mutex> lock(impl.reset_mutex);
    impl.reset_pending.store(false);
    if (impl.reset_task && impl.reset_failed.exchange(false)) {
        // That reset gave up without erasing and has returned: a new
        // request may try again.
        impl.reset_task->join();
        impl.reset_task.reset();
    }
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

void set_reset_erase_budget(App& app, uint32_t ms) { app.impl_->reset_erase_ms = ms; }

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
