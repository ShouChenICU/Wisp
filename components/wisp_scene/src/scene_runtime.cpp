#include "wisp/scene/scene_runtime.hpp"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <utility>

#include "scene_instance.hpp"
#include "scene_message.hpp"
#include "wisp/base/json.hpp"
#include "wisp/core/log.hpp"
#include "wisp/core/scene_store.hpp"
#include "wisp/hal/display_geometry.hpp"
#include "wisp/net/scene_http.hpp"
#include "wisp/scene/minute_ticker.hpp"
#include "wisp/scene/scene_view.hpp"
#include "wisp/ui/strings.hpp"

namespace wisp::scene {

namespace {

constexpr const char* kTag = "scene";
// How long open(), close(), reload() and check_sources() wait for the
// scene task, and how long they wait for room in its queue.
constexpr uint32_t kWaitMs = 10000;
constexpr uint32_t kPushMs = 1000;
// A message that takes longer than this is logged.
constexpr uint64_t kSlowMs = 50;
// How often, and how far apart, the background list is offered to a full
// scene queue at boot.
constexpr int kRestoreAttempts = 10;
constexpr uint32_t kRestoreRetryMs = 100;

// "scene.<id>": the tag of the scene's own log.
std::string scene_tag(std::string_view id) {
    std::string tag = "scene.";
    tag.append(id);
    return tag;
}

// The ids of the background list at `path` (scene-device spec 3.6):
// {"ids": [...]}, each a non-empty string, once. A missing file is an
// empty list; an unreadable one is logged and empty too. On the worker.
std::vector<std::string> read_background_list(core::StorageManager& storage,
                                              const std::string& path) {
    std::vector<std::string> ids;
    auto data = storage.read_file(path);
    if (!data) {
        if (data.error().code != ErrorCode::NotFound)
            WLOG_W(kTag, "background list not read: %s", data.error().message.c_str());
        return ids;
    }
    auto v = from_json(
        std::string_view(reinterpret_cast<const char*>(data.value().data()), data.value().size()));
    const Value* list = v ? v.value().find("ids") : nullptr;
    if (list == nullptr || list->type() != Value::Type::List) {
        WLOG_W(kTag, "background list unreadable; nothing restored");
        return ids;
    }
    for (const Value& id : list->as_list()) {
        if (id.type() != Value::Type::String || id.as_string().empty()) continue;
        if (std::find(ids.begin(), ids.end(), id.as_string()) == ids.end())
            ids.push_back(id.as_string());
    }
    return ids;
}

os::LargeUnique<SceneMessage> make_message(std::string id, uint32_t instance, msg::Body body) {
    auto m = os::make_large<SceneMessage>();
    if (m) {
        m->scene_id = std::move(id);
        m->instance = instance;
        m->body = std::move(body);
    }
    return m;
}

}  // namespace

// Lets callbacks that may outlive the runtime (worker jobs, store timers)
// reach it while it exists.
struct SceneRuntime::Link {
    os::Mutex mu;
    SceneRuntime* rt = nullptr;
};

const char* to_string(LaunchReason r) {
    switch (r) {
        case LaunchReason::User:
            return "user";
        case LaunchReason::Boot:
            return "boot";
        case LaunchReason::Notification:
            return "notification";
    }
    return "user";
}

// One scene's downloads: begun on the worker while `open`, cancelled all
// at once by cancel_files(). The lock is held across a download's
// SceneHttp::submit(), so once cancel_files() has closed the gate every
// request of the scene is either cancelled here or not sent at all.
struct SceneRuntime::FilesGate {
    os::Mutex mu;
    bool open = true;
    std::vector<std::shared_ptr<core::FileDownload>> active;

