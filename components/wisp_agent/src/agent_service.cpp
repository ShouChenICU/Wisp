#include "wisp/agent/agent_service.hpp"

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "calibration.hpp"
#include "wisp/agent/tools.hpp"
#include "wisp/base/json.hpp"
#include "wisp/core/log.hpp"
#include "wisp/core/topics.hpp"
#include "wisp/os/large.hpp"
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
    // AgentEvent::reason, set where the note that explains it is written.
    const char* reason = "";
    uint32_t steps = 0;
    uint32_t tokens = 0;
    // The turn's last reply or note: the done notice's text.
    os::LargeString last_text;
};

static_assert(std::is_trivially_copyable_v<AgentStatus>,
              "the cell copies the status under its lock without allocating");

// Reproduces the agent.state event set_state() published for every
// change: the state, and the tool's name while it is Tool.
void status_mirror(core::EventBus& bus, const AgentStatus&, const AgentStatus& after) {
    Properties payload{{"state", to_string(after.state)}};
    if (after.state == AgentState::Tool) payload["tool"] = after.tool.str();
    bus.publish(core::Event{core::topics::kAgentState, kAgentSource, 0, std::move(payload)});
}

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
        // new_conversation_async(): called instead of anyone waiting.
        std::function<void(Result<void>)> then;

        void finish(Result<void> r) {
            result = r;
            if (then) then(std::move(r));
            sem.give();
        }
    };

    struct Command {
        enum class Kind : uint8_t { Message, New, Clear };
        Kind kind = Kind::Message;
        os::LargeString text;
        Source source = Source::Web;
        std::shared_ptr<Done> done;         // New
        std::function<void(bool)> cleared;  // Clear
    };

    struct Listener {
        uint32_t id = 0;
        std::function<void(const AgentEvent&)> cb;
        // Held while cb runs, so remove_listener() can wait for a call.
        os::Mutex call;
        std::atomic<bool> removed{false};
    };

    Impl(AgentDeps d, AgentConfig c)
        : deps(std::move(d)),
          cfg(c),
          status(deps.bus, AgentStatus{}, &status_mirror),
          next_steps(c.max_steps),
          next_turn_ms(c.turn_ms) {}

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
        if (in_turn.load() && now() - turn_start.load() > turn_ms.load()) trigger(Abort::Time);
        return abort.load() != static_cast<uint8_t>(Abort::None);
    }

    // The check timer: short, no locks but the watch's own, no flash.
    void on_tick() {
        if (!in_turn.load()) return;
        aborted();
        if (in_request.load() && now() - last_byte.load() > cfg.stall_ms) trigger(Abort::Stall);
    }

    // The Wi-Fi cell's callback, on wisp.bus: atomics and the watch only.
    // The first delivery (at start(), nothing in flight) does nothing.
    void on_wifi(const net::WifiSnapshot& w) {
        if (w.state != net::WifiState::Connected && in_request.load()) trigger(Abort::Network);
    }

    // The cell itself, not a copy fed from its callback, which could only
    // lag behind it.
    bool network_up() const {
        if (wifi != nullptr) return wifi->get().state == net::WifiState::Connected;
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

    // run_io() on the worker's Interactive lane (deps.run_io_interactive,
    // or run_io without one): for the conversation appends, which are
    // short and hold up the turn. The service waits for every store call,
    // so an append still cannot pass an archive, clear or load it issued
    // earlier, though they take the Normal lane.
    bool run_io_interactive(const std::function<void()>& fn) {
        if (!deps.run_io_interactive) return run_io(fn);
        return deps.run_io_interactive([&fn] { fn(); }).is_ok();
    }

    // --- wisp.agent (and start()) -------------------------------------

    uint64_t append(Message m) {
        m.ts = deps.wall_s ? deps.wall_s() : 0;
        const Role role = m.role;
        Result<uint64_t> r = Result<uint64_t>::err(ErrorCode::Busy, "storage worker refused");
        run_io_interactive([&] { r = deps.store.append(std::move(m)); });
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
        note_text(ui::tr(id, lang()), turn, out);
    }

    void note_text(std::string_view text, uint32_t turn, Outcome* out = nullptr) {
        Message m;
        m.role = Role::Note;
        m.turn = turn;
        m.text.assign(text);
        if (out != nullptr) out->last_text = m.text;
        append(std::move(m));
    }

    // A time limit in whole minutes for the note, at least 1.
    static uint64_t minutes(uint64_t ms) { return std::max<uint64_t>(1, (ms + 30000) / 60000); }

    void marker(uint32_t turn, const char* kind, const char* status) {
        Message m;
        m.role = Role::Marker;
        m.turn = turn;
        m.marker = kind;
        m.status = status;
        append(std::move(m));
    }

    void set_state(AgentState s, std::string_view tool_name = {}) {
        // Cut before any lock is taken, so that the work under the cell's
        // lock is memory work only.
        decltype(AgentStatus::tool) shown;
        if (s == AgentState::Tool) shown.assign_utf8(tool_name);
        {
            std::lock_guard<os::Mutex> lock(mu);
            if (state == s && tool == tool_name) return;
            state = s;
            tool.assign(tool_name);
            // Under mu, so agent.state goes out in the order of the
            // changes. Past the check above the state or the whole name
            // changed, so the status changes too -- except for two names
            // alike in their first 72 bytes in a row, both made up by the
            // model (plan deviation 8): that agent.state is not published,
            // the listeners' State below still is.
            status.update([&](AgentStatus& v) {
                if (s == AgentState::Error && v.state != AgentState::Error) ++v.errors;
                v.state = s;
                v.tool = shown;
            });
        }
        WLOG_I(kTag, "state %s", to_string(s));
        AgentEvent e;
        e.kind = AgentEvent::Kind::State;
        e.state = s;
        e.tool.assign(tool_name);
        emit(e);
    }

    Result<void> archive() {
        // A new conversation drops http.fetch's response cache too (spec
        // 3.3), whether or not archiving the old one succeeds.
        http_tools_drop_cache();
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

    // Asks on the device for whatever runs on wisp.agent (a tool, the
    // clear), showing WaitingConfirm and the question meanwhile, then puts
    // the state back as it was. true only for Allowed.
    bool ask_user(const std::string& text, const std::function<bool()>& cancelled) {
        if (confirm.load() == nullptr) {
            WLOG_I(kTag, "confirm refused: no device confirmation");
            return false;
        }
        AgentState before;
        std::string before_tool;
        {
            std::lock_guard<os::Mutex> lock(mu);
            before = state;
            before_tool = tool;
            confirm_text = text;
        }
        set_state(AgentState::WaitingConfirm);
        AgentEvent e;
        e.kind = AgentEvent::Kind::Confirm;
        e.text = text;
        emit(e);
        const Answer a = confirm.load()->ask(text, cancelled);
        {
            std::lock_guard<os::Mutex> lock(mu);
            confirm_text.reset();
        }
        e.text.clear();
        emit(e);
        set_state(before, before_tool);
        WLOG_I(kTag, "confirm %s", to_string(a));
        return a == Answer::Allowed;
    }

    void clear_all(const Command& c) {
        bool ok = ask_user(ui::tr(ui::StrId::AgentConfirmClear, lang()), [this] {
            return quit.load() || abort.load() != static_cast<uint8_t>(Abort::None);
        });
        if (ok) {
            Result<void> r = Result<void>::err(ErrorCode::Busy, "storage worker refused");
            run_io([&] { r = deps.store.clear_all(); });
            if (!r) WLOG_W(kTag, "clear failed: %s", r.error().message.c_str());
            ok = r.is_ok();
        }
        if (ok) WLOG_I(kTag, "conversations cleared");
        {
            std::lock_guard<os::Mutex> lock(mu);
            clearing = false;
        }
        AgentEvent e;
        e.kind = AgentEvent::Kind::Cleared;
        e.ok = ok;
        emit(e);
        if (c.cleared) c.cleared(ok);
    }

    // new_conversation()'s checks and queueing; `done` finishes it.
    Result<void> queue_new(const std::shared_ptr<Done>& done) {
        Impl& s = *this;
        // From inside a turn (a tool, a listener) a turn is running.
        if (s.task && s.task->is_current()) return Result<void>::err(ErrorCode::Busy, "busy");
        {
            std::lock_guard<os::Mutex> lock(s.mu);
            if (!s.accepting) return Result<void>::err(ErrorCode::Busy, "not_running");
            if (s.slot || s.active || s.clearing || !s.pending.empty())
                return Result<void>::err(ErrorCode::Busy, "busy");
            Impl::Command c;
            c.kind = Impl::Command::Kind::New;
            c.done = done;
            s.pending.push_back(std::move(c));
        }
        s.wake.give();
        return Result<void>::ok();
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
                    } else if (c.kind == Command::Kind::Clear) {
                        clearing = true;
                        abort = static_cast<uint8_t>(Abort::None);
                    }
                }
                if (c.kind == Command::Kind::New) {
                    c.done->finish(archive());
                } else if (c.kind == Command::Kind::Clear) {
                    clear_all(c);
                } else {
                    run_turn(c);
                }
            }
        }
    }

    void run_turn(const Command& c) {
        const bool rolled = deps.store.needs_rollover() && archive();
        const uint32_t turn = deps.store.last_turn() + 1;
        ++turn_serial;
        {
            std::lock_guard<os::Mutex> lock(mu);
            running_turn = turn;
        }
        // This turn's limits: a change from now on waits for the next one.
        max_steps = next_steps.load();
        turn_ms = next_turn_ms.load();
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
        const ActiveLlm llm = active_llm();
        cal.begin_turn(llm.profile_id, llm.model);
        out.status = loop(turn, c.source, llm.window, out);

        if (timer) timer->stop();
        in_turn = false;
        marker(turn, "turn_end", out.status);
        // Drops http.fetch's response cache (agent-tools spec 3.3): a
        // `ref` never reaches past the turn that made it.
        http_tools_drop_cache();
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
        if (std::string_view(out.status) != "done") {
            end.reason = out.reason;
            end.note = summary(sv(out.last_text));
        }
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
                note_text(ui::tr_fill(ui::StrId::AgentTimeLimit, lang(),
                                      std::to_string(minutes(turn_ms.load()))),
                          turn, &out);
                out.reason = "time_limit";
                return "time_limit";
            case Abort::Network:
            case Abort::Stall:
                set_state(AgentState::Error);
                note(ui::StrId::AgentErrNetworkLost, turn, &out);
                out.reason = "network_lost";
                return "error";
            case Abort::Stop:
            case Abort::None:
                break;
        }
        note(ui::StrId::AgentStopped, turn, &out);
        out.reason = "stopped";
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

    struct ActiveLlm {
        uint32_t window = kDefaultContextWindow;
        std::string model{};
        std::string profile_id{};
    };

    // The provider's active model's context window, model and profile,
    // read once per turn.
    ActiveLlm active_llm() const {
        const std::optional<net::LlmModelInfo> m = deps.llm.active_model();
        if (!m) return {kDefaultContextWindow, "", ""};
        return {m->context_window, m->model, m->profile_id};
    }

    BuiltRequest build(std::span<const Message> h, Source source, BuildOptions opts = {}) const {
        if (!deps.context) return build_history(h, opts);
        DeviceSnapshot d = deps.context(source);
        d.source = source;
        return build_request(h, d, opts);
    }

    // One model request under the turn's controls: the user's stop, the
    // network, the stall check and the time limit.
    net::LlmResult request(std::span<const net::LlmMessage> messages, std::string_view tools_json,
                           std::function<void(std::string_view)> on_text) {
        const uint64_t spent = now() - turn_start.load();
        const uint64_t limit = turn_ms.load();
        const uint64_t left = spent >= limit ? 1 : limit - spent;
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
        call.on_tool = [this](const net::LlmToolProgress& p) {
            const ToolDef* def = deps.tools.find_wire(p.name);
            AgentEvent e;
            e.kind = AgentEvent::Kind::ToolPrep;
            e.tool.assign(def != nullptr ? std::string_view(def->name) : p.name);
            e.bytes = static_cast<uint32_t>(std::min<size_t>(p.arg_bytes, UINT32_MAX));
            emit(e);
        };
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
        return res.error == net::LlmError::None ||
               (res.error == net::LlmError::Transport && finished);
    }

    // Summarizes history[first..last] and stores the summary, or, when the
    // model fails to give one, stores an empty one (the turns are dropped
    // from the context; the file keeps them) and says so. Without a model,
    // with a refused key or without a connection nothing is stored: the
    // request that follows fails the same way and explains it, and the
    // turns stay for a later attempt (Kept).
    enum class Compressed : uint8_t { Stored, Kept, Aborted };
    Compressed compress(uint32_t turn, std::span<const Message> h,
                        std::pair<uint64_t, uint64_t> range, Outcome& out) {
        net::LlmResult res;
        {
            const BuiltRequest req = build_summary_request(h, range.first, range.second);
            res = request({req.messages.data(), req.messages.size()}, {}, nullptr);
        }
        if (res.usage) out.tokens += res.usage->total;
        if (aborted()) return Compressed::Aborted;
        const bool ok = whole(res) && !res.text.empty();
        if (!ok &&
            (res.error == net::LlmError::NoModel || res.error == net::LlmError::Unauthorized ||
             res.error == net::LlmError::Transport)) {
            WLOG_W(kTag, "context not compressed: %s", net::to_string(res.error));
            return Compressed::Kept;
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
        return Compressed::Stored;
    }

    const char* loop(uint32_t turn, Source source, uint32_t window, Outcome& out) {
        uint32_t generated = 0;
        // Set once the context has been compressed for the coming request
        // (first sparing the last kKeepRecentTurns turns, then only the
        // running one), so a history still too long after that is sent as
        // it is, with the running turn's results cut.
        bool compressed = false;
        bool compressed_deep = false;
        // The model refused the last request as too long: the next one is
        // treated as over the window whatever the estimate says. Once per
        // turn.
        bool overflow = false;
        bool overflow_retried = false;
        // The refused request's estimate while its retry is being
        // prepared: a retry that did not shrink is the same request.
        std::optional<size_t> refused_tokens;
        const std::string_view tools_json = sv(deps.tools.tools_json());
        const size_t tools_tokens = tools_json.size() / 3;
        for (;;) {
            if (aborted()) return finish_abort(turn, out);
            if (!network_up()) {
                set_state(AgentState::Error);
                note(ui::StrId::AgentErrNetwork, turn, &out);
                out.reason = "network";
                return "error";
            }
            set_state(AgentState::Thinking);

            net::LlmResult res;
            size_t sent_tokens = 0;
            {
                const std::shared_ptr<const History> hist = deps.store.history();
                BuiltRequest req = build(*hist, source);
                const bool over =
                    overflow ||
                    needs_compression(cal.scaled(req.est_tokens + tools_tokens), window);
                if (over && !compressed_deep) {
                    // pick_compression() counts bytes / 3: the window it
                    // fills is the real one unscaled.
                    const uint32_t est_window = cal.unscaled_window(window);
                    const size_t fixed = req.fixed_tokens + tools_tokens;
                    std::optional<std::pair<uint64_t, uint64_t>> range;
                    if (!compressed) range = pick_compression(*hist, est_window, fixed);
                    compressed = true;
                    if (!range) {
                        compressed_deep = true;
                        range = pick_compression(*hist, est_window, fixed, 1);
                    }
                    if (range) {
                        const Compressed c = compress(turn, *hist, *range, out);
                        if (c == Compressed::Aborted) return finish_abort(turn, out);
                        // A summary request that failed this way fails the
                        // same way again: go on without summarizing.
                        if (c == Compressed::Kept) compressed_deep = true;
                        // Rebuilt with the summary in place, and judged by
                        // the estimate again.
                        if (c == Compressed::Stored) overflow = false;
                        continue;
                    }
                }
                if (overflow ||
                    uint64_t{cal.scaled(req.est_tokens + tools_tokens)} > uint64_t{window}) {
                    // Nothing more to summarize: the running turn's own
                    // results are what does not fit.
                    WLOG_W(kTag, "context over the window: cutting this turn's tool results");
                    req = build(*hist, source, {.live_tool_bytes = kToolResultKeepBytes});
                }
                compressed = false;
                compressed_deep = false;
                overflow = false;
                sent_tokens = req.est_tokens + tools_tokens;
                if (refused_tokens && sent_tokens >= *refused_tokens) {
                    // Nothing was summarized or cut: the model would refuse
                    // it again.
                    set_state(AgentState::Error);
                    note(ui::StrId::AgentErrContext, turn, &out);
                    out.reason = "context";
                    return "error";
                }
                refused_tokens.reset();
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
            if (whole(res) && res.usage && res.usage->prompt > 0) {
                cal.observe(res.profile_id, res.model, res.usage->prompt, sent_tokens);
            }

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
                if (res.error == net::LlmError::ContextLength && !overflow_retried) {
                    // Our estimate was short of the model's count: trust it
                    // less from now on, compress harder and try once more.
                    overflow_retried = true;
                    overflow = true;
                    refused_tokens = sent_tokens;
                    cal.overflow(res.profile_id, res.model);
                    WLOG_W(kTag, "context too long for the model: compressing and retrying");
                    continue;
                }
                struct Why {
                    ui::StrId id;
                    const char* reason;
                };
                Why why{ui::StrId::AgentErrServer, "server"};
                if (res.error == net::LlmError::ContextLength)
                    why = {ui::StrId::AgentErrContext, "context"};
                if (res.error == net::LlmError::NoModel)
                    why = {ui::StrId::AgentErrNoModel, "no_model"};
                if (res.error == net::LlmError::Unauthorized)
                    why = {ui::StrId::AgentErrUnauthorized, "unauthorized"};
                if (res.error == net::LlmError::Transport)
                    why = res.any_body ? Why{ui::StrId::AgentErrNetworkLost, "network_lost"}
                                       : Why{ui::StrId::AgentErrNetwork, "network"};
                set_state(AgentState::Error);
                note(why.id, turn, &out);
                out.reason = why.reason;
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
                if (out.steps >= max_steps) {
                    skip_calls(turn, calls.subspan(i), "the turn reached its step limit");
                    note(ui::StrId::AgentStepLimit, turn, &out);
                    out.reason = "step_limit";
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
        const std::string dotted = def != nullptr ? std::string(sv(def->name)) : c.name;
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
            const uint64_t limit = turn_ms.load();
            const uint64_t left = spent >= limit ? 0 : limit - spent;
            ToolCtx ctx;
            ctx.cancelled = [this] { return aborted(); };
            ctx.deadline_ms = os::now_ms() + left;
            ctx.lang = ui::lang_code(lang());
            ctx.turn = turn_serial;
            o = deps.tools.call(c.name, sv(c.arguments), ctx);
        }
        ++out.steps;
        const auto ms = static_cast<unsigned>(now() - t0);

        // A successful step the system tells the user about (spec 6.2):
        // the model learns it was told from the result itself, so the
        // history gains no extra system or user message.
        const bool noted = o.ok && !o.note.empty();
        if (noted && !o.result_json.empty()) {
            os::LargeString fields;
            fields.reserve(o.result_json.size() + o.note.size() + 32);
            const std::string_view json(o.result_json.data(), o.result_json.size());
            if (json.size() >= 2 && json.front() == '{' && json.back() == '}') {
                fields.append(json.data(), json.size() - 1);
                if (fields.size() > 1) fields.push_back(',');
                append_json_string(fields, "system_note");
                fields.push_back(':');
                append_json_string(fields, o.note);
                fields.push_back('}');
                o.result_json = std::move(fields);
            }
        } else if (noted && o.result.type() == Value::Type::Map) {
            Map fields = o.result.as_map();
            fields["system_note"] = Value(o.note);
            o.result = Value(std::move(fields));
        }

        Message m;
        m.role = Role::Tool;
        m.turn = turn;
        m.call_id.assign(c.id);
        m.tool.assign(dotted);
        m.ok = o.ok;
        {
            if (!o.result_json.empty()) {
                m.text.assign(o.result_json.data(), o.result_json.size());
            } else {
                const std::string json = to_json(o.result);
                m.text.assign(json);
            }
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
        if (noted) {
            Message n;
            n.role = Role::Note;
            n.turn = turn;
            n.text.assign(o.note);
            append(std::move(n));
        }
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
    bool active = false;    // run_turn() is running
    bool clearing = false;  // clear_all() is running
    std::optional<std::string> confirm_text;
    AgentState state = AgentState::Idle;
    std::string tool;
    // Set with state and tool, under mu (set_state()). Declared after deps
    // (the bus its constructor uses); App::stop() destroys the service
    // before the bus stops.
    core::StateCell<AgentStatus> status;
    std::optional<uint32_t> running_turn;
    std::optional<Source> running_source;
    // ToolCtx::turn of the running turn. Conversation turn numbers start
    // at 1 again in a new conversation, so tools that keep state per turn
    // (SceneWorkspace's sessions) get this serial instead; it is never
    // reset. Only the agent task reads or writes it.
    uint32_t turn_serial = 0;

    std::atomic<ConfirmService*> confirm{nullptr};

    os::Semaphore wake{64, 0};
    std::unique_ptr<os::Task> task;
    std::atomic<bool> quit{false};
    // set_wifi()'s cell; followed from start() to stop().
    const core::StateCell<net::WifiSnapshot>* wifi = nullptr;
    core::CellSubscription wifi_sub;

    // Turn control, read by the check timer and the Wi-Fi callback.
    std::atomic<uint8_t> abort{static_cast<uint8_t>(Abort::None)};
    std::atomic<bool> in_turn{false};
    std::atomic<bool> in_request{false};
    std::atomic<uint32_t> ticket{0};
    std::atomic<uint64_t> turn_start{0};
    // set_turn_limits()'s values, taken at each turn's start into
    // max_steps (wisp.agent only) and turn_ms (also read by the timer).
    std::atomic<uint32_t> next_steps;
    std::atomic<uint32_t> next_turn_ms;
    uint32_t max_steps = 0;
    std::atomic<uint32_t> turn_ms{0};
    std::atomic<uint64_t> last_byte{0};
    os::LargeUnique<net::RequestWatch> watch;
    std::unique_ptr<os::Timer> timer;

    // The estimate in the model's own tokens: bytes / 3, scaled by what
    // the answering profile and model last reported (wisp.agent only).
    Calibration cal;

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

    if (s.wifi != nullptr) {
        s.wifi_sub = s.wifi->subscribe([&s](const net::WifiSnapshot& w) { s.on_wifi(w); });
    }
    s.task = os::Task::spawn(task_config(s.cfg), [&s] { s.run(); });
    if (!s.task) {
        s.wifi_sub.reset();
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
            if (c.done) c.done->finish(Result<void>::err(ErrorCode::Busy, "not_running"));
        }
        s.pending.clear();
        if (s.active || s.clearing) s.trigger(Abort::Stop);
    }
    s.quit = true;
    s.wake.give();
    if (s.task) {
        s.task->join();
        s.task.reset();
    }
    s.wifi_sub.reset();
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
    if (s.active || s.clearing) s.trigger(Abort::Stop);
}

void AgentService::set_turn_limits(uint32_t max_steps, uint32_t turn_ms) {
    if (max_steps != 0) impl_->next_steps.store(max_steps);
    if (turn_ms != 0) impl_->next_turn_ms.store(turn_ms);
}

AgentService::TurnLimits AgentService::turn_limits() const {
    return {impl_->next_steps.load(), impl_->next_turn_ms.load()};
}

Result<void> AgentService::new_conversation() {
    auto done = std::make_shared<Impl::Done>();
    WISP_TRY(impl_->queue_new(done));
    done->sem.take(os::kForever);
    return done->result;
}

Result<void> AgentService::new_conversation_async(std::function<void(Result<void>)> done) {
    auto d = std::make_shared<Impl::Done>();
    d->then = std::move(done);
    return impl_->queue_new(d);
}

void AgentService::set_confirm(ConfirmService* c) { impl_->confirm = c; }

void AgentService::set_wifi(const core::StateCell<net::WifiSnapshot>* cell) { impl_->wifi = cell; }

ConfirmFn AgentService::confirm_fn() {
    Impl* s = impl_.get();
    return [s](const std::string& text, ToolCtx& ctx) {
        if (!s->task || !s->task->is_current()) {
            WLOG_W(kTag, "confirm refused: asked off wisp.agent");
            return false;
        }
        return s->ask_user(text, ctx.cancelled ? ctx.cancelled : [s] { return s->aborted(); });
    };
}

Result<void> AgentService::request_clear(std::function<void(bool cleared)> done) {
    Impl& s = *impl_;
    {
        std::lock_guard<os::Mutex> lock(s.mu);
        if (!s.accepting) return Result<void>::err(ErrorCode::Busy, "not_running");
        if (s.slot || s.active || s.clearing || !s.pending.empty())
            return Result<void>::err(ErrorCode::Busy, "busy");
        Impl::Command c;
        c.kind = Impl::Command::Kind::Clear;
        c.cleared = std::move(done);
        s.pending.push_back(std::move(c));
    }
    s.wake.give();
    return Result<void>::ok();
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
    v.confirm = s.confirm_text;
    v.running_turn = s.running_turn;
    v.running_source = s.running_source;
    v.queued = std::any_of(s.pending.begin(), s.pending.end(), [](const Impl::Command& c) {
        return c.kind == Impl::Command::Kind::Message;
    });
    return v;
}

const core::StateCell<AgentStatus>& AgentService::status_cell() const { return impl_->status; }

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
