#include "wisp/agent/agent_service.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <mutex>
#include <span>
#include <string_view>
#include <utility>

#include "wisp/base/json.hpp"
#include "wisp/core/log.hpp"
#include "wisp/ui/strings.hpp"

namespace wisp::agent {

namespace {

constexpr const char* kTag = "agent";
constexpr const char* kHolder = "system.agent";
constexpr const char* kLockReason = "agent_turn";

// Why a turn is being cut short; the first reason wins.
enum class Abort : uint8_t { None, Stop, Network, Stall, Time };

std::string_view sv(const os::LargeString& s) { return {s.data(), s.size()}; }

size_t code_points(std::string_view s) {
    size_t n = 0;
    for (char c : s) n += (static_cast<unsigned char>(c) & 0xC0) != 0x80 ? 1 : 0;
    return n;
}

// Bytes in the longest prefix of `s` with at most `max_cp` code points.
size_t prefix_code_points(std::string_view s, size_t max_cp) {
    size_t cp = 0;
    for (size_t i = 0; i < s.size(); ++i) {
        if ((static_cast<unsigned char>(s[i]) & 0xC0) != 0x80) {
            if (cp == max_cp) return i;
            ++cp;
        }
    }
    return s.size();
}

// Bytes in the longest prefix of `s` of at most `max_bytes` bytes that
// does not split a character.
size_t prefix_bytes(std::string_view s, size_t max_bytes) {
    if (s.size() <= max_bytes) return s.size();
    size_t n = max_bytes;
    while (n > 0 && (static_cast<unsigned char>(s[n]) & 0xC0) == 0x80) --n;
    return n;
}

std::string summary(std::string_view s) {
    return std::string(s.substr(0, prefix_code_points(s, kSummaryChars)));
}

struct Outcome {
    const char* status = "done";
    uint32_t steps = 0;
    uint32_t tokens = 0;
    // The turn's last reply or note: the done notice's text.
    os::LargeString last_text;
};

}  // namespace

const char* to_string(AgentState s) {
    switch (s) {
        case AgentState::Idle:
            return "idle";
        case AgentState::Thinking:
            return "thinking";
        case AgentState::Tool:
            return "tool";
        case AgentState::Output:
            return "output";
        case AgentState::WaitingConfirm:
            return "waiting_confirm";
        case AgentState::Error:
            return "error";
    }
    return "idle";
}

struct AgentService::Impl {
    struct Done {
        os::Semaphore sem{1, 0};
        Result<void> result = Result<void>::ok();
    };

    struct Command {
        enum class Kind : uint8_t { Message, New };
        Kind kind = Kind::Message;
        os::LargeString text;
        Source source = Source::Web;
        std::shared_ptr<Done> done;  // New
    };

    struct Listener {
        uint32_t id = 0;
        std::function<void(const AgentEvent&)> cb;
        // Held while cb runs, so remove_listener() can wait for a call.
        os::Mutex call;
        std::atomic<bool> removed{false};
    };

    Impl(AgentDeps d, AgentConfig c) : deps(std::move(d)), cfg(c) {}

    // A start() that failed leaves nothing running, so it may be retried.
    void reset_start() {
        timer.reset();
        watch.reset();
        std::lock_guard<os::Mutex> lock(mu);
        started = false;
    }

    // --- any thread ----------------------------------------------------

    uint64_t now() const { return deps.now_ms ? deps.now_ms() : os::now_ms(); }

    void trigger(Abort why) {
        uint8_t none = static_cast<uint8_t>(Abort::None);
        abort.compare_exchange_strong(none, static_cast<uint8_t>(why));
        // A ticket from an earlier request is a no-op; on_armed covers a
        // request armed after this.
        if (watch) watch->abort(ticket.load());
    }

    // Also the turn's time limit: every check of it counts.
    bool aborted() {
        if (in_turn.load() && now() - turn_start.load() > cfg.turn_ms) trigger(Abort::Time);
        return abort.load() != static_cast<uint8_t>(Abort::None);
    }

