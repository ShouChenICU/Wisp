#include "scene_wiring.hpp"

#include <algorithm>
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
    config.worker = &worker_;
    const std::string data_root = config.data_root;
    repo_ = os::make_large<core::SceneRepository>(deps_.storage, std::move(config));
    if (!repo_) {
        WLOG_E(kTag, "no memory for the scene repository; scenes are off");
        return;
    }
    data_root_ = data_root;

    // wisp.files. StorageManager renames and removes only inside a storage
    // mount, which a dev-mode data root need not be, so the files there go
    // under <int>/scenes as on the device.
    if (internal) {
        core::SceneFilesConfig files_config;
        files_config.data_root = internal.value() + "/scenes";
        files_config.submit_io = [this](std::function<void()> job) {
            return worker_.submit(std::move(job));
        };
        // The repository never calls into SceneFiles, so taking its lock
        // under SceneFiles' locks cannot deadlock.
        files_config.declared = [repo =
                                     repo_.get()](std::string_view id) -> Result<core::FilesWant> {
            Result<core::SceneManifest> m = repo->manifest(id);
            if (!m) return Result<core::FilesWant>::err(m.error().code, m.error().message);
            return Result<core::FilesWant>::ok(m.value().files);
        };
        files_ = os::make_large<core::SceneFiles>(deps_.storage, std::move(files_config));
        if (!files_) WLOG_W(kTag, "no memory for scene files; wisp.files is off");
    }
    core::SceneFiles* files = files_.get();
    if (files != nullptr) {
        // The card's scenes are counted again on the worker (spec 7.2).
        for (const char* topic : {"storage.sd.inserted", "storage.sd.removed"}) {
            sd_subs_.push_back(deps_.bus.subscribe(topic, [this, files](const core::Event&) {
                if (!worker_.submit([files] { files->on_sd_changed(); }))
                    WLOG_W(kTag, "storage worker busy: scene files on the card not recounted");
            }));
        }
    }

    // On the worker, like every file job; nothing lists or opens a scene
    // before it is done.
    core::SceneRepository* repo = repo_.get();
    // An unbounded wait, so the job may use these plain pointers.
    if (auto r = worker_.run_sync(
            [repo, files] {
                if (auto r = repo->recover(); !r) {
                    WLOG_W(kTag, "recovering interrupted installs failed: %s",
                           r.error().message.c_str());
                }
                // Usage and interrupted downloads, once the packages (and
                // the files/ they carry over) are in place.
                if (files != nullptr) {
                    if (auto r = files->scan(); !r)
                        WLOG_W(kTag, "counting scene files failed: %s", r.error().message.c_str());
                }
            },
            os::kForever);
        !r) {
        WLOG_W(kTag, "storage worker busy: interrupted installs not recovered");
    }
}

void SceneWiring::build_runtime(ui::UiRuntime& ui, ui::SystemLayer& system,
                                const hal::ScreenShape& shape, std::function<Value()> device_status,
                                std::function<void()> wake, core::WakeLockRegistry* locks,
                                core::NotificationService* notices, core::SensorService* sensors) {
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
                               .background_list_path = {},
                               .sensors = sensors,
                               .files = files_.get()};
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
    notices_ = notices;
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
    if (ui_ != nullptr && system_ != nullptr) {
        ui_->run_blocking([this] {
            system_ = nullptr;
            *ui_alive_ = false;
        });
    }
}

void SceneWiring::stop() {
    stop_runtime();
    for (core::SubscriptionId id : sd_subs_) deps_.bus.unsubscribe(id);
    sd_subs_.clear();
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
    const std::string name = found.value().name;
    // The dialog rule (one at a time) lives in the system layer; ask it on
    // its own thread and report what it said.
    R result = R::ok();
    ui_->run_blocking([&] { result = open_delete_dialog(id, name, with_data); });
    return result;
}

