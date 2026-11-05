#include "wisp/scene/scene_runtime.hpp"

#include <algorithm>
#include <cinttypes>
#include <cstdio>
#include <ctime>
#include <mutex>
#include <utility>

#include "scene_instance.hpp"
#include "scene_message.hpp"
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
// post_ui() retries this often, this long apart, before it gives up.
constexpr int kUiPostTries = 1000;
constexpr uint32_t kUiPostRetryMs = 2;

// "scene.<id>": the tag of the scene's own log.
std::string scene_tag(std::string_view id) {
    std::string tag = "scene.";
    tag.append(id);
    return tag;
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

struct SceneRuntime::Foreground {
    std::string id;
    std::string name;
    uint32_t instance = 0;
    Value args;
    std::shared_ptr<const core::ScenePackage> package;
    std::shared_ptr<core::SceneStore> store;
    // Null once the scene faulted: its error panel is up.
    std::unique_ptr<SceneInstance> scene;
    // scene.on_show() ran and on_hide() has not.
    bool shown = false;
    MinuteTicker ticker;
    // What the scene last saw, for "battery" and "lang".
    bool has_battery = false;
    int64_t battery = 0;
    bool charging = false;
    std::string lang;
};

struct SceneRuntime::UiSide {
    std::unique_ptr<SceneView> view;
    uint32_t instance = 0;
    // The screen the view replaced, loaded again when it goes.
    lv_obj_t* home = nullptr;
};

SceneRuntime::SceneRuntime(SceneRuntimeDeps deps, SceneRuntimeConfig config)
    : deps_(std::move(deps)),
      config_(config),
      queue_(config.queue_capacity),
      link_(std::allocate_shared<Link>(os::LargeAllocator<Link>())) {
    link_->rt = this;
}

void SceneRuntime::seed_utc_offset(int minutes) {
    if (minutes < -720 || minutes > 840) return;
    int unset = kUnsetOffset;
    utc_offset_min_.compare_exchange_strong(unset, minutes);
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
        auto it = e.payload.find("language");
        if (it == e.payload.end() || it->second.type() != Value::Type::String) return;
        auto lang = ui::parse_lang(it->second.as_string());
        if (!lang) return;
        lang_.store(static_cast<int>(*lang));
        if (!push(make_message("", 0, msg::Device{DeviceEvent::Lang}), 0) && running_)
            WLOG_W(kTag, "language change not passed on now (scene queue full): next minute");
    });
    return Result<void>::ok();
}

void SceneRuntime::stop() {
    if (!started_ || !running_.exchange(false)) return;
    deps_.bus.unsubscribe(power_sub_);
    deps_.bus.unsubscribe(settings_sub_);
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
    // Also a fence: every UI job the scene task posted has run once this
    // returns, and the old frame hook is no longer running.
    deps_.ui.run_blocking([this] { ui_drop_view(); });
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
    if (deps_.repo.installing(id))
        return Result<void>::err(ErrorCode::Busy, "scene " + id + " is being installed");
    auto waiter = std::make_shared<SceneWaiter>();
    return request(make_message(id, 0, msg::Load{std::move(args), waiter}), waiter);
}

Result<void> SceneRuntime::close(const std::string& id) {
    auto waiter = std::make_shared<SceneWaiter>();
    return request(make_message(id, 0, msg::Close{waiter}), waiter);
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
    push(make_message(id, 0, msg::Close{waiter}), 0);
}

void SceneRuntime::on_installed(const std::string& id) {
    if (!push(make_message(id, 0, msg::Reload{true, nullptr}), 0))
        WLOG_W(kTag, "%s: installed, but the scene task was not told", id.c_str());
}

