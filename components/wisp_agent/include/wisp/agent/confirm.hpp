#pragma once

// ConfirmService (agent-core spec 6.1): asks the user on the device before
// an irreversible step, with the system layer's modal dialog (title
// AgentConfirmTitle, a Danger "Allow" button and a "Deny" one).
//
// ask() blocks its caller -- wisp.agent, never the UI thread or wisp.fs --
// and polls every kPollMs for the answer, the caller's cancellation and
// the end of the window (timeout_ms, kConfirmMs in production):
//   - one request at a time: a second ask() waits until the first is
//     over, then gets its own window;
//   - the screen is woken (deps.wake, on the UI thread) before the dialog
//     opens;
//   - while another system dialog is up (a factory reset, a delete asked
//     from the web) the dialog opens once that one closes, within the
//     same window; one that never closes leaves the request TimedOut;
//   - silence for the whole window is TimedOut, a cancellation Cancelled;
//     either closes the dialog without an answer.
//
// The dialog lives on the UI thread; everything ask() does there goes
// through UiRuntime::post(). `ui` and `layer` must outlive the service
// and every ask(); a close posted as an ask() returns may run after it.

#include <cstdint>
#include <functional>
#include <optional>
#include <string>

#include "wisp/agent/limits.hpp"
#include "wisp/os/os.hpp"
#include "wisp/ui/system_layer.hpp"
#include "wisp/ui/ui_runtime.hpp"

namespace wisp::agent {

struct ConfirmDeps {
    ui::UiRuntime& ui;
    ui::SystemLayer& layer;
    std::function<void()> wake;  // turn the screen on; UI thread; empty: none
    std::function<uint64_t()> now_ms = os::now_ms;
    uint32_t timeout_ms = kConfirmMs;  // tests shorten it
};

enum class Answer : uint8_t { Allowed, Denied, TimedOut, Cancelled };
// "allowed" "denied" "timed_out" "cancelled"
const char* to_string(Answer a);

class ConfirmService {
public:
    static constexpr uint32_t kPollMs = 20;

    explicit ConfirmService(ConfirmDeps deps);

    ConfirmService(const ConfirmService&) = delete;
    ConfirmService& operator=(const ConfirmService&) = delete;

    // Shows `text` and waits for the answer (see above). `cancelled` may
    // be empty.
    Answer ask(const std::string& text, const std::function<bool()>& cancelled);

    // The question being asked now, if any.
    std::optional<std::string> pending() const;

    // Called on the asking thread with the question when a request starts
    // and with nullopt when it ends. Set it before the first ask().
    void set_listener(std::function<void(const std::optional<std::string>&)> cb);

private:
    void announce(const std::optional<std::string>& text);

    ConfirmDeps deps_;
    mutable os::Mutex mu_;
    bool busy_ = false;
    std::optional<std::string> pending_;
    std::function<void(const std::optional<std::string>&)> listener_;
};

}  // namespace wisp::agent
