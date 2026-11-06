#pragma once

// FakeLlmServer: an in-process net::HttpClient that plays back scripted
// OpenAI chat-completions replies (spec 8.1's FakeLlm). Each request()
// takes the next pushed FakeReply; once the script runs out it answers
// 500. Replies are deterministic: status, SSE chunks fed to on_body one by
// one, a transport failure instead of a response (or, with chunks, after
// them: a connection dropped mid-stream), or a stream that stops
// after `hold_after` chunks until release() or the request's watch ends it
// (Cancelled on abort, Timeout at the watch's deadline).
//
// Every request is recorded with its body copied out of body_ref, which
// only lives for the call. max_concurrent() is the most requests that were
// inside request() at once.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <utility>
#include <vector>

#include "wisp/base/json.hpp"
#include "wisp/base/value.hpp"
#include "wisp/net/http.hpp"
#include "wisp/net/request_watch.hpp"

namespace wisp::test {

struct FakeReply {
    int status = 200;
    std::vector<std::string> chunks;
    net::TransportError fail = net::TransportError::None;
    int hold_after = -1;
};

class FakeLlmServer : public net::HttpClient {
public:
    void push(FakeReply reply) {
        std::lock_guard<std::mutex> lock(mu_);
        script_.push_back(std::move(reply));
    }

    net::HttpClientResponse request(const net::HttpClientRequest& req) override {
        FakeReply reply{.status = 500, .chunks = {}};
        {
            std::lock_guard<std::mutex> lock(mu_);
            net::HttpClientRequest copy = req;
            if (!req.body_ref.empty()) {
                copy.body.assign(reinterpret_cast<const char*>(req.body_ref.data()),
                                 req.body_ref.size());
                copy.body_ref = {};
            }
            copy.on_body = nullptr;
            copy.on_status = nullptr;
            copy.cancelled = nullptr;
            copy.watch = nullptr;
            requests_.push_back(std::move(copy));
            if (!script_.empty()) {
                reply = std::move(script_.front());
                script_.pop_front();
            }
            ++active_;
            max_concurrent_ = std::max(max_concurrent_, active_);
        }
        net::HttpClientResponse res = play(req, reply);
        // As a real client does before it lets the connection go.
        if (req.watch != nullptr) req.watch->disarm();
        {
            std::lock_guard<std::mutex> lock(mu_);
            --active_;
        }
        return res;
    }

    // Lets every held reply (now and later) carry on.
    void release() { released_ = true; }

    // Requests currently stopped at their hold_after point.
    int held() const { return held_.load(); }

    std::vector<net::HttpClientRequest> requests() const {
        std::lock_guard<std::mutex> lock(mu_);
        return requests_;
    }

    Value body_json(size_t i) const {
        std::lock_guard<std::mutex> lock(mu_);
        auto v = from_json(requests_.at(i).body);
        return v ? v.value() : Value();
    }

    // The value of request i's header `name` (any case), "" when absent.
    std::string header(size_t i, std::string_view name) const {
        std::lock_guard<std::mutex> lock(mu_);
        for (const auto& [k, v] : requests_.at(i).headers) {
            if (k.size() != name.size()) continue;
            bool same = true;
            for (size_t j = 0; j < k.size() && same; ++j) same = lower(k[j]) == lower(name[j]);
            if (same) return v;
        }
        return {};
    }

    int max_concurrent() const {
        std::lock_guard<std::mutex> lock(mu_);
        return max_concurrent_;
    }

    // SSE builders: one event each, blank line included.
    static std::string sse_event(const Value& v) { return "data: " + to_json(v) + "\n\n"; }

    static std::string sse_text(std::string_view s) {
        return sse_event(Map{{"choices", List{Map{{"index", 0},
                                                  {"delta", Map{{"content", std::string(s)}}},
                                                  {"finish_reason", Value()}}}}});
    }

    // Leaves out id and name when empty, as servers do after the first
    // fragment of a call.
    static std::string sse_tool(int index, std::string_view id, std::string_view name,
                                std::string_view args_part) {
        Map fn{{"arguments", std::string(args_part)}};
        if (!name.empty()) fn["name"] = std::string(name);
        Map call{{"index", index}, {"function", std::move(fn)}};
        if (!id.empty()) {
            call["id"] = std::string(id);
            call["type"] = "function";
        }
        return sse_event(
            Map{{"choices", List{Map{{"index", 0},
                                     {"delta", Map{{"tool_calls", List{std::move(call)}}}},
                                     {"finish_reason", Value()}}}}});
    }

    static std::string sse_finish(std::string_view reason) {
        return sse_event(Map{
            {"choices",
             List{Map{{"index", 0}, {"delta", Map{}}, {"finish_reason", std::string(reason)}}}}});
    }

    static std::string sse_usage(int prompt, int completion) {
        return sse_event(Map{{"choices", List{}},
                             {"usage", Map{{"prompt_tokens", prompt},
                                           {"completion_tokens", completion},
                                           {"total_tokens", prompt + completion}}}});
    }

    static std::string sse_done() { return "data: [DONE]\n\n"; }

private:
    static char lower(char c) {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    }

    net::HttpClientResponse play(const net::HttpClientRequest& req, const FakeReply& reply) {
        if (reply.fail != net::TransportError::None && reply.chunks.empty())
            return {.transport = reply.fail};
        if (req.on_status) req.on_status(reply.status);
        net::HttpClientResponse res{.status = reply.status};
        for (size_t i = 0; i <= reply.chunks.size(); ++i) {
            if (reply.hold_after >= 0 && static_cast<size_t>(reply.hold_after) == i) {
                if (auto stopped = hold(req)) return {.transport = *stopped};
            }
            if (i == reply.chunks.size()) {
                if (reply.fail != net::TransportError::None) return {.transport = reply.fail};
                break;
            }
            if (req.cancelled && req.cancelled())
                return {.transport = net::TransportError::Cancelled};
            const std::string& c = reply.chunks[i];
            if (req.on_body) {
                if (!req.on_body({reinterpret_cast<const uint8_t*>(c.data()), c.size()}))
                    return {.transport = net::TransportError::Cancelled};
            } else {
                res.body += c;
            }
        }
        return res;
    }

    // Blocks like a client stuck in a read; the error when the watch ended
    // it, nullopt after release().
    std::optional<net::TransportError> hold(const net::HttpClientRequest& req) {
        auto shut = std::make_shared<std::atomic<bool>>(false);
        if (req.watch != nullptr) req.watch->attach([shut] { *shut = true; });
        ++held_;
        while (!released_.load() && !shut->load())
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        --held_;
        if (!shut->load()) return std::nullopt;
        const bool deadline = req.watch->reason() == net::RequestWatch::Reason::Deadline;
        return deadline ? net::TransportError::Timeout : net::TransportError::Cancelled;
    }

    mutable std::mutex mu_;
    std::deque<FakeReply> script_;
    std::vector<net::HttpClientRequest> requests_;
    int active_ = 0;
    int max_concurrent_ = 0;
    std::atomic<bool> released_{false};
    std::atomic<int> held_{0};
};

}  // namespace wisp::test
