#include "scene_wiring.hpp"

#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "wisp/core/log.hpp"
#include "wisp/os/os.hpp"
#include "wisp/ui/strings.hpp"

namespace wisp::app {

namespace {

constexpr const char* kTag = "scenes";
// How long stop() lets the worker finish what is queued (the closed
// scene's last flush among it) before it stops it anyway.
constexpr uint32_t kDrainMs = 5000;
// How often dev mode looks at the working directory (spec 6.4).
constexpr uint32_t kWatchMs = 500;

// The system notice for each SceneNoticeKind; its argument is the scene's
// name.
const char* notice_code(scene::SceneNoticeKind kind) {
    switch (kind) {
        case scene::SceneNoticeKind::EvictedMemory:
            return core::notice::kSceneEvictedMemory;
        case scene::SceneNoticeKind::EvictedCount:
            return core::notice::kSceneEvictedCount;
        case scene::SceneNoticeKind::Faulted:
            return core::notice::kSceneError;
        case scene::SceneNoticeKind::RestoreFailed:
            return core::notice::kRestoreFailed;
    }
    return core::notice::kSceneError;
}

}  // namespace

SceneWiring::SceneWiring(SceneWiringDeps deps) : deps_(deps) {}

SceneWiring::~SceneWiring() { stop(); }

void SceneWiring::start_storage() {
    if (auto r = worker_.start(); !r) {
        WLOG_E(kTag, "storage worker not started (%s); scenes are off", r.error().message.c_str());
        return;
    }
    core::SceneRepoConfig config;
    const Platform& platform = deps_.platform;
    auto internal = deps_.storage.internal_root();
    if (!platform.scenes_dir.empty()) {
        config.dev = true;
        config.code_root = platform.scenes_dir;
        if (!platform.scenes_data_dir.empty()) {
            config.data_root = platform.scenes_data_dir;
        } else if (internal) {
            config.data_root = internal.value() + "/scenes";
        } else {
            WLOG_E(kTag, "no internal storage for scene data; scenes are off");
            return;
        }
    } else if (internal) {
        config.code_root = internal.value() + "/scenes";
        config.data_root = config.code_root;
    } else {
        WLOG_E(kTag, "no internal storage (%s); scenes are off", internal.error().message.c_str());
        return;
    }
    repo_ = os::make_large<core::SceneRepository>(deps_.storage, std::move(config));
    if (!repo_) {
        WLOG_E(kTag, "no memory for the scene repository; scenes are off");
        return;
    }

    // On the worker, like every file job; nothing lists or opens a scene
    // before it is done.
    core::SceneRepository* repo = repo_.get();
    auto done = std::make_shared<os::Semaphore>(1, 0);
    if (!worker_.submit([repo, done] {
            if (auto r = repo->recover(); !r) {
                WLOG_W(kTag, "recovering interrupted installs failed: %s",
                       r.error().message.c_str());
            }
            done->give();
        })) {
        WLOG_W(kTag, "storage worker busy: interrupted installs not recovered");
        return;
    }
    done->take(os::kForever);
}

void SceneWiring::build_runtime(ui::UiRuntime& ui, ui::SystemLayer& system,
                                const hal::ScreenShape& shape, std::function<Value()> device_status,
                                std::function<void()> wake, core::WakeLockRegistry* locks,
                                core::NotificationService* notices) {
    if (!repo_) return;
    if (deps_.platform.http_client) {
        http_ = os::make_large<net::SceneHttp>(*deps_.platform.http_client);
        if (!http_) WLOG_W(kTag, "no memory for the scene HTTP proxy; wisp.http fails");
    }
    scene::SceneRuntimeDeps rt{.bus = deps_.bus,
                               .storage = deps_.storage,
                               .worker = worker_,
                               .repo = *repo_,
                               .ui = ui,
                               .system = system,
                               .shape = shape,
                               .http = http_.get(),
                               .device_status = std::move(device_status),
                               .wall_clock = {},
                               .locks = locks,
                               .on_notice = {},
                               .notifications = notices,
                               .background_list_path = {}};
    if (auto system = deps_.storage.write_root(core::Area::System)) {
        rt.background_list_path = system.value() + "/scenes/background.json";
    } else {
        WLOG_W(kTag, "no System area (%s); background scenes are not restored at boot",
               system.error().message.c_str());
    }
    if (notices != nullptr) {
        // On the scene task: post_system() only records the notice and
        // leaves saving it to the storage worker.
        rt.on_notice = [notices](const scene::SceneNotice& n) {
            notices->post_system(notice_code(n.kind), n.name);
        };
    }
    runtime_ = os::make_large<scene::SceneRuntime>(std::move(rt));
    if (!runtime_) {
        WLOG_E(kTag, "no memory for the scene runtime; scenes are off");
        http_.reset();
        return;
    }
    ui_ = &ui;
    system_ = &system;
    wake_ = std::move(wake);
}

void SceneWiring::start_runtime(std::function<bool()> standby) {
    if (!runtime_ || runtime_started_) return;
    if (http_) {
        if (auto r = http_->start(); !r) {
            WLOG_W(kTag, "scene HTTP proxy not started (%s); wisp.http fails",
                   r.error().message.c_str());
        }
    }
    if (auto r = runtime_->start(); !r) {
        WLOG_E(kTag, "scene runtime not started: %s", r.error().message.c_str());
        return;
    }
    runtime_started_ = true;
    // The state is read rather than taken from the payload, so a late
    // event cannot leave the scene hidden on a lit screen.
    scene::SceneRuntime* rt = runtime_.get();
    power_sub_ = deps_.bus.subscribe(
        "power.state", [rt, standby](const core::Event&) { rt->set_visible(!standby()); });
    if (standby()) rt->set_visible(false);
    if (repo_->dev()) start_watching();
}

void SceneWiring::restore_background() {
    if (runtime_started_) runtime_->restore_background();
}

void SceneWiring::start_watching() {
    const std::string& root = deps_.platform.scenes_dir;
    watcher_ = os::make_large<scene::DirWatcher>(deps_.storage, root);
    if (!watcher_) {
        WLOG_W(kTag, "no memory for the scene watcher; edits need a manual reload");
        return;
    }
    auto tick = [this] {
        if (poll_queued_.exchange(true)) return;
        if (!worker_.submit([this] { poll_scenes(); })) poll_queued_ = false;
    };
    // The first poll is the baseline, so take it now: edits made before the
    // first tick still count.
    tick();
    watch_timer_ = os::Timer::create("scene.watch", kWatchMs, true, tick);
    if (!watch_timer_) {
        WLOG_W(kTag, "no timer for the scene watcher; edits need a manual reload");
        return;
    }
    watch_timer_->start();
    WLOG_I(kTag, "watching %s for scene changes", root.c_str());
}

void SceneWiring::poll_scenes() {
    std::vector<std::string> changed = watcher_->poll();
    poll_queued_ = false;
    if (changed.empty() || !runtime_started_) return;
    const std::optional<std::string> front = runtime_->foreground();
    for (const std::string& id : changed) {
        if (!front || *front != id) {
            WLOG_D(kTag, "%s: files changed", id.c_str());
            continue;
        }
        WLOG_I(kTag, "%s: files changed; reloading", id.c_str());
        // On the worker this only queues the reload; how it went is logged
        // and published by the runtime.
        if (auto r = runtime_->reload(id); !r) {
            WLOG_W(kTag, "%s: reload not queued: %s", id.c_str(), r.error().message.c_str());
        }
    }
}

void SceneWiring::stop_runtime() {
    // A poll already queued finds the runtime stopped and reloads nothing.
    if (watch_timer_) watch_timer_->stop();
    if (power_sub_ != 0) {
        deps_.bus.unsubscribe(power_sub_);
        power_sub_ = 0;
    }
    if (runtime_) runtime_->stop();
    runtime_started_ = false;
    // App::stop() destroys the system layer next, while the admin server
    // still takes delete requests; request_delete() reads this on the UI
    // thread, so clearing it there orders it against every dialog.
    if (ui_ != nullptr && system_ != nullptr) ui_->run_blocking([this] { system_ = nullptr; });
}

void SceneWiring::stop() {
    stop_runtime();
    if (http_) http_->stop();
    if (!worker_.drain(kDrainMs)) {
        WLOG_W(kTag, "storage worker still busy after %u ms; stopping it",
               static_cast<unsigned>(kDrainMs));
    }
    worker_.stop();
}

Result<void> SceneWiring::request_delete(const std::string& id, bool with_data) {
    using R = Result<void>;
    if (!repo_) return R::err(ErrorCode::Unsupported, "scenes are off");
    if (repo_->dev()) return R::err(ErrorCode::Unsupported, "scenes are read-only in dev mode");
    if (ui_ == nullptr) return R::err(ErrorCode::Unsupported, "no screen to confirm on");
    auto found = repo_->removal(id, with_data);
    if (!found) return R::err(found.error().code, found.error().message);

    std::string text = ui::tr_fill(ui::StrId::SceneDeletePrompt, found.value().name);
    if (with_data) {
        text += "\n";
        text += ui::tr(ui::StrId::SceneDeleteWithData);
    }
    // The dialog rule (one at a time) lives in the system layer; ask it on
    // its own thread and report what it said.
    R result = R::ok();
    ui_->run_blocking([&] {
        if (system_ == nullptr) {
            result = R::err(ErrorCode::Busy, "scenes are shutting down");
            return;
        }
        lv_obj_t* box = nullptr;
        if (!system_->confirm_open()) {
            box = system_->open_confirm(
                ui::StrId::SceneDeleteTitle, text, [this, id, with_data](bool yes) {
                    if (!yes) {
                        WLOG_I(kTag, "%s: delete declined on the device", id.c_str());
                        return;
                    }
                    // A scene that is up closes first; its last flush is on
                    // disk before the files go. From here until the files
                    // are gone the scene is not opened again.
                    repo_->begin_removing(id);
                    auto then = [this, id, with_data](Result<void> closed) {
                        if (!closed) {
                            WLOG_W(kTag, "%s: not deleted: the scene did not close: %s", id.c_str(),
                                   closed.error().message.c_str());
                            repo_->end_removing(id);
                            return;
                        }
                        if (!worker_.submit([this, id, with_data] {
                                remove(id, with_data);
                                repo_->end_removing(id);
                            })) {
                            WLOG_W(kTag, "%s: not deleted: storage worker busy", id.c_str());
                            repo_->end_removing(id);
                        }
                    };
                    if (runtime_started_) {
                        runtime_->close_async(id, std::move(then));
                    } else {
                        then(Result<void>::ok());
                    }
                });
        }
        if (box == nullptr) {
            result = R::err(ErrorCode::Busy, "another dialog is open");
            return;
        }
        if (wake_) wake_();
    });
    if (result) WLOG_I(kTag, "%s: delete waiting for confirmation on the screen", id.c_str());
    return result;
}

void SceneWiring::remove(const std::string& id, bool with_data) {
    if (auto r = repo_->remove(id, with_data); !r) {
        WLOG_W(kTag, "%s: delete failed: %s", id.c_str(), r.error().message.c_str());
        return;
    }
    publish(id, "deleted");
}

void SceneWiring::publish(const std::string& id, const char* state) {
    core::Event e;
    e.topic = "scene.state";
    e.source = "scene";
    e.payload = {{"id", Value(id)}, {"state", Value(state)}};
    deps_.bus.publish(std::move(e));
}

bool SceneWiring::list_async(std::function<void(std::vector<SceneEntry>)> on_ui) {
    if (!repo_ || ui_ == nullptr) return false;
    core::SceneRepository* repo = repo_.get();
    ui::UiRuntime* ui = ui_;
    return worker_.submit([repo, ui, on_ui = std::move(on_ui)]() mutable {
        auto listed = repo->list();
        if (!listed) {
            WLOG_W(kTag, "listing scenes failed: %s", listed.error().message.c_str());
            return;
        }
        std::vector<SceneEntry> entries;
        entries.reserve(listed.value().size());
        for (const core::SceneInfo& info : listed.value()) {
            entries.push_back({info.manifest.id, info.manifest.name, info.manifest.version});
        }
        if (!ui->post([on_ui = std::move(on_ui), entries = std::move(entries)]() mutable {
                on_ui(std::move(entries));
            })) {
            WLOG_W(kTag, "scene list not shown (UI queue full)");
        }
    });
}

}  // namespace wisp::app
