#pragma once

// A net::Sntp for tests: answers every host with `time()` unless the host
// is marked down (then a Timeout), and records each query.

#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <set>
#include <string>
#include <utility>
#include <vector>

#include "wisp/base/error.hpp"
#include "wisp/net/time_sync.hpp"

namespace wisp::test {

class FakeSntp : public wisp::net::Sntp {
public:
    explicit FakeSntp(std::function<int64_t()> time) : time_(std::move(time)) {}

    wisp::Result<int64_t> query(const std::string& host, uint32_t timeout_ms) override {
        if (on_query) on_query(host);
        std::lock_guard<std::mutex> lock(mutex_);
        calls_.push_back(host);
        timeouts_.push_back(timeout_ms);
        if (down_.count(host) != 0) {
            return wisp::Result<int64_t>::err(wisp::ErrorCode::Timeout, "no answer from " + host);
        }
        return wisp::Result<int64_t>::ok(time_());
    }

    // Called at the start of each query, outside the lock. Set before use.
    std::function<void(const std::string&)> on_query;

    void set_down(const std::string& host, bool down) {
        std::lock_guard<std::mutex> lock(mutex_);
        if (down) {
            down_.insert(host);
        } else {
            down_.erase(host);
        }
    }
    void all_up() {
        std::lock_guard<std::mutex> lock(mutex_);
        down_.clear();
    }

    // The hosts asked since the last call, in order.
    std::vector<std::string> take_calls() {
        std::lock_guard<std::mutex> lock(mutex_);
        std::vector<std::string> out;
        out.swap(calls_);
        return out;
    }
    std::vector<uint32_t> timeouts() {
        std::lock_guard<std::mutex> lock(mutex_);
        return timeouts_;
    }

private:
    std::function<int64_t()> time_;
    std::mutex mutex_;
    std::set<std::string> down_;
    std::vector<std::string> calls_;
    std::vector<uint32_t> timeouts_;
};

}  // namespace wisp::test
