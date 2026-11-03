#include "wisp/net/wifi_service.hpp"

#include <algorithm>
#include <cstdio>
#include <map>
#include <mutex>
#include <utility>

#include "wisp/base/error.hpp"
#include "wisp/base/json.hpp"
#include "wisp/base/value.hpp"
#include "wisp/core/log.hpp"

namespace wisp::net {

namespace {

constexpr const char* kTag = "wifi";
constexpr const char* kWifiFile = "/net/wifi.json";
constexpr size_t kMaxSsidBytes = 32;
constexpr size_t kMinPasswordBytes = 8;
constexpr size_t kMaxPasswordBytes = 63;
constexpr int64_t kDefaultIdleOffMin = 10;
constexpr size_t kApPasswordLen = 8;
constexpr char kApAlphabet[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789";
constexpr size_t kApAlphabetLen = sizeof(kApAlphabet) - 1;
static_assert(kApAlphabetLen == 62);

Result<void> validate(std::string_view ssid, std::string_view password) {
    if (ssid.empty() || ssid.size() > kMaxSsidBytes) {
        return Result<void>::err(ErrorCode::InvalidArgument, "ssid must be 1-32 bytes");
    }
    if (!password.empty() &&
        (password.size() < kMinPasswordBytes || password.size() > kMaxPasswordBytes)) {
        return Result<void>::err(ErrorCode::InvalidArgument,
                                 "password must be empty (open network) or 8-63 bytes");
    }
    return Result<void>::ok();
}

const char* reason_text(hal::WifiFailReason r) {
    switch (r) {
        case hal::WifiFailReason::NotFound:
            return "network not found";
        case hal::WifiFailReason::AuthFailed:
            return "authentication failed";
        case hal::WifiFailReason::Other:
        case hal::WifiFailReason::None:
            break;
    }
    return "connection failed";
}

// Values of net.wifi.state's "reason" (see WifiService::fail_reason_).
constexpr const char* kReasonNotFound = "not_found";
constexpr const char* kReasonAuthFailed = "auth_failed";
constexpr const char* kReasonOther = "other";

const char* reason_code(hal::WifiFailReason r) {
    switch (r) {
        case hal::WifiFailReason::NotFound:
            return kReasonNotFound;
        case hal::WifiFailReason::AuthFailed:
            return kReasonAuthFailed;
        case hal::WifiFailReason::Other:
        case hal::WifiFailReason::None:
            break;
    }
    return kReasonOther;
}

}  // namespace

const char* to_string(WifiState s) {
    switch (s) {
        case WifiState::Off:
            return "off";
        case WifiState::Connecting:
            return "connecting";
        case WifiState::Backoff:
            return "backoff";
        case WifiState::Connected:
            return "connected";
    }
    return "off";
}

WifiService::WifiService(std::shared_ptr<hal::Wifi> wifi, core::StorageManager& storage,
                         core::EventBus& bus, core::Settings settings, RandomFn random,
                         WifiServiceConfig config)
    : wifi_(std::move(wifi)),
      storage_(storage),
      bus_(bus),
      settings_(std::move(settings)),
      random_(std::move(random)),
      config_(config),
      source_(wifi_->info().id),
      backoff_ms_(config.backoff_min_ms) {}

// ---------------------------------------------------------------------------
// Persistence

std::string WifiService::wifi_path() const {
    auto root = storage_.write_root(core::Area::System);
    return root ? root.value() + kWifiFile : std::string();
}

Result<void> WifiService::persist_locked(const std::vector<Network>& networks) {
    std::string path = wifi_path();
    if (path.empty()) {
        return Result<void>::err(ErrorCode::StorageUnavailable, "system storage unavailable");
    }
    List list;
    for (const auto& n : networks) {
        list.push_back(Map{{"ssid", n.ssid}, {"password", n.password}});
    }
    std::string json = to_json_raw(Map{{"networks", std::move(list)}});
    auto r = storage_.write_atomic(path, std::vector<uint8_t>(json.begin(), json.end()));
    if (!r) {
        WLOG_E(kTag, "saving networks failed: %s", r.error().message.c_str());
    }
    return r;
}

const WifiService::Network* WifiService::find_locked(std::string_view ssid) const {
    for (const auto& n : networks_) {
        if (n.ssid == ssid) return &n;
    }
    return nullptr;
}

Result<void> WifiService::init(uint64_t now_ms) {
    std::lock_guard<os::Mutex> lock(mutex_);
    networks_.clear();

    Result<void> result = Result<void>::ok();
    std::string path = wifi_path();
    if (path.empty()) {
        result = Result<void>::err(ErrorCode::StorageUnavailable, "system storage unavailable");
    } else if (auto bytes = storage_.read_file(path); !bytes) {
        if (bytes.error().code != ErrorCode::NotFound) {
            result = Result<void>::err(bytes.error().code,
                                       "reading net/wifi.json: " + bytes.error().message);
        }
    } else {
        // The parser's own message may quote file content (i.e. a
        // password), so it is deliberately not passed on.
        auto doc = from_json(std::string_view(reinterpret_cast<const char*>(bytes.value().data()),
                                              bytes.value().size()));
        const Value* list = doc ? doc.value().find("networks") : nullptr;
        if (list == nullptr || list->type() != Value::Type::List) {
            result = Result<void>::err(ErrorCode::InvalidArgument, "net/wifi.json is malformed");
        } else {
            size_t skipped = 0;
            for (const auto& entry : list->as_list()) {
                const Value* ssid = entry.find("ssid");
                const Value* pw = entry.find("password");
                if (ssid == nullptr || pw == nullptr || ssid->type() != Value::Type::String ||
                    pw->type() != Value::Type::String ||
                    !validate(ssid->as_string(), pw->as_string()) ||
                    find_locked(ssid->as_string()) != nullptr ||
                    networks_.size() >= config_.max_networks) {
                    ++skipped;
                    continue;
                }
                networks_.push_back({ssid->as_string(), pw->as_string()});
            }
            if (skipped > 0) {
                WLOG_W(kTag, "ignored %zu invalid saved network entr%s", skipped,
                       skipped == 1 ? "y" : "ies");
            }
        }
    }
    if (!result) {
        WLOG_W(kTag, "%s; starting with no saved networks", result.error().message.c_str());
    }
    WLOG_I(kTag, "%zu saved network(s)", networks_.size());

    backoff_ms_ = config_.backoff_min_ms;
    if (!networks_.empty()) {
        schedule_attempt_locked(now_ms);
        set_state_locked(WifiState::Connecting, "", "", 0);
    } else {
        set_state_locked(WifiState::Off, "", "", 0);
    }
    return result;
}

// ---------------------------------------------------------------------------
// State machine

void WifiService::set_state_locked(WifiState state, std::string ssid, std::string ip, int rssi) {
    // The reason only describes a failed attempt, so it is only reported
    // while waiting to retry one.
    const char* reason = state == WifiState::Backoff ? fail_reason_ : "";
    bool changed = state != status_.state || ssid != status_.ssid || ip != status_.ip ||
                   std::string_view(reason) != status_.reason;
    status_.state = state;
    status_.ssid = std::move(ssid);
    status_.ip = std::move(ip);
    status_.rssi = rssi;
    status_.reason = reason;
    if (!changed) return;
    bus_.publish(core::Event{"net.wifi.state", source_, 0,
                             Properties{{"state", to_string(state)},
                                        {"ssid", status_.ssid},
                                        {"ip", status_.ip},
                                        {"rssi", status_.rssi},
                                        {"reason", reason}}});
}

void WifiService::schedule_attempt_locked(uint64_t at_ms) {
    attempt_due_ = true;
    next_attempt_ms_ = at_ms;
}

void WifiService::fail_attempt_locked(uint64_t now_ms, const char* why, const char* reason) {
    std::string ssid = in_flight_ssid_;
    fail_reason_ = reason;
    in_flight_ = false;
    in_flight_ssid_.clear();
    direct_ssid_.reset();

    uint32_t wait = backoff_ms_;
    backoff_ms_ = static_cast<uint32_t>(
        std::min<uint64_t>(static_cast<uint64_t>(backoff_ms_) * 2, config_.backoff_max_ms));
    schedule_attempt_locked(now_ms + wait);
    WLOG_W(kTag, "connect to '%s' failed: %s; retrying in %u ms", ssid.c_str(), why,
           static_cast<unsigned>(wait));
    set_state_locked(WifiState::Backoff, std::move(ssid), "", 0);
}

void WifiService::start_connect_locked(const Network& net, uint64_t now_ms) {
    attempt_due_ = false;
    in_flight_ = true;
    in_flight_ssid_ = net.ssid;
    in_flight_deadline_ms_ = now_ms + config_.connect_timeout_ms;
    WLOG_I(kTag, "connecting to '%s'", net.ssid.c_str());
    auto r = wifi_->connect(net.ssid, net.password);
    if (!r) {
        // Only the code: a driver message is not guaranteed free of the
        // credentials it was handed.
        fail_attempt_locked(now_ms, to_string(r.error().code), kReasonOther);
    }
}

void WifiService::poll_link_locked(uint64_t now_ms) {
    if (in_flight_) {
        hal::WifiLink link = wifi_->link();
        if (link.state == hal::WifiLinkState::Connected && link.ssid == in_flight_ssid_) {
            in_flight_ = false;
            in_flight_ssid_.clear();
            direct_ssid_.reset();
            backoff_ms_ = config_.backoff_min_ms;
            WLOG_I(kTag, "connected to '%s' ip=%s rssi=%d", link.ssid.c_str(), link.ip.c_str(),
                   link.rssi);
            if (auto ps = wifi_->set_power_save(true); !ps) {
                WLOG_W(kTag, "enabling modem sleep failed: %s", to_string(ps.error().code));
            }
            if (ap_ && !ap_close_at_ms_ && provisioned_ssids_.count(link.ssid) != 0) {
                ap_close_at_ms_ = now_ms + config_.ap_linger_ms;
                WLOG_I(kTag, "provisioning succeeded; hotspot closes in %u ms",
                       static_cast<unsigned>(config_.ap_linger_ms));
            }
            set_state_locked(WifiState::Connected, link.ssid, link.ip, link.rssi);
        } else if (link.state == hal::WifiLinkState::Failed &&
                   (link.ssid.empty() || link.ssid == in_flight_ssid_)) {
            // A Failed naming another SSID is left over from an earlier
            // attempt; this one's outcome (or the timeout) is still to come.
            fail_attempt_locked(now_ms, reason_text(link.reason), reason_code(link.reason));
        } else if (now_ms >= in_flight_deadline_ms_) {
            (void)wifi_->disconnect();  // stop the driver from retrying on its own
            fail_attempt_locked(now_ms, "timed out", kReasonOther);
        }
        return;
    }

    if (status_.state == WifiState::Connected) {
        hal::WifiLink link = wifi_->link();
        if (link.state == hal::WifiLinkState::Idle || link.state == hal::WifiLinkState::Failed) {
            WLOG_W(kTag, "lost '%s'; reconnecting", status_.ssid.c_str());
            schedule_attempt_locked(now_ms);
            set_state_locked(WifiState::Connecting, "", "", 0);
        } else if (link.state == hal::WifiLinkState::Connected) {
            set_state_locked(WifiState::Connected, status_.ssid, link.ip, link.rssi);
        }
    }
}

void WifiService::tick_standby_locked(uint64_t now_ms) {
    if (!standby_ || standby_off_ || now_ms < standby_since_ms_ + standby_off_after_ms_) {
        return;
    }
    standby_off_ = true;
    bool active = in_flight_ || status_.state != WifiState::Off;
    attempt_due_ = false;
    in_flight_ = false;
    in_flight_ssid_.clear();
    direct_ssid_.reset();
    if (active) {
        WLOG_I(kTag, "standby: turning Wi-Fi off");
        (void)wifi_->disconnect();
    }
    set_state_locked(WifiState::Off, "", "", 0);
}

std::optional<WifiService::AttemptPlan> WifiService::plan_attempt_locked(uint64_t now_ms) {
    if (!attempt_due_ || in_flight_ || now_ms < next_attempt_ms_) return std::nullopt;
    if (networks_.empty()) {
        attempt_due_ = false;
        set_state_locked(WifiState::Off, "", "", 0);
        return std::nullopt;
    }
    AttemptPlan plan;
    plan.generation = generation_;
    if (direct_ssid_ && find_locked(*direct_ssid_) != nullptr) {
        plan.direct_ssid = *direct_ssid_;
    } else {
        plan.scan = true;
    }
    if (status_.state != WifiState::Connecting) {
        set_state_locked(WifiState::Connecting, plan.direct_ssid, "", 0);
    }
    return plan;
}

void WifiService::finish_attempt_locked(const AttemptPlan& plan,
                                        const Result<std::vector<hal::WifiScanEntry>>* scan,
                                        uint64_t now_ms) {
    // Someone added/forgot a network or the device went to sleep while
    // mutex_ was released for the scan: drop this plan, the next tick()
    // re-plans from the current state.
    if (plan.generation != generation_ || !attempt_due_ || in_flight_) return;

    const Network* target = nullptr;
    const char* why = "no saved network in range";
    const char* reason = kReasonNotFound;
    if (!plan.direct_ssid.empty()) {
        target = find_locked(plan.direct_ssid);
    } else if (scan != nullptr && !*scan) {
        why = "scan failed";
        reason = kReasonOther;
    } else if (scan != nullptr) {
        int best_rssi = 0;
        for (const auto& entry : scan->value()) {
            const Network* saved = find_locked(entry.ssid);
            if (saved != nullptr && (target == nullptr || entry.rssi > best_rssi)) {
                target = saved;
                best_rssi = entry.rssi;
            }
        }
    }
    if (target == nullptr) {
        in_flight_ssid_ = plan.direct_ssid;
        fail_attempt_locked(now_ms, why, reason);
        return;
    }
    start_connect_locked(*target, now_ms);
}

void WifiService::tick(uint64_t now_ms) {
    std::lock_guard<os::Mutex> radio(radio_mutex_);
    std::optional<AttemptPlan> plan;
    {
        std::lock_guard<os::Mutex> lock(mutex_);
        poll_link_locked(now_ms);
        tick_standby_locked(now_ms);
        tick_ap_locked(now_ms);
        plan = plan_attempt_locked(now_ms);
    }
    if (!plan) return;

    if (plan->scan) {
        auto entries = wifi_->scan();  // blocking; mutex_ deliberately not held
        std::lock_guard<os::Mutex> lock(mutex_);
        finish_attempt_locked(*plan, &entries, now_ms);
    } else {
        std::lock_guard<os::Mutex> lock(mutex_);
        finish_attempt_locked(*plan, nullptr, now_ms);
    }
}

void WifiService::set_standby(bool standby, uint64_t now_ms) {
    std::lock_guard<os::Mutex> lock(mutex_);
    if (standby == standby_) return;
    standby_ = standby;
    if (standby) {
        int64_t minutes = settings_.get_int("idle_off_min", kDefaultIdleOffMin);
        if (minutes < 0) minutes = kDefaultIdleOffMin;
        standby_since_ms_ = now_ms;
        standby_off_after_ms_ = static_cast<uint64_t>(minutes) * 60 * 1000;
        return;
    }
    if (!standby_off_) return;
    standby_off_ = false;
    ++generation_;
    if (!networks_.empty()) {
        WLOG_I(kTag, "awake: reconnecting");
        backoff_ms_ = config_.backoff_min_ms;
        schedule_attempt_locked(now_ms);
        set_state_locked(WifiState::Connecting, "", "", 0);
    }
}

WifiStatus WifiService::status() const {
    std::lock_guard<os::Mutex> lock(mutex_);
    return status_;
}

// ---------------------------------------------------------------------------
// Networks

Result<std::vector<ScanResult>> WifiService::scan() {
    std::lock_guard<os::Mutex> radio(radio_mutex_);
    auto entries = wifi_->scan();  // blocking; mutex_ deliberately not held
    if (!entries) {
        return Result<std::vector<ScanResult>>::err(entries.error().code, entries.error().message);
    }

    std::map<std::string, hal::WifiScanEntry> best;
    for (const auto& e : entries.value()) {
        if (e.ssid.empty()) continue;  // hidden networks cannot be picked from a list
        auto it = best.find(e.ssid);
        if (it == best.end() || e.rssi > it->second.rssi) best[e.ssid] = e;
    }

    std::lock_guard<os::Mutex> lock(mutex_);
    std::vector<ScanResult> out;
    out.reserve(best.size());
    for (const auto& [ssid, e] : best) {
        out.push_back({ssid, e.rssi, e.secured, find_locked(ssid) != nullptr});
    }
    std::stable_sort(out.begin(), out.end(),
                     [](const ScanResult& a, const ScanResult& b) { return a.rssi > b.rssi; });
    return Result<std::vector<ScanResult>>::ok(std::move(out));
}

Result<void> WifiService::add_network(std::string_view ssid, std::string_view password,
                                      uint64_t now_ms) {
    WISP_TRY(validate(ssid, password));

    std::lock_guard<os::Mutex> lock(mutex_);
    std::vector<Network> updated = networks_;
    auto it = std::find_if(updated.begin(), updated.end(),
                           [&](const Network& n) { return n.ssid == ssid; });
    if (it != updated.end()) {
        it->password = std::string(password);
    } else {
        if (updated.size() >= config_.max_networks) {
            return Result<void>::err(
                ErrorCode::QuotaExceeded,
                "at most " + std::to_string(config_.max_networks) + " networks can be saved");
        }
        updated.push_back({std::string(ssid), std::string(password)});
    }
    WISP_TRY(persist_locked(updated));
    networks_ = std::move(updated);
    ++generation_;
    WLOG_I(kTag, "saved network '%.*s' (%zu saved)", static_cast<int>(ssid.size()), ssid.data(),
           networks_.size());

    if (ap_) provisioned_ssids_.emplace(ssid);
    if (standby_) {
        // A user configuring Wi-Fi counts as activity: restart the
        // standby clock instead of switching straight back off.
        standby_off_ = false;
        standby_since_ms_ = now_ms;
    }
    in_flight_ = false;
    in_flight_ssid_.clear();
    direct_ssid_ = std::string(ssid);
    backoff_ms_ = config_.backoff_min_ms;
    schedule_attempt_locked(now_ms);
    set_state_locked(WifiState::Connecting, std::string(ssid), "", 0);
    return Result<void>::ok();
}

Result<void> WifiService::forget_network(std::string_view ssid) {
    std::lock_guard<os::Mutex> lock(mutex_);
    if (find_locked(ssid) == nullptr) {
        return Result<void>::err(ErrorCode::NotFound, "network is not saved");
    }
    std::vector<Network> updated;
    for (const auto& n : networks_) {
        if (n.ssid != ssid) updated.push_back(n);
    }
    WISP_TRY(persist_locked(updated));
    networks_ = std::move(updated);
    ++generation_;
    provisioned_ssids_.erase(std::string(ssid));
    if (direct_ssid_ && *direct_ssid_ == ssid) direct_ssid_.reset();
    WLOG_I(kTag, "forgot network '%.*s'", static_cast<int>(ssid.size()), ssid.data());

    bool in_use = (status_.state == WifiState::Connected && status_.ssid == ssid) ||
                  (in_flight_ && in_flight_ssid_ == ssid);
    if (in_use) {
        (void)wifi_->disconnect();
        in_flight_ = false;
        in_flight_ssid_.clear();
    }
    if (networks_.empty()) {
        // Every attempt targets a saved network, so with none left any
        // attempt in flight was for `ssid` and was dropped just above.
        attempt_due_ = false;
        in_flight_ = false;
        in_flight_ssid_.clear();
        backoff_ms_ = config_.backoff_min_ms;
        set_state_locked(WifiState::Off, "", "", 0);
    } else if (in_use) {
        schedule_attempt_locked(0);  // due on the very next tick()
        set_state_locked(WifiState::Connecting, "", "", 0);
    }
    return Result<void>::ok();
}

std::vector<std::string> WifiService::saved_ssids() const {
    std::lock_guard<os::Mutex> lock(mutex_);
    std::vector<std::string> out;
    out.reserve(networks_.size());
    for (const auto& n : networks_) out.push_back(n.ssid);
    return out;
}

void WifiService::forget_all() {
    std::lock_guard<os::Mutex> lock(mutex_);
    ++generation_;
    networks_.clear();
    if (!persist_locked(networks_)) {
        // persist_locked() already logged the storage error; say what it
        // means: the networks come back after a reboot.
        WLOG_E(kTag, "factory reset of saved networks was not persisted");
    }
    if (ap_) (void)close_ap_locked("factory reset");
    if (in_flight_ || status_.state != WifiState::Off) (void)wifi_->disconnect();
    attempt_due_ = false;
    in_flight_ = false;
    in_flight_ssid_.clear();
    direct_ssid_.reset();
    standby_off_ = false;
    backoff_ms_ = config_.backoff_min_ms;
    WLOG_I(kTag, "all saved networks forgotten");
    set_state_locked(WifiState::Off, "", "", 0);
}

// ---------------------------------------------------------------------------
// Provisioning hotspot

void WifiService::publish_ap_locked(bool active, const std::string& ssid) {
    bus_.publish(core::Event{
        "net.ap.state", source_, 0,
        Properties{{"active", active}, {"ssid", ssid}, {"clients", active ? ap_clients_ : 0}}});
}

Result<void> WifiService::close_ap_locked(const char* why) {
    auto r = wifi_->stop_ap();
    if (!r) {
        WLOG_W(kTag, "stopping hotspot failed: %s", to_string(r.error().code));
    }
    std::string ssid = ap_ ? ap_->ssid : std::string();
    ap_.reset();
    ap_clients_ = 0;
    ap_idle_since_ms_.reset();
    ap_close_at_ms_.reset();
    provisioned_ssids_.clear();
    WLOG_I(kTag, "hotspot '%s' closed (%s)", ssid.c_str(), why);
    publish_ap_locked(false, ssid);
    return r;
}

void WifiService::tick_ap_locked(uint64_t now_ms) {
    if (!ap_) return;
    int clients = wifi_->ap_clients();
    if (clients != ap_clients_) {
        ap_clients_ = clients;
        publish_ap_locked(true, ap_->ssid);
    }
    if (clients > 0) {
        ap_idle_since_ms_.reset();
    } else if (!ap_idle_since_ms_) {
        ap_idle_since_ms_ = now_ms;
    }

    if (ap_close_at_ms_ && now_ms >= *ap_close_at_ms_) {
        (void)close_ap_locked("provisioning done");
    } else if (ap_idle_since_ms_ && now_ms >= *ap_idle_since_ms_ &&
               now_ms - *ap_idle_since_ms_ >= config_.ap_idle_timeout_ms) {
        (void)close_ap_locked("no client joined");
    }
}

Result<ApSession> WifiService::start_provisioning(uint64_t now_ms) {
    std::lock_guard<os::Mutex> lock(mutex_);
    if (ap_) return Result<ApSession>::ok(*ap_);

    const Value* ap_cap =
        wifi_->info().caps.count("ap") != 0 ? &wifi_->info().caps.at("ap") : nullptr;
    if (ap_cap != nullptr && ap_cap->type() == Value::Type::Bool && !ap_cap->as_bool()) {
        return Result<ApSession>::err(ErrorCode::Unsupported, "this radio has no access point");
    }

    auto mac = wifi_->mac();
    char ssid[16];
    std::snprintf(ssid, sizeof(ssid), "Wisp-%02X%02X", mac[4], mac[5]);

    uint8_t bytes[kApPasswordLen] = {};
    random_(bytes, sizeof(bytes));
    std::string password(kApPasswordLen, 'A');
    for (size_t i = 0; i < kApPasswordLen; ++i) {
        password[i] = kApAlphabet[bytes[i] % kApAlphabetLen];
    }

    auto ip = wifi_->start_ap(ssid, password);
    if (!ip) {
        WLOG_E(kTag, "starting hotspot failed: %s", to_string(ip.error().code));
        return Result<ApSession>::err(ip.error().code, "starting the hotspot failed");
    }

    ap_ = ApSession{ssid, std::move(password), ip.value()};
    ap_clients_ = 0;
    ap_idle_since_ms_ = now_ms;
    ap_close_at_ms_.reset();
    provisioned_ssids_.clear();
    WLOG_I(kTag, "hotspot '%s' up at %s", ssid, ap_->ip.c_str());
    publish_ap_locked(true, ap_->ssid);
    return Result<ApSession>::ok(*ap_);
}

Result<void> WifiService::stop_provisioning() {
    std::lock_guard<os::Mutex> lock(mutex_);
    if (!ap_) return Result<void>::ok();
    return close_ap_locked("stopped");
}

std::optional<ApSession> WifiService::ap_session() const {
    std::lock_guard<os::Mutex> lock(mutex_);
    return ap_;
}

ApStatus WifiService::ap_status() const {
    std::lock_guard<os::Mutex> lock(mutex_);
    if (!ap_) return ApStatus{};
    return ApStatus{true, ap_->ssid, ap_clients_};
}

}  // namespace wisp::net
