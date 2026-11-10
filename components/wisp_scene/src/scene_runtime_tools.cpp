// SceneRuntime::call_tool() and headless scenes (scene_runtime.hpp, "Scene
// tools" and "headless"; agent-tools spec 5.2, 5.4): the caller's side,
// which waits, and the scene task's side, which loads the scene if it must
// and runs the tool's task in its VM.
//
// Every call ends in tool_end(), on the scene task, exactly once: from the
// task's outcome (tool_settle(), after each pass of the task, since
// task_done only records it), from its timeout or its caller's cancel (the
// task alone is cancelled with cancel_task() and the scene runs on), or
// from the scene closing or faulting under it (tool_abort(), called first
// thing by close_slot() and fault()). A call whose scene is still loading
// ends when its load leaves loading_ (tool_loads_dropped()) or arrives
// (finish_headless()).

#include <algorithm>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "scene_instance.hpp"
#include "scene_message.hpp"
#include "wisp/base/json.hpp"
#include "wisp/core/log.hpp"
#include "wisp/scene/scene_runtime.hpp"

namespace wisp::scene {

namespace {

constexpr const char* kTag = "scene";
// How often the caller looks at its cancel flag.
constexpr uint32_t kPollMs = 20;
// How long, past the call's own deadline, the caller waits for the scene
// task to answer before it gives up.
constexpr uint32_t kGraceMs = 4000;
// How long call_tool() waits for room in the scene queue.
constexpr uint32_t kPushMs = 1000;

ToolCallResult failure(const char* error, std::string message) {
    ToolCallResult r;
    r.error = error;
    r.message = std::move(message);
    return r;
}

// What a task's error is to the caller: a slice over its time is
// "timeout", anything else the scene's own error.
ToolCallResult failure(const ScriptError& e) {
    return failure(e.fault == ScriptFault::Timeout ? "timeout" : "scene_error", e.text());
}

ToolCallResult from_outcome(TaskOutcome& o) {
    if (o.ok) {
        ToolCallResult r;
        r.ok = true;
        r.json = std::move(o.json);
        return r;
    }
    return o.error ? failure(*o.error) : failure("scene_error", "the tool failed");
}

os::LargeUnique<SceneMessage> make_tool_message(const std::string& id, msg::Body body) {
    auto m = os::make_large<SceneMessage>();
    if (m) {
        m->scene_id = id;
        m->body = std::move(body);
    }
    return m;
}

}  // namespace

// --- Any thread ----------------------------------------------------------------

Result<ToolCallResult> SceneRuntime::call_tool(const std::string& scene_id, const std::string& tool,
                                               std::string_view args_json, uint32_t timeout_ms,
                                               std::function<bool()> cancelled) {
    using R = Result<ToolCallResult>;
    if (timeout_ms < kToolCallMinMs || timeout_ms > kToolCallMaxMs)
        return R::err(ErrorCode::InvalidArgument, "timeout_ms must be 1000-120000");
    Value args{Map{}};
    if (!args_json.empty()) {
        auto parsed = from_json(args_json);
        if (!parsed || parsed.value().type() != Value::Type::Map)
            return R::err(ErrorCode::InvalidArgument, "the arguments are not a JSON object");
        args = std::move(parsed.value());
    }
    if (!running_) return R::err(ErrorCode::Busy, "scene runtime not running");
    if (must_not_wait()) return R::err(ErrorCode::Busy, "cannot wait for the scene task here");
    if (deps_.repo.installing(scene_id))
        return R::err(ErrorCode::Busy, "scene " + scene_id + " is being installed");
    if (deps_.repo.removing(scene_id))
        return R::err(ErrorCode::Busy, "scene " + scene_id + " is being deleted");
    {
        std::lock_guard<os::Mutex> lock(tool_mu_);
        if (std::find(tool_scenes_.begin(), tool_scenes_.end(), scene_id) != tool_scenes_.end())
            return R::err(ErrorCode::Busy, "a tool of scene " + scene_id + " is running");
        tool_scenes_.push_back(scene_id);
    }
    struct Release {
        SceneRuntime& rt;
        const std::string& id;
        ~Release() {
            std::lock_guard<os::Mutex> lock(rt.tool_mu_);
            std::erase(rt.tool_scenes_, id);
        }
    } release{*this, scene_id};

    auto job = std::allocate_shared<ToolCallJob>(os::LargeAllocator<ToolCallJob>());
    job->scene_id = scene_id;
    job->tool = tool;
    job->args = std::move(args);
    job->timeout_ms = timeout_ms;
    job->deadline_ms = os::now_ms() + timeout_ms;
    if (!push(make_tool_message(scene_id, msg::ToolCall{job}), kPushMs))
        return R::err(ErrorCode::Busy, "scene queue full");
    // The scene task ends the call at its deadline itself; cancelling is
    // the caller's to notice.
    bool stop_sent = false;
    for (;;) {
        if (job->sem.take(kPollMs)) break;
        if (!stop_sent && cancelled && cancelled())
            stop_sent = push(make_tool_message(scene_id, msg::ToolStop{job}), 0);
        if (os::now_ms() > job->deadline_ms + kGraceMs) {
            std::lock_guard<os::Mutex> lock(job->mu);
            if (!job->done) return R::err(ErrorCode::Timeout, "the scene task did not answer");
            break;
        }
    }
    std::lock_guard<os::Mutex> lock(job->mu);
    return std::move(job->result);
}

// --- Scene task ------------------------------------------------------------------

void SceneRuntime::tool_call(const std::shared_ptr<ToolCallJob>& job) {
    using R = Result<ToolCallResult>;
    const std::string& id = job->scene_id;
    if (os::now_ms() >= job->deadline_ms) {
        job->finish(R::ok(failure("timeout", "timed out before it started")));
        return;
    }
    // call_tool() lets one through per scene; one whose caller gave up may
    // still be here.
    for (const ToolRun& r : tool_runs_) {
        if (r.scene_id == id) {
            job->finish(R::err(ErrorCode::Busy, "a tool of scene " + id + " is running"));
            return;
        }
    }
    Slot* s = find_id(id);
    if (s != nullptr && s->scene) {
        tool_runs_.push_back({job, id, s->instance, false, 0, false, {}});
        tool_start(*s, job);
        return;
    }
    if (s != nullptr) {
        // In front with its error panel up: it is the user's to reload.
        job->finish(R::ok(failure("scene_error", status(id).error)));
        return;
    }
    if (std::any_of(loading_.begin(), loading_.end(),
                    [&](const PendingLoad& p) { return p.id == id; })) {
        job->finish(R::err(ErrorCode::Busy, "scene " + id + " is being opened"));
        return;
    }
    // At most one headless scene, and one load of one (file header).
    std::erase_if(loading_, [](const PendingLoad& p) { return p.headless; });
    tool_loads_dropped();
    for (uint32_t instance : instances()) {
        Slot* other = find(instance);
        if (other != nullptr && other->headless) close_slot(*other);
    }
    const uint32_t instance = next_instance_++;
    if (next_instance_ == 0) next_instance_ = 1;
    PendingLoad load;
    load.instance = instance;
    load.id = id;
    load.seq = next_seq_++;
    load.headless = true;
    loading_.push_back(std::move(load));
    tool_runs_.push_back({job, id, instance, true, 0, false, {}});
    if (!queue_load(id, instance, LaunchReason::User, {}, nullptr)) {
        std::erase_if(loading_, [&](const PendingLoad& p) { return p.instance == instance; });
        tool_end(job.get(), failure("load_failed", "storage worker busy"));
    }
}

void SceneRuntime::tool_start(Slot& s, const std::shared_ptr<ToolCallJob>& job) {
    if (s.headless) ++s.idle_gen;
    bool registered = false;
    uint32_t task = 0;
    std::optional<ScriptError> err = s.scene->tool(job->tool, job->args, &registered, &task);
    if (!err && !registered) {
        s.scene->batch().clear();
        tool_end(job.get(), failure("not_registered", "the scene has not registered " + job->tool));
        return;
    }
    for (ToolRun& r : tool_runs_) {
        if (r.job == job && !r.ended) r.task = task;
    }
    // A fault ends the call through fault() (tool_abort()).
    run_step(s, err);
}

void SceneRuntime::finish_headless(SceneMessage& m, const PendingLoad& load) {
    auto& open = std::get<msg::Open>(m.body);
    const std::string id = m.scene_id;
    std::shared_ptr<ToolCallJob> job;
    for (const ToolRun& r : tool_runs_) {
        if (r.loading && r.instance == load.instance) job = r.job;
    }
    auto fail = [&](const char* error, std::string message) {
        if (open.store) open.store->close();
        if (job) tool_end(job.get(), failure(error, std::move(message)));
    };
    if (!job) {
        // Its call ended while it loaded (tool_loads_dropped() saw to it).
        fail("cancelled", "");
        return;
    }
    if (open.error.code == ErrorCode::Ok &&
        (deps_.repo.installing(id) || deps_.repo.removing(id))) {
        const bool installing = deps_.repo.installing(id);
        WLOG_W(kTag, "%s: not started: it is being %s", id.c_str(),
               installing ? "installed" : "deleted");
        fail("load_failed",
             std::string("the scene is being ") + (installing ? "installed" : "deleted"));
        return;
    }
    if (open.error.code == ErrorCode::NotFound) {
        WLOG_W(kTag, "%s: no such scene", id.c_str());
        fail("not_found", "no such scene");
        return;
    }
    if (open.error.code != ErrorCode::Ok) {
        std::string text = "cannot load: " + open.error.message;
        WLOG_E(("scene." + id).c_str(), "%s", text.c_str());
        set_status(id, core::SceneState::Error, text);
        publish(id, "error");
        fail("load_failed", std::move(text));
        return;
    }
    if (Slot* running = find_id(id); running != nullptr && running->scene) {
        // Opened (or started some other way) while this loaded: the call
        // runs there.
        open.store->close();
        for (ToolRun& r : tool_runs_) {
            if (r.job == job) {
                r.loading = false;
                r.instance = running->instance;
            }
        }
        tool_start(*running, job);
        return;
    }
    for (uint32_t instance : instances()) {
        Slot* other = find(instance);
        if (other != nullptr && other->headless) close_slot(*other);
    }
    const size_t room = headless_room();
    VmLimits limits = config_.vm;
    limits.memory_bytes = std::min(limits.memory_bytes, room);
    Slot* added = limits.memory_bytes == 0 ? nullptr : add_slot(m, LaunchReason::User, &limits);
    if (added == nullptr) {
        char text[96];
        std::snprintf(text, sizeof(text), "the scenes leave %zu KiB of the Lua budget",
                      room / 1024);
        WLOG_W(kTag, "%s: not started headless: %s", id.c_str(), text);
        fail("no_memory", text);
        return;
    }
    Slot& s = *added;
    s.headless = true;
    SceneApi& api = s.scene->api();
    api.background = true;
    api.headless = true;
    update_background_list();
    std::optional<ScriptError> err = start_scene(s);
    if (!err) err = s.scene->hook("on_state", "headless");
    if (err) {
        // The call's error, not the scene's: fault() leaves a call still
        // loading alone.
        ToolCallResult r = err->fault == ScriptFault::Memory ? failure("no_memory", err->text())
                                                             : failure("load_failed", err->text());
        fault(s, *err);
        tool_end(job.get(), std::move(r));
        return;
    }
    s.scene->batch().clear();
    set_status(id, core::SceneState::Headless, "");
    publish(id, "headless");
    for (ToolRun& r : tool_runs_) {
        if (r.job == job) r.loading = false;
    }
    tool_start(s, job);
}

size_t SceneRuntime::headless_room() {
    for (;;) {
        // What the scene in front is guaranteed (spec 3.5) stays its own;
        // everything else that holds memory (the background, the agent's
        // Lua run) counts against the headless scene.
        const size_t front_bytes =
            front_ != nullptr && front_->scene ? front_->scene->vm_bytes() : 0;
        const size_t reserve =
            front_ != nullptr ? std::max(config_.vm.memory_bytes, front_bytes) : 0;
        const size_t used = pool_.used();
        const size_t others = used > front_bytes ? used - front_bytes : 0;
        const size_t room =
            config_.vm_budget > reserve + others ? config_.vm_budget - reserve - others : 0;
        if (room >= config_.vm.memory_bytes) return room;
        const std::vector<const Slot*> order = background_order();
        if (order.empty()) return room;
        Slot* victim = find(order.front()->instance);
        if (victim == nullptr) return room;
        evict(*victim, true);
    }
}

void SceneRuntime::tool_task_done(uint32_t instance, uint32_t task, TaskOutcome outcome) {
    for (ToolRun& r : tool_runs_) {
        if (r.loading || r.instance != instance || r.ended) continue;
        // task 0: the first slice, inside start_task(), before its id is
        // known.
        if (r.task != 0 && r.task != task) continue;
        r.ended = true;
        r.outcome = std::move(outcome);
        return;
    }
}

void SceneRuntime::tool_answer(std::optional<ToolEnding> ending) {
    if (!ending) return;
    const ToolCallResult& r = ending->result;
    if (!r.ok)
        WLOG_I(kTag, "%s: tool %s ended: %s", ending->job->scene_id.c_str(),
               ending->job->tool.c_str(), r.error.c_str());
    ending->job->finish(Result<ToolCallResult>::ok(std::move(ending->result)));
}

void SceneRuntime::tool_end(const ToolCallJob* job, ToolCallResult r) {
    auto it = std::find_if(tool_runs_.begin(), tool_runs_.end(),
                           [&](const ToolRun& t) { return t.job.get() == job; });
    if (it == tool_runs_.end()) return;
    std::shared_ptr<ToolCallJob> held = it->job;
    const uint32_t instance = it->loading ? 0 : it->instance;
    tool_runs_.erase(it);
    tool_answer(ToolEnding{std::move(held), std::move(r)});
    Slot* s = find(instance);
    if (s == nullptr || !s->headless || quit_) return;
    const uint64_t gen = ++s->idle_gen;
    schedule(config_.headless_idle_ms, [this, instance, gen] { headless_idle(instance, gen); });
}

void SceneRuntime::headless_idle(uint32_t instance, uint64_t gen) {
    Slot* s = find(instance);
    if (s == nullptr || !s->headless || s->idle_gen != gen) return;
    for (const ToolRun& r : tool_runs_) {
        if (!r.loading && r.instance == instance) return;
    }
    WLOG_I(kTag, "%s: headless and idle", s->id.c_str());
    close_slot(*s);
}

std::optional<SceneRuntime::ToolEnding> SceneRuntime::tool_abort(uint32_t instance,
                                                                 const char* error,
                                                                 const std::string& message) {
    auto it = std::find_if(tool_runs_.begin(), tool_runs_.end(),
                           [&](const ToolRun& r) { return !r.loading && r.instance == instance; });
    if (it == tool_runs_.end()) return std::nullopt;
    // A task that ended before its scene closed or faulted (wisp.scene.exit()
    // in the tool, a flush that failed after it) keeps its own outcome.
    ToolEnding ending{it->job, it->ended ? from_outcome(it->outcome) : failure(error, message)};
    tool_runs_.erase(it);
    return ending;
}

void SceneRuntime::tool_loads_dropped() {
    for (size_t i = 0; i < tool_runs_.size();) {
        const ToolRun& r = tool_runs_[i];
        const bool gone =
            r.loading && std::none_of(loading_.begin(), loading_.end(), [&](const PendingLoad& p) {
                return p.instance == r.instance;
            });
        if (gone) {
            tool_end(r.job.get(), failure("cancelled", "the scene's load was cancelled"));
        } else {
            ++i;
        }
    }
}

void SceneRuntime::tool_settle() {
    for (size_t i = 0; i < tool_runs_.size();) {
        ToolRun& r = tool_runs_[i];
        if (r.ended) {
            tool_end(r.job.get(), from_outcome(r.outcome));
        } else {
            ++i;
        }
    }
}

void SceneRuntime::tool_stop(const ToolCallJob* job) {
    auto it = std::find_if(tool_runs_.begin(), tool_runs_.end(),
                           [&](const ToolRun& r) { return r.job.get() == job; });
    if (it == tool_runs_.end() || it->ended) return;
    const ToolRun run = *it;
    // Ended first, so the task's own "cancelled" finds no call.
    tool_end(job, failure("cancelled", "cancelled"));
    if (run.loading) {
        std::erase_if(loading_, [&](const PendingLoad& p) { return p.instance == run.instance; });
        return;
    }
    Slot* s = find(run.instance);
    if (s != nullptr && s->scene && run.task != 0) s->scene->cancel_task(run.task);
}

void SceneRuntime::tool_clock() {
    const uint64_t now = os::now_ms();
    for (size_t i = 0; i < tool_runs_.size();) {
        const ToolRun& r = tool_runs_[i];
        if (r.ended || now < r.job->deadline_ms) {
            ++i;
            continue;
        }
        const ToolRun run = r;
        char text[64];
        std::snprintf(text, sizeof(text), "timed out after %u ms",
                      static_cast<unsigned>(run.job->timeout_ms));
        // Spec 5.4: only the call ends; the scene runs on.
        tool_end(run.job.get(), failure("timeout", text));
        if (run.loading) {
            std::erase_if(loading_,
                          [&](const PendingLoad& p) { return p.instance == run.instance; });
            continue;
        }
        Slot* s = find(run.instance);
        if (s != nullptr && s->scene && run.task != 0) s->scene->cancel_task(run.task);
    }
}

uint64_t SceneRuntime::tool_wait_ms(uint64_t now) const {
    uint64_t wait = os::kForever;
    for (const ToolRun& r : tool_runs_) {
        if (r.ended) return 0;
        wait = std::min<uint64_t>(wait, r.job->deadline_ms > now ? r.job->deadline_ms - now : 0);
    }
    return wait;
}

}  // namespace wisp::scene