    // The check timer: short, no locks but the watch's own, no flash.
    void on_tick() {
        if (!in_turn.load()) return;
        aborted();
        if (in_request.load() && now() - last_byte.load() > cfg.stall_ms) trigger(Abort::Stall);
    }

    void on_wifi(const core::Event& e) {
        if (e.topic != "net.wifi.state") return;
        auto it = e.payload.find("state");
        if (it == e.payload.end() || it->second.type() != Value::Type::String) return;
        const bool up = it->second.as_string() == "connected";
        net_state = up ? 1 : 0;
        if (!up && in_request.load()) trigger(Abort::Network);
    }

    bool network_up() const {
        const int n = net_state.load();
        if (n >= 0) return n == 1;
        return deps.network_up ? deps.network_up() : true;
    }

    ui::Lang lang() const {
        if (!deps.lang) return ui::Lang::Zh;
        return ui::parse_lang(deps.lang()).value_or(ui::Lang::Zh);
    }

    void emit(const AgentEvent& e) {
        std::vector<std::shared_ptr<Listener>> copy;
        {
            std::lock_guard<os::Mutex> lock(lmu);
            copy = listeners;
        }
        for (const auto& l : copy) {
            std::lock_guard<os::Mutex> lock(l->call);
            if (!l->removed.load()) l->cb(e);
        }
    }

    // Runs `fn` on the storage worker and waits for it; false if it could
    // not be queued (then it never runs).
    bool run_io(const std::function<void()>& fn) {
        if (!deps.run_io) {
            fn();
            return true;
        }
        return deps.run_io([&fn] { fn(); }).is_ok();
    }

    // --- wisp.agent (and start()) -------------------------------------

    uint64_t append(Message m) {
        m.ts = deps.wall_s ? deps.wall_s() : 0;
        const Role role = m.role;
        Result<uint64_t> r = Result<uint64_t>::err(ErrorCode::Busy, "storage worker refused");
        run_io([&] { r = deps.store.append(std::move(m)); });
        if (!r) {
            WLOG_W(kTag, "append %s failed: %s", to_string(role), r.error().message.c_str());
            return 0;
        }
        AgentEvent e;
        e.kind = AgentEvent::Kind::Message;
        e.seq = r.value();
        emit(e);
        return r.value();
    }

    void note(ui::StrId id, uint32_t turn, Outcome* out = nullptr) {
        Message m;
        m.role = Role::Note;
        m.turn = turn;
        m.text = ui::tr(id, lang());
        if (out != nullptr) out->last_text = m.text;
        append(std::move(m));
    }

    void marker(uint32_t turn, const char* kind, const char* status) {
        Message m;
        m.role = Role::Marker;
        m.turn = turn;
        m.marker = kind;
        m.status = status;
        append(std::move(m));
    }

    void set_state(AgentState s, std::string_view tool_name = {}) {
        {
            std::lock_guard<os::Mutex> lock(mu);
            if (state == s && tool == tool_name) return;
            state = s;
            tool.assign(tool_name);
        }
        WLOG_I(kTag, "state %s", to_string(s));
        Properties payload{{"state", to_string(s)}};
        if (s == AgentState::Tool) payload["tool"] = std::string(tool_name);
        deps.bus.publish(core::Event{"agent.state", "agent", 0, std::move(payload)});
        AgentEvent e;
        e.kind = AgentEvent::Kind::State;
        e.state = s;
        e.tool.assign(tool_name);
        emit(e);
    }

    Result<void> archive() {
        Result<void> r = Result<void>::err(ErrorCode::Busy, "storage worker refused");
        run_io([&] { r = deps.store.archive(); });
        if (!r) {
            WLOG_W(kTag, "archive failed: %s", r.error().message.c_str());
            return r;
        }
        WLOG_I(kTag, "new conversation");
        AgentEvent e;
        e.kind = AgentEvent::Kind::Cleared;
        emit(e);
        return r;
    }