Result<void> SceneWiring::ask_delete(const std::string& id, bool with_data) {
    using R = Result<void>;
    if (!repo_) return R::err(ErrorCode::Unsupported, "scenes are off");
    if (repo_->dev()) return R::err(ErrorCode::Unsupported, "scenes are read-only in dev mode");
    if (ui_ == nullptr) return R::err(ErrorCode::Unsupported, "no screen to confirm on");
    core::SceneRepository* repo = repo_.get();
    ui::UiRuntime* ui = ui_;
    const bool queued = worker_.submit([this, repo, ui, id, with_data, alive = ui_alive_] {
        auto found = repo->removal(id, with_data);
        if (!found) {
            WLOG_W(kTag, "%s: nothing to delete (%s)", id.c_str(), found.error().message.c_str());
            return;
        }
        if (!ui->post([this, alive, id, name = found.value().name, with_data] {
                if (!*alive) return;
                if (auto r = open_delete_dialog(id, name, with_data); !r)
                    WLOG_W(kTag, "%s: delete not asked (%s)", id.c_str(),
                           r.error().message.c_str());
            })) {
            WLOG_W(kTag, "%s: delete not asked (UI queue full)", id.c_str());
        }
    });
    return queued ? R::ok() : R::err(ErrorCode::Busy, "storage queue full");
}

Result<void> SceneWiring::open_delete_dialog(const std::string& id, const std::string& name,
                                             bool with_data) {
    using R = Result<void>;
    if (system_ == nullptr) return R::err(ErrorCode::Busy, "scenes are shutting down");
    std::string text = ui::tr_fill(ui::StrId::SceneDeletePrompt, name);
    if (with_data) {
        text += "\n";
        text += ui::tr(ui::StrId::SceneDeleteWithData);
    }
    lv_obj_t* box = nullptr;
    if (!system_->confirm_open()) {
        box = system_->open_confirm(
            ui::StrId::SceneDeleteTitle, text,
            [this, id, name, with_data](bool yes) {
                if (!yes) {
                    WLOG_I(kTag, "%s: delete declined on the device", id.c_str());
                    return;
                }
                delete_named(id, name, with_data, [this, id, name](Result<void> r) {
                    if (!r) delete_failed(id, name, r.error().message.c_str());
                });
            },
            ui::ButtonRole::Danger);
    }
    if (box == nullptr) return R::err(ErrorCode::Busy, "another dialog is open");
    if (wake_) wake_();
    WLOG_I(kTag, "%s: delete waiting for confirmation on the screen", id.c_str());
    return R::ok();
}

void SceneWiring::delete_scene(const std::string& id, bool with_data,
                               std::function<void(Result<void>)> done) {
    using R = Result<void>;
    if (!repo_) return done(R::err(ErrorCode::Unsupported, "scenes are off"));
    if (repo_->dev())
        return done(R::err(ErrorCode::Unsupported, "scenes are read-only in dev mode"));
    auto found = repo_->removal(id, with_data);
    if (!found) return done(R::err(found.error().code, found.error().message));
    delete_named(id, found.value().name, with_data, std::move(done));
}

void SceneWiring::delete_named(const std::string& id, const std::string& name, bool with_data,
                               std::function<void(Result<void>)> done) {
    // A scene that is up closes first; its last flush is on disk before
    // the files go. From here until the files are gone the scene is not
    // opened again.
    repo_->begin_removing(id);
    auto then = [this, id, name, with_data, done = std::move(done)](Result<void> closed) {
        if (!closed) {
            repo_->end_removing(id);
            done(Result<void>::err(closed.error().code,
                                   "the scene did not close: " + closed.error().message));
            return;
        }
        if (!worker_.submit([this, id, name, with_data, done] {
                Result<void> r = remove(id, name, with_data);
                repo_->end_removing(id);
                done(std::move(r));
            })) {
            repo_->end_removing(id);
            done(Result<void>::err(ErrorCode::Busy, "storage worker busy"));
        }
    };
    if (runtime_started_) {
        runtime_->close_async(id, std::move(then));
    } else {
        then(Result<void>::ok());
    }
}

void SceneWiring::delete_failed(const std::string& id, const std::string& name, const char* why) {
    WLOG_W(kTag, "%s: delete failed: %s", id.c_str(), why);
    // post_system() only records the notice (no flash): safe on the scene
    // task and the UI thread, where a failed close or submit lands.
    if (notices_ != nullptr) notices_->post_system(core::notice::kSceneDeleteFailed, name);
}

