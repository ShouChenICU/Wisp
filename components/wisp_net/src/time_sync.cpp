#include "wisp/net/time_sync.hpp"

#include <algorithm>
#include <mutex>
#include <utility>

#include "wisp/core/log.hpp"

namespace wisp::net {

namespace {

constexpr const char* kTag = "time";

}  // namespace

std::vector<std::string> split_servers(std::string_view list) {
    std::vector<std::string> out;
    while (!list.empty()) {
        const size_t comma = list.find(',');
        std::string_view entry = list.substr(0, comma);
        if (!entry.empty()) out.emplace_back(entry);
        if (comma == std::string_view::npos) break;
        list.remove_prefix(comma + 1);
    }
    return out;
}

TimeSyncService::TimeSyncService(TimeSyncDeps deps, TimeSyncConfig config)
    : deps_(std::move(deps)), config_(config), backoff_ms_(config.backoff_min_ms) {}

bool TimeSyncService::tick(bool online) {
    const uint64_t now = deps_.now_ms();
    {
        std::lock_guard<os::Mutex> lock(mutex_);
        if (!online) {
            online_ = false;
            requested_ = false;
            next_attempt_ms_.reset();
            backoff_ms_ = config_.backoff_min_ms;
            return false;
        }
        if (!online_) {
            online_ = true;
            next_attempt_ms_ = now;
        }
        if (requested_) {
            requested_ = false;
            next_attempt_ms_ = now;
        }
        if (in_flight_ || !next_attempt_ms_ || now < *next_attempt_ms_) return false;
        in_flight_ = true;
        next_attempt_ms_.reset();
    }
    // Outside the lock: a Settings read can wait on flash.
    std::vector<std::string> servers = split_servers(deps_.servers ? deps_.servers() : "");
    std::lock_guard<os::Mutex> lock(mutex_);
    job_ = std::move(servers);
    return true;
}

void TimeSyncService::run_attempt() {
    std::vector<std::string> servers;
    {
        std::lock_guard<os::Mutex> lock(mutex_);
        if (!in_flight_) return;
        servers.swap(job_);
    }
    std::string tried;
    for (const std::string& host : servers) {
        tried = host;
        auto r = deps_.sntp.query(host, config_.query_timeout_ms);
        if (r) {
            finish(true, r.value(), host);
            return;
        }
        WLOG_W(kTag, "%s: %s", host.c_str(), r.error().message.c_str());
    }
    finish(false, 0, tried);
}

void TimeSyncService::finish(bool ok, int64_t unix_time, const std::string& server) {
    if (ok) {
        if (deps_.set_system_time) deps_.set_system_time(unix_time);
        if (deps_.rtc) {
            if (auto r = deps_.rtc->set_unix(unix_time); !r) {
                WLOG_W(kTag, "RTC not set: %s", r.error().message.c_str());
            }
        }
        WLOG_I(kTag, "synced from %s: unix %lld", server.c_str(),
               static_cast<long long>(unix_time));
    } else {
        WLOG_W(kTag, "no time server answered");
    }

    const uint64_t now = deps_.now_ms();
    uint32_t retry_ms = 0;
    {
        std::lock_guard<os::Mutex> lock(mutex_);
        in_flight_ = false;
        if (ok) {
            status_ = TimeSyncStatus{.synced = true, .last_sync = unix_time, .server = server};
            synced_at_ms_ = now;
            backoff_ms_ = config_.backoff_min_ms;
            if (online_) next_attempt_ms_ = now + config_.resync_ms;
        } else {
            retry_ms = backoff_ms_;
            backoff_ms_ = std::min(backoff_ms_ * 2, config_.backoff_max_ms);
            if (online_) next_attempt_ms_ = now + retry_ms;
        }
        if (!online_) retry_ms = 0;
    }
    if (retry_ms != 0) {
        WLOG_I(kTag, "next attempt in %u s", static_cast<unsigned>(retry_ms / 1000));
    }

    Properties payload{{"state", Value(ok ? "synced" : "failed")}, {"server", Value(server)}};
    if (ok) payload.emplace("time", Value(unix_time));
    core::Event event;
    event.topic = "system.time";
    event.payload = std::move(payload);
    deps_.bus.publish(std::move(event));
}

bool TimeSyncService::sync_now() {
    {
        std::lock_guard<os::Mutex> lock(mutex_);
        if (!online_) return false;
        if (in_flight_) return true;
        requested_ = true;
    }
    if (wake_) wake_();
    return true;
}

TimeSyncStatus TimeSyncService::status() const {
    std::lock_guard<os::Mutex> lock(mutex_);
    return status_;
}

std::optional<int64_t> TimeSyncService::now_unix() const {
    const uint64_t now = deps_.now_ms();
    std::lock_guard<os::Mutex> lock(mutex_);
    if (!status_.synced) return std::nullopt;
    return status_.last_sync + static_cast<int64_t>((now - synced_at_ms_) / 1000);
}

}  // namespace wisp::net
