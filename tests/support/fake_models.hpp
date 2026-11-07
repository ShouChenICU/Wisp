#pragma once

// FakeModelServer: one in-process net::HttpClient for tests that need
// some mix of the three model kinds without a real server. Chat-
// completions traffic (.../chat/completions) is handed straight to the
// embedded FakeLlmServer (push through llm()); ASR (.../audio/
// transcriptions) and TTS (.../audio/speech) are scripted here with their
// own reply shapes, reusing FakeReply for ASR (a plain JSON body, no SSE
// framing) and FakeTtsReply (raw PCM byte chunks) for TTS. requests() and
// max_concurrent() report the combined total across all three kinds;
// release() lets every held reply (including the embedded FakeLlmServer's)
// carry on.

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cstddef>
#include <cstdint>
#include <deque>
#include <mutex>
#include <optional>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "support/fake_llm.hpp"
#include "wisp/base/json.hpp"
#include "wisp/base/value.hpp"
#include "wisp/net/http.hpp"
#include "wisp/net/request_watch.hpp"

namespace wisp::test {

struct FakeTtsReply {
    int status = 200;
    std::vector<std::vector<uint8_t>> chunks;
    int hold_after = -1;
    net::TransportError fail = net::TransportError::None;
};

class FakeModelServer : public net::HttpClient {
public:
    FakeLlmServer& llm() { return llm_; }

    void push_asr(FakeReply reply) {
        std::lock_guard<std::mutex> lock(mu_);
        asr_script_.push_back(std::move(reply));
    }

    void push_tts(FakeTtsReply reply) {
        std::lock_guard<std::mutex> lock(mu_);
        tts_script_.push_back(std::move(reply));
    }

    net::HttpClientResponse request(const net::HttpClientRequest& req) override {
        {
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
            std::lock_guard<std::mutex> lock(mu_);
            requests_.push_back(std::move(copy));
            ++active_;
            max_concurrent_ = std::max(max_concurrent_, active_);
        }
        net::HttpClientResponse res;
        if (req.url.find("/chat/completions") != std::string::npos) {
            res = llm_.request(req);
        } else if (req.url.find("/audio/transcriptions") != std::string::npos) {
            res = play_asr(req);
        } else if (req.url.find("/audio/speech") != std::string::npos) {
            res = play_tts(req);
        } else {
            res = {.status = 404};
        }
        if (req.watch != nullptr) req.watch->disarm();
        {
            std::lock_guard<std::mutex> lock(mu_);
            --active_;
        }
        return res;
    }

    // Lets every held reply (now and later), including the embedded
    // FakeLlmServer's, carry on.
    void release() {
        released_ = true;
        llm_.release();
    }

    std::vector<net::HttpClientRequest> requests() const {
        std::lock_guard<std::mutex> lock(mu_);
        return requests_;
    }

    Value body_json(size_t i) const {
        std::lock_guard<std::mutex> lock(mu_);
        auto v = from_json(requests_.at(i).body);
        return v ? v.value() : Value();
    }

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

    // Requests currently stopped at their hold_after point.
    int held() const { return held_.load(); }

private:
    static char lower(char c) {
        return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
    }

    // Blocks like a client stuck mid-transfer; the error when the watch
    // ended it (or plain Cancelled when there is no watch and `cancelled`
    // said to stop), nullopt after release().
    std::optional<net::TransportError> hold(const net::HttpClientRequest& req) {
        auto shut = std::make_shared<std::atomic<bool>>(false);
        if (req.watch != nullptr) req.watch->attach([shut] { *shut = true; });
        ++held_;
        while (!released_.load() && !shut->load()) {
            if (req.watch == nullptr && req.cancelled && req.cancelled()) {
                *shut = true;
                break;
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        --held_;
        if (!shut->load()) return std::nullopt;
        if (req.watch == nullptr) return net::TransportError::Cancelled;
        const bool deadline = req.watch->reason() == net::RequestWatch::Reason::Deadline;
        return deadline ? net::TransportError::Timeout : net::TransportError::Cancelled;
    }

    net::HttpClientResponse play_asr(const net::HttpClientRequest& req) {
        FakeReply reply{.status = 500, .chunks = {}};
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (!asr_script_.empty()) {
                reply = std::move(asr_script_.front());
                asr_script_.pop_front();
            }
        }
        if (reply.hold_after == 0) {
            if (auto stopped = hold(req)) return {.transport = *stopped};
        }
        if (reply.fail != net::TransportError::None) return {.transport = reply.fail};
        if (req.on_status) req.on_status(reply.status);
        net::HttpClientResponse res{.status = reply.status};
        for (const std::string& c : reply.chunks) res.body += c;
        return res;
    }

    net::HttpClientResponse play_tts(const net::HttpClientRequest& req) {
        FakeTtsReply reply{.status = 500, .chunks = {}};
        {
            std::lock_guard<std::mutex> lock(mu_);
            if (!tts_script_.empty()) {
                reply = std::move(tts_script_.front());
                tts_script_.pop_front();
            }
        }
        if (reply.fail != net::TransportError::None && reply.chunks.empty()) {
            return {.transport = reply.fail};
        }
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
            const std::vector<uint8_t>& c = reply.chunks[i];
            if (req.on_body) {
                if (!req.on_body({c.data(), c.size()}))
                    return {.transport = net::TransportError::Cancelled};
            } else {
                res.body.append(reinterpret_cast<const char*>(c.data()), c.size());
            }
        }
        return res;
    }

    mutable std::mutex mu_;
    FakeLlmServer llm_;
    std::deque<FakeReply> asr_script_;
    std::deque<FakeTtsReply> tts_script_;
    std::vector<net::HttpClientRequest> requests_;
    int active_ = 0;
    int max_concurrent_ = 0;
    std::atomic<bool> released_{false};
    std::atomic<int> held_{0};
};

}  // namespace wisp::test