    void forget(const core::FileDownload* dl) {
        std::lock_guard<os::Mutex> lock(mu);
        std::erase_if(active, [dl](const auto& p) { return p.get() == dl; });
    }
};

struct SceneRuntime::Slot {
    std::string id;
    std::string name;
    uint32_t instance = 0;
    Value args;
    std::shared_ptr<const core::ScenePackage> package;
    std::shared_ptr<core::SceneStore> store;
    // Null once the scene faulted (in front: its error panel is up).
    os::LargeUnique<SceneInstance> scene;
    // scene.json's "background": going back sends it there.
    bool declares_background = false;
    // In the background: no view, its batches are dropped.
    bool background = false;
    // scene.on_show() ran and on_hide() has not.
    bool shown = false;
    // os::now_ms() when it last left the front (0: never in front), and
    // when it last went to the background.
    uint64_t last_front_ms = 0;
    uint64_t background_since_ms = 0;
    // Orders scenes that left the front in the same millisecond (and
    // those never in front): taken each time it goes to the background.
    uint64_t seq = 0;
    // Starting from a boot launch: a failure is a RestoreFailed notice.
    bool restoring = false;
    // What wisp.scene.launch() reports.
    LaunchReason reason = LaunchReason::User;
    MinuteTicker ticker;
    // Its downloads in flight (wire_files()); shared with the jobs.
    std::shared_ptr<FilesGate> files_gate;
    // What the scene last saw, for "battery" and "lang".
    bool has_battery = false;
    int64_t battery = 0;
    bool charging = false;
    std::string lang;
};

struct SceneRuntime::UiSide {
    std::unique_ptr<SceneView> view;
    uint32_t instance = 0;
    // The show_view() token of this view (ui_view_).
    uint64_t token = 0;
    // The screen the view replaced, loaded again when it goes.
    lv_obj_t* home = nullptr;
    // Loaded instead when `home` is gone by then.
    lv_obj_t* stand_in = nullptr;
};

SceneRuntime::SceneRuntime(SceneRuntimeDeps deps, SceneRuntimeConfig config)
    : deps_(std::move(deps)),
      config_(config),
      pool_(config.vm_budget),
      queue_(config.queue_capacity),
      link_(std::allocate_shared<Link>(os::LargeAllocator<Link>())) {
    link_->rt = this;
    config_.vm.pool = &pool_;
    config_.max_background = std::min(config_.max_background, kMaxBackground);
}

void SceneRuntime::seed_utc_offset(int minutes) {
    if (minutes < -720 || minutes > 840) return;
    int unset = kUnsetOffset;
    utc_offset_min_.compare_exchange_strong(unset, minutes);
}

void SceneRuntime::seed_time_24h(bool h24) {
    int unset = -1;
    time_24h_.compare_exchange_strong(unset, h24 ? 1 : 0);
}

int SceneRuntime::utc_offset_min() const {
    const int v = utc_offset_min_.load();
    return v == kUnsetOffset ? deps_.utc_offset_min : v;
}

SceneRuntime::~SceneRuntime() {
    stop();
    drain_queue();
}

os::TaskConfig SceneRuntime::task_config(const SceneRuntimeConfig& c) {
    os::TaskConfig t;
    t.name = "wisp.scene";
    t.stack_bytes = c.stack_bytes;
    t.priority = c.priority;
    t.core = -1;
    t.flash_access = false;
    return t;
}

Result<void> SceneRuntime::start() {
    if (started_) return Result<void>::err(ErrorCode::Busy, "scene runtime already started");
    started_ = true;
    ui_side_ = os::make_large<UiSide>();
    if (!ui_side_) return Result<void>::err(ErrorCode::NoMemory, "cannot make the scene UI state");
    deps_.ui.set_frame_hook([this] { ui_frame(); });
    running_ = true;
    task_ = os::Task::spawn(task_config(config_), [this] { run(); });
    if (!task_) {
        running_ = false;
        deps_.ui.set_frame_hook({});
        return Result<void>::err(ErrorCode::NoMemory, "cannot start the scene task");
    }
    // The bus's dispatcher only queues a message: the scene task reads
    // the state itself, so a dropped or late event cannot leave stale data.
    power_sub_ = deps_.bus.subscribe("power.charging.changed", [this](const core::Event&) {
        push(make_message("", 0, msg::Device{DeviceEvent::Battery}), 0);
    });
    settings_sub_ = deps_.bus.subscribe("system.settings", [this](const core::Event& e) {
        auto tz = e.payload.find("utc_offset_min");
        if (tz != e.payload.end() && tz->second.type() == Value::Type::Int &&
            tz->second.as_int() >= -720 && tz->second.as_int() <= 840) {
            const int now = static_cast<int>(tz->second.as_int());
            int before = utc_offset_min_.exchange(now);
            if (before == kUnsetOffset) before = deps_.utc_offset_min;
            if (before != now && !push(make_message("", 0, msg::Device{DeviceEvent::Minute}), 0) &&
                running_)
                WLOG_W(kTag, "time zone change not passed on now (scene queue full): next minute");
        }
        auto h24 = e.payload.find("time_24h");
        if (h24 != e.payload.end() && h24->second.type() == Value::Type::Bool) {
            const int now = h24->second.as_bool() ? 1 : 0;
            int before = time_24h_.exchange(now);
            if (before < 0) before = 1;
            if (before != now && !push(make_message("", 0, msg::Device{DeviceEvent::Minute}), 0) &&
                running_)
                WLOG_W(kTag,
                       "time format change not passed on now (scene queue full): next minute");
        }
        auto it = e.payload.find("language");
        if (it == e.payload.end() || it->second.type() != Value::Type::String) return;
        auto lang = ui::parse_lang(it->second.as_string());
        if (!lang) return;
        lang_.store(static_cast<int>(*lang));
        if (!push(make_message("", 0, msg::Device{DeviceEvent::Lang}), 0) && running_)
            WLOG_W(kTag, "language change not passed on now (scene queue full): next minute");
    });
    // A sync may have moved the wall clock: a clock should redraw now, not
    // at the next minute boundary of the new time (clock_set()).
    time_sub_ = deps_.bus.subscribe("system.time", [this](const core::Event& e) {
        auto state = e.payload.find("state");
        if (state == e.payload.end() || state->second.type() != Value::Type::String ||
            state->second.as_string() != "synced")
            return;
        if (!push(make_message("", 0, msg::ClockSet{}), 0) && running_)
            WLOG_W(kTag, "time sync not passed on now (scene queue full): next minute");
    });
    return Result<void>::ok();
}

void SceneRuntime::stop() {
    if (!started_ || !running_.exchange(false)) return;
    deps_.bus.unsubscribe(power_sub_);
    deps_.bus.unsubscribe(settings_sub_);
    deps_.bus.unsubscribe(time_sub_);
    if (task_) {
        // The task keeps taking messages, so this always gets in.
        auto quit = make_message("", 0, msg::Quit{});
        SceneMessage* raw = quit.release();
        if (raw != nullptr && !queue_.push(raw, os::kForever)) {
            os::LargeUnique<SceneMessage> dropped(raw);
        }
        task_->join();
        task_.reset();
    }
    {
        std::lock_guard<os::Mutex> lock(link_->mu);
        link_->rt = nullptr;
    }
    drain_queue();
    {
        std::lock_guard<os::Mutex> lock(delayed_mu_);
        delayed_.clear();
    }
    deps_.ui.set_frame_hook({});
    // Also a fence: every UI job the scene task handed over has run once
    // this returns, and the old frame hook is no longer running.
    deps_.ui.run_blocking([this] {
        run_ui_jobs();
        ui_drop_view(0);
    });
}

void SceneRuntime::drain_queue() {
    SceneMessage* raw = nullptr;
    while (queue_.pop(raw, 0)) {
        os::LargeUnique<SceneMessage> m(raw);
        fail_message(*m, Result<void>::err(ErrorCode::Busy, "scene runtime stopped"));
    }
}

// --- Any thread --------------------------------------------------------------

bool SceneRuntime::push(os::LargeUnique<SceneMessage> m, uint32_t timeout_ms) {
    if (!m) return false;
    if (!running_) {
        fail_message(*m, Result<void>::err(ErrorCode::Busy, "scene runtime not running"));
        return false;
    }
    SceneMessage* raw = m.release();
    if (!queue_.push(raw, timeout_ms)) {
        os::LargeUnique<SceneMessage> back(raw);
        fail_message(*back, Result<void>::err(ErrorCode::Busy, "scene queue full"));
        return false;
    }
    return true;
}

bool post_scene_message(SceneRuntime& rt, os::LargeUnique<SceneMessage> m) {
    return rt.push(std::move(m), 0);
}

size_t pending_patches(const SceneRuntime& rt) { return rt.patches_.pending(); }

uint32_t foreground_instance(const SceneRuntime& rt) {
    std::lock_guard<os::Mutex> lock(rt.status_mu_);
    return rt.foreground_instance_;
}

size_t pending_ui_jobs(SceneRuntime& rt) {
    std::lock_guard<os::Mutex> lock(rt.ui_jobs_mu_);
    return rt.ui_jobs_.size();
}

uint32_t scene_instance(const SceneRuntime& rt, const std::string& id) {
    std::lock_guard<os::Mutex> lock(rt.status_mu_);
    auto it = rt.slot_instances_.find(id);
    return it != rt.slot_instances_.end() ? it->second : 0;
}

bool SceneRuntime::must_not_wait() const {
    return deps_.ui.on_ui_thread() || (task_ && task_->is_current()) || deps_.worker.on_worker();
}

Result<void> SceneRuntime::request(os::LargeUnique<SceneMessage> m,
                                   std::shared_ptr<SceneWaiter> waiter) {
    if (!running_) return Result<void>::err(ErrorCode::Busy, "scene runtime not running");
    const bool wait = !must_not_wait();
    if (!push(std::move(m), wait ? kPushMs : 0))
        return Result<void>::err(ErrorCode::Busy, "scene queue full");
    if (!wait) return Result<void>::ok();
    if (!waiter->sem.take(kWaitMs))
        return Result<void>::err(ErrorCode::Timeout, "the scene task did not answer");
    std::lock_guard<os::Mutex> lock(waiter->mu);
    return waiter->result;
}

Result<void> SceneRuntime::open(const std::string& id, Value args) {
    return launch(id, LaunchReason::User, std::move(args));
}

Result<void> SceneRuntime::launch(const std::string& id, LaunchReason reason, Value args) {
    return launch_load(id, reason, std::move(args), reason == LaunchReason::Boot);
}

Result<void> SceneRuntime::open_background(const std::string& id) {
    return launch_load(id, LaunchReason::Boot, {}, false, true);
}

Result<void> SceneRuntime::launch_load(const std::string& id, LaunchReason reason, Value args,
                                       bool restore, bool reopen) {
    // A boot launch is refused for these when its load arrives, where the
    // refusal can be a notice (finish_open()).
    if (reason != LaunchReason::Boot) {
        if (deps_.repo.installing(id))
            return Result<void>::err(ErrorCode::Busy, "scene " + id + " is being installed");
        if (deps_.repo.removing(id))
            return Result<void>::err(ErrorCode::Busy, "scene " + id + " is being deleted");
    }
    auto waiter = std::make_shared<SceneWaiter>();
    return request(make_message(id, 0, msg::Load{std::move(args), waiter, reason, restore, reopen}),
                   waiter);
}

void SceneRuntime::restore_background() {
    if (!running_ || deps_.background_list_path.empty()) return;
    core::StorageManager& storage = deps_.storage;
    // Read on the worker (the scene task touches no flash), then handed to
    // the scene task as one message, so every launch in it is queued at
    // once and the list is compared with the file only after all of them.
    bool queued = deps_.worker.submit([link = link_, &storage, path = deps_.background_list_path] {
        const std::vector<std::string> ids = read_background_list(storage, path);
        // Until it arrives nothing writes the list, so a full queue is
        // waited out a little (without the link's lock, which the scene
        // task may need to drain it) rather than given up at once.
        for (int attempt = 0; attempt < kRestoreAttempts; ++attempt) {
            if (attempt > 0) os::sleep_ms(kRestoreRetryMs);
            std::lock_guard<os::Mutex> lock(link->mu);
            if (link->rt == nullptr) return;
            if (post_scene_message(*link->rt, make_message("", 0, msg::Restore{ids}))) return;
        }
        WLOG_W(kTag, "background scenes not restored: scene queue full");
    });
    if (!queued) WLOG_W(kTag, "background scenes not restored: storage worker busy");
}

Result<void> SceneRuntime::close(const std::string& id) {
    auto waiter = std::make_shared<SceneWaiter>();
    return request(make_message(id, 0, msg::Close{waiter}), waiter);
}

Result<void> SceneRuntime::exit(const std::string& id) {
    auto waiter = std::make_shared<SceneWaiter>();
    return request(make_message(id, 0, msg::Exit{waiter}), waiter);
}

Result<void> SceneRuntime::reload(const std::string& id) {
    auto waiter = std::make_shared<SceneWaiter>();
    return request(make_message(id, 0, msg::Reload{false, waiter}), waiter);
}

void SceneRuntime::close_async(const std::string& id, std::function<void(Result<void>)> done) {
    auto waiter = std::make_shared<SceneWaiter>();
    waiter->then = std::move(done);
    // A message that cannot be queued is failed through the waiter: `done`
    // still runs, with Busy.
    push(make_message(id, 0, msg::Exit{waiter}), 0);
}

Result<void> SceneRuntime::deliver_notification(const std::string& scene_id,
                                                const std::string& note_id) {
    if (!running_) return Result<void>::err(ErrorCode::Busy, "scene runtime not running");
    if (deps_.repo.installing(scene_id))
        return Result<void>::err(ErrorCode::Busy, "scene " + scene_id + " is being installed");
    if (deps_.repo.removing(scene_id))
        return Result<void>::err(ErrorCode::Busy, "scene " + scene_id + " is being deleted");
    if (!push(make_message(scene_id, 0, msg::Notify{note_id}), 0))
        return Result<void>::err(ErrorCode::Busy, "scene queue full");
    return Result<void>::ok();
}

void SceneRuntime::on_installed(const std::string& id) {
    if (!push(make_message(id, 0, msg::Reload{true, nullptr}), 0))
        WLOG_W(kTag, "%s: installed, but the scene task was not told", id.c_str());
}

void SceneRuntime::set_visible(bool visible) {
    want_visible_.store(visible);
    // The message only wakes the task; on a full queue the task is busy
    // and follows want_visible_ after the message it is on.
    msg::Body body = visible ? msg::Body{msg::Show{}} : msg::Body{msg::Hide{}};
    if (!push(make_message("", 0, std::move(body)), 0) && running_)
        WLOG_I(kTag, "screen %s: scene queue full; the scene task catches up",
               visible ? "on" : "off");
}

core::SceneStatus SceneRuntime::status(const std::string& id) const {
    std::lock_guard<os::Mutex> lock(status_mu_);
    auto it = statuses_.find(id);
    return it != statuses_.end() ? it->second : core::SceneStatus{};
}

std::optional<std::string> SceneRuntime::foreground() const {
    std::lock_guard<os::Mutex> lock(status_mu_);
    return foreground_id_;
}

std::vector<BackgroundScene> SceneRuntime::background_scenes() const {
    std::lock_guard<os::Mutex> lock(status_mu_);
    return background_;
}

std::string SceneRuntime::running_name(std::string_view id) const {
    std::lock_guard<os::Mutex> lock(status_mu_);
    if (foreground_id_ && *foreground_id_ == id) return foreground_name_;
    for (const BackgroundScene& b : background_)
        if (b.id == id) return b.name;
    return {};
}

Result<void> SceneRuntime::check_sources(std::span<const core::SourceFile> files) {
    if (!running_) return Result<void>::err(ErrorCode::Busy, "scene runtime not running");
    // Up to 10 s of waiting would freeze the screen, or stall every file
    // job behind this one (the scene task may itself be waiting on one).
    if (deps_.ui.on_ui_thread() || deps_.worker.on_worker())
        return Result<void>::err(ErrorCode::Busy, "cannot wait for the scene task here");
    auto job = std::make_shared<SourceCheckJob>();
    job->files = files;
    if (task_->is_current()) {
        // Already on the scene task's stack.
        for (const auto& f : files)
            if (auto err = compile_check(f.path, f.text))
                return Result<void>::err(ErrorCode::InvalidArgument, err->text());
        return Result<void>::ok();
    }
    if (!push(make_message("", 0, msg::CheckSources{job}), kPushMs))
        return Result<void>::err(ErrorCode::Busy, "scene queue full");
    bool answered = job->sem.take(kWaitMs);
    std::lock_guard<os::Mutex> lock(job->mu);
    if (!answered && !job->done) {
        // The files are about to go out of scope: the task must not read them.
        job->cancelled = true;
        return Result<void>::err(ErrorCode::Timeout, "the scene task did not answer");
    }
    return job->result;
}

Result<void> SceneRuntime::check_build(std::shared_ptr<const core::ScenePackage> package) {
    if (!running_) return Result<void>::err(ErrorCode::Busy, "scene runtime not running");
    if (deps_.ui.on_ui_thread() || deps_.worker.on_worker())
        return Result<void>::err(ErrorCode::Busy, "cannot wait for the scene task here");
    if (!package) return Result<void>::err(ErrorCode::InvalidArgument, "no package");
    const std::string& id = package->manifest.id;
    if (!core::valid_scene_id(id))
        return Result<void>::err(ErrorCode::InvalidArgument, "invalid scene id: " + id);
    // The store is loaded from a directory that never exists, so it starts
    // empty; with no way to queue a flush or a timer it can never write.
    // load() is the only file access (two misses), on the worker.
    core::SceneStoreDeps sd{deps_.storage, [](std::function<void()>) { return false; },
                            [](uint32_t, std::function<void()>) {}};
    auto store = core::SceneStore::create(std::move(sd), deps_.repo.data_dir(id) + "/.check");
    Result<void> loaded =
        core::call_on_worker<void>(&deps_.worker, [&store] { return store->load(); });
    if (!loaded) return loaded;
    auto job = std::make_shared<BuildCheckJob>();
    job->package = std::move(package);
    job->store = std::move(store);
    if (task_->is_current()) return run_check_build(job->package, *job->store);
    if (!push(make_message("", 0, msg::CheckBuild{job}), kPushMs))
        return Result<void>::err(ErrorCode::Busy, "scene queue full");
    bool answered = job->sem.take(kWaitMs);
    std::lock_guard<os::Mutex> lock(job->mu);
    if (!answered && !job->done) {
        job->cancelled = true;
        return Result<void>::err(ErrorCode::Timeout, "the scene task did not answer");
    }
    return job->result;
}

Result<os::LargeString> SceneRuntime::ask(os::LargeUnique<SceneMessage> m,
                                          const std::shared_ptr<SceneQuery>& job) {
    using R = Result<os::LargeString>;
    if (!running_) return R::err(ErrorCode::Busy, "scene runtime not running");
    if (must_not_wait()) return R::err(ErrorCode::Busy, "cannot wait for the scene task here");
    if (!push(std::move(m), kPushMs)) return R::err(ErrorCode::Busy, "scene queue full");
    if (!job->sem.take(kWaitMs)) {
        std::lock_guard<os::Mutex> lock(job->mu);
        if (!job->done) return R::err(ErrorCode::Timeout, "the scene task did not answer");
    }
    std::lock_guard<os::Mutex> lock(job->mu);
    return job->result;
}

Result<os::LargeString> SceneRuntime::eval(const std::string& id, std::string_view lua,
                                           uint32_t budget_ms) {
    auto job = std::make_shared<SceneQuery>();
    job->text.assign(lua.data(), lua.size());
    job->budget_ms = budget_ms;
    return ask(make_message(id, 0, msg::Eval{job}), job);
}

Result<os::LargeString> SceneRuntime::store_dump(const std::string& id, size_t max_bytes) {
    auto job = std::make_shared<SceneQuery>();
    job->max_bytes = max_bytes;
    return ask(make_message(id, 0, msg::StoreDump{job}), job);
}

Result<void> SceneRuntime::store_set(const std::string& id, std::string_view key,
                                     std::string_view json) {
    auto job = std::make_shared<SceneQuery>();
    job->key.assign(key);
    job->text.assign(json.data(), json.size());
    auto r = ask(make_message(id, 0, msg::StoreSet{job}), job);
    if (!r) return Result<void>::err(r.error().code, r.error().message);
    return Result<void>::ok();
}

void SceneRuntime::answer(const std::string& id, SceneMessage& m) {
    using R = Result<os::LargeString>;
    SceneQuery* job = nullptr;
    if (auto* b = std::get_if<msg::Eval>(&m.body)) job = b->job.get();
    if (auto* b = std::get_if<msg::StoreDump>(&m.body)) job = b->job.get();
    if (auto* b = std::get_if<msg::StoreSet>(&m.body)) job = b->job.get();
    if (job == nullptr) return;
    Slot* s = find_id(id);
    if (s == nullptr || !s->scene) {
        job->finish(R::err(ErrorCode::NotFound, "scene " + id + " is not running"));
        return;
    }
    if (std::holds_alternative<msg::Eval>(m.body)) {
        os::LargeString out;
        bool fatal = false;
        std::optional<ScriptError> err = s->scene->eval(job->text, job->budget_ms, out, &fatal);
        if (fatal) {
            const std::string text = err ? err->text() : std::string("eval failed");
            if (err) fault(*s, *err);
            job->finish(R::err(ErrorCode::InvalidArgument, text));
            return;
        }
        // The answer goes out once the changes are on their way.
        submit(*s);
        if (err) {
            job->finish(R::err(ErrorCode::InvalidArgument, err->text()));
        } else {
            job->finish(R::ok(std::move(out)));
        }
        return;
    }
    if (!s->store) {
        job->finish(R::err(ErrorCode::NotFound, "scene " + id + " has no store"));
        return;
    }
    if (std::holds_alternative<msg::StoreDump>(m.body)) {
        job->finish(R::ok(s->store->dump_json(job->max_bytes)));
        return;
    }
    Result<void> set = s->store->kv_set(job->key, std::string_view(job->text));
    job->finish(set ? R::ok(os::LargeString()) : R::err(set.error().code, set.error().message));
}

const SceneView* SceneRuntime::ui_current_view() const {
    if (!ui_side_) return nullptr;
    const UiSide& u = *ui_side_;
    if (!u.view || u.token == 0 || u.token != ui_view_.load()) return nullptr;
    return u.view.get();
}

Result<void> SceneRuntime::click_ref(std::string_view ref) {
    if (!running_ || !ui_side_)
        return Result<void>::err(ErrorCode::Busy, "scene runtime not running");
    Result<void> r = Result<void>::err(ErrorCode::NotFound, "no scene is running");
    auto click = [&] {
        UiSide& u = *ui_side_;
        if (!u.view || u.token == 0 || u.token != ui_view_.load()) return;
        auto node = u.view->find_ref(ref);
        lv_obj_t* obj = node ? u.view->object(*node) : nullptr;
        if (obj == nullptr) {
            r = Result<void>::err(ErrorCode::NotFound, "no widget with that ref");
            return;
        }
        lv_obj_send_event(obj, LV_EVENT_CLICKED, nullptr);
        r = Result<void>::ok();
    };
    if (deps_.ui.on_ui_thread())
        click();
    else
        deps_.ui.run_blocking(click);
    return r;
}

Result<void> SceneRuntime::swipe_ref(std::string_view ref, std::string_view dir) {
    const SwipeDir d = swipe_from_name(dir);
    if (d == SwipeDir::None)
        return Result<void>::err(ErrorCode::InvalidArgument,
                                 "the direction is not up, down, left or right");
    if (!running_ || !ui_side_)
        return Result<void>::err(ErrorCode::Busy, "scene runtime not running");
    Result<void> r = Result<void>::err(ErrorCode::NotFound, "no scene is running");
    auto swipe = [&] {
        UiSide& u = *ui_side_;
        if (!u.view || u.token == 0 || u.token != ui_view_.load()) return;
        auto node = u.view->find_ref(ref);
        if (!node) {
            r = Result<void>::err(ErrorCode::NotFound, "no widget with that ref");
            return;
        }
        r = u.view->swipe(*node, d)
                ? Result<void>::ok()
                : Result<void>::err(ErrorCode::NotFound, "no on_swipe at or above that widget");
    };
    if (deps_.ui.on_ui_thread())
        swipe();
    else
        deps_.ui.run_blocking(swipe);
    return r;
}

void SceneRuntime::set_status(const std::string& id, core::SceneState state, std::string error) {
    std::lock_guard<os::Mutex> lock(status_mu_);
    core::SceneStatus& s = statuses_[id];
    s.state = state;
    s.error = std::move(error);
}

void SceneRuntime::publish(const std::string& id, const char* state) {
    // Logged as well, so whoever reads only the log (the serial console,
    // the simulator's smoke tests) can follow a scene too.
    WLOG_I(kTag, "%s: %s", id.c_str(), state);
    core::Event e;
    e.topic = "scene.state";
    e.source = "scene";
    e.payload = {{"id", Value(id)}, {"state", Value(state)}};
    deps_.bus.publish(std::move(e));
}

void SceneRuntime::schedule(uint32_t ms, std::function<void()> fn) {
    {
        std::lock_guard<os::Mutex> lock(delayed_mu_);
        delayed_.push_back({os::now_ms() + ms, std::move(fn)});
    }
    // Wakes a task blocked without a deadline. If the queue is full the
    // task is busy and looks at the deadlines after its next message.
    push(make_message("", 0, msg::Wake{}), 0);
}

// --- Scene task --------------------------------------------------------------

void SceneRuntime::run() {
    while (!quit_) {
        uint32_t timeout = scene_wait_ms();
        {
            std::lock_guard<os::Mutex> lock(delayed_mu_);
            uint64_t now = os::now_ms();
            for (const auto& d : delayed_) {
                uint64_t wait = d.due_ms > now ? d.due_ms - now : 0;
                timeout = static_cast<uint32_t>(std::min<uint64_t>(timeout, wait));
            }
        }
        SceneMessage* raw = nullptr;
        if (queue_.pop(raw, timeout)) {
            os::LargeUnique<SceneMessage> m(raw);
            const uint64_t t0 = os::now_ms();
            // Spec 3.5: before each message while a scene is in front.
            if (front_ != nullptr) enforce_limits(false);
            handle(*m);
            close_if_asked();
            const uint64_t ms = os::now_ms() - t0;
            if (ms > kSlowMs) {
                std::string_view id = m->scene_id;
                if (id.empty() && front_ != nullptr) id = front_->id;
                std::string tag = id.empty() ? std::string(kTag) : scene_tag(id);
                WLOG_W(tag.c_str(), "slow message %s %" PRIu64 " ms", message_kind(m->body), ms);
            }
        }
        follow_visibility();
        run_delayed();
        if (front_ != nullptr) enforce_limits(false);
        run_scene_clock();
        persist_background();
    }
}

void SceneRuntime::run_delayed() {
    std::vector<Delayed> due;
    {
        std::lock_guard<os::Mutex> lock(delayed_mu_);
        uint64_t now = os::now_ms();
        auto split = std::stable_partition(delayed_.begin(), delayed_.end(),
                                           [now](const Delayed& d) { return d.due_ms > now; });
        due.assign(std::make_move_iterator(split), std::make_move_iterator(delayed_.end()));
        delayed_.erase(split, delayed_.end());
    }
    for (auto& d : due) d.fn();
}

SceneRuntime::Slot* SceneRuntime::find(uint32_t instance) {
    if (instance == 0) return nullptr;
    for (auto& s : slots_)
        if (s->instance == instance) return s.get();
    return nullptr;
}

SceneRuntime::Slot* SceneRuntime::find_id(std::string_view id) {
    for (auto& s : slots_)
        if (s->id == id) return s.get();
    return nullptr;
}

SceneRuntime::Instances SceneRuntime::instances() const {
    Instances out;
    // finish_open() never makes more than kMaxSlots.
    for (const auto& s : slots_) {
        if (out.n == out.ids.size()) break;
        out.ids[out.n++] = s->instance;
    }
    return out;
}

size_t SceneRuntime::background_count(const Slot* except) const {
    size_t n = 0;
    for (const auto& s : slots_)
        if (s->background && s.get() != except) ++n;
    return n;
}

SceneRuntime::Slot* SceneRuntime::target(const SceneMessage& m) {
    Slot* s = find(m.instance);
    if (s == nullptr) ++stale_dropped_;
    return s;
}

void SceneRuntime::handle(SceneMessage& m) {
    msg::Body& body = m.body;
    if (auto* b = std::get_if<msg::Load>(&body)) {
        begin_open(m.scene_id, std::move(b->args), b->reason, b->restore, b->done, b->reopen);
    } else if (std::holds_alternative<msg::Open>(body)) {
        finish_open(m);
    } else if (auto* b = std::get_if<msg::Close>(&body)) {
        // From the API (instance 0) by id; from the view, the panel or
        // the gesture for one instance.
        Slot* s = m.instance == 0 ? find_id(m.scene_id) : target(m);
        if (s != nullptr && s == front_) {
            go_back(*s, b->done);
        } else {
            // A load of it still on the worker is cancelled: whoever closed
            // it may go on to delete or replace its files.
            if (m.instance == 0 && s == nullptr) cancel_loads(m.scene_id);
            finish_after_queued_io(b->done);
        }
    } else if (auto* b = std::get_if<msg::Exit>(&body)) {
        Slot* s = m.instance == 0 ? find_id(m.scene_id) : target(m);
        if (s != nullptr) {
            close_slot(*s, b->done);
        } else {
            if (m.instance == 0) cancel_loads(m.scene_id);
            finish_after_queued_io(b->done);
        }
    } else if (auto* b = std::get_if<msg::Reload>(&body)) {
        Slot* s = find_id(m.scene_id);
        if (b->if_foreground && (s == nullptr || s != front_)) {
            // A new package: a scene in the background stops (it ran the
            // old one), any other is only marked stopped.
            if (s != nullptr) {
                close_slot(*s);
            } else {
                set_status(m.scene_id, core::SceneState::Stopped, "");
                publish(m.scene_id, "stopped");
            }
        } else {
            Value args = s != nullptr ? s->args : Value();
            // One in front closes in begin_open(), ahead of the load.
            if (s != nullptr && s != front_) close_slot(*s);
            begin_open(m.scene_id, std::move(args), LaunchReason::User, false, b->done);
        }
    } else if (std::holds_alternative<msg::Show>(body)) {
        const bool came_on = !visible_;
        visible_ = true;
        show_scene(came_on);
    } else if (std::holds_alternative<msg::Hide>(body)) {
        visible_ = false;
        hide_scenes();
    } else if (auto* b = std::get_if<msg::Event>(&body)) {
        Slot* s = target(m);
        if (s != nullptr && s != front_) {
            // From the view of a scene that has gone to the background.
            ++stale_dropped_;
        } else if (s != nullptr && s->scene) {
            run_step(*s, s->scene->event(b->node, b->ev, b->value));
        }
    } else if (auto* b = std::get_if<msg::ListRange>(&body)) {
        Slot* s = target(m);
        if (s != nullptr && s != front_) {
            ++stale_dropped_;
        } else if (s != nullptr && s->scene) {
            run_step(*s, s->scene->list_range(b->list, b->first, b->count));
        }
    } else if (auto* b = std::get_if<msg::Device>(&body)) {
        device_event(b->kind);
    } else if (std::holds_alternative<msg::ClockSet>(body)) {
        clock_set();
    } else if (auto* b = std::get_if<msg::HttpDone>(&body)) {
        Slot* s = target(m);
        if (s != nullptr && s->scene) run_step(*s, s->scene->http(b->id, b->result));
    } else if (auto* b = std::get_if<msg::FilesDone>(&body)) {
        Slot* s = target(m);
        if (s != nullptr && s->scene) run_step(*s, s->scene->files(b->id, b->result));
    } else if (auto* b = std::get_if<msg::Sensor>(&body)) {
        Slot* s = target(m);
        // Paused in the background: a batch already on its way is dropped.
        if (s != nullptr && s->scene && !s->background)
            run_step(*s, s->scene->sensor(b->handle, b->samples));
    } else if (std::holds_alternative<msg::StoreDone>(body)) {
        // Its binding comes later; a current one has nothing to do yet.
        target(m);
    } else if (auto* b = std::get_if<msg::CheckSources>(&body)) {
        SourceCheckJob& job = *b->job;
        std::lock_guard<os::Mutex> lock(job.mu);
        if (job.cancelled) return;
        Result<void> r = Result<void>::ok();
        for (const auto& f : job.files) {
            if (auto err = compile_check(f.path, f.text)) {
                r = Result<void>::err(ErrorCode::InvalidArgument, err->text());
                break;
            }
        }
        job.finish_locked(std::move(r));
    } else if (auto* b = std::get_if<msg::CheckBuild>(&body)) {
        BuildCheckJob& job = *b->job;
        std::lock_guard<os::Mutex> lock(job.mu);
        if (job.cancelled) return;
        job.finish_locked(run_check_build(job.package, *job.store));
    } else if (std::holds_alternative<msg::Eval>(body) ||
               std::holds_alternative<msg::StoreDump>(body) ||
               std::holds_alternative<msg::StoreSet>(body)) {
        answer(m.scene_id, m);
    } else if (auto* b = std::get_if<msg::Restore>(&body)) {
        restore(std::move(b->ids));
    } else if (auto* b = std::get_if<msg::Notify>(&body)) {
        notify(m.scene_id, b->note_id);
    } else if (std::holds_alternative<msg::Quit>(body)) {
        // First: the scenes closed here stay on the background list
        // (persist_background() writes nothing from now on).
        quit_ = true;
        for (uint32_t instance : instances())
            if (Slot* s = find(instance)) close_slot(*s);
    }
    // msg::Wake: run() looks at the deadlines next.
}

void SceneRuntime::notify(const std::string& id, const std::string& note_id) {
    Slot* s = find_id(id);
    if (s == nullptr || !s->scene) {
        // Not running (or its error panel is up): launched for the
        // notification, which it gets once it is up (finish_open()).
        begin_open(id, {}, LaunchReason::Notification, false, nullptr, false, note_id);
        return;
    }
    if (s->background) {
        const uint32_t instance = s->instance;
        // As an open of it would: the opens still on their way lose.
        supersede_loads({});
        resume(*s, nullptr);
        s = find(instance);
        // It failed coming forward.
        if (s == nullptr || s != front_ || !s->scene) return;
    }
    run_step(*s, s->scene->hook("on_notification", note_id.c_str()));
}

void SceneRuntime::begin_open(const std::string& id, Value args, LaunchReason reason, bool restore,
                              std::shared_ptr<SceneWaiter> waiter, bool reopen,
                              std::string notification) {
    list_dirty_ = true;
    Slot* running = find_id(id);
    const bool boot = reason == LaunchReason::Boot;
    if (boot) {
        // Already running, or a load of it is on its way: nothing to
        // start, and nothing else is touched (a user open still loading
        // stays). Riding on a user open, it is not lost if a later open
        // supersedes that one (PendingLoad).
        auto coming = std::find_if(loading_.begin(), loading_.end(),
                                   [&](const PendingLoad& p) { return p.id == id; });
        if (coming != loading_.end() && coming->reason != LaunchReason::Boot) {
            coming->then_boot = true;
            coming->restore = coming->restore || restore;
        }
        if (running != nullptr || coming != loading_.end()) {
            if (waiter) waiter->finish(Result<void>::ok());
            return;
        }
    } else if (running != nullptr && running->background) {
        // Nothing to load; the user opens still on their way were
        // superseded by this one.
        supersede_loads({});
        resume(*running, std::move(waiter));
        return;
    }
    // The same scene closes first: its final store flush then goes to the
    // worker ahead of this load, so the reopened store reads what it
    // wrote. Another one stays until this one has loaded, so a scene that
    // cannot be opened leaves it running.
    if (running != nullptr) close_slot(*running);
    const uint32_t instance = next_instance_++;
    if (next_instance_ == 0) next_instance_ = 1;
    PendingLoad load{
        instance, id, reason, restore, false, 0, next_seq_++, reason, std::move(notification)};
    if (boot && reopen) {
        auto d = std::find_if(departed_.begin(), departed_.end(),
                              [&](const Departed& e) { return e.id == id; });
        if (d != departed_.end()) {
            load.shown = d->reason;
            load.last_front_ms = d->last_front_ms;
            load.seq = d->seq;
            departed_.erase(d);
        }
    }
    // A user open supersedes the other user opens (the latest one wins)
    // and a boot launch of the same scene (it wants it in front), whose
    // place in the background list it keeps.
    if (!boot) {
        const Carried carried = supersede_loads(id);
        if (carried.then_boot) {
            load.then_boot = true;
            load.restore = carried.restore;
            load.seq = carried.seq;
        }
    }
    loading_.push_back(std::move(load));

    std::shared_ptr<Link> link = link_;
    core::StorageWorker& worker = deps_.worker;
    core::SceneStoreDeps store_deps{
        deps_.storage,
        [&worker](std::function<void()> job) { return worker.submit(std::move(job)); },
        [link](uint32_t ms, std::function<void()> cb) {
            std::lock_guard<os::Mutex> lock(link->mu);
            if (link->rt != nullptr) link->rt->schedule(ms, std::move(cb));
        }};
    core::SceneRepository& repo = deps_.repo;
    bool queued = worker.submit([link, &repo, store_deps, id, instance, reason,
                                 args = std::move(args), waiter]() mutable {
        msg::Open open;
        open.args = std::move(args);
        open.done = waiter;
        open.reason = reason;
        auto pkg = repo.load(id);
        if (!pkg) {
            open.error = pkg.error();
        } else {
            auto store = core::SceneStore::create(store_deps, repo.data_dir(id));
            auto loaded = store->load();
            if (!loaded) {
                open.error = loaded.error();
            } else {
                open.package = std::make_shared<const core::ScenePackage>(std::move(pkg.value()));
                open.store = std::move(store);
            }
        }
        auto m = make_message(id, instance, std::move(open));
        std::lock_guard<os::Mutex> lock(link->mu);
        if (link->rt == nullptr) {
            if (m) fail_message(*m, Result<void>::err(ErrorCode::Busy, "scene runtime stopped"));
            return;
        }
        if (!post_scene_message(*link->rt, std::move(m)))
            WLOG_W(kTag, "%s: loaded, but the scene queue is full", id.c_str());
    });
    if (!queued) {
        std::erase_if(loading_, [&](const PendingLoad& p) { return p.instance == instance; });
        WLOG_W(kTag, "%s: not opened: storage worker busy", id.c_str());
        if (waiter) waiter->finish(Result<void>::err(ErrorCode::Busy, "storage worker busy"));
    }
}

void SceneRuntime::finish_open(SceneMessage& m) {
    list_dirty_ = true;
    auto& open = std::get<msg::Open>(m.body);
    auto done = [&](Result<void> r) {
        if (open.done) open.done->finish(std::move(r));
    };
    auto pending = std::find_if(loading_.begin(), loading_.end(),
                                [&](const PendingLoad& p) { return p.instance == m.instance; });
    if (pending == loading_.end()) {
        // Another open came after this one, or a close of it.
        ++stale_dropped_;
        if (open.store) open.store->close();
        done(Result<void>::err(ErrorCode::Busy, "superseded by another open or a close"));
        return;
    }
    const PendingLoad load = *pending;
    loading_.erase(pending);
    const std::string id = m.scene_id;
    const LaunchReason reason = load.reason;
    const bool boot = reason == LaunchReason::Boot;
    if (boot && open.reason != LaunchReason::Boot) {
        // A user open a later one superseded, carried on for the boot
        // launch that rode on it: its caller hears it was superseded, and
        // the scene starts as a boot launch would (no arguments).
        done(Result<void>::err(ErrorCode::Busy, "superseded by another open"));
        open.done = nullptr;
        open.args = Value();
    }
    const std::string name = open.package ? open.package->manifest.name : id;
    // A refusal of a boot launch (or of a user open one rode on) is the
    // restore failing.
    auto refused = [&](const char* why) {
        if (load.restore) restore_failed(id, name, why);
    };
    if (open.error.code == ErrorCode::Ok &&
        (deps_.repo.installing(id) || deps_.repo.removing(id))) {
        // Loaded before an install or a delete of it began: the package
        // and the data directory are about to be replaced or removed.
        const bool installing = deps_.repo.installing(id);
        WLOG_W(kTag, "%s: not opened: it is being %s", id.c_str(),
               installing ? "installed" : "deleted");
        if (open.store) open.store->close();
        refused(installing ? "it is being installed" : "it is being deleted");
        done(Result<void>::err(ErrorCode::Busy, "scene " + id + " is being " +
                                                    (installing ? "installed" : "deleted")));
        return;
    }
    if (open.error.code != ErrorCode::Ok) {
        if (open.error.code == ErrorCode::NotFound) {
            WLOG_W(kTag, "%s: no such scene", id.c_str());
            refused("no such scene");
        } else {
            std::string text = "cannot load: " + open.error.message;
            WLOG_E(scene_tag(id).c_str(), "%s", text.c_str());
            set_status(id, core::SceneState::Error, text);
            publish(id, "error");
            refused("cannot load");
        }
        done(Result<void>::err(open.error.code, open.error.message));
        return;
    }
    if (boot && find_id(id) != nullptr) {
        // Already running (begin_open() lets no other load of it start
        // while this one is out; this only keeps two slots of one scene
        // impossible).
        open.store->close();
        done(Result<void>::ok());
        return;
    }
    if (boot && !open.package->manifest.background) {
        WLOG_W(kTag, "%s: not started in the background: its scene.json does not declare it",
               id.c_str());
        open.store->close();
        refused("it does not declare \"background\"");
        done(Result<void>::err(ErrorCode::InvalidArgument,
                               "scene " + id + " does not declare \"background\""));
        return;
    }
    if (boot && background_count(nullptr) >= config_.max_background) {
        WLOG_W(kTag, "%s: not started: %u scenes already run in the background", id.c_str(),
               static_cast<unsigned>(config_.max_background));
        open.store->close();
        refused("too many scenes in the background");
        done(Result<void>::err(ErrorCode::Busy, "too many scenes in the background"));
        return;
    }
    // Never two slots of one scene (begin_open() closed it or brought it
    // forward instead of loading it).
    if (Slot* old = find_id(id)) close_slot(*old);
    if (!boot) {
        if (front_ != nullptr) go_back(*front_, nullptr);
        // Spec 3.5: room for the scene about to open.
        enforce_limits(true);
    }
    if (slots_.size() > config_.max_background) {
        // Never: the scene in front just left (a boot launch keeps it, but
        // the background had room for one more), and the background keeps
        // to max_background.
        WLOG_E(kTag, "%s: not opened: %u scenes already run", id.c_str(),
               static_cast<unsigned>(slots_.size()));
        open.store->close();
        refused("too many scenes running");
        done(Result<void>::err(ErrorCode::Busy, "too many scenes running"));
        return;
    }

    Slot* added = add_slot(m, load.shown);
    if (added == nullptr) {
        WLOG_E(kTag, "%s: no memory for the scene", id.c_str());
        open.store->close();
        refused("no memory for the scene");
        done(Result<void>::err(ErrorCode::NoMemory, "no memory for the scene"));
        return;
    }
    Slot& s = *added;
    auto failed = [&] {
        std::string error = status(id).error;
        done(Result<void>::err(ErrorCode::InvalidArgument, error));
    };

    if (boot) {
        // Spec 3.6: the tree is built, but no view and no on_show. A
        // script error here is the restore failing (fault()).
        const uint64_t now = os::now_ms();
        s.background = true;
        s.background_since_ms = now;
        s.last_front_ms = load.last_front_ms;
        s.seq = load.seq;
        s.restoring = load.restore;
        s.scene->api().background = true;
        update_background_list();
        if (!run_step(s, start_scene(s))) return failed();
        if (!run_step(s, s.scene->hook("on_state", "background"))) return failed();
        s.restoring = false;
        set_status(id, core::SceneState::Background, "");
        publish(id, "background");
        done(Result<void>::ok());
        return;
    }

    front_ = &s;
    {
        std::lock_guard<os::Mutex> lock(status_mu_);
        foreground_id_ = id;
        foreground_name_ = s.name;
        foreground_instance_ = s.instance;
    }
    update_background_list();
    show_view(s);

    if (!run_step(s, start_scene(s))) return failed();
    set_status(id, core::SceneState::Running, "");
    publish(id, "running");
    if (visible_) {
        s.shown = true;
        if (!run_step(s, s.scene->hook("on_show"))) return failed();
    }
    if (!run_step(s, s.scene->hook("on_state", "foreground"))) return failed();
    if (!load.notification.empty() &&
        !run_step(s, s.scene->hook("on_notification", load.notification.c_str())))
        return failed();
    done(Result<void>::ok());
}

void SceneRuntime::cancel_loads(std::string_view id) {
    list_dirty_ = true;
    std::erase_if(loading_, [&](const PendingLoad& p) { return p.id == id; });
}

SceneRuntime::Carried SceneRuntime::supersede_loads(std::string_view id) {
    list_dirty_ = true;
    Carried carried;
    for (PendingLoad& p : loading_) {
        if (!id.empty() && p.id == id) {
            if (p.reason == LaunchReason::Boot || p.then_boot) {
                carried.then_boot = true;
                carried.restore = carried.restore || p.restore;
                carried.seq = p.seq;
            }
        } else if (p.reason != LaunchReason::Boot && p.then_boot) {
            // The boot launch that rode on it carries on alone.
            p.reason = LaunchReason::Boot;
            p.shown = LaunchReason::Boot;
            p.then_boot = false;
        }
    }
    std::erase_if(loading_, [&](const PendingLoad& p) {
        return (!id.empty() && p.id == id) || p.reason != LaunchReason::Boot;
    });
    return carried;
}

void SceneRuntime::restore_failed(const std::string& id, const std::string& name, const char* why) {
    WLOG_W(kTag, "%s: not restored: %s", id.c_str(), why);
    if (deps_.on_notice) deps_.on_notice(SceneNotice{SceneNoticeKind::RestoreFailed, id, name});
}

void SceneRuntime::restore(std::vector<std::string> ids) {
    // What the file holds now: the launches below are already on it, so
    // only a change from here on is written.
    persisted_ = ids;
    list_dirty_ = true;
    for (const std::string& id : ids) begin_open(id, {}, LaunchReason::Boot, true, nullptr);
}

void SceneRuntime::enforce_limits(bool opening, const Slot* coming) {
    // The common case, with nothing to close, allocates nothing: this runs
    // before every message while a scene is in front.
    const size_t reserve = config_.vm.memory_bytes;
    const size_t room = config_.vm_budget > reserve ? config_.vm_budget - reserve : 0;
    bool front = opening;
    size_t count = 0;
    size_t bytes = 0;
    for (const auto& s : slots_) {
        if (!s->background || s.get() == coming) {
            front = true;
            continue;
        }
        ++count;
        bytes += s->scene ? s->scene->vm_bytes() : 0;
    }
    if (count <= config_.max_background && (!front || bytes <= room)) return;

    std::vector<SlotInfo> infos;
    for (const Slot* s : background_order()) {
        if (s == coming) continue;
        infos.push_back({s->id, false, s->last_front_ms, s->scene ? s->scene->vm_bytes() : 0});
    }
    for (const Eviction& e :
         pick_evictions(infos, front, config_.max_background, config_.vm_budget, reserve)) {
        Slot* s = find_id(e.id);
        if (s != nullptr && s->background) evict(*s, e.memory);
    }
}

void SceneRuntime::evict(Slot& s, bool memory) {
    const SceneNotice n{memory ? SceneNoticeKind::EvictedMemory : SceneNoticeKind::EvictedCount,
                        s.id, s.name};
    WLOG_I(kTag, "%s: evicted (%s)", n.id.c_str(),
           memory ? "memory budget" : "too many in the background");
    // Spec 3.5: closed like any close (on_close, the final flush), then
    // the notice.
    close_slot(s);
    if (deps_.on_notice) deps_.on_notice(n);
}

std::vector<std::string> SceneRuntime::wanted_background() const {
    struct Entry {
        uint64_t last_front_ms;
        uint64_t seq;
        const std::string* id;
    };
    std::vector<Entry> entries;
    for (const Slot* s : background_order()) entries.push_back({s->last_front_ms, s->seq, &s->id});
    // Boot launches still loading (never in front), unless the scene is
    // already in the background.
    for (const PendingLoad& p : loading_) {
        if (p.reason != LaunchReason::Boot && !p.then_boot) continue;
        const bool listed = std::any_of(entries.begin(), entries.end(),
                                        [&](const Entry& e) { return *e.id == p.id; });
        if (!listed) entries.push_back({p.last_front_ms, p.seq, &p.id});
    }
    std::stable_sort(entries.begin(), entries.end(), [](const Entry& a, const Entry& b) {
        if (a.last_front_ms != b.last_front_ms) return a.last_front_ms < b.last_front_ms;
        return a.seq < b.seq;
    });
    std::vector<std::string> ids;
    for (const Entry& e : entries)
        if (std::find(ids.begin(), ids.end(), *e.id) == ids.end()) ids.push_back(*e.id);
    return ids;
}

void SceneRuntime::persist_background() {
    // Only after something the list depends on changed: this runs on
    // every pass of the task, timer ticks too, and the comparison
    // allocates.
    if (quit_ || !persisted_ || deps_.background_list_path.empty() || !list_dirty_) return;
    std::vector<std::string> want = wanted_background();
    if (want == *persisted_) {
        list_dirty_ = false;
        return;
    }
    List ids;
    for (const std::string& id : want) ids.emplace_back(id);
    std::string json = to_json(Value(Map{{"ids", Value(std::move(ids))}}));
    core::StorageManager& storage = deps_.storage;
    // The worker runs jobs in order: the last one written is the latest.
    const bool queued =
        deps_.worker.submit([&storage, path = deps_.background_list_path, json = std::move(json)] {
            auto data = std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(json.data()),
                                                 json.size());
            if (auto r = storage.write_atomic(path, data); !r)
                WLOG_W(kTag, "background list not saved: %s", r.error().message.c_str());
        });
    if (!queued) {
        // Stays dirty: tried again on the next pass of the task, which may
        // be a while if nothing runs; said once until a write gets through.
        if (!persist_warned_)
            WLOG_W(kTag, "background list not saved yet: storage worker busy; will retry");
        persist_warned_ = true;
        return;
    }
    persist_warned_ = false;
    list_dirty_ = false;
    persisted_ = std::move(want);
}