    void run() {
        while (!quit.load()) {
            wake.take(os::kForever);
            for (;;) {
                Command c;
                {
                    std::lock_guard<os::Mutex> lock(mu);
                    if (quit.load() || pending.empty()) break;
                    c = std::move(pending.front());
                    pending.erase(pending.begin());
                    if (c.kind == Command::Kind::Message) {
                        slot = true;
                        active = true;
                        running_source = c.source;
                        abort = static_cast<uint8_t>(Abort::None);
                    }
                }
                if (c.kind == Command::Kind::New) {
                    c.done->result = archive();
                    c.done->sem.give();
                } else {
                    run_turn(c);
                }
            }
        }
    }

    void run_turn(const Command& c) {
        const bool rolled = deps.store.needs_rollover() && archive();
        const uint32_t turn = deps.store.last_turn() + 1;
        {
            std::lock_guard<os::Mutex> lock(mu);
            running_turn = turn;
        }
        const uint64_t start = now();
        turn_start = start;
        last_byte = start;
        in_turn = true;
        if (timer) timer->start();
        core::WakeLock cpu_lock, net_lock;
        if (deps.locks != nullptr) {
            cpu_lock = core::WakeLock(*deps.locks, kHolder, core::WakeLockKind::Cpu, kLockReason);
            net_lock =
                core::WakeLock(*deps.locks, kHolder, core::WakeLockKind::Network, kLockReason);
        }
        WLOG_I(kTag, "turn %u start source=%s", static_cast<unsigned>(turn), to_string(c.source));

        AgentEvent begin;
        begin.kind = AgentEvent::Kind::TurnBegin;
        begin.text.assign(sv(c.text));
        begin.turn = turn;
        begin.source = c.source;
        emit(begin);

        if (rolled) note(ui::StrId::AgentRolledOver, turn);
        marker(turn, "turn_begin", "");
        Message user;
        user.role = Role::User;
        user.turn = turn;
        user.text = c.text;
        user.source = c.source;
        append(std::move(user));
        set_state(AgentState::Thinking);

        Outcome out;
        out.status = loop(turn, c.source, context_window(), out);

        if (timer) timer->stop();
        in_turn = false;
        marker(turn, "turn_end", out.status);
        set_state(AgentState::Idle);
        cpu_lock.release();
        net_lock.release();
        post_notice(out);
        WLOG_I(kTag, "turn %u end %s steps=%u tokens=%u", static_cast<unsigned>(turn), out.status,
               static_cast<unsigned>(out.steps), static_cast<unsigned>(out.tokens));
        {
            std::lock_guard<os::Mutex> lock(mu);
            active = false;
            running_turn.reset();
            running_source.reset();
            // The slot passes straight to a waiting message, so no third
            // message slips in between this turn and the next.
            slot = std::any_of(pending.begin(), pending.end(),
                               [](const Command& p) { return p.kind == Command::Kind::Message; });
        }
        AgentEvent end;
        end.kind = AgentEvent::Kind::TurnEnd;
        end.turn = turn;
        end.status = out.status;
        emit(end);
    }

    void post_notice(const Outcome& out) {
        if (deps.notes == nullptr || !deps.screen_off) return;
        if (std::string_view(out.status) == "stopped" || !deps.screen_off()) return;
        std::string_view line = sv(out.last_text);
        line = line.substr(0, line.find('\n'));
        line = line.substr(0, prefix_bytes(line, kNoticeBytes));
        if (line.empty()) return;
        deps.notes->post_system(core::notice::kAgentDone, std::string(line));
    }

