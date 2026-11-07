#pragma once

// AgentService (agent-core spec 3): the conversation loop on its own task,
// wisp.agent. One turn at a time: the user message, then model requests
// and the tool calls they ask for, until the model answers without a tool
// or the turn hits a limit, is stopped or fails.
//
// A turn
//   - is framed by turn_begin/turn_end markers in the ConversationStore;
//     every message is appended on the storage worker (run_io) and the
//     loop waits for it, so the file always holds what listeners saw;
//   - runs at most max_steps tool calls and turn_ms in all;
//   - holds a cpu and a network wake lock ("system.agent", "agent_turn");
//   - ends with one of the statuses "done", "stopped", "step_limit",
//     "time_limit", "error" (and, found after a restart, "interrupted").
//   Problems the user should know about become note messages in the
//   device language: no model, no network, a lost or stalled stream, a
//   refused key, a server error, a limit, a stop.
//
// Each model request is laid out by context.hpp: with a context source,
// the fixed system prompt, the history and the dynamic info (a snapshot
// taken for that request with the turn's source); without one, the history
// alone. Past kCompressPercent of the active llm profile's context window
// (read once per turn) the oldest turns are first summarized by an extra
// request without tools. If the model fails to summarize, they are left out
// of the context (an empty summary) with a note; with no model, a refused
// key or no connection nothing is stored and the request goes out whole.
//
// A model request is aborted (its RequestWatch, and its cancelled() check,
// which also covers the waits between retries) when the user stops the
// turn, when net.wifi.state leaves "connected", when no byte arrives for
// stall_ms, or at the turn's time limit. A check timer runs every
// kCheckMs during a turn for the last two. Text already received is kept.
//
// Queueing: one message runs and kMaxQueued more wait; past that submit()
// is Busy. stop_turn() also drops the waiting ones.
//
// Irreversible steps ask on the device (spec 6.1) through the
// ConfirmService set with set_confirm(): the tools' ConfirmFn
// (confirm_fn()) and request_clear(). A tool's successful result with a
// note (spec 6.2) is followed by a note message with that text, and the
// stored result -- what the model sees next -- carries it as
// "system_note".
//
// Threads: submit(), stop_turn(), new_conversation(), state(), view() and
// the listener calls may come from any thread. Listeners and the
// agent.state event run on wisp.agent; a listener must not block for
// long or call back into submit()/new_conversation().
//
// wisp.agent declares flash_access = false (its stack may be in PSRAM):
// everything it calls that touches flash -- the conversation file -- goes
// through run_io. The model client, the tool registry (which posts
// each tool to the thread it declares), the wake locks, the event bus and
// NotificationService::post_system() (which saves on the storage worker)
// do not touch flash themselves; deps.lang, screen_off, network_up, wall_s
// and context must read cached values only.
//
// Logs carry metadata only (no user or model text):
//   agent: turn <n> start source=<s>
//   agent: state <s>
//   agent: tool <name> <ok|error:<code>> <ms> ms
//   agent: turn <n> end <status> steps=<k> tokens=<t>
//   agent: context <summarized|dropped> seq <first>-<last>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "wisp/agent/confirm.hpp"
#include "wisp/agent/context.hpp"
#include "wisp/agent/conversation.hpp"
#include "wisp/agent/limits.hpp"
#include "wisp/agent/tool_registry.hpp"
#include "wisp/base/result.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/core/model_profiles.hpp"
#include "wisp/core/notification_service.hpp"
#include "wisp/core/wakelock.hpp"
#include "wisp/net/llm_client.hpp"
#include "wisp/net/request_watch.hpp"
#include "wisp/os/large.hpp"
#include "wisp/os/os.hpp"