void SceneRuntime::show_view(Slot& s) {
    const uint64_t token = next_view_++;
    // The view exists before any batch for it is queued: the frame hook
    // applies patches only to the view whose token it is told.
    post_ui({UiJob::Kind::Show, s.instance,
             [this, instance = s.instance, token, id = s.id, package = s.package] {
                 ui_show_view(instance, token, id, package);
             }});
    ui_view_.store(token);
}

SceneRuntime::Slot* SceneRuntime::add_slot(SceneMessage& m, LaunchReason reason) {
    auto& open = std::get<msg::Open>(m.body);
    auto slot = os::make_large<Slot>();
    if (!slot) return nullptr;
    Slot& s = *slot;
    s.id = m.scene_id;
    s.name = open.package->manifest.name;
    s.instance = m.instance;
    s.package = open.package;
    // Allocated before the store and the arguments move in: on failure the
    // caller still holds the store and closes it.
    s.scene = os::make_large<SceneInstance>(s.id, s.package, config_.vm);
    if (!s.scene) return nullptr;
    s.args = std::move(open.args);
    s.store = std::move(open.store);
    s.declares_background = s.package->manifest.background;
    SceneApi& api = s.scene->api();
    api.device = [this] { return device_info(); };
    api.screen = deps_.shape;
    api.args = &s.args;
    api.store = s.store.get();
    s.reason = reason;
    api.launch = to_string(reason);
    api.locks = deps_.locks;
    api.holder = scene_tag(s.id);
    api.notifications = deps_.notifications;
    api.scene_id = s.id;
    api.scene_name = s.name;
    if (deps_.http != nullptr) wire_http(s);
    if (deps_.sensors != nullptr) wire_sensors(s);
    if (deps_.files != nullptr) wire_files(s);
    // Opened in front with the screen off: the timers of a scene that does
    // not declare "background" wait for it to come on.
    if (!visible_ && reason != LaunchReason::Boot && !s.declares_background)
        api.timers.pause(os::now_ms());
    const DeviceInfo now = device_info();
    s.has_battery = now.has_battery;
    s.battery = now.battery;
    s.charging = now.charging;
    s.lang = now.lang;
    slots_.push_back(std::move(slot));
    return &s;
}

