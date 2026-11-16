#pragma once

// FakeProvider: a scripted net::LlmProvider, for tests of the agent that
// do not need the HTTP and SSE path under it. Each complete() takes the
// next pushed FakeStep and plays it through the call's callbacks: every
// delta as on_bytes() then on_text(), every tool call once through
// on_tool(), then `during`, then (with `hold`) waits until cancelled(),
// at most kHoldMs (a call without cancelled() is never cancelled).
// Without an active model it answers NoModel and records nothing; once the
// script runs out it answers error Server, status 500.
//
// Every call is recorded with the body LlmClient would have sent for it
// (build_chat_body), built while the call's messages are alive. All state
// is under one mutex; it allocates freely, being a test double.

#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <functional>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "wisp/net/llm_client.hpp"
#include "wisp/net/llm_provider.hpp"

namespace wisp::test {

struct FakeStep {
    std::vector<std::string> deltas{};      // each: on_bytes(), then on_text()
    std::vector<net::LlmToolCall> calls{};  // each reported once through on_tool
    // error, finish, usage, any_body; text and tool_calls are filled from
    // deltas and calls when left empty.
    net::LlmResult result{};
    // After the deltas, poll cancelled() every 1 ms; then error Cancelled,
    // transport Cancelled. Not cancelled within FakeProvider::kHoldMs: error
    // Transport, transport Timeout, so a test that never cancels fails
    // instead of hanging.
    bool hold = false;
    std::function<void(const net::LlmCall&)> during{};  // inside complete(), after the deltas
};

class FakeProvider final : public net::LlmProvider {
public:
    static constexpr uint32_t kHoldMs = 10000;

    struct Recorded {
        std::string body;  // build_chat_body(model, call.messages, call.tools_json)
        std::string tools_json;
        uint32_t timeout_ms = 0;
        size_t messages = 0;
    };

    void set_model(std::optional<net::LlmModelInfo> m) {
        std::lock_guard<std::mutex> lock(mu_);
        model_ = std::move(m);
    }

    void push(FakeStep s) {
        std::lock_guard<std::mutex> lock(mu_);
        script_.push_back(std::move(s));
    }

    std::optional<net::LlmModelInfo> active_model() const override {
        std::lock_guard<std::mutex> lock(mu_);
        ++active_model_calls_;
        return model_;
    }

    net::LlmResult complete(const net::LlmCall& call) override {
        FakeStep step;
        std::optional<net::LlmModelInfo> model;
        {
            std::lock_guard<std::mutex> lock(mu_);
            model = model_;
            if (!model) {
                net::LlmResult none;
                none.error = net::LlmError::NoModel;
                return none;
            }
            const os::LargeString body =
                net::build_chat_body(model->model, call.messages, call.tools_json);
            calls_.push_back({.body = std::string(body.data(), body.size()),
                              .tools_json = std::string(call.tools_json),
                              .timeout_ms = call.timeout_ms,
                              .messages = call.messages.size()});
            if (script_.empty()) {
                step.result.error = net::LlmError::Server;
                step.result.status = 500;
            } else {
                step = std::move(script_.front());
                script_.pop_front();
            }
        }

        net::LlmResult res = std::move(step.result);
        res.model = model->model;
        res.profile_id = model->profile_id;
        res.attempts = 1;
        res.any_body = res.any_body || !step.deltas.empty() || !step.calls.empty();

        os::LargeString text;
        for (const std::string& d : step.deltas) {
            if (call.on_bytes) call.on_bytes();
            if (call.on_text) call.on_text(d);
            text.append(d.data(), d.size());
        }
        for (size_t i = 0; i < step.calls.size(); ++i) {
            if (call.on_bytes) call.on_bytes();
            if (call.on_tool)
                call.on_tool({.index = i,
                              .name = step.calls[i].name,
                              .arg_bytes = step.calls[i].arguments.size()});
        }
        if (res.text.empty()) res.text = std::move(text);
        if (res.tool_calls.empty()) res.tool_calls = std::move(step.calls);
        if (step.during) step.during(call);
        if (step.hold) {
            const auto until =
                std::chrono::steady_clock::now() + std::chrono::milliseconds(kHoldMs);
            bool cancelled = false;
            while (!(cancelled = call.cancelled && call.cancelled()) &&
                   std::chrono::steady_clock::now() < until)
                std::this_thread::sleep_for(std::chrono::milliseconds(1));
            res.error = cancelled ? net::LlmError::Cancelled : net::LlmError::Transport;
            res.transport =
                cancelled ? net::TransportError::Cancelled : net::TransportError::Timeout;
        }
        return res;
    }

    std::vector<Recorded> calls() const {
        std::lock_guard<std::mutex> lock(mu_);
        return calls_;
    }

    int active_model_calls() const {
        std::lock_guard<std::mutex> lock(mu_);
        return active_model_calls_;
    }

private:
    mutable std::mutex mu_;
    std::optional<net::LlmModelInfo> model_ =
        net::LlmModelInfo{.profile_id = "p1", .model = "m", .context_window = 65536};
    std::deque<FakeStep> script_;
    std::vector<Recorded> calls_;
    mutable int active_model_calls_ = 0;
};

}  // namespace wisp::test
