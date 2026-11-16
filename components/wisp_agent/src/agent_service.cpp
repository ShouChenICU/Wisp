#include "wisp/agent/agent_service.hpp"

#include <algorithm>
#include <atomic>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <type_traits>
#include <utility>

#include "calibration.hpp"
#include "turn_runner.hpp"
#include "utf8.hpp"
#include "wisp/agent/tools.hpp"
#include "wisp/core/log.hpp"
#include "wisp/core/topics.hpp"
#include "wisp/os/large.hpp"
#include "wisp/ui/strings.hpp"

namespace wisp::agent {

namespace {

constexpr const char* kTag = "agent";
constexpr const char* kHolder = "system.agent";
constexpr const char* kLockReason = "agent_turn";

size_t code_points(std::string_view s) {
    size_t n = 0;
    for (char c : s) n += (static_cast<unsigned char>(c) & 0xC0) != 0x80 ? 1 : 0;
    return n;
}

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

struct AgentService::Impl final : TurnEnv {
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
        ctl.watch = nullptr;
        watch.reset();
        std::lock_guard<os::Mutex> lock(mu);
        started = false;
    }

    // --- any thread ----------------------------------------------------

    uint64_t now() const { return deps.now_ms ? deps.now_ms() : os::now_ms(); }
    uint64_t now_ms() const override { return now(); }

    void trigger(Abort why) {
        uint8_t none = static_cast<uint8_t>(Abort::None);
        abort.compare_exchange_strong(none, static_cast<uint8_t>(why));
        // A ticket from an earlier request is a no-op; on_armed covers a
        // request armed after this.
        if (ctl.watch) ctl.watch->abort(ctl.ticket.load());
    }

    // Also the turn's time limit: every check of it counts.
    bool aborted() override {
        if (in_turn.load() && now() - turn_start.load() > turn_ms.load()) trigger(Abort::Time);
        return abort.load() != static_cast<uint8_t>(Abort::None);
    }

    Abort abort_reason() const override { return static_cast<Abort>(abort.load()); }

    TurnControls& controls() override { return ctl; }

    // The check timer: short, no locks but the watch's own, no flash.
    void on_tick() {
        if (!in_turn.load()) return;
        aborted();
        if (ctl.in_request.load() && now() - ctl.last_byte.load() > cfg.stall_ms)
            trigger(Abort::Stall);
    }

    // The Wi-Fi cell's callback, on wisp.bus: atomics and the watch only.
    // The first delivery (at start(), nothing in flight) does nothing.
    void on_wifi(const net::WifiSnapshot& w) {
        if (w.state != net::WifiState::Connected && ctl.in_request.load()) trigger(Abort::Network);
    }

    // The cell itself, not a copy fed from its callback, which could only
    // lag behind it.
    bool network_up() const override {
        if (wifi != nullptr) return wifi->get().state == net::WifiState::Connected;
        return deps.network_up ? deps.network_up() : true;
    }

    ui::Lang lang() const override {
        if (!deps.lang) return ui::Lang::Zh;
        return ui::parse_lang(deps.lang()).value_or(ui::Lang::Zh);
    }