void SceneRuntime::wire_http(Slot& s) {
    net::SceneHttp& http = *deps_.http;
    http.set_hosts(s.id, s.package->manifest.http_hosts);
    // Results come back from the proxy's task (or, for a request refused
    // at once, from inside submit() on this task) as messages for this
    // instance; one for an instance that is gone is dropped like any
    // stale message.
    auto deliver = [link = link_, id = s.id, instance = s.instance](uint32_t cb,
                                                                    net::SceneHttpResult r) {
        auto m = make_message(id, instance, msg::HttpDone{cb, std::move(r)});
        std::lock_guard<os::Mutex> lock(link->mu);
        if (link->rt == nullptr) return;
        if (!post_scene_message(*link->rt, std::move(m)))
            WLOG_W(scene_tag(id).c_str(), "http: a result was dropped: scene queue full");
    };
    SceneApi& api = s.scene->api();
    api.http_submit = [&http, deliver, id = s.id, instance = s.instance](
                          uint32_t cb, net::SceneHttpRequest req) {
        req.scene_id = id;
        req.instance = instance;
        http.submit(std::move(req),
                    [deliver, cb](net::SceneHttpResult r) { deliver(cb, std::move(r)); });
    };
    api.http_fail = [deliver](uint32_t cb, const char* error) {
        net::SceneHttpResult r;
        r.error = error;
        deliver(cb, std::move(r));
    };
}

