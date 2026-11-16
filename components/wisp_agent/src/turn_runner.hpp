#pragma once

// TurnRunner: one agent turn, from the first model request to the last
// tool result (spec 3.10). It runs on wisp.agent against a TurnEnv, which
// AgentService::Impl implements: everything the turn writes to the store,
// shows to the listeners or reads of the service's state goes through it.
// Internal to wisp_agent.

#include <atomic>
#include <cstdint>
#include <functional>
#include <optional>
#include <span>
#include <string>
#include <string_view>
#include <utility>

#include "calibration.hpp"
#include "wisp/agent/agent_service.hpp"
#include "wisp/agent/context.hpp"
#include "wisp/agent/conversation.hpp"
#include "wisp/agent/tool_registry.hpp"
#include "wisp/net/llm_provider.hpp"
#include "wisp/net/request_watch.hpp"
#include "wisp/os/large.hpp"
#include "wisp/ui/strings.hpp"

namespace wisp::agent {

// Why a turn is being cut short; the first reason wins.
enum class Abort : uint8_t { None, Stop, Network, Stall, Time };

struct Outcome {
    const char* status = "done";
    // AgentEvent::reason, set where the note that explains it is written.
    const char* reason = "";
    uint32_t steps = 0;
    uint32_t tokens = 0;
    // The turn's last reply or note: the done notice's text.
    os::LargeString last_text;
};

// The request in flight as the check timer and the Wi-Fi callback see it;
// owned by AgentService::Impl, written by TurnRunner on wisp.agent.
struct TurnControls {
    net::RequestWatch* watch = nullptr;  // null: no watch (RequestWatch::create() failed)
    std::atomic<uint32_t> ticket{0};
    std::atomic<uint64_t> last_byte{0};
    std::atomic<bool> in_request{false};
};

// What the service lends a turn (spec 3.10). Implemented by
// AgentService::Impl.
struct TurnEnv {
    virtual uint64_t append(Message m) = 0;  // on the storage worker, waits; 0 on failure
    virtual void note(ui::StrId id) = 0;     // a note of the running turn
    virtual void note_text(std::string_view t) = 0;
    virtual void set_state(AgentState s, std::string_view tool = {}) = 0;
    virtual void emit(const AgentEvent& e) = 0;
    virtual bool aborted() = 0;  // stop, network, stall, time (checks the limit)
    virtual Abort abort_reason() const = 0;
    virtual bool network_up() const = 0;
    virtual ui::Lang lang() const = 0;
    virtual TurnControls& controls() = 0;
    // The turn's clock (AgentDeps::now_ms), which tests move ahead.
    virtual uint64_t now_ms() const = 0;

protected:
    ~TurnEnv() = default;
};

struct TurnInput {
    uint32_t turn = 0;    // conversation turn number
    uint32_t serial = 0;  // ToolCtx::turn
    uint32_t max_steps = 0;
    uint64_t start_ms = 0;  // TurnEnv::now_ms() at the turn's start
    uint64_t turn_ms = 0;
    Source source = Source::Web;
};

// Lives on wisp.agent's stack for one turn: references and small scalars
// only.
class TurnRunner {
public:
    TurnRunner(net::LlmProvider& llm, ToolRegistry& tools, ConversationStore& store,
               const ContextSource& context, Calibration& cal, TurnEnv& env)
        : llm_(llm), tools_(tools), store_(store), context_(context), cal_(cal), env_(env) {}

    TurnRunner(const TurnRunner&) = delete;
    TurnRunner& operator=(const TurnRunner&) = delete;

    // Reads llm.active_model() once (the window and the calibration's
    // pair), then runs the turn's requests and tools.
    Outcome run(const TurnInput& in);

private:
    // Summarizing the history: what came of it (see compress()).
    enum class Compressed : uint8_t { Stored, Kept, Aborted };

    const char* loop(uint32_t window, Outcome& out);
    void note(ui::StrId id, Outcome* out = nullptr);
    void note_text(std::string_view text, Outcome* out = nullptr);
    const char* finish_abort(Outcome& out);
    void skip_calls(std::span<const net::LlmToolCall> calls, std::string_view why);
    BuiltRequest build(std::span<const Message> h, BuildOptions opts = {}) const;
    net::LlmResult request(std::span<const net::LlmMessage> messages, std::string_view tools_json,
                           std::function<void(std::string_view)> on_text);
    static bool whole(const net::LlmResult& res);
    Compressed compress(std::span<const Message> h, std::pair<uint64_t, uint64_t> range,
                        Outcome& out);
    void run_tool(const net::LlmToolCall& c, bool cut, Outcome& out);

    net::LlmProvider& llm_;
    ToolRegistry& tools_;
    ConversationStore& store_;
    const ContextSource& context_;
    Calibration& cal_;
    TurnEnv& env_;
    TurnInput in_{};
};

std::string_view sv(const os::LargeString& s);
std::string summary(std::string_view s);  // at most kSummaryChars code points

}  // namespace wisp::agent
