#include "scene_wiring.hpp"

#include <utility>

#include "wisp/core/log.hpp"
#include "wisp/os/os.hpp"
#include "wisp/ui/strings.hpp"

namespace wisp::app {

namespace {

constexpr const char* kTag = "scenes";
// How long stop() lets the worker finish what is queued (the closed
// scene's last flush among it) before it stops it anyway.
constexpr uint32_t kDrainMs = 5000;

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
    repo_ = std::make_unique<core::SceneRepository>(deps_.storage, std::move(config));

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
                                std::function<void()> wake) {
    if (!repo_) return;
    ui_ = &ui;
    system_ = &system;
    wake_ = std::move(wake);
    if (deps_.platform.http_client) {
        http_ = std::make_unique<net::SceneHttp>(*deps_.platform.http_client);
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
                               .wall_clock = {}};
    runtime_ = std::make_unique<scene::SceneRuntime>(std::move(rt));
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
}

void SceneWiring::stop_runtime() {
    if (power_sub_ != 0) {
        deps_.bus.unsubscribe(power_sub_);
        power_sub_ = 0;
    }
    if (runtime_) runtime_->stop();
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
    if (ui_ == nullptr || system_ == nullptr) {
        return R::err(ErrorCode::Unsupported, "no screen to confirm on");
    }
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
        lv_obj_t* box = nullptr;
        if (!system_->confirm_open()) {
            box = system_->open_confirm(
                ui::StrId::SceneDeleteTitle, text, [this, id, with_data](bool yes) {
                    if (!yes) {
                        WLOG_I(kTag, "%s: delete declined on the device", id.c_str());
                        return;
                    }
                    // A scene that is up closes first; its last flush is on
                    // disk before the files go.
                    auto then = [this, id, with_data](Result<void> closed) {
                        if (!closed) {
                            WLOG_W(kTag, "%s: not deleted: the scene did not close: %s", id.c_str(),
                                   closed.error().message.c_str());
                            return;
                        }
                        if (!worker_.submit([this, id, with_data] { remove(id, with_data); })) {
                            WLOG_W(kTag, "%s: not deleted: storage worker busy", id.c_str());
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