    const char* finish_abort(uint32_t turn, Outcome& out) {
        switch (static_cast<Abort>(abort.load())) {
            case Abort::Time:
                note(ui::StrId::AgentTimeLimit, turn, &out);
                return "time_limit";
            case Abort::Network:
            case Abort::Stall:
                set_state(AgentState::Error);
                note(ui::StrId::AgentErrNetworkLost, turn, &out);
                return "error";
            case Abort::Stop:
            case Abort::None:
                break;
        }
        note(ui::StrId::AgentStopped, turn, &out);
        return "stopped";
    }

    // Answers the calls from `from` on without running them, so the next
    // request still pairs every call with a result.
    void skip_calls(uint32_t turn, std::span<const net::LlmToolCall> calls, std::string_view why) {
        for (const net::LlmToolCall& c : calls) {
            Message m;
            m.role = Role::Tool;
            m.turn = turn;
            m.call_id.assign(c.id);
            const ToolDef* def = deps.tools.find_wire(c.name);
            m.tool.assign(def != nullptr ? std::string_view(def->name) : std::string_view(c.name));
            m.ok = false;
            m.text.assign(to_json(tool_error("cancelled", why).result));
            append(std::move(m));
        }
    }

    // The active llm profile's context window, read once per turn.
    uint32_t context_window() const {
        const core::ProfilesState st = deps.models.get();
        const std::string& id = st.active_for(core::ModelKind::Llm);
        for (const core::ModelProfile& p : st.profiles)
            if (p.id == id) return core::profile_context_window(p.options);
        return kDefaultContextWindow;
    }

    BuiltRequest build(std::span<const Message> h, Source source) const {
        if (!deps.context) return build_history(h);
        DeviceSnapshot d = deps.context(source);
        d.source = source;
        return build_request(h, d);
    }

    // One model request under the turn's controls: the user's stop, the
    // network, the stall check and the time limit.
    net::LlmResult request(std::span<const net::LlmMessage> messages, std::string_view tools_json,
                           std::function<void(std::string_view)> on_text) {
        const uint64_t spent = now() - turn_start.load();
        const uint64_t left = spent >= cfg.turn_ms ? 1 : cfg.turn_ms - spent;
        net::LlmCall call;
        call.messages = messages;
        call.tools_json = tools_json;
        // Past the turn's deadline by a margin, so the time limit (checked
        // every kCheckMs) ends the request, not its own deadline, which
        // would read as a transport error.
        call.timeout_ms =
            static_cast<uint32_t>(std::min<uint64_t>(left + kDeadlineMarginMs, kRequestMs));
        call.watch = watch.get();
        call.on_armed = [this](uint32_t t) {
            ticket = t;
            if (abort.load() != static_cast<uint8_t>(Abort::None)) watch->abort(t);
        };
        call.on_text = std::move(on_text);
        call.on_bytes = [this] { last_byte = now(); };
        call.cancelled = [this] { return aborted(); };
        last_byte = now();
        in_request = true;
        net::LlmResult res = deps.llm.complete(call);
        in_request = false;
        return res;
    }

    // A connection dropped after finish_reason but before [DONE] still
    // delivered the whole reply.
    static bool whole(const net::LlmResult& res) {
        const bool finished = res.finish == net::LlmFinish::Stop ||
                              res.finish == net::LlmFinish::ToolCalls ||
                              res.finish == net::LlmFinish::Length;
        return res.error.empty() || (res.error == "transport" && finished);
    }

