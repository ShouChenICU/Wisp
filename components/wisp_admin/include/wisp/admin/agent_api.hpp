#pragma once

// What the admin backend needs from the agent (agent-core spec 7.1): the
// routes under /api/v1/agent call it, App implements it over the
// AgentService (wisp_admin sits below wisp_agent, so it only knows this
// interface).
//
// Conversations: seq and turn numbers start over in every conversation (a
// new one, or a cleared history). "epoch" in conversation() and in the
// WebSocket's "cleared" frame names the conversation within one boot (the
// boot's id is /info's "boot_id"): a client that sees either change drops
// what it has cached.
//
// Messages are agent::to_value()'s shape, with a tool result and call
// arguments cut to 200 code points ("result_len"/"args_len" give the full
// size) except in message(), plus:
//   - "hidden": true on a summary (it stands in for older turns in the
//     model's context, it is not something anyone said) and on a turn
//     marker; the chat page does not show them as messages;
//   - "dropped": true on a summary with no text: those turns were left out
//     of the model's context without a summary.
//
// Documents that can be large (a conversation, an archive, a whole tool
// result) come back as JSON text in Mem::Large, sent without a copy.
//
// Every method may be called from any HTTP server thread.

#include <cstddef>
#include <cstdint>
#include <memory>
#include <string>

#include "wisp/base/result.hpp"
#include "wisp/base/value.hpp"
#include "wisp/os/large.hpp"

namespace wisp::admin {

using JsonDoc = std::shared_ptr<const os::LargeString>;

class AgentApi {
public:
    virtual ~AgentApi() = default;

    // {"epoch","state","tool":name|null,"model":{"name","model","vision"}|
    //  null,"turn":n|null,"queued":bool,"confirm":text|null,
    //  "total":n,"messages":[the last max_messages]}
    virtual JsonDoc conversation(size_t max_messages) = 0;
    // One message of the current conversation, uncut; NotFound.
    virtual Result<JsonDoc> message(uint64_t seq) = 0;
    // Sends a message from the web (source "web"). True when it waits
    // behind a running turn. InvalidArgument "empty"/"too_long"; Busy
    // "busy" when the queue is full, "not_running" when the agent is
    // stopped.
    virtual Result<bool> submit(std::string text) = 0;
    // Stops the running turn and drops the waiting message.
    virtual void stop() = 0;
    // Archives the conversation and starts a new one; Busy "busy" while a
    // turn runs or a message waits.
    virtual Result<void> new_conversation() = 0;
    // {"archives":[{"n","ts","first"}]}, newest first.
    virtual Result<Value> archives() = 0;
    // {"n","messages":[...]}; NotFound.
    virtual Result<JsonDoc> archive(uint32_t n) = 0;
    // Asks on the device whether to clear every conversation; the outcome
    // arrives as the "cleared" frame. Busy "busy" while a turn runs or a
    // message waits.
    virtual Result<void> request_clear() = 0;
};

}  // namespace wisp::admin