Result<void> SceneWiring::remove(const std::string& id, const std::string& name, bool with_data) {
    Result<void> removed = repo_->remove(id, with_data);
    // The repository removed the files with the data; this forgets their
    // location and usage. After a failed delete only if it got past
    // removing scene.json (the scene is no longer installed): a delete
    // that failed earlier leaves the scene, and its files, as they were.
    bool gone = static_cast<bool>(removed);
    if (!removed && removed.error().code != ErrorCode::Unsupported) {
        Result<core::SceneManifest> m = repo_->manifest(id);
        gone = !m && m.error().code == ErrorCode::NotFound;
    }
    if (with_data && files_ && gone) {
        if (auto r = files_->remove_all(id); !r)
            WLOG_W(kTag, "%s: removing its files failed: %s", id.c_str(),
                   r.error().message.c_str());
    }
    if (!removed) return removed;
    // A retry that succeeds supersedes an earlier "could not be deleted"
    // notice for the same scene: it must not keep telling the user to try
    // again, including after a reboot (the notice is persisted).
    if (notices_ != nullptr) {
        notices_->dismiss(core::kSystemSource,
                          std::string(core::notice::kSceneDeleteFailed) + ":" + name,
                          core::DismissReason::Dismiss);
    }
    publish(id, "deleted");
    return removed;
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

bool SceneWiring::storage_status_async(ui::UiRuntime& ui,
                                       std::function<void(core::StorageStatus)> on_ui) {
    core::StorageManager* storage = &deps_.storage;
    ui::UiRuntime* target = &ui;
    return worker_.submit([storage, target, on_ui = std::move(on_ui)]() mutable {
        core::StorageStatus status = storage->status();
        if (!target->post([on_ui = std::move(on_ui), status = std::move(status)]() mutable {
                on_ui(std::move(status));
            })) {
            WLOG_W(kTag, "storage status not shown (UI queue full)");
        }
    });
}

std::vector<shell::ShellScene> SceneWiring::shell_scenes() {
    std::vector<shell::ShellScene> out;
    core::SceneRepository* repo = repository();
    if (repo == nullptr) return out;
    auto listed = repo->list();
    if (!listed) {
        WLOG_W(kTag, "listing scenes failed: %s", listed.error().message.c_str());
        return out;
    }
    std::vector<std::string> background;
    if (scene::SceneRuntime* rt = runtime())
        for (const scene::BackgroundScene& b : rt->background_scenes()) background.push_back(b.id);
    core::SceneFiles* sf = files_.get();
    for (const core::SceneInfo& info : listed.value()) {
        const core::SceneManifest& m = info.manifest;
        shell::ShellScene sc;
        sc.id = m.id;
        sc.name = m.name;
        sc.background = std::find(background.begin(), background.end(), m.id) != background.end();
        if (sf != nullptr && !sf->sd_online()) {
            if (auto fi = sf->info(m.id); fi) {
                const bool on_card = fi.value().location == core::FilesLocation::Sd ||
                                     (fi.value().location == core::FilesLocation::None &&
                                      fi.value().want == core::FilesWant::Sd);
                sc.available = !on_card;
            } else {
                // A scene that wants the card and has no files yet is
                // refused outright while the card is out.
                sc.available = fi.error().code != ErrorCode::StorageUnavailable;
            }
        }
        if (auto rm = repo->removal(m.id, true); rm) sc.has_data = rm.value().data;
        if (!m.icon.empty()) {
            if (auto icon = repo->read_asset(m.id, m.icon, shell::kIconMaxBytes); icon) {
                sc.icon_png = std::make_shared<const os::LargeBuffer>(std::move(icon.value()));
            } else {
                WLOG_W(kTag, "%s: icon not shown (%s)", m.id.c_str(), icon.error().message.c_str());
            }
        }
        out.push_back(std::move(sc));
    }
    return out;
}

}  // namespace wisp::app