    // Summarizes history[first..last] and stores the summary, or, when the
    // model fails to give one, stores an empty one (the turns are dropped
    // from the context; the file keeps them) and says so. Without a model,
    // with a refused key or without a connection nothing is stored: the
    // request that follows fails the same way and explains it, and the
    // turns stay for a later attempt. false when the turn was aborted
    // meanwhile.
    bool compress(uint32_t turn, std::span<const Message> h, std::pair<uint64_t, uint64_t> range,
                  Outcome& out) {
        net::LlmResult res;
        {
            const BuiltRequest req = build_summary_request(h, range.first, range.second);
            res = request({req.messages.data(), req.messages.size()}, {}, nullptr);
        }
        if (res.usage) out.tokens += res.usage->total;
        if (aborted()) return false;
        const bool ok = whole(res) && !res.text.empty();
        if (!ok &&
            (res.error == "no_model" || res.error == "unauthorized" || res.error == "transport")) {
            WLOG_W(kTag, "context not compressed: %s", res.error.c_str());
            return true;
        }
        WLOG_I(kTag, "context %s seq %llu-%llu", ok ? "summarized" : "dropped",
               static_cast<unsigned long long>(range.first),
               static_cast<unsigned long long>(range.second));
        Message m;
        m.role = Role::Summary;
        m.turn = turn;
        m.covers_first = range.first;
        m.covers_last = range.second;
        if (ok) m.text = std::move(res.text);
        append(std::move(m));
        if (!ok) note(ui::StrId::AgentContextDropped, turn);
        return true;
    }

    const char* loop(uint32_t turn, Source source, uint32_t window, Outcome& out) {
        uint32_t generated = 0;
        // Set once the context has been compressed for the coming request,
        // so a history still too long after that is sent as it is.
        bool compressed = false;
        const std::string_view tools_json = sv(deps.tools.tools_json());
        const size_t tools_tokens = tools_json.size() / 3;
        for (;;) {
            if (aborted()) return finish_abort(turn, out);
            if (!network_up()) {
                set_state(AgentState::Error);
                note(ui::StrId::AgentErrNetwork, turn, &out);
                return "error";
            }
            set_state(AgentState::Thinking);

            net::LlmResult res;
            {
                const std::shared_ptr<const History> hist = deps.store.history();
                const BuiltRequest req = build(*hist, source);
                if (!compressed && needs_compression(req.est_tokens + tools_tokens, window)) {
                    compressed = true;
                    const auto range =
                        pick_compression(*hist, window, req.fixed_tokens + tools_tokens);
                    if (range) {
                        if (!compress(turn, *hist, *range, out)) return finish_abort(turn, out);
                        continue;  // rebuilt with the summary in place
                    }
                }
                compressed = false;
                bool output = false;
                res = request({req.messages.data(), req.messages.size()}, tools_json,
                              [&](std::string_view d) {
                                  if (!output) {
                                      output = true;
                                      set_state(AgentState::Output);
                                  }
                                  AgentEvent e;
                                  e.kind = AgentEvent::Kind::Delta;
                                  e.text.assign(d);
                                  emit(e);
                              });
            }
            if (res.usage) out.tokens += res.usage->total;

            if (!whole(res)) {
                if (!res.text.empty()) {
                    Message m;
                    m.role = Role::Assistant;
                    m.turn = turn;
                    m.text = res.text;
                    append(std::move(m));
                }
                // aborted() also checks the time limit: a request that ran
                // into it ends as time_limit, not as an error.
                if (aborted()) return finish_abort(turn, out);
                ui::StrId why = ui::StrId::AgentErrServer;
                if (res.error == "no_model") why = ui::StrId::AgentErrNoModel;
                if (res.error == "unauthorized") why = ui::StrId::AgentErrUnauthorized;
                if (res.error == "transport")
                    why =
                        res.any_body ? ui::StrId::AgentErrNetworkLost : ui::StrId::AgentErrNetwork;
                set_state(AgentState::Error);
                note(why, turn, &out);
                return "error";
            }

            Message reply;
            reply.role = Role::Assistant;
            reply.turn = turn;
            reply.text = res.text;
            for (net::LlmToolCall& c : res.tool_calls) {
                // Every call needs an id for its result to refer to.
                // The turn keeps the id unique in the whole conversation,
                // where the request builder pairs calls with results.
                if (c.id.empty())
                    c.id = "call_" + std::to_string(turn) + "_" + std::to_string(++generated);
                const ToolDef* def = deps.tools.find_wire(c.name);
                ToolCallRec rec;
                rec.id.assign(c.id);
                rec.name.assign(def != nullptr ? std::string_view(def->name)
                                               : std::string_view(c.name));
                rec.args = c.arguments;
                reply.calls.push_back(std::move(rec));
            }
            if (!res.text.empty()) out.last_text = res.text;
            if (!reply.text.empty() || !reply.calls.empty()) append(std::move(reply));
            if (res.tool_calls.empty()) {
                if (aborted()) return finish_abort(turn, out);
                return "done";
            }

            const std::span<const net::LlmToolCall> calls(res.tool_calls);
            for (size_t i = 0; i < calls.size(); ++i) {
                if (aborted()) {
                    skip_calls(turn, calls.subspan(i), "the turn was stopped");
                    return finish_abort(turn, out);
                }
                if (out.steps >= cfg.max_steps) {
                    skip_calls(turn, calls.subspan(i), "the turn reached its step limit");
                    note(ui::StrId::AgentStepLimit, turn, &out);
                    return "step_limit";
                }
                // With finish_reason "length" the last call's arguments
                // may be cut anywhere: never hand them to the tool.
                const bool cut = res.finish == net::LlmFinish::Length && i + 1 == calls.size();
                run_tool(turn, calls[i], cut, out);
            }
        }
    }