void SceneRuntime::wire_sensors(Slot& s) {
    SceneApi& api = s.scene->api();
    api.sensors = deps_.sensors;
    // On the service's "wisp.imu" task, whose stack may be in PSRAM: this
    // only copies the samples into a message (PSRAM) and queues it without
    // waiting, never touching flash. A full queue drops the batch quietly:
    // the next one comes 100 ms later, and a log line per batch would
    // flood the log.
    api.sensor_post = [link = link_, id = s.id, instance = s.instance](
                          uint32_t handle, const core::SensorBatch* batch) {
        msg::Sensor body{handle, {}};
        if (batch != nullptr) body.samples.assign(batch->begin(), batch->end());
        auto m = make_message(id, instance, std::move(body));
        std::lock_guard<os::Mutex> lock(link->mu);
        if (link->rt != nullptr) post_scene_message(*link->rt, std::move(m));
    };
}

void SceneRuntime::wire_files(Slot& s) {
    core::SceneFiles& files = *deps_.files;
    core::StorageWorker& worker = deps_.worker;
    s.files_gate = std::allocate_shared<FilesGate>(os::LargeAllocator<FilesGate>());
    // Results come back from the worker (or, for a refusal, from this
    // task) as messages for this instance; one for an instance that is
    // gone is dropped like any stale message.
    auto deliver = [link = link_, id = s.id, instance = s.instance](uint32_t cb, FilesResult r) {
        auto m = make_message(id, instance, msg::FilesDone{cb, std::move(r)});
        std::lock_guard<os::Mutex> lock(link->mu);
        if (link->rt == nullptr) return;
        if (!post_scene_message(*link->rt, std::move(m)))
            WLOG_W(scene_tag(id).c_str(), "files: a result was dropped: scene queue full");
    };
    auto busy = [deliver](uint32_t cb) {
        FilesResult r;
        r.error = "busy";
        deliver(cb, std::move(r));
    };
    SceneApi& api = s.scene->api();
    api.files_run = [&files, &worker, deliver, busy, id = s.id](uint32_t cb, FilesJob job) {
        const bool queued = worker.submit(
            [&files, deliver, id, cb, job = std::move(job)] { deliver(cb, job(files, id)); });
        if (!queued) busy(cb);
    };
    api.files_fail = [deliver](uint32_t cb, const char* error) {
        FilesResult r;
        r.error = error;
        deliver(cb, std::move(r));
    };
    if (deps_.http == nullptr) return;
    net::SceneHttp& http = *deps_.http;
    api.files_download = [&files, &worker, &http, deliver, busy, gate = s.files_gate, id = s.id,
                          instance = s.instance, hosts = s.package->manifest.http_hosts](
                             uint32_t cb, net::SceneHttpRequest req, std::string name) {
        // SceneHttp checks the URL again; checked here first so a refused
        // download never picks the scene's files location.
        if (std::string code = net::SceneHttp::check_url(req.url, hosts); !code.empty()) {
            FilesResult r;
            r.error = std::move(code);
            deliver(cb, std::move(r));
            return;
        }
        req.scene_id = id;
        req.instance = instance;
        req.download = true;
        auto shared_req = std::allocate_shared<net::SceneHttpRequest>(
            os::LargeAllocator<net::SceneHttpRequest>(), std::move(req));
        // On the worker: begin the file, then send the request.
        auto begin = [&files, &worker, &http, deliver, busy, gate, id, cb, shared_req,
                      name = std::move(name)] {
            auto begun = files.begin_download(id, name);
            if (!begun) {
                FilesResult r;
                r.error = core::scene_files_code(begun.error());
                deliver(cb, std::move(r));
                return;
            }
            std::shared_ptr<core::FileDownload> dl = std::move(begun.value());
            // On the worker once the transfer is over (done() runs on the
            // proxy's task, which must not touch flash).
            auto done = [&worker, deliver, busy, gate, dl, cb](net::SceneHttpResult res) {
                auto result = std::allocate_shared<net::SceneHttpResult>(
                    os::LargeAllocator<net::SceneHttpResult>(), std::move(res));
                auto finish = [deliver, gate, dl, cb, result] {
                    const int status = result->status;
                    const bool ok = result->error.empty() && status >= 200 && status < 300;
                    auto kept = dl->finish(ok);
                    gate->forget(dl.get());
                    FilesResult r;
                    if (std::string own = dl->error(); !own.empty()) {
                        r.error = std::move(own);
                    } else if (!result->error.empty()) {
                        r.error = result->error;
                    } else if (ok && !kept) {
                        r.error = core::scene_files_code(kept.error());
                    } else {
                        r.payload =
                            FilesDownloaded{status, ok ? kept.value() : 0, result->deferred};
                    }
                    deliver(cb, std::move(r));
                };
                // Without the worker the reservation goes and the .dl waits
                // for the next count. Not forgotten here: this may run inside
                // submit(), under the gate's lock.
                if (!worker.submit(std::move(finish))) {
                    dl->abandon();
                    busy(cb);
                }
            };
            {
                std::lock_guard<os::Mutex> lock(gate->mu);
                if (gate->open) {
                    gate->active.push_back(dl);
                    shared_req->on_body = [dl](std::span<const uint8_t> chunk) {
                        return dl->push(chunk);
                    };
                    http.submit(std::move(*shared_req), std::move(done));
                    return;
                }
            }
            // The scene closed while this waited for the worker.
            dl->finish(false);
        };
        if (!worker.submit(std::move(begin))) busy(cb);
    };
}

