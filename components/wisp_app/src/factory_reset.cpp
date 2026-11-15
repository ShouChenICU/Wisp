#include "factory_reset.hpp"

#include <algorithm>
#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <type_traits>
#include <utility>

#include "app_impl.hpp"
#include "wisp/base/error.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/core/log.hpp"
#include "wisp/core/notification_service.hpp"
#include "wisp/core/reminders.hpp"
#include "wisp/core/storage_router.hpp"
#include "wisp/core/topics.hpp"
#include "wisp/net/pairing_service.hpp"
#include "wisp/net/wifi_service.hpp"
#include "wisp/os/os.hpp"

namespace wisp::app {

static_assert(std::is_trivially_copyable_v<ResetStatus>, "a set() of the status must not allocate");

namespace {

constexpr const char* kTag = "reset";
constexpr const char* kTopic = core::topics::kSystemFactoryReset;
// How long the "done" event gets to reach the browser before the
// revocation closes its socket. Bounded: a stalled socket must not keep
// the device from resetting.
constexpr uint32_t kFlushMs = 500;
// How long the erase may take on the flash thread (an NVS erase of every
// namespace and a few file deletions; spec 3.7).
constexpr uint32_t kEraseMs = 30000;
// Waits for the network and the flash thread; touches no flash itself.
constexpr size_t kResetStackBytes = 6144;

const char* phase_name(ResetStatus::Phase phase) {
    switch (phase) {
        case ResetStatus::Phase::Idle:
            return "";
        case ResetStatus::Phase::Requested:
            return "requested";
        case ResetStatus::Phase::Cancelled:
            return "cancelled";
        case ResetStatus::Phase::Done:
            return "done";
        case ResetStatus::Phase::Failed:
            return "failed";
    }
    return "";
}

// Moves the reset status (App::Impl::reset_status) to `phase`; its mirror
// publishes the event on the caller's thread, at this point of its flow,
// as the publish here used to. Never an equal value, so never a lost
// event: every call moves `seq`, even one that repeats the last state.
void publish_state(core::StateCell<ResetStatus>& cell, ResetStatus::Phase phase) {
    cell.update([phase](ResetStatus& s) {
        s.phase = phase;
        ++s.seq;
    });
}

}  // namespace

void mirror_reset_status(core::EventBus& bus, const ResetStatus&, const ResetStatus& after) {
    if (after.phase == ResetStatus::Phase::Idle) return;
    bus.publish(core::Event{kTopic, kFactoryResetSource, 0,
                            Properties{{"state", Value(phase_name(after.phase))}}});
}

const core::StateCell<ResetStatus>& reset_status(App& app) { return app.impl_->reset_status; }

void request_factory_reset(App& app) {
    publish_state(app.impl_->reset_status, ResetStatus::Phase::Requested);
}

void cancel_factory_reset(App& app) {
    WLOG_I(kTag, "factory reset cancelled on the device");
    publish_state(app.impl_->reset_status, ResetStatus::Phase::Cancelled);
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
        publish_state(app.impl_->reset_status, ResetStatus::Phase::Done);
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
        // The todos and the memory notes go from memory before their
        // files, so no timer pass, tool call or reminder action still
        // running can write them back before the restart.
        if (app.impl_->reminders) app.impl_->reminders->forget();
        if (app.impl_->agent) app.impl_->agent->memory()->forget();
        // The scene order, likewise, so a save still pending cannot bring
        // the file back.
        if (app.impl_->recents) app.impl_->recents->forget_all();
        // Notices carry todo titles and scene text; the list goes, and its
        // file with it (a save after this writes what is left: nothing, or
        // what was posted since).
        if (core::NotificationService* notes = app.notifications()) notes->clear_all();
        // The agent's conversation, archives, memory notes, todos, the
        // notifications and the scene order (agent-core spec 3.5; mvp spec
        // 5.1, 5.2, 6.4).
        // lua.run's files (agent-tools spec 4.2): the agent's, not a
        // scene's, in every Library root (the card's while it is in).
        for (const std::string& root : app.storage().roots(core::Area::Library)) {
            if (auto r = app.storage().remove_tree(root + "/agent"); !r) {
                WLOG_E(kTag, "erasing the agent's files failed: %s", r.error().message.c_str());
            }
        }
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
            if (auto r = app.storage().remove_tree(root.value() + "/shell"); !r) {
                WLOG_E(kTag, "erasing the scene order failed: %s", r.error().message.c_str());
            }
        }
    };

    // The erase touches the card too, from wisp.fs: the card's worker
    // finishes what it has queued and stops first, so no card job runs
    // beside the erase; card work from here on queues behind it on
    // wisp.fs (flash-thread spec 3.7). Not restarted if the reset fails:
    // card work then simply stays on wisp.fs.
    if (core::StorageRouter* router = app.storage_router()) router->stop_sd();
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
        app.impl_->reset_running.store(false);
        publish_state(app.impl_->reset_status, ResetStatus::Phase::Failed);
        return;
    }
    // The wipe erased Settings without SystemSettings::set(), so the
    // settings cell still holds the old values: seed it again from storage
    // (the defaults, unless the erase failed), publishing nothing, as
    // nothing was published for the erase before the cell. Here on
    // "wisp.reset", once the whole wipe job has returned, never inside it:
    // seed_cell() takes the settings lock, which a set() may hold while it
    // waits for the flash thread (no settings cache), so on the flash
    // thread it could stall the job before its later steps; and it must
    // not sit between wipe steps. It reads through the settings front: from
    // the cache in memory, or inline without a storage worker, as the erase
    // ran. Without the cache but with a worker, every settings call (the
    // seed's read, and a set() holding the lock) waits for the flash thread
    // with no bound, so the seed is skipped there: the restart comes next
    // and must not wait on it.
    const bool still_running = !erased;
    if (still_running) {
        WLOG_E(kTag, "factory reset erase did not finish in time: %s",
               erased.error().message.c_str());
    } else if (!app.impl_->settings_cache && app.storage_worker() != nullptr) {
        WLOG_W(kTag, "settings read straight from flash; their cell is not reseeded");
    } else {
        app.impl_->system_settings.seed_cell();
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
    // Before the spawn: the task may give up (and clear it) before the
    // spawn returns, and a request seen after "done" must find it set.
    impl.reset_running.store(true);
    impl.reset_task =
        impl.reset_spawner ? impl.reset_spawner(config, body) : os::Task::spawn(config, body);
    if (!impl.reset_task) {
        WLOG_E(kTag, "factory reset task could not start; nothing was erased");
        // Before "cancelled", so the screen it reaches lets a new request
        // ask again.
        impl.reset_running.store(false);
        publish_state(app.impl_->reset_status, ResetStatus::Phase::Cancelled);
        return false;
    }
    return true;
}

bool factory_reset_running(App& app) { return app.impl_->reset_running.load(); }

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
    app.impl_->reset_running.store(false);
    // A reset that failed in this run says nothing about the next one.
    app.impl_->reset_failed.store(false);
}

}  // namespace wisp::app