    void run_tool(uint32_t turn, const net::LlmToolCall& c, bool cut, Outcome& out) {
        const ToolDef* def = deps.tools.find_wire(c.name);
        const std::string dotted = def != nullptr ? def->name : c.name;
        set_state(AgentState::Tool, dotted);
        AgentEvent start;
        start.kind = AgentEvent::Kind::ToolStart;
        start.tool = dotted;
        start.args = summary(sv(c.arguments));
        emit(start);

        const uint64_t t0 = now();
        ToolOutput o;
        if (cut) {
            o = tool_error("bad_args", "the arguments were cut off (finish_reason length)",
                           Map{{"field", ""}});
        } else {
            const uint64_t spent = now() - turn_start.load();
            const uint64_t left = spent >= cfg.turn_ms ? 0 : cfg.turn_ms - spent;
            ToolCtx ctx;
            ctx.cancelled = [this] { return aborted(); };
            ctx.deadline_ms = os::now_ms() + left;
            ctx.lang = ui::lang_code(lang());
            o = deps.tools.call(c.name, sv(c.arguments), ctx);
        }
        ++out.steps;
        const auto ms = static_cast<unsigned>(now() - t0);

        Message m;
        m.role = Role::Tool;
        m.turn = turn;
        m.call_id.assign(c.id);
        m.tool.assign(dotted);
        m.ok = o.ok;
        {
            // The result is a Value already; its text lives only as long
            // as it takes to copy into the message.
            const std::string json = to_json(o.result);
            m.text.assign(json);
        }
        std::string code;
        if (!o.ok) {
            const Value* err = o.result.find("error");
            code =
                err != nullptr && err->type() == Value::Type::String ? err->as_string() : "failed";
        }
        // The model chose the name: only registered names go into the log.
        const char* logged = def != nullptr ? def->name.c_str() : "unknown";
        if (o.ok) {
            WLOG_I(kTag, "tool %s ok %u ms", logged, ms);
        } else {
            WLOG_I(kTag, "tool %s error:%s %u ms", logged, code.c_str(), ms);
        }
        AgentEvent end;
        end.kind = AgentEvent::Kind::ToolEnd;
        end.tool = dotted;
        end.ok = o.ok;
        end.result = summary(sv(m.text));
        append(std::move(m));
        emit(end);
    }

    AgentDeps deps;
    AgentConfig cfg;

    // Guards everything up to the listeners.
    mutable os::Mutex mu;
    os::LargeVector<Command> pending;
    bool started = false;
    bool accepting = false;
    // A message holds the running slot: from when the task takes it until
    // its turn ends (then it passes to the next waiting message).
    bool slot = false;
    bool active = false;  // run_turn() is running
    AgentState state = AgentState::Idle;
    std::string tool;
    std::optional<uint32_t> running_turn;
    std::optional<Source> running_source;