void SceneRuntime::cancel_files(Slot& s) {
    if (!s.files_gate) return;
    std::vector<std::shared_ptr<core::FileDownload>> active;
    {
        std::lock_guard<os::Mutex> lock(s.files_gate->mu);
        s.files_gate->open = false;
        active.swap(s.files_gate->active);
    }
    for (auto& dl : active) {
        dl->cancel();
        if (!deps_.worker.submit([dl] { dl->finish(false); })) dl->abandon();
    }
}

bool SceneRuntime::step(Slot& s, const std::optional<ScriptError>& err) {
    if (!err) return true;
    fault(s, *err);
    return false;
}

bool SceneRuntime::submit(Slot& s) {
    if (&s != front_) {
        // No view: the tree is up to date, and a rebuild sends it whole.
        s.scene->batch().clear();
        return true;
    }
    UiBatch b = s.scene->batch().take(s.instance);
    if (!b.ops.empty()) patches_.submit(std::move(b));
    if (patches_.take_refresh_request(s.instance)) {
        if (!step(s, s.scene->refresh())) return false;
        UiBatch r = s.scene->batch().take(s.instance);
        r.refresh = true;
        patches_.submit(std::move(r));
    }
    return true;
}

bool SceneRuntime::run_step(Slot& s, const std::optional<ScriptError>& err) {
    return step(s, err) && submit(s);
}

std::optional<ScriptError> SceneRuntime::start_scene(Slot& s) {
    const int want = s.package->manifest.schema_version;
    const int have = s.store ? s.store->data_version() : want;
    if (have == core::kDataVersionUnreadable)
        return ScriptError{ScriptFault::Runtime, "", 0, "data version unreadable"};
    if (have == 0) s.store->set_initial_version(want);
    if (have > want)
        return ScriptError{
            ScriptFault::Runtime, "", 0,
            "data version " + std::to_string(have) + " newer than scene " + std::to_string(want)};
    if (have != 0 && have < want) {
        WLOG_I(kTag, "%s: migrating data from version %d to %d", s.id.c_str(), have, want);
        return s.scene->start(have);
    }
    return s.scene->start();
}

void SceneRuntime::release_locks(Slot& s) {
    if (deps_.locks != nullptr) deps_.locks->release_holder(scene_tag(s.id));
    if (s.scene) s.scene->api().screen_lock = 0;
}

void SceneRuntime::notice(SceneNoticeKind kind, const Slot& s) {
    if (deps_.on_notice) deps_.on_notice(SceneNotice{kind, s.id, s.name});
}

std::vector<const SceneRuntime::Slot*> SceneRuntime::background_order() const {
    std::vector<const Slot*> in_back;
    for (const auto& s : slots_)
        if (s->background) in_back.push_back(s.get());
    std::sort(in_back.begin(), in_back.end(), [](const Slot* a, const Slot* b) {
        if (a->last_front_ms != b->last_front_ms) return a->last_front_ms < b->last_front_ms;
        return a->seq < b->seq;
    });
    return in_back;
}

void SceneRuntime::update_background_list() {
    list_dirty_ = true;
    std::map<std::string, uint32_t, std::less<>> ids;
    for (const auto& s : slots_) ids[s->id] = s->instance;
    const std::vector<const Slot*> in_back = background_order();
    std::vector<BackgroundScene> list;
    list.reserve(in_back.size());
    for (const Slot* s : in_back) list.push_back({s->id, s->name, s->background_since_ms});
    std::lock_guard<os::Mutex> lock(status_mu_);
    background_ = std::move(list);
    slot_instances_ = std::move(ids);
}