    void emit(const AgentEvent& e) override {
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

    uint64_t append(Message m) override {
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

    void note(ui::StrId id, uint32_t turn) { note_text(ui::tr(id, lang()), turn); }

    void note_text(std::string_view text, uint32_t turn) {
        Message m;
        m.role = Role::Note;
        m.turn = turn;
        m.text.assign(text);
        append(std::move(m));
    }

    // The running turn's notes (TurnEnv).
    void note(ui::StrId id) override { note(id, turn_no); }
    void note_text(std::string_view text) override { note_text(text, turn_no); }

    void marker(uint32_t turn, const char* kind, const char* status) {
        Message m;
        m.role = Role::Marker;
        m.turn = turn;
        m.marker = kind;
        m.status = status;
        append(std::move(m));
    }

    void set_state(AgentState s, std::string_view tool_name = {}) override {
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

    // `n`, when given, receives the archive's number (0: nothing was
    // archived), which a rollover's carry names (spec 3.8).
    Result<void> archive(uint32_t* n = nullptr) {
        // A new conversation drops http.fetch's response cache too (spec
        // 3.3), whether or not archiving the old one succeeds.
        http_tools_drop_cache();
        Result<uint32_t> r = Result<uint32_t>::err(ErrorCode::Busy, "storage worker refused");
        run_io([&] { r = deps.store.archive(); });
        if (!r) {
            WLOG_W(kTag, "archive failed: %s", r.error().message.c_str());
            return Result<void>::err(r.error().code, r.error().message);
        }
        if (n != nullptr) *n = r.value();
        WLOG_I(kTag, "new conversation");
        AgentEvent e;
        e.kind = AgentEvent::Kind::Cleared;
        emit(e);
        return Result<void>::ok();
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
        // This turn's limits: a change from now on waits for the next one.
        max_steps = next_steps.load();
        turn_ms = next_turn_ms.load();
        const uint64_t start = now();
        turn_start = start;
        ctl.last_byte = start;
        in_turn = true;
        if (timer) timer->start();
        core::WakeLock cpu_lock, net_lock;
        if (deps.locks != nullptr) {
            cpu_lock = core::WakeLock(*deps.locks, kHolder, core::WakeLockKind::Cpu, kLockReason);
            net_lock =
                core::WakeLock(*deps.locks, kHolder, core::WakeLockKind::Network, kLockReason);
        }
        TurnRunner runner(deps.llm, deps.tools, deps.store, deps.context, cal, *this);

        // The rollover (spec 3.8) comes first, under the turn's limits and
        // locks: its carry request can take as long as a reply. A stop (or
        // Wi-Fi loss, stall, time) during it leaves the old conversation
        // unarchived, and the turn, framed there, ends at once; the next
        // turn tries again. Archiving emits Cleared, which must reach the
        // listeners before TurnBegin, or the web chat wipes the new turn.
        // Shown as running from here, so that during the carry request
        // view() still tells a waiting message (or a voice hold) that a
        // turn is under way. The number is the old conversation's next
        // one, which an archive below changes before TurnBegin.
        {
            std::lock_guard<os::Mutex> lock(mu);
            running_turn = deps.store.last_turn() + 1;
        }
        bool rolled = false;
        std::optional<os::LargeString> carry;
        uint32_t archived = 0;
        if (deps.store.needs_rollover()) {
            {
                const std::shared_ptr<const History> old = deps.store.history();
                carry = runner.make_carry({.start_ms = start, .turn_ms = turn_ms.load()}, *old);
            }
            if (!aborted()) rolled = archive(&archived).is_ok();
        }

        const uint32_t turn = deps.store.last_turn() + 1;
        ++turn_serial;
        {
            std::lock_guard<os::Mutex> lock(mu);
            running_turn = turn;
        }
        WLOG_I(kTag, "turn %u start source=%s", static_cast<unsigned>(turn), to_string(c.source));

        AgentEvent begin;
        begin.kind = AgentEvent::Kind::TurnBegin;
        begin.text.assign(sv(c.text));
        begin.turn = turn;
        begin.source = c.source;
        emit(begin);

        // A failed archive drops the carry: the old conversation goes on.
        if (rolled && carry) {
            Message m;
            m.role = Role::Carry;
            m.turn = 0;
            m.text = std::move(*carry);
            m.from = archived;
            append(std::move(m));
            note(ui::StrId::AgentRolledOverCarry, turn);
        } else if (rolled) {
            note(ui::StrId::AgentRolledOver, turn);
        }
        marker(turn, "turn_begin", "");
        Message user;
        user.role = Role::User;
        user.turn = turn;
        user.text = c.text;
        user.source = c.source;
        append(std::move(user));
        set_state(AgentState::Thinking);

        turn_no = turn;
        Outcome out = runner.run({turn, turn_serial, max_steps, start, turn_ms.load(), c.source});

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
    // The running turn's number, for TurnEnv's notes (wisp.agent only).
    uint32_t turn_no = 0;

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
    std::atomic<uint64_t> turn_start{0};
    // set_turn_limits()'s values, taken at each turn's start into
    // max_steps (wisp.agent only) and turn_ms (also read by the timer).
    std::atomic<uint32_t> next_steps;
    std::atomic<uint32_t> next_turn_ms;
    uint32_t max_steps = 0;
    std::atomic<uint32_t> turn_ms{0};
    os::LargeUnique<net::RequestWatch> watch;
    // The request in flight; ctl.watch is watch.get() from start() on.
    TurnControls ctl;
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
    s.ctl.watch = s.watch.get();
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