namespace wisp::agent {

enum class AgentState : uint8_t { Idle, Thinking, Tool, Output, WaitingConfirm, Error };
// "idle" "thinking" "tool" "output" "waiting_confirm" "error"
const char* to_string(AgentState s);

struct AgentEvent {
    enum class Kind : uint8_t {
        TurnBegin,
        TurnEnd,
        Delta,
        ToolStart,
        ToolEnd,
        State,
        Confirm,
        Message,
        Cleared
    };
    Kind kind = Kind::State;
    std::string text;                     // Delta; Confirm ("" = over); TurnBegin: the user text
    std::string tool;                     // ToolStart, ToolEnd, State (Tool): dotted
    std::string args, result;             // ToolStart, ToolEnd: cut to kSummaryChars code points
    bool ok = true;                       // ToolEnd; Cleared: false when nothing changed
    AgentState state = AgentState::Idle;  // State
    uint64_t seq = 0;                     // Message: the appended message's seq
    uint32_t turn = 0;                    // TurnBegin, TurnEnd
    Source source = Source::Web;          // TurnBegin
    std::string status;                   // TurnEnd
};

struct ConversationView {
    // A shared snapshot of the conversation (no copy).
    std::shared_ptr<const History> messages;
    AgentState state = AgentState::Idle;
    std::string tool;  // while state is Tool
    std::optional<uint32_t> running_turn;
    std::optional<Source> running_source;
    bool queued = false;
    std::optional<std::string> confirm;
};

struct AgentDeps {
    core::EventBus& bus;
    ConversationStore& store;
    ToolRegistry& tools;
    net::LlmClient& llm;
    core::ModelProfileService& models;  // get() only
    // Runs a job on the storage worker and waits for it to finish; an
    // error if it could not be queued (it then never runs). The service
    // passes jobs that reference its own stack, so the wait must be
    // unbounded. Production: StorageWorker::run_sync with os::kForever.
    // Empty: jobs run on the calling thread (tests without a worker).
    std::function<Result<void>(std::function<void()>)> run_io;
    core::WakeLockRegistry* locks = nullptr;
    core::NotificationService* notes = nullptr;  // the agent_done notice
    std::function<bool()> screen_off;            // empty: never off
    // Whether the network was up before any net.wifi.state event arrived;
    // after one, the latest event decides. Empty: up.
    std::function<bool()> network_up;
    std::function<std::string()> lang;  // "zh" | "ja" | "en"; empty: "zh"
    std::function<uint64_t()> now_ms = os::now_ms;
    std::function<int64_t()> wall_s;  // message timestamps; empty: 0
    ContextSource context;            // empty: the history only
};

struct AgentConfig {
    uint32_t max_steps = kMaxSteps;
    uint32_t turn_ms = kTurnMs;
    uint32_t stall_ms = kStallMs;
    size_t stack_bytes = 16 * 1024;
    int priority = 3;
};

class AgentService {
public:
    // How often a running turn checks for a stalled stream and its time
    // limit.
    static constexpr uint32_t kCheckMs = 250;
    // The longest model request (each one gets the smaller of this and
    // what is left of the turn).
    static constexpr uint32_t kRequestMs = 300000;
    // A model request's own deadline lies this far past the turn's, so
    // the turn's time limit ends it first.
    static constexpr uint32_t kDeadlineMarginMs = 2 * kCheckMs;
    // A done notice's text: the reply's first line, at most this many
    // bytes.
    static constexpr size_t kNoticeBytes = core::NotificationService::kArgMax;

    AgentService(AgentDeps deps, AgentConfig config = {});
    ~AgentService();  // stop()

    AgentService(const AgentService&) = delete;
    AgentService& operator=(const AgentService&) = delete;

    // "wisp.agent", flash_access false.
    static os::TaskConfig task_config(const AgentConfig& c);

    // Loads the conversation on the storage worker (and waits), notes a
    // turn a restart cut short, then starts the task. Busy if already
    // started; after a failure it may be called again.
    Result<void> start();
    // Stops the running turn and the task; waiting messages are dropped.
    void stop();

    // InvalidArgument "empty" or "too_long" (> kMaxUserChars code points);
    // Busy "busy" when a turn runs and kMaxQueued messages already wait;
    // Busy "not_running" before start() or after stop().
    Result<void> submit(std::string text, Source source);
    // Stops the running turn (if any) and drops the waiting messages.
    void stop_turn();
    // Archives the conversation and starts a new one (Cleared). Busy while
    // a turn runs or a message waits.
    Result<void> new_conversation();
    // new_conversation() without the wait, for a caller that must not
    // block (the UI thread): the same Busy answers at once; otherwise
    // `done` runs once with the archive's outcome, on wisp.agent (Busy
    // "not_running" from stop() if it had not run yet). It must be quick
    // and must not call back into the service.
    Result<void> new_conversation_async(std::function<void(Result<void>)> done);

    // The device confirmation the tools' ConfirmFn and request_clear() go
    // through; null (the default) refuses every request. Set it before
    // start(); it must outlive the service's task.
    void set_confirm(ConfirmService* c);
    // The ConfirmFn for the tools: on wisp.agent it puts the agent in
    // WaitingConfirm, emits Confirm(text), asks (cancelled by the turn's
    // stop or limits), emits Confirm("") and goes back to the tool state;
    // true only for Allowed. Called on any other thread it refuses
    // without asking: a wait for the user must never hold the UI thread
    // or wisp.fs.
    ConfirmFn confirm_fn();
    // Asks on the device (AgentConfirmClear) and, when allowed, removes
    // the current conversation and every archive on the storage worker.
    // Runs on wisp.agent after anything already queued; stop_turn() while
    // it waits cancels it. Then emits Cleared (ok: whether the history was
    // cleared) and calls `done` with the same, on wisp.agent. Busy "busy"
    // (and `done` is never called) while a turn runs or a message waits;
    // "not_running" before start() or after stop().
    Result<void> request_clear(std::function<void(bool cleared)> done);

    AgentState state() const;
    ConversationView view() const;

    uint32_t add_listener(std::function<void(const AgentEvent&)> cb);
    // Once it returns, `id`'s callback is not running and never runs
    // again (except when called from inside a listener, which cannot
    // wait for itself).
    void remove_listener(uint32_t id);

private:
    struct Impl;
    os::LargeUnique<Impl> impl_;
};

}  // namespace wisp::agent