void SceneRuntime::erase_slot(Slot& s) {
    if (front_ == &s) front_ = nullptr;
    std::erase_if(slots_, [&](const os::LargeUnique<Slot>& p) { return p.get() == &s; });
    update_background_list();
}

void SceneRuntime::fault(Slot& s, const ScriptError& err) {
    const std::string text = err.text();
    if (s.scene) s.scene->batch().clear();
    release_locks(s);
    if (s.scene) release_sensors(s.scene->api());
    s.scene.reset();  // the VM goes with it, and its timers
    cancel_files(s);
    if (deps_.http != nullptr) deps_.http->cancel_scene(s.id);
    s.shown = false;
    patches_.clear(s.instance);
    if (s.store) {
        s.store->flush();
        s.store->close();
        s.store.reset();
    }
    set_status(s.id, core::SceneState::Error, text);
    WLOG_E(scene_tag(s.id).c_str(), "%s", text.c_str());
    publish(s.id, "error");
    if (&s != front_) {
        // Spec 3.2: no panel for a scene nobody is looking at, a system
        // notice instead; the slot goes. One that fails while it is being
        // restored never came up: the restore failed (spec 3.6).
        notice(s.restoring ? SceneNoticeKind::RestoreFailed : SceneNoticeKind::Faulted, s);
        erase_slot(s);
        return;
    }
    ui_view_.store(0);
    ui::ErrorPanelInfo info{s.name, err.file, err.line, err.message};
    const uint32_t instance = s.instance;
    post_ui({UiJob::Kind::Other, instance,
             [this, info = std::move(info), id = s.id, instance]() mutable {
                 deps_.system.show_error(
                     std::move(info),
                     [this, id] {
                         if (!push(make_message(id, 0, msg::Reload{false, nullptr}), 0))
                             WLOG_W(kTag, "%s: reload not passed on: scene queue full", id.c_str());
                     },
                     [this, id, instance] {
                         if (!push(make_message(id, instance, msg::Close{}), 0))
                             WLOG_W(kTag, "%s: back not passed on: scene queue full", id.c_str());
                     });
             }});
}

void SceneRuntime::go_back(Slot& s, std::shared_ptr<SceneWaiter> done, const Slot* except) {
    if (s.scene && s.declares_background)
        to_background(s, std::move(done), except);
    else
        close_slot(s, std::move(done));
}

void SceneRuntime::forget_front() {
    front_ = nullptr;
    std::lock_guard<os::Mutex> lock(status_mu_);
    foreground_id_.reset();
    foreground_name_.clear();
    foreground_instance_ = 0;
}

void SceneRuntime::drop_view(const Slot& s, bool closing) {
    ui_view_.store(0);
    patches_.clear(s.instance);
    // Logged once the screen it covered is back.
    post_ui({UiJob::Kind::Drop, s.instance, [this, instance = s.instance, id = s.id, closing] {
                 ui_drop_view(instance);
                 if (closing) WLOG_I(kTag, "%s: closed", id.c_str());
             }});
}

void SceneRuntime::to_background(Slot& s, std::shared_ptr<SceneWaiter> done, const Slot* except) {
    auto finish = [&] {
        if (done) done->finish(Result<void>::ok());
    };
    // Spec 3.2: on_hide is the scene's last step in front.
    std::optional<ScriptError> hide_error;
    if (s.shown) {
        s.shown = false;
        hide_error = s.scene->hook("on_hide");
        // What it changed would only go to a view that is about to go;
        // the tree has it, and a resume sends the tree whole.
        s.scene->batch().clear();
    }
    release_locks(s);
    // Spec 6.2: its subscriptions pause until it comes back.
    pause_sensors(s.scene->api(), true);
    const uint64_t now = os::now_ms();
    if (&s == front_) {
        forget_front();
        drop_view(s, false);
    }
    s.background = true;
    s.last_front_ms = now;
    s.background_since_ms = now;
    s.seq = next_seq_++;
    SceneApi& api = s.scene->api();
    api.background = true;
    api.close_requested = false;
    update_background_list();
    // The user asked to leave: an error in on_hide does not bring the
    // scene back as a panel; it faults as one in the background.
    if (hide_error) {
        fault(s, *hide_error);
        return finish();
    }
    if (!run_step(s, s.scene->hook("on_state", "background"))) return finish();
    set_status(s.id, core::SceneState::Background, "");
    publish(s.id, "background");
    if (s.store) s.store->flush();
    // Spec 3.5: one too many there now closes the least recently in front
    // (never this one, the most recent; it may go for the budget when
    // `except` comes forward).
    enforce_limits(false, except);
    finish();
}

void SceneRuntime::resume(Slot& s, std::shared_ptr<SceneWaiter> waiter) {
    auto done = [&](Result<void> r) {
        if (waiter) waiter->finish(std::move(r));
    };
    const std::string id = s.id;
    auto failed = [&] { done(Result<void>::err(ErrorCode::InvalidArgument, status(id).error)); };
    // The scene in front goes back; this one does not count towards the
    // background's limit, as it is leaving it.
    if (front_ != nullptr && front_ != &s) go_back(*front_, nullptr, &s);
    s.background = false;
    s.last_front_ms = os::now_ms();
    SceneApi& api = s.scene->api();
    api.background = false;
    pause_sensors(api, false);
    if (!visible_ && !s.declares_background) api.timers.pause(os::now_ms());
    front_ = &s;
    {
        std::lock_guard<os::Mutex> lock(status_mu_);
        foreground_id_ = id;
        foreground_name_ = s.name;
        foreground_instance_ = s.instance;
    }
    update_background_list();
    // Spec 3.5: room for the scene coming forward.
    enforce_limits(false);
    show_view(s);

    const uint64_t t0 = os::now_ms();
    if (!run_step(s, s.scene->rebuild())) return failed();
    // For the device measurement of spec 9.3.
    WLOG_I(scene_tag(id).c_str(), "rebuilt in %" PRIu64 " ms", os::now_ms() - t0);
    if (visible_) {
        s.shown = true;
        if (!run_step(s, s.scene->hook("on_show"))) return failed();
    }
    if (!run_step(s, s.scene->hook("on_state", "foreground"))) return failed();
    set_status(id, core::SceneState::Running, "");
    publish(id, "running");
    done(Result<void>::ok());
}

void SceneRuntime::close_slot(Slot& s, std::shared_ptr<SceneWaiter> done) {
    const bool was_running = s.scene != nullptr;
    if (was_running && s.background && !quit_) {
        // For open_background() (a reinstall closes the scene first).
        std::erase_if(departed_, [&](const Departed& d) { return d.id == s.id; });
        if (departed_.size() >= kMaxSlots) departed_.erase(departed_.begin());
        departed_.push_back({s.id, s.reason, s.last_front_ms, s.seq});
    }
    if (s.scene) {
        // Spec 3.4: each hook has its own time limit, and closing goes on
        // whatever they do.
        const std::string tag = scene_tag(s.id);
        auto run_hook = [&](const char* hook) {
            if (auto err = s.scene->hook(hook))
                WLOG_W(tag.c_str(), "%s: %s", hook, err->text().c_str());
        };
        if (s.shown) run_hook("on_hide");
        run_hook("on_close");
    }
    // After the hooks, so a lock, a request or a subscription on_close
    // takes goes too.
    release_locks(s);
    if (s.scene) release_sensors(s.scene->api());
    s.scene.reset();
    cancel_files(s);
    if (deps_.http != nullptr) deps_.http->cancel_scene(s.id);
    // Before the caller can be answered (from the worker, below): whoever
    // close() answers sees the scene stopped, and anything it publishes
    // next ("deleted") comes after "stopped". A faulted scene publishes
    // nothing here; it stays in the error state.
    const bool in_front = &s == front_;
    if (in_front) forget_front();
    if (was_running) {
        set_status(s.id, core::SceneState::Stopped, "");
        publish(s.id, "stopped");
    }
    if (s.store) {
        // A caller of close() is answered once the final flush has run, so
        // it may move the scene's data/ (an install) without a write
        // landing in the middle. A failed flush is logged by the store.
        s.store->flush([done](Result<void>) {
            if (done) done->finish(Result<void>::ok());
        });
        s.store->close();
        s.store.reset();
    } else {
        // Faulted: its store was flushed and closed then, and that flush
        // may still be queued.
        finish_after_queued_io(std::move(done));
    }
    if (in_front) {
        drop_view(s, true);
    } else {
        patches_.clear(s.instance);
        WLOG_I(kTag, "%s: closed", s.id.c_str());
    }
    erase_slot(s);
}

void SceneRuntime::finish_after_queued_io(std::shared_ptr<SceneWaiter> done) {
    if (!done) return;
    // The worker runs jobs in order: this one runs after every flush the
    // scene queued before, so the caller may then move or delete its data.
    if (!deps_.worker.submit([done] { done->finish(Result<void>::ok()); }))
        done->finish(Result<void>::err(ErrorCode::Busy, "storage worker busy"));
}

void SceneRuntime::hide_scenes() {
    for (uint32_t instance : instances()) {
        Slot* s = find(instance);
        if (s == nullptr || !s->scene) continue;
        if (s == front_) {
            // Spec 3.3: a scene that declares "background" keeps its
            // timers going with the screen off.
            if (!s->declares_background) s->scene->api().timers.pause(os::now_ms());
            if (s->shown) {
                s->shown = false;
                if (!run_step(*s, s->scene->hook("on_hide"))) continue;
            }
        }
        // Spec 6.1: the store flushes before the screen goes off.
        if (s->store) s->store->flush();
    }
}

void SceneRuntime::follow_visibility() {
    const bool want = want_visible_.load();
    if (want == visible_) return;
    visible_ = want;
    if (want) {
        show_scene(true);
    } else {
        hide_scenes();
    }
}

void SceneRuntime::show_scene(bool screen_came_on) {
    Slot* s = front_;
    if (s == nullptr || !s->scene) return;
    const uint32_t instance = s->instance;
    auto still_front = [&] {
        s = find(instance);
        return s != nullptr && s == front_ && s->scene;
    };
    // Spec 3.4: timers first (each missed one once), the minute, on_show.
    // A scene whose timers ran on gets the minute too (spec 3.3).
    bool catch_up = screen_came_on && s->declares_background;
    TimerSet& timers = s->scene->api().timers;
    if (timers.paused()) {
        catch_up = true;
        timers.resume(os::now_ms());
        run_timers();
        if (!still_front()) return;
    }
    if (catch_up && wants_minute(*s)) {
        s->ticker.force();
        if (s->ticker.poll(wall_now(), os::now_ms())) minute(instance);
        if (!still_front()) return;
    }
    if (s->shown) return;
    s->shown = true;
    run_step(*s, s->scene->hook("on_show"));
}

// --- Scene task: timers and device events -------------------------------------

std::optional<int64_t> SceneRuntime::wall_now() const {
    const int64_t t =
        deps_.wall_clock ? deps_.wall_clock() : static_cast<int64_t>(std::time(nullptr));
    if (t < MinuteTicker::kValidSince) return std::nullopt;
    return t;
}