    os::Semaphore wake{64, 0};
    std::unique_ptr<os::Task> task;
    std::atomic<bool> quit{false};
    core::SubscriptionId sub = 0;

    // Turn control, read by the check timer and the bus handler.
    std::atomic<uint8_t> abort{static_cast<uint8_t>(Abort::None)};
    std::atomic<bool> in_turn{false};
    std::atomic<bool> in_request{false};
    std::atomic<uint32_t> ticket{0};
    std::atomic<uint64_t> turn_start{0};
    std::atomic<uint64_t> last_byte{0};
    std::atomic<int> net_state{-1};  // -1 before any net.wifi.state event
    os::LargeUnique<net::RequestWatch> watch;
    std::unique_ptr<os::Timer> timer;

    os::Mutex lmu;
    std::vector<std::shared_ptr<Listener>> listeners;
    uint32_t next_listener = 1;
};

AgentService::AgentService(AgentDeps deps, AgentConfig config)
    : impl_(os::make_large<Impl>(std::move(deps), config)) {
    // As LargeAllocator does: the service cannot work without its state.
    if (!impl_) std::abort();
}

AgentService::~AgentService() { stop(); }

os::TaskConfig AgentService::task_config(const AgentConfig& c) {
    os::TaskConfig t;
    t.name = "wisp.agent";
    t.stack_bytes = c.stack_bytes;
    t.priority = c.priority;
    // Every file access goes through run_io (see the header).
    t.flash_access = false;
    return t;
}

Result<void> AgentService::start() {
    Impl& s = *impl_;
    {
        std::lock_guard<os::Mutex> lock(s.mu);
        if (s.started) return Result<void>::err(ErrorCode::Busy, "started");
        s.started = true;
    }
    s.watch = net::RequestWatch::create();
    if (!s.watch) WLOG_W(kTag, "no request watch: a stop waits for the stream's next bytes");
    s.timer = os::Timer::create("wisp.agent", kCheckMs, /*periodic=*/true, [&s] { s.on_tick(); });
    if (!s.timer) WLOG_W(kTag, "no check timer: stalled streams end at their deadline only");

    Result<void> loaded = Result<void>::err(ErrorCode::Busy, "storage worker refused");
    s.run_io([&] { loaded = s.deps.store.load(); });
    if (!loaded) {
        WLOG_E(kTag, "conversation load failed: %s", loaded.error().message.c_str());
        s.reset_start();
        return loaded;
    }
    if (std::optional<uint32_t> cut = s.deps.store.interrupted_turn())
        s.note(ui::StrId::AgentInterrupted, *cut);

    s.sub = s.deps.bus.subscribe("net.wifi.state", [&s](const core::Event& e) { s.on_wifi(e); });
    s.task = os::Task::spawn(task_config(s.cfg), [&s] { s.run(); });
    if (!s.task) {
        s.deps.bus.unsubscribe(s.sub);
        s.sub = 0;
        s.reset_start();
        return Result<void>::err(ErrorCode::NoMemory, "cannot start wisp.agent");
    }
    std::lock_guard<os::Mutex> lock(s.mu);
    s.accepting = true;
    return Result<void>::ok();
}

void AgentService::stop() {
    Impl& s = *impl_;
    {
        std::lock_guard<os::Mutex> lock(s.mu);
        s.accepting = false;
        for (Impl::Command& c : s.pending) {
            if (c.done) {
                c.done->result = Result<void>::err(ErrorCode::Busy, "not_running");
                c.done->sem.give();
            }
        }
        s.pending.clear();
        if (s.active) s.trigger(Abort::Stop);
    }
    s.quit = true;
    s.wake.give();
    if (s.task) {
        s.task->join();
        s.task.reset();
    }
    if (s.sub != 0) {
        s.deps.bus.unsubscribe(s.sub);
        s.sub = 0;
    }
    if (s.timer) s.timer->stop();
}

Result<void> AgentService::submit(std::string text, Source source) {
    if (text.empty()) return Result<void>::err(ErrorCode::InvalidArgument, "empty");
    if (code_points(text) > kMaxUserChars)
        return Result<void>::err(ErrorCode::InvalidArgument, "too_long");
    Impl& s = *impl_;
    {
        std::lock_guard<os::Mutex> lock(s.mu);
        if (!s.accepting) return Result<void>::err(ErrorCode::Busy, "not_running");
        size_t taken = s.slot ? 1 : 0;
        for (const Impl::Command& c : s.pending)
            taken += c.kind == Impl::Command::Kind::Message ? 1 : 0;
        if (taken >= 1 + kMaxQueued) return Result<void>::err(ErrorCode::Busy, "busy");
        Impl::Command c;
        c.kind = Impl::Command::Kind::Message;
        c.text.assign(text);
        c.source = source;
        s.pending.push_back(std::move(c));
    }
    s.wake.give();
    return Result<void>::ok();
}

void AgentService::stop_turn() {
    Impl& s = *impl_;
    std::lock_guard<os::Mutex> lock(s.mu);
    std::erase_if(s.pending,
                  [](const Impl::Command& c) { return c.kind == Impl::Command::Kind::Message; });
    if (s.active) s.trigger(Abort::Stop);
}

Result<void> AgentService::new_conversation() {
    Impl& s = *impl_;
    // From inside a turn (a tool, a listener) a turn is running.
    if (s.task && s.task->is_current()) return Result<void>::err(ErrorCode::Busy, "busy");
    auto done = std::make_shared<Impl::Done>();
    {
        std::lock_guard<os::Mutex> lock(s.mu);
        if (!s.accepting) return Result<void>::err(ErrorCode::Busy, "not_running");
        if (s.slot || s.active || !s.pending.empty())
            return Result<void>::err(ErrorCode::Busy, "busy");
        Impl::Command c;
        c.kind = Impl::Command::Kind::New;
        c.done = done;
        s.pending.push_back(std::move(c));
    }
    s.wake.give();
    done->sem.take(os::kForever);
    return done->result;
}

AgentState AgentService::state() const {
    std::lock_guard<os::Mutex> lock(impl_->mu);
    return impl_->state;
}

ConversationView AgentService::view() const {
    const Impl& s = *impl_;
    ConversationView v;
    v.messages = s.deps.store.history();
    std::lock_guard<os::Mutex> lock(s.mu);
    v.state = s.state;
    v.tool = s.tool;
    v.running_turn = s.running_turn;
    v.running_source = s.running_source;
    v.queued = std::any_of(s.pending.begin(), s.pending.end(), [](const Impl::Command& c) {
        return c.kind == Impl::Command::Kind::Message;
    });
    return v;
}

uint32_t AgentService::add_listener(std::function<void(const AgentEvent&)> cb) {
    Impl& s = *impl_;
    auto l = std::make_shared<Impl::Listener>();
    l->cb = std::move(cb);
    std::lock_guard<os::Mutex> lock(s.lmu);
    l->id = s.next_listener++;
    s.listeners.push_back(l);
    return l->id;
}

void AgentService::remove_listener(uint32_t id) {
    Impl& s = *impl_;
    std::shared_ptr<Impl::Listener> gone;
    {
        std::lock_guard<os::Mutex> lock(s.lmu);
        auto it = std::find_if(s.listeners.begin(), s.listeners.end(),
                               [id](const auto& l) { return l->id == id; });
        if (it == s.listeners.end()) return;
        gone = *it;
        s.listeners.erase(it);
    }
    gone->removed = true;
    // A listener removing itself (or another) from inside a call on
    // wisp.agent cannot wait for that call.
    if (s.task && s.task->is_current()) return;
    std::lock_guard<os::Mutex> wait(gone->call);
}

}  // namespace wisp::agent
