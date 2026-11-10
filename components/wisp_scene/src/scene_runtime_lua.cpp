// SceneRuntime::run_lua() (scene_runtime.hpp, "The agent's Lua";
// agent-tools spec 4.2, 4.3): the caller's side, which waits, and the scene
// task's side, which owns the run's VM from its first message to the end.
//
// Every run ends in lua_end(), on the scene task, which is the only place
// the VM is destroyed: after the result is taken, every task is cancelled
// (lua_task.hpp: before the VM goes), the VM's requests and downloads are
// cancelled, and only then is the caller answered, so a caller that sees
// the result also sees the VM's memory back in the pool. task_done only
// records the outcome; the step that ended the task returns to
// lua_settle(), which ends the run.

#include <algorithm>
#include <cstdio>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "scene_api.hpp"
#include "scene_message.hpp"
#include "wisp/core/log.hpp"
#include "wisp/core/scene_files.hpp"
#include "wisp/net/scene_http.hpp"
#include "wisp/scene/lua_task.hpp"
#include "wisp/scene/scene_runtime.hpp"
#include "wisp/scene/vm.hpp"

namespace wisp::scene {

namespace {

// How often the caller looks at its cancel flag.
constexpr uint32_t kPollMs = 20;
// How long, past the run's own deadline, the caller waits for the scene
// task to answer before it gives up.
constexpr uint32_t kGraceMs = 10000;
// How long run_lua() waits for room in the scene queue.
constexpr uint32_t kPushMs = 1000;
// The log tag of print and wisp.log in the run's VM.
constexpr const char* kOutputTag = "agent.lua";
// What wisp.notify posts as (spec 4.2: the source is Wisp itself).
constexpr const char* kNotifyName = "Wisp";

os::LargeUnique<SceneMessage> make_lua_message(msg::Body body) {
    auto m = os::make_large<SceneMessage>();
    if (m) {
        m->scene_id = net::kAgentHttpId;
        m->body = std::move(body);
    }
    return m;
}

// Removes what the agent's VM must not have (spec 4.2): install_api() is
// shared with the scenes and adds every binding.
int strip_bindings(lua_State* L) {
    if (lua_getglobal(L, "wisp") != LUA_TTABLE) return luaL_error(L, "wisp table missing");
    for (const char* name : {"timer", "store", "scene", "sensor", "power", "import"}) {
        lua_pushnil(L);
        lua_setfield(L, -2, name);
    }
    return 0;
}

const char* error_code(const ScriptError& e) {
    switch (e.fault) {
        case ScriptFault::Compile:
            return "compile";
        case ScriptFault::Timeout:
            return "timeout";
        case ScriptFault::Memory:
            return "memory";
        case ScriptFault::Runtime:
            break;
    }
    // start_task()'s words for a result past kEvalResultMaxBytes.
    return e.message == "result too large" ? "too_large" : "runtime";
}

LuaRunResult failure(const char* error, std::string message) {
    LuaRunResult r;
    r.error = error;
    r.message = std::move(message);
    return r;
}

LuaRunResult failure(const ScriptError& e) { return failure(error_code(e), e.text()); }

}  // namespace

void OutputTail::add(std::string_view piece) {
    constexpr size_t cap = kLuaRunOutputBytes;
    if (piece.size() > cap) {
        piece.remove_prefix(piece.size() - cap);
        buf_.clear();
        cut_ = true;
    }
    buf_.append(piece.data(), piece.size());
    // Trimmed in steps of at least `cap`, not on every line.
    if (buf_.size() > 2 * cap) {
        buf_.erase(0, buf_.size() - cap);
        cut_ = true;
    }
}

os::LargeString OutputTail::take() {
    constexpr size_t cap = kLuaRunOutputBytes;
    if (buf_.size() > cap) {
        buf_.erase(0, buf_.size() - cap);
        cut_ = true;
    }
    if (cut_) {
        // A cut may land inside a character: its stray bytes go too.
        size_t skip = 0;
        while (skip < buf_.size() && (static_cast<unsigned char>(buf_[skip]) & 0xC0) == 0x80)
            ++skip;
        buf_.erase(0, skip);
        buf_.insert(0, "…");
    }
    os::LargeString out = std::move(buf_);
    buf_.clear();
    cut_ = false;
    return out;
}

// --- Any thread ----------------------------------------------------------------

Result<LuaRunResult> SceneRuntime::run_lua(std::string_view code, LuaRunOptions opts) {
    using R = Result<LuaRunResult>;
    if (opts.timeout_ms < kLuaRunMinMs || opts.timeout_ms > kLuaRunMaxMs)
        return R::err(ErrorCode::InvalidArgument, "timeout_ms must be 1000-300000");
    if (opts.memory_bytes < kLuaRunMinMemory || opts.memory_bytes > kLuaRunMaxMemory)
        return R::err(ErrorCode::InvalidArgument, "memory must be 256 KiB - 2 MiB");
    if (code.size() > kLuaRunMaxSource)
        return R::err(ErrorCode::InvalidArgument, "the code is over 64 KiB");
    if (!running_) return R::err(ErrorCode::Busy, "scene runtime not running");
    if (must_not_wait()) return R::err(ErrorCode::Busy, "cannot wait for the scene task here");
    // One at a time (the agent is serial anyway); the scene task refuses a
    // second VM as well, should a caller that gave up leave one behind.
    if (lua_busy_.exchange(true)) return R::err(ErrorCode::Busy, "a Lua run is under way");
    struct Release {
        std::atomic<bool>& busy;
        ~Release() { busy = false; }
    } release{lua_busy_};

    auto job = std::allocate_shared<LuaRunJob>(os::LargeAllocator<LuaRunJob>());
    job->code.assign(code.data(), code.size());
    job->memory_bytes = opts.memory_bytes;
    job->timeout_ms = opts.timeout_ms;
    job->start_ms = os::now_ms();
    job->deadline_ms = job->start_ms + opts.timeout_ms;
    if (!push(make_lua_message(msg::LuaRun{job}), kPushMs))
        return R::err(ErrorCode::Busy, "scene queue full");
    // The scene task ends the run at its deadline itself; cancelling is
    // the caller's to notice.
    bool stop_sent = false;
    for (;;) {
        if (job->sem.take(kPollMs)) break;
        if (!stop_sent && opts.cancelled && opts.cancelled())
            stop_sent = push(make_lua_message(msg::LuaStop{job}), 0);
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

void SceneRuntime::lua_start(const std::shared_ptr<LuaRunJob>& job) {
    using R = Result<LuaRunResult>;
    if (agent_) {
        job->finish(R::err(ErrorCode::Busy, "a Lua run is under way"));
        return;
    }
    if (os::now_ms() >= job->deadline_ms) {
        job->finish(R::ok(failure("timeout", "timed out before it started")));
        return;
    }
    // Spec 4.2: what the scenes leave, never more; no scene is evicted.
    const size_t used = pool_.used();
    const size_t room = pool_.budget() > used ? pool_.budget() - used : 0;
    const size_t limit = std::min(job->memory_bytes, room);
    auto no_memory = [&] {
        char text[96];
        std::snprintf(text, sizeof(text), "the scenes leave %zu KiB of the Lua budget",
                      room / 1024);
        job->finish(R::ok(failure("no_memory", text)));
    };
    if (limit < kLuaRunMinMemory) {
        no_memory();
        return;
    }
    auto run = os::make_large<AgentRun>();
    if (!run) {
        no_memory();
        return;
    }
    AgentRun& a = *run;
    a.job = job;
    a.instance = next_instance_++;
    SceneApi& api = a.api;
    api.device = [this] { return device_info(); };
    api.screen = deps_.shape;
    api.notifications = deps_.notifications;
    api.scene_id = net::kAgentHttpId;
    api.scene_name = kNotifyName;
    api.http_principal = net::HttpPrincipal::Agent;
    api.task_slice_ms = kLuaRunSliceMs;
    api.task_max_json = kEvalResultMaxBytes;
    // Only records it (lua_task.hpp: never destroy the VM in here).
    api.task_done = [&a](uint32_t, TaskOutcome outcome) {
        if (a.ended) return;
        a.ended = true;
        a.outcome = std::move(outcome);
    };
    if (deps_.http != nullptr) wire_http_api(api, net::kAgentHttpId, a.instance);
    if (deps_.files != nullptr) {
        a.files_gate = std::allocate_shared<FilesGate>(os::LargeAllocator<FilesGate>());
        wire_files_api(api, core::kAgentFilesId, a.instance, a.files_gate, nullptr);
    }
    VmLimits limits = config_.vm;
    limits.memory_bytes = limit;
    limits.exec_ms = kLuaRunSliceMs;
    limits.pool = &pool_;
    a.vm = Vm::create(net::kAgentHttpId, limits);
    if (!a.vm) {
        no_memory();
        return;
    }
    a.vm->set_output(kOutputTag, [&a](std::string_view piece) { a.output.add(piece); });
    agent_ = std::move(run);

    std::optional<ScriptError> err = install_api(*a.vm, a.api);
    lua_State* L = a.vm->L();
    if (!err) {
        if (lua_checkstack(L, 1)) {
            lua_pushcfunction(L, strip_bindings);
            err = a.vm->call(0, 0);
        } else {
            err = ScriptError{ScriptFault::Memory, "", 0, "stack overflow"};
        }
    }
    if (!err) err = a.vm->load("run", std::string_view(job->code.data(), job->code.size()));
    if (err) {
        lua_end(failure(*err));
        return;
    }
    Result<uint32_t> started = start_task(*a.vm, nullptr, kLuaRunSliceMs, kEvalResultMaxBytes);
    if (!started) {
        const ErrorCode code = started.error().code;
        lua_end(failure(code == ErrorCode::Timeout    ? "timeout"
                        : code == ErrorCode::NoMemory ? "memory"
                                                      : "runtime",
                        started.error().message));
        return;
    }
    lua_settle(std::nullopt);
}

void SceneRuntime::lua_stop(const LuaRunJob* job, const char* error, std::string message) {
    if (!agent_ || (job != nullptr && agent_->job.get() != job)) return;
    lua_end(failure(error, std::move(message)));
}

bool SceneRuntime::lua_message(SceneMessage& m) {
    if (!agent_ || m.instance == 0 || m.instance != agent_->instance) return false;
    if (auto* b = std::get_if<msg::HttpDone>(&m.body)) {
        lua_settle(run_http_callback(*agent_->vm, b->id, b->result));
        return true;
    }
    if (auto* b = std::get_if<msg::FilesDone>(&m.body)) {
        lua_settle(run_files_callback(*agent_->vm, b->id, b->result));
        return true;
    }
    return false;
}

void SceneRuntime::lua_settle(const std::optional<ScriptError>& err) {
    if (!agent_) return;
    AgentRun& a = *agent_;
    if (a.ended) {
        if (a.outcome.ok) {
            LuaRunResult r;
            r.ok = true;
            r.json = std::move(a.outcome.json);
            lua_end(std::move(r));
        } else {
            lua_end(a.outcome.error ? failure(*a.outcome.error)
                                    : failure("runtime", "the task failed"));
        }
        return;
    }
    // A callback the script passed (not the task) failed, or the VM
    // faulted under it: the run ends with that error, as a scene would.
    if (err) {
        lua_end(failure(*err));
        return;
    }
    if (a.vm->faulted()) lua_end(failure("memory", "the VM faulted"));
}

void SceneRuntime::lua_end(LuaRunResult r) {
    AgentRun& a = *agent_;
    r.ms = static_cast<uint32_t>(os::now_ms() - a.job->start_ms);
    r.memory_peak = a.vm->peak_bytes();
    r.output = a.output.take();
    cancel_tasks(*a.vm);
    if (deps_.http != nullptr) deps_.http->cancel_scene(net::kAgentHttpId);
    close_gate(a.files_gate);
    std::shared_ptr<LuaRunJob> job = a.job;
    if (!r.ok)
        WLOG_I(kOutputTag, "run ended: %s (%u ms)", r.error.c_str(), static_cast<unsigned>(r.ms));
    agent_.reset();
    job->finish(Result<LuaRunResult>::ok(std::move(r)));
}

void SceneRuntime::lua_clock() {
    if (!agent_) return;
    const uint64_t now = os::now_ms();
    if (now >= agent_->job->deadline_ms) {
        char text[64];
        std::snprintf(text, sizeof(text), "timed out after %u ms",
                      static_cast<unsigned>(agent_->job->timeout_ms));
        lua_end(failure("timeout", text));
        return;
    }
    const uint32_t instance = agent_->instance;
    for (uint32_t id : agent_->api.timers.due(now)) {
        // Each one may end the run.
        if (!agent_ || agent_->instance != instance) break;
        const bool once = !agent_->api.timers.contains(id);
        lua_settle(run_timer(*agent_->vm, id, once));
    }
}

uint64_t SceneRuntime::lua_wait_ms(uint64_t now) const {
    if (!agent_) return os::kForever;
    uint64_t wait = agent_->job->deadline_ms > now ? agent_->job->deadline_ms - now : 0;
    if (auto due = agent_->api.timers.next_due())
        wait = std::min<uint64_t>(wait, *due > now ? *due - now : 0);
    return wait;
}

}  // namespace wisp::scene