DeviceInfo SceneRuntime::device_info() const {
    DeviceInfo i;
    i.shape = deps_.shape;
    i.safe = hal::safe_rect(deps_.shape);
    i.status_area = ui::SystemLayer::status_area(deps_.shape);
    const int set = lang_.load();
    const char* lang = ui::lang_code(set < 0 ? ui::lang() : static_cast<ui::Lang>(set));
    std::snprintf(i.lang, sizeof(i.lang), "%s", lang);
    i.utc_offset_min = utc_offset_min();
    i.time_24h = time_24h_.load() != 0;
    if (auto t = wall_now()) {
        i.has_time = true;
        i.time = *t;
    }
    if (!deps_.device_status) return i;
    const Value status = deps_.device_status();
    if (const Value* b = status.find("battery"); b != nullptr) {
        if (const Value* p = b->find("percent");
            p != nullptr && p->type() == Value::Type::Int && p->as_int() >= 0) {
            i.has_battery = true;
            i.battery = p->as_int();
        }
        if (const Value* c = b->find("charging"); c != nullptr && c->type() == Value::Type::Bool)
            i.charging = c->as_bool();
    }
    if (const Value* w = status.find("wifi"); w != nullptr && w->type() == Value::Type::String) {
        const std::string& v = w->as_string();
        if (v == "connecting" || v == "backoff" || v == "connected")
            std::snprintf(i.wifi, sizeof(i.wifi), "%s", v.c_str());
    }
    return i;
}

bool SceneRuntime::wants_minute(const Slot& s) const {
    if (!s.scene) return false;
    const SceneApi& api = s.scene->api();
    return api.wants(DeviceEvent::Minute) || api.wants(DeviceEvent::Battery) ||
           api.wants(DeviceEvent::Lang);
}

bool SceneRuntime::minute_ticks(const Slot& s) const {
    return wants_minute(s) && (visible_ || s.declares_background || s.background);
}

uint32_t SceneRuntime::scene_wait_ms() {
    const uint64_t now = os::now_ms();
    uint64_t wait = os::kForever;
    for (const auto& s : slots_) {
        if (!s->scene) continue;
        if (auto due = s->scene->api().timers.next_due())
            wait = std::min<uint64_t>(wait, *due > now ? *due - now : 0);
        if (minute_ticks(*s)) wait = std::min(wait, s->ticker.next_poll_ms(wall_now(), now));
    }
    return static_cast<uint32_t>(std::min<uint64_t>(wait, os::kForever));
}

void SceneRuntime::run_scene_clock() {
    run_timers();
    for (uint32_t instance : instances()) {
        Slot* s = find(instance);
        if (s != nullptr && minute_ticks(*s) && s->ticker.poll(wall_now(), os::now_ms()))
            minute(instance);
    }
    close_if_asked();
}

void SceneRuntime::clock_set() {
    // Fires once and records the new minute, so the poll after the message
    // sees no jump; a poll that already saw it (the task woke between the
    // clock moving and this message) already fired, and this adds nothing.
    // A scene whose minute does not tick now gets one when the screen
    // comes on.
    for (uint32_t instance : instances()) {
        Slot* s = find(instance);
        if (s != nullptr && minute_ticks(*s) && s->ticker.sync(wall_now(), os::now_ms()))
            minute(instance);
    }
}

void SceneRuntime::run_timers() {
    for (uint32_t instance : instances()) {
        Slot* s = find(instance);
        if (s == nullptr || !s->scene) continue;
        const std::string tag = scene_tag(s->id);
        const std::vector<uint32_t> due = s->scene->api().timers.due(os::now_ms());
        for (uint32_t id : due) {
            s = find(instance);
            if (s == nullptr || !s->scene) break;
            // Spec 3.5: each callback is a message of its own; one of the
            // scene in front may follow a background one that grew.
            if (s == front_) enforce_limits(false);
            const bool once = !s->scene->api().timers.contains(id);
            const uint64_t t0 = os::now_ms();
            run_step(*s, s->scene->timer(id, once));
            const uint64_t ms = os::now_ms() - t0;
            if (ms > kSlowMs) WLOG_W(tag.c_str(), "slow message timer %" PRIu64 " ms", ms);
            close_if_asked();
        }
    }
}

void SceneRuntime::minute(uint32_t instance) {
    const DeviceInfo info = device_info();
    fire(instance, DeviceEvent::Minute, info);
    // Also catches a battery or lang message the full queue dropped.
    device_event(instance, DeviceEvent::Battery, info);
    device_event(instance, DeviceEvent::Lang, info);
}

void SceneRuntime::device_event(DeviceEvent ev) {
    const DeviceInfo info = device_info();
    for (uint32_t instance : instances()) device_event(instance, ev, info);
}

void SceneRuntime::device_event(uint32_t instance, DeviceEvent ev, const DeviceInfo& info) {
    Slot* s = find(instance);
    if (s == nullptr || !s->scene) return;
    if (ev == DeviceEvent::Battery) {
        if (info.has_battery == s->has_battery && info.battery == s->battery &&
            info.charging == s->charging)
            return;
        s->has_battery = info.has_battery;
        s->battery = info.battery;
        s->charging = info.charging;
    } else if (ev == DeviceEvent::Lang) {
        if (s->lang == info.lang) return;
        s->lang = info.lang;
    } else if (!minute_ticks(*s)) {
        // A "minute" for a new time zone; the screen coming on sends one.
        return;
    }
    fire(instance, ev, info);
}

void SceneRuntime::fire(uint32_t instance, DeviceEvent ev, const DeviceInfo& info) {
    Slot* s = find(instance);
    if (s == nullptr || !s->scene) return;
    // Spec 3.5: one event reaches every scene in the same pass; the
    // handler of the scene in front may follow a background one that grew.
    // (Never evicts `s` itself: the scene in front is never a candidate.)
    if (s == front_) enforce_limits(false);
    const std::string tag = scene_tag(s->id);
    const uint64_t t0 = os::now_ms();
    run_step(*s, s->scene->device(ev, info));
    const uint64_t ms = os::now_ms() - t0;
    if (ms > kSlowMs)
        WLOG_W(tag.c_str(), "slow message %s %" PRIu64 " ms", device_event_name(ev), ms);
}

void SceneRuntime::close_if_asked() {
    for (uint32_t instance : instances()) {
        Slot* s = find(instance);
        if (s == nullptr || !s->scene) continue;
        SceneApi& api = s->scene->api();
        if (api.exit_requested) {
            close_slot(*s);
        } else if (api.close_requested) {
            // "Back" means nothing to a scene already in the background.
            api.close_requested = false;
            if (s == front_) go_back(*s, nullptr);
        }
    }
}

// --- UI thread ---------------------------------------------------------------

void SceneRuntime::post_ui(UiJob job) {
    {
        std::lock_guard<os::Mutex> lock(ui_jobs_mu_);
        // The drop still runs (it may log the close); only what it undoes
        // goes.
        if (job.kind == UiJob::Kind::Drop) cancel_show_locked(job.instance);
        ui_jobs_.push_back(std::move(job));
    }
    // Only a nudge: if the UI queue is full, the next frame (or the next
    // nudge) runs the job. Never retried here, so the scene task never
    // waits for the UI thread.
    deps_.ui.post([this] { run_ui_jobs(); });
}

bool SceneRuntime::cancel_show_locked(uint32_t instance) {
    size_t show = ui_jobs_.size();
    for (size_t i = ui_jobs_.size(); i-- > 0;) {
        if (ui_jobs_[i].kind == UiJob::Kind::Show && ui_jobs_[i].instance == instance) {
            show = i;
            break;
        }
    }
    if (show == ui_jobs_.size()) return false;
    // A Drop of it after the Show would have cancelled it already, so what
    // follows the Show for this instance is its error panel at most.
    os::LargeVector<UiJob> kept;
    for (size_t i = 0; i < ui_jobs_.size(); ++i) {
        if (i >= show && ui_jobs_[i].instance == instance) continue;
        kept.push_back(std::move(ui_jobs_[i]));
    }
    ui_jobs_.swap(kept);
    return true;
}

void SceneRuntime::run_ui_jobs() {
    os::LargeVector<UiJob> jobs;
    {
        std::lock_guard<os::Mutex> lock(ui_jobs_mu_);
        jobs.swap(ui_jobs_);
    }
    for (auto& job : jobs) job.fn();
}

void SceneRuntime::ui_frame() {
    // Before the patches: a view these patches are for may be in here.
    run_ui_jobs();
    UiSide& u = *ui_side_;
    const uint64_t view = ui_view_.load();
    if (!u.view || view == 0 || u.token != view) return;
    patches_.apply_frame(*u.view, u.instance);
}

void SceneRuntime::ui_show_view(uint32_t instance, uint64_t token, std::string id,
                                std::shared_ptr<const core::ScenePackage> package) {
    UiSide& u = *ui_side_;
    if (u.view) ui_drop_view(0);
    ViewCallbacks cb;
    cb.on_event = [this, instance, id](NodeId node, UiEvent ev, PropValue value) {
        // Focus only opens the keyboard; no handler takes it.
        if (ev == UiEvent::Focus) return;
        if (!push(make_message(id, instance, msg::Event{node, ev, std::move(value)}), 0) &&
            running_)
            WLOG_W(kTag, "%s: event dropped: scene queue full", id.c_str());
    };
    cb.on_list_range = [this, instance, id](NodeId list, uint32_t first, uint32_t count) {
        if (!push(make_message(id, instance, msg::ListRange{list, first, count}), 0) && running_)
            WLOG_W(kTag, "%s: list rows not asked for: scene queue full", id.c_str());
    };
    cb.on_warning = [tag = scene_tag(id)](NodeId, std::string_view message) {
        WLOG_W(tag.c_str(), "%.*s", static_cast<int>(message.size()), message.data());
    };
    u.view = std::make_unique<SceneView>(deps_.system, deps_.shape, std::move(cb));
    u.view->set_assets(std::move(package));
    u.instance = instance;
    u.token = token;
    u.home = lv_screen_active();
    lv_screen_load(u.view->screen());
    deps_.system.set_back_handler([this, instance, id] {
        if (!push(make_message(id, instance, msg::Close{}), 0))
            WLOG_W(kTag, "%s: back not passed on: scene queue full", id.c_str());
    });
}

void SceneRuntime::ui_drop_view(uint32_t instance) {
    if (!ui_side_) return;
    UiSide& u = *ui_side_;
    // Another scene's view (one that came up after this one went) stays.
    if (instance != 0 && u.instance != instance) return;
    // The back gesture is the view's only while it is in front: whoever
    // loaded another screen over it (the shell going home, say) has
    // taken the gesture since, and keeps it.
    if (!u.view || lv_screen_active() == u.view->screen()) deps_.system.set_back_handler({});
    deps_.system.hide_error();
    if (!u.view) return;
    // An active screen must not be deleted: show the one the view
    // replaced first (or, if that is gone, the one empty stand-in, made the
    // first time it is needed and reused after).
    if (lv_screen_active() == u.view->screen()) {
        lv_obj_t* back = u.home;
        if (back == nullptr || !lv_obj_is_valid(back)) {
            if (u.stand_in == nullptr || !lv_obj_is_valid(u.stand_in))
                u.stand_in = lv_obj_create(nullptr);
            back = u.stand_in;
        }
        lv_screen_load(back);
    }
    u.view.reset();
    u.instance = 0;
    u.token = 0;
    u.home = nullptr;
}

}  // namespace wisp::scene