void SceneRuntime::set_visible(bool visible) {
    msg::Body body = visible ? msg::Body{msg::Show{}} : msg::Body{msg::Hide{}};
    if (!push(make_message("", 0, std::move(body)), 0) && running_)
        WLOG_W(kTag, "screen %s not passed on: scene queue full", visible ? "on" : "off");
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

Result<void> SceneRuntime::click_ref(std::string_view ref) {
    if (!running_ || !ui_side_)
        return Result<void>::err(ErrorCode::Busy, "scene runtime not running");
    Result<void> r = Result<void>::err(ErrorCode::NotFound, "no scene is running");
    auto click = [&] {
        UiSide& u = *ui_side_;
        if (!u.view || u.instance == 0 || u.instance != ui_instance_.load()) return;
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
            handle(*m);
            close_if_asked();
            const uint64_t ms = os::now_ms() - t0;
            if (ms > kSlowMs) {
                std::string_view id = m->scene_id;
                if (id.empty() && fg_) id = fg_->id;
                std::string tag = id.empty() ? std::string(kTag) : scene_tag(id);
                WLOG_W(tag.c_str(), "slow message %s %" PRIu64 " ms", message_kind(m->body), ms);
            }
        }
        run_delayed();
        run_scene_clock();
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

SceneRuntime::Foreground* SceneRuntime::target(const SceneMessage& m) {
    if (!fg_ || fg_->instance != m.instance) {
        ++stale_dropped_;
        return nullptr;
    }
    return fg_.get();
}

void SceneRuntime::handle(SceneMessage& m) {
    msg::Body& body = m.body;
    if (auto* b = std::get_if<msg::Load>(&body)) {
        begin_open(m.scene_id, std::move(b->args), b->done);
    } else if (std::holds_alternative<msg::Open>(body)) {
        finish_open(m);
    } else if (auto* b = std::get_if<msg::Close>(&body)) {
        // From the API (instance 0) by id; from the view, the panel or
        // the gesture for one instance.
        if (m.instance == 0 ? (fg_ && fg_->id == m.scene_id) : target(m) != nullptr)
            close_foreground(b->done);
        else
            finish_after_queued_io(b->done);
    } else if (auto* b = std::get_if<msg::Reload>(&body)) {
        const bool in_front = fg_ && fg_->id == m.scene_id;
        if (b->if_foreground && !in_front) {
            set_status(m.scene_id, core::SceneState::Stopped, "");
            publish(m.scene_id, "stopped");
        } else {
            begin_open(m.scene_id, in_front ? fg_->args : Value(), b->done);
        }
    } else if (std::holds_alternative<msg::Show>(body)) {
        visible_ = true;
        show_scene();
    } else if (std::holds_alternative<msg::Hide>(body)) {
        visible_ = false;
        hide_scene();
    } else if (auto* b = std::get_if<msg::Event>(&body)) {
        Foreground* f = target(m);
        if (f != nullptr && f->scene && step(f->scene->event(b->node, b->ev, b->value))) submit();
    } else if (auto* b = std::get_if<msg::ListRange>(&body)) {
        Foreground* f = target(m);
        if (f != nullptr && f->scene && step(f->scene->list_range(b->list, b->first, b->count)))
            submit();
    } else if (auto* b = std::get_if<msg::Device>(&body)) {
        device_event(b->kind);
    } else if (auto* b = std::get_if<msg::HttpDone>(&body)) {
        Foreground* f = target(m);
        if (f != nullptr && f->scene && step(f->scene->http(b->id, b->result))) submit();
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
    } else if (std::holds_alternative<msg::Quit>(body)) {
        close_foreground();
        quit_ = true;
    }
    // msg::Wake: run() looks at the deadlines next.
}

void SceneRuntime::begin_open(const std::string& id, Value args,
                              std::shared_ptr<SceneWaiter> waiter) {
    // The same scene closes first: its final store flush then goes to the
    // worker ahead of this load, so the reopened store reads what it
    // wrote. Another one stays until this one has loaded, so a scene that
    // cannot be opened leaves it running.
    if (fg_ && fg_->id == id) close_foreground();
    const uint32_t instance = next_instance_++;
    if (next_instance_ == 0) next_instance_ = 1;
    loading_ = instance;

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
    bool queued = worker.submit([link, &repo, store_deps, id, instance, args = std::move(args),
                                 waiter]() mutable {
        msg::Open open;
        open.args = std::move(args);
        open.done = waiter;
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
        loading_ = 0;
        WLOG_W(kTag, "%s: not opened: storage worker busy", id.c_str());
        if (waiter) waiter->finish(Result<void>::err(ErrorCode::Busy, "storage worker busy"));
    }
}

void SceneRuntime::finish_open(SceneMessage& m) {
    auto& open = std::get<msg::Open>(m.body);
    auto done = [&](Result<void> r) {
        if (open.done) open.done->finish(std::move(r));
    };
    if (m.instance != loading_) {
        // Another open came after this one.
        ++stale_dropped_;
        done(Result<void>::err(ErrorCode::Busy, "superseded by another open"));
        return;
    }
    loading_ = 0;
    const std::string& id = m.scene_id;
    if (open.error.code == ErrorCode::Ok && deps_.repo.installing(id)) {
        // Loaded before an install of it began: the package and the data
        // directory are about to be replaced under it.
        WLOG_W(kTag, "%s: not opened: its package is being installed", id.c_str());
        if (open.store) open.store->close();
        done(Result<void>::err(ErrorCode::Busy, "scene " + id + " is being installed"));
        return;
    }
    if (open.error.code != ErrorCode::Ok) {
        if (open.error.code == ErrorCode::NotFound) {
            WLOG_W(kTag, "%s: no such scene", id.c_str());
        } else {
            std::string text = "cannot load: " + open.error.message;
            WLOG_E(scene_tag(id).c_str(), "%s", text.c_str());
            set_status(id, core::SceneState::Error, text);
            publish(id, "error");
        }
        done(Result<void>::err(open.error.code, open.error.message));
        return;
    }

    close_foreground();
    fg_ = std::make_unique<Foreground>();
    Foreground& f = *fg_;
    f.id = id;
    f.name = open.package->manifest.name;
    f.instance = m.instance;
    f.args = std::move(open.args);
    f.package = open.package;
    f.store = std::move(open.store);
    f.scene = std::make_unique<SceneInstance>(id, f.package, config_.vm);
    SceneApi& api = f.scene->api();
    api.device = [this] { return device_info(); };
    api.args = &f.args;
    api.store = f.store.get();
    if (deps_.http != nullptr) wire_http(f);
    // Opened with the screen off: timers wait for it to come on.
    if (!visible_) api.timers.pause(os::now_ms());
    {
        const DeviceInfo now = device_info();
        f.has_battery = now.has_battery;
        f.battery = now.battery;
        f.charging = now.charging;
        f.lang = now.lang;
    }
    {
        std::lock_guard<os::Mutex> lock(status_mu_);
        foreground_id_ = id;
        foreground_instance_ = f.instance;
    }
    // The view exists before any batch of this instance is queued: the
    // frame hook applies only to the view of the instance it is told.
    post_ui([this, instance = f.instance, id, package = f.package] {
        ui_show_view(instance, id, package);
    });
    ui_instance_.store(f.instance);

    auto failed = [&] {
        std::string error = status(id).error;
        done(Result<void>::err(ErrorCode::InvalidArgument, error));
    };
    if (!step(fg_->scene->start()) || !submit()) return failed();
    set_status(id, core::SceneState::Running, "");
    publish(id, "running");
    if (visible_) {
        fg_->shown = true;
        if (!step(fg_->scene->hook("on_show")) || !submit()) return failed();
    }
    done(Result<void>::ok());
}

void SceneRuntime::wire_http(Foreground& f) {
    net::SceneHttp& http = *deps_.http;
    http.set_hosts(f.id, f.package->manifest.http_hosts);
    // Results come back from the proxy's task (or, for a request refused
    // at once, from inside submit() on this task) as messages for this
    // instance; one for an instance that is gone is dropped like any
    // stale message.
    auto deliver = [link = link_, id = f.id, instance = f.instance](uint32_t cb,
                                                                    net::SceneHttpResult r) {
        auto m = make_message(id, instance, msg::HttpDone{cb, std::move(r)});
        std::lock_guard<os::Mutex> lock(link->mu);
        if (link->rt == nullptr) return;
        if (!post_scene_message(*link->rt, std::move(m)))
            WLOG_W(scene_tag(id).c_str(), "http: a result was dropped: scene queue full");
    };
    SceneApi& api = f.scene->api();
    api.http_submit = [&http, deliver, id = f.id, instance = f.instance](
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

bool SceneRuntime::step(const std::optional<ScriptError>& err) {
    if (!err) return true;
    fault(*err);
    return false;
}

bool SceneRuntime::submit() {
    Foreground& f = *fg_;
    UiBatch b = f.scene->batch().take(f.instance);
    if (!b.ops.empty()) patches_.submit(std::move(b));
    if (patches_.take_refresh_request()) {
        if (!step(f.scene->refresh())) return false;
        UiBatch r = f.scene->batch().take(f.instance);
        r.refresh = true;
        patches_.submit(std::move(r));
    }
    return true;
}

void SceneRuntime::fault(const ScriptError& err) {
    Foreground& f = *fg_;
    const std::string text = err.text();
    if (f.scene) f.scene->batch().clear();
    f.scene.reset();  // the VM goes with it
    if (deps_.http != nullptr) deps_.http->cancel_scene(f.id);
    f.shown = false;
    ui_instance_.store(0);
    patches_.clear();
    if (f.store) {
        f.store->flush();
        f.store->close();
        f.store.reset();
    }
    set_status(f.id, core::SceneState::Error, text);
    WLOG_E(scene_tag(f.id).c_str(), "%s", text.c_str());
    publish(f.id, "error");
    ui::ErrorPanelInfo info{f.name, err.file, err.line, err.message};
    post_ui([this, info = std::move(info), id = f.id, instance = f.instance]() mutable {
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
    });
}

void SceneRuntime::close_foreground(std::shared_ptr<SceneWaiter> done) {
    if (!fg_) return;
    Foreground& f = *fg_;
    const bool was_running = f.scene != nullptr;
    if (f.scene) {
        // Spec 3.4: each hook has its own time limit, and closing goes on
        // whatever they do.
        const std::string tag = scene_tag(f.id);
        auto run_hook = [&](const char* hook) {
            if (auto err = f.scene->hook(hook))
                WLOG_W(tag.c_str(), "%s: %s", hook, err->text().c_str());
        };
        if (f.shown) run_hook("on_hide");
        run_hook("on_close");
        f.scene.reset();
    }
    // After the hooks, so a request on_close makes is cancelled too.
    if (deps_.http != nullptr) deps_.http->cancel_scene(f.id);
    if (f.store) {
        // A caller of close() is answered once the final flush has run, so
        // it may move the scene's data/ (an install) without a write
        // landing in the middle. A failed flush is logged by the store.
        f.store->flush([done](Result<void>) {
            if (done) done->finish(Result<void>::ok());
        });
        f.store->close();
        f.store.reset();
    } else {
        // Faulted: its store was flushed and closed then, and that flush
        // may still be queued.
        finish_after_queued_io(std::move(done));
    }
    ui_instance_.store(0);
    patches_.clear();
    // Logged once the screen it covered is back: a faulted scene publishes
    // nothing when it closes, as it stays in the error state.
    post_ui([this, id = f.id] {
        ui_drop_view();
        WLOG_I(kTag, "%s: closed", id.c_str());
    });
    {
        std::lock_guard<os::Mutex> lock(status_mu_);
        foreground_id_.reset();
        foreground_instance_ = 0;
    }
    if (was_running) {
        set_status(f.id, core::SceneState::Stopped, "");
        publish(f.id, "stopped");
    }
    fg_.reset();
}

void SceneRuntime::finish_after_queued_io(std::shared_ptr<SceneWaiter> done) {
    if (!done) return;
    // The worker runs jobs in order: this one runs after every flush the
    // scene queued before, so the caller may then move or delete its data.
    if (!deps_.worker.submit([done] { done->finish(Result<void>::ok()); }))
        done->finish(Result<void>::err(ErrorCode::Busy, "storage worker busy"));
}

void SceneRuntime::hide_scene() {
    if (!fg_ || !fg_->scene) return;
    fg_->scene->api().timers.pause(os::now_ms());
    if (fg_->shown) {
        fg_->shown = false;
        if (step(fg_->scene->hook("on_hide"))) submit();
    }
    // Spec 6.1: the store flushes before the screen goes off.
    if (fg_ && fg_->store) fg_->store->flush();
}

void SceneRuntime::show_scene() {
    if (!fg_ || !fg_->scene) return;
    // Spec 3.4: timers first (each missed one once), the minute, on_show.
    TimerSet& timers = fg_->scene->api().timers;
    if (timers.paused()) {
        timers.resume(os::now_ms());
        run_timers();
        if (!fg_ || !fg_->scene) return;
        if (wants_minute()) {
            fg_->ticker.force();
            if (fg_->ticker.poll(wall_now(), os::now_ms())) minute();
            if (!fg_ || !fg_->scene) return;
        }
    }
    if (fg_->shown) return;
    fg_->shown = true;
    if (step(fg_->scene->hook("on_show"))) submit();
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
    const int set = lang_.load();
    const char* lang = ui::lang_code(set < 0 ? ui::lang() : static_cast<ui::Lang>(set));
    std::snprintf(i.lang, sizeof(i.lang), "%s", lang);
    i.utc_offset_min = utc_offset_min();
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

bool SceneRuntime::wants_minute() const {
    if (!fg_ || !fg_->scene) return false;
    const SceneApi& api = fg_->scene->api();
    return api.wants(DeviceEvent::Minute) || api.wants(DeviceEvent::Battery) ||
           api.wants(DeviceEvent::Lang);
}

uint32_t SceneRuntime::scene_wait_ms() {
    if (!fg_ || !fg_->scene) return os::kForever;
    const uint64_t now = os::now_ms();
    uint64_t wait = os::kForever;
    if (auto due = fg_->scene->api().timers.next_due()) wait = *due > now ? *due - now : 0;
    if (visible_ && wants_minute())
        wait = std::min(wait, fg_->ticker.next_poll_ms(wall_now(), now));
    return static_cast<uint32_t>(std::min<uint64_t>(wait, os::kForever));
}

void SceneRuntime::run_scene_clock() {
    run_timers();
    if (visible_ && wants_minute() && fg_->ticker.poll(wall_now(), os::now_ms())) minute();
    close_if_asked();
}

void SceneRuntime::run_timers() {
    if (!fg_ || !fg_->scene) return;
    const std::vector<uint32_t> due = fg_->scene->api().timers.due(os::now_ms());
    for (uint32_t id : due) {
        if (!fg_ || !fg_->scene) return;
        const bool once = !fg_->scene->api().timers.contains(id);
        const uint64_t t0 = os::now_ms();
        if (step(fg_->scene->timer(id, once))) submit();
        const uint64_t ms = os::now_ms() - t0;
        if (fg_ && ms > kSlowMs)
            WLOG_W(scene_tag(fg_->id).c_str(), "slow message timer %" PRIu64 " ms", ms);
        close_if_asked();
    }
}

void SceneRuntime::minute() {
    if (!fg_ || !fg_->scene) return;
    const DeviceInfo info = device_info();
    fire(DeviceEvent::Minute, info);
    // Also catches a battery or lang message the full queue dropped.
    device_event(DeviceEvent::Battery);
    device_event(DeviceEvent::Lang);
}

void SceneRuntime::device_event(DeviceEvent ev) {
    if (!fg_ || !fg_->scene) return;
    Foreground& f = *fg_;
    const DeviceInfo info = device_info();
    if (ev == DeviceEvent::Battery) {
        if (info.has_battery == f.has_battery && info.battery == f.battery &&
            info.charging == f.charging)
            return;
        f.has_battery = info.has_battery;
        f.battery = info.battery;
        f.charging = info.charging;
    } else if (ev == DeviceEvent::Lang) {
        if (f.lang == info.lang) return;
        f.lang = info.lang;
    } else if (!visible_) {
        // A "minute" for a new time zone; the screen coming on sends one.
        return;
    }
    fire(ev, info);
}

void SceneRuntime::fire(DeviceEvent ev, const DeviceInfo& info) {
    if (!fg_ || !fg_->scene) return;
    const uint64_t t0 = os::now_ms();
    if (step(fg_->scene->device(ev, info))) submit();
    const uint64_t ms = os::now_ms() - t0;
    if (fg_ && ms > kSlowMs)
        WLOG_W(scene_tag(fg_->id).c_str(), "slow message %s %" PRIu64 " ms", device_event_name(ev),
               ms);
}

void SceneRuntime::close_if_asked() {
    if (fg_ && fg_->scene && fg_->scene->api().close_requested) close_foreground();
}

// --- UI thread ---------------------------------------------------------------

void SceneRuntime::post_ui(std::function<void()> fn) {
    for (int i = 0; i < kUiPostTries; ++i) {
        if (deps_.ui.post(fn)) return;
        os::sleep_ms(kUiPostRetryMs);
    }
    WLOG_E(kTag, "UI queue stayed full: a scene UI change was dropped");
}

void SceneRuntime::ui_frame() {
    UiSide& u = *ui_side_;
    const uint32_t instance = ui_instance_.load();
    if (!u.view || instance == 0 || u.instance != instance) return;
    patches_.apply_frame(*u.view, instance);
}

void SceneRuntime::ui_show_view(uint32_t instance, std::string id,
                                std::shared_ptr<const core::ScenePackage> package) {
    UiSide& u = *ui_side_;
    if (u.view) ui_drop_view();
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
    u.home = lv_screen_active();
    lv_screen_load(u.view->screen());
    deps_.system.set_back_handler([this, instance, id] {
        if (!push(make_message(id, instance, msg::Close{}), 0))
            WLOG_W(kTag, "%s: back not passed on: scene queue full", id.c_str());
    });
}

void SceneRuntime::ui_drop_view() {
    if (!ui_side_) return;
    UiSide& u = *ui_side_;
    deps_.system.set_back_handler({});
    deps_.system.hide_error();
    if (!u.view) return;
    // An active screen must not be deleted: show the one the view
    // replaced first (or, if that is gone, an empty one).
    if (lv_screen_active() == u.view->screen()) {
        lv_obj_t* back =
            u.home != nullptr && lv_obj_is_valid(u.home) ? u.home : lv_obj_create(nullptr);
        lv_screen_load(back);
    }
    u.view.reset();
    u.instance = 0;
    u.home = nullptr;
}

}  // namespace wisp::scene
