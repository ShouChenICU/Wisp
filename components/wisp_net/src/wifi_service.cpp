#include "wisp/net/wifi_service.hpp"

#include <algorithm>
#include <cstdio>
#include <map>
#include <mutex>
#include <span>
#include <utility>

#include "wisp/base/error.hpp"
#include "wisp/base/json.hpp"
#include "wisp/base/sha256.hpp"
#include "wisp/base/value.hpp"
#include "wisp/core/log.hpp"
#include "wisp/os/large.hpp"

namespace wisp::net {

namespace {

constexpr const char* kTag = "wifi";
constexpr const char* kWifiFile = "/net/wifi.json";
constexpr size_t kMaxSsidBytes = 32;
constexpr size_t kMinPasswordBytes = 8;
constexpr size_t kMaxPasswordBytes = 63;
constexpr size_t kApPasswordLen = 8;
// A single rejected handshake can be a lost EAPOL frame (router reboot,
// busy channel), so a network is parked only after this many rejections
// in a row (spec 4.2).
constexpr uint8_t kRejectionsToPark = 3;
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
                         core::EventBus& bus, RandomFn random, WifiServiceConfig config,
                         core::LatestSave::SubmitIo submit_io)
    : wifi_(std::move(wifi)),
      storage_(storage),
      bus_(bus),
      random_(std::move(random)),
      config_(config),
      source_(wifi_->info().id),
      saver_(std::move(submit_io)),
      backoff_ms_(config.backoff_min_ms) {}

// ---------------------------------------------------------------------------
// Persistence

std::string WifiService::wifi_path() const {
    auto root = storage_.write_root(core::Area::System);
    return root ? root.value() + kWifiFile : std::string();
}

Result<void> WifiService::persist_locked(const std::vector<Network>& networks, const char* lost) {
    List list;
    for (const auto& n : networks) {
        Map entry{{"ssid", n.ssid}, {"password", n.password}};
        if (n.auth_failed) entry.emplace("auth_failed", true);
        list.push_back(std::move(entry));
    }
    const std::string json = to_json_raw(Map{{"networks", std::move(list)}, {"enabled", enabled_}});
    // The write runs on the flash thread, possibly after this service is
    // gone: it owns its snapshot and finds the path itself.
    return saver_.save([&storage = storage_, snapshot = os::LargeString(json.begin(), json.end()),
                        lost]() -> Result<void> {
        auto root = storage.write_root(core::Area::System);
        Result<void> r =
            root ? storage.write_atomic(
                       root.value() + kWifiFile,
                       std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(snapshot.data()),
                                                snapshot.size()))
                 : Result<void>::err(ErrorCode::StorageUnavailable, "system storage unavailable");
        if (!r) {
            WLOG_E(kTag, "saving networks failed: %s", r.error().message.c_str());
            if (lost != nullptr) WLOG_E(kTag, "%s", lost);
        }
        return r;
    });
}

void WifiService::note_rejection_locked(const std::string& ssid) {
    auto it = std::find_if(networks_.begin(), networks_.end(),
                           [&](const Network& n) { return n.ssid == ssid; });
    if (it == networks_.end() || it->auth_failed) return;
    if (it->rejections < kRejectionsToPark) ++it->rejections;
    if (it->rejections < kRejectionsToPark) {
        WLOG_W(kTag, "'%s' rejected the password (%u of %u before giving up on it)", ssid.c_str(),
               static_cast<unsigned>(it->rejections), static_cast<unsigned>(kRejectionsToPark));
        return;
    }
    it->auth_failed = true;
    WLOG_W(kTag,
           "'%s' rejected the password %u times in a row; not retrying it until it is "
           "re-entered",
           ssid.c_str(), static_cast<unsigned>(kRejectionsToPark));
    // On a failed write the flag still holds until reboot; the write logs
    // the storage error.
    (void)persist_locked(networks_);
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
    enabled_ = true;
    manually_disconnected_ = false;

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
        const Value* enabled = doc ? doc.value().find("enabled") : nullptr;
        if (enabled && enabled->type() == Value::Type::Bool) enabled_ = enabled->as_bool();
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
                // A missing or non-boolean flag means "not rejected": the
                // worst case is one more attempt with the stored password.
                const Value* rejected = entry.find("auth_failed");
                bool auth_failed = rejected != nullptr && rejected->type() == Value::Type::Bool &&
                                   rejected->as_bool();
                networks_.push_back({ssid->as_string(), pw->as_string(), auth_failed});
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
    if (!enabled_) apply_radio_locked(now_ms);
    if (!radio_off_ && !networks_.empty()) {
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
    if (state != WifiState::Connected) status_.details = {};
    status_.state = state;
    status_.ssid = std::move(ssid);
    status_.ip = std::move(ip);
    status_.rssi = rssi;
    status_.reason = reason;
    if (!changed) return;
    const auto* ssid_bytes = reinterpret_cast<const uint8_t*>(status_.ssid.data());
    bus_.publish(core::Event{"net.wifi.state", source_, 0,
                             Properties{{"state", to_string(state)},
                                        {"ssid", status_.ssid},
                                        {"ssid_hex", to_hex({ssid_bytes, status_.ssid.size()})},
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
            for (auto& n : networks_) {
                if (n.ssid == link.ssid) n.rejections = 0;
            }
            WLOG_I(kTag, "connected to '%s' ip=%s rssi=%d", link.ssid.c_str(), link.ip.c_str(),
                   link.rssi);
            if (auto ps = wifi_->set_power_save(true); !ps) {
                WLOG_W(kTag, "enabling modem sleep failed: %s", to_string(ps.error().code));
            }
            if (ap_ && !ap_close_at_ms_ && provisioned_ssids_.count(link.ssid) != 0) {
                // The upper bound; tick_ap_locked() and note_result_seen()
                // close it sooner.
                ap_close_at_ms_ = now_ms + config_.ap_linger_ms;
                provisioned_ = ProvisioningResult{ap_->id, link.ssid, link.ip};
                WLOG_I(kTag, "provisioning succeeded; hotspot closes in at most %u ms",
                       static_cast<unsigned>(config_.ap_linger_ms));
            }
            status_.details = link.details;
            set_state_locked(WifiState::Connected, link.ssid, link.ip, link.rssi);
        } else if (link.state == hal::WifiLinkState::Failed &&
                   (link.ssid.empty() || link.ssid == in_flight_ssid_)) {
            // A Failed naming another SSID is left over from an earlier
            // attempt; this one's outcome (or the timeout) is still to come.
            if (link.reason == hal::WifiFailReason::AuthFailed) {
                note_rejection_locked(in_flight_ssid_);
            }
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
            status_.details = link.details;
            set_state_locked(WifiState::Connected, status_.ssid, link.ip, link.rssi);
        }
    }
}

std::optional<WifiService::AttemptPlan> WifiService::plan_attempt_locked(uint64_t now_ms) {
    if (radio_off_ || !attempt_due_ || in_flight_ || now_ms < next_attempt_ms_) {
        return std::nullopt;
    }
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
            if (saved != nullptr && !saved->auth_failed &&
                (target == nullptr || entry.rssi > best_rssi)) {
                target = saved;
                best_rssi = entry.rssi;
            }
        }
    }
    if (target == nullptr && scan != nullptr && *scan && !networks_.empty() &&
        std::all_of(networks_.begin(), networks_.end(),
                    [](const Network& n) { return n.auth_failed; })) {
        // Only new credentials can help, so say so ("auth_failed") instead
        // of "not_found", naming the strongest rejected network in range
        // (else the first saved one) for the screen and the admin page.
        why = "every saved network rejected its password";
        reason = kReasonAuthFailed;
        std::string named = networks_.front().ssid;
        int best_rssi = 0;
        bool found = false;
        for (const auto& entry : scan->value()) {
            if (find_locked(entry.ssid) != nullptr && (!found || entry.rssi > best_rssi)) {
                named = entry.ssid;
                best_rssi = entry.rssi;
                found = true;
            }
        }
        in_flight_ssid_ = std::move(named);
        fail_attempt_locked(now_ms, why, reason);
        return;
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

void WifiService::set_power_level(hal::PowerLevel level, uint64_t now_ms) {
    // radio_mutex_ first: the radio must not go off under a scan.
    std::lock_guard<os::Mutex> radio(radio_mutex_);
    std::lock_guard<os::Mutex> lock(mutex_);
    power_off_ = level == hal::PowerLevel::DeepStandby;
    apply_radio_locked(now_ms);
}

bool WifiService::enabled() const {
    std::lock_guard<os::Mutex> lock(mutex_);
    return enabled_;
}

Result<void> WifiService::set_enabled(bool enabled, uint64_t now_ms) {
    std::lock_guard<os::Mutex> radio(radio_mutex_);
    std::lock_guard<os::Mutex> lock(mutex_);
    if (enabled_ == enabled) return Result<void>::ok();
    enabled_ = enabled;
    auto result = persist_locked(networks_);
    if (!result) {
        enabled_ = !enabled;
        return result;
    }
    manually_disconnected_ = false;
    apply_radio_locked(now_ms);
    bus_.publish(core::Event{"net.wifi.enabled", source_, 0, Properties{{"enabled", enabled_}}});
    return Result<void>::ok();
}

void WifiService::apply_radio_locked(uint64_t now_ms) {
    const bool off = !enabled_ || power_off_;
    if (off == radio_off_) return;
    radio_off_ = off;
    ++generation_;
    if (off) {
        if (ap_) (void)close_ap_locked("radio disabled");
        const bool active = in_flight_ || status_.state != WifiState::Off;
        attempt_due_ = false;
        in_flight_ = false;
        in_flight_ssid_.clear();
        direct_ssid_.reset();
        if (active) (void)wifi_->disconnect();
        if (auto r = wifi_->set_radio(false); !r && r.error().code != ErrorCode::Unsupported) {
            WLOG_W(kTag, "turning the radio off failed: %s", to_string(r.error().code));
        }
        WLOG_I(kTag, "Wi-Fi radio off");
        set_state_locked(WifiState::Off, "", "", 0);
        return;
    }
    if (auto r = wifi_->set_radio(true); !r && r.error().code != ErrorCode::Unsupported) {
        WLOG_W(kTag, "turning the radio on failed: %s", to_string(r.error().code));
    }
    if (!networks_.empty() && !manually_disconnected_) {
        WLOG_I(kTag, "awake: reconnecting");
        backoff_ms_ = config_.backoff_min_ms;
        schedule_attempt_locked(now_ms);
        set_state_locked(WifiState::Connecting, direct_ssid_.value_or(""), "", 0);
    }
}

WifiStatus WifiService::status() const {
    std::lock_guard<os::Mutex> lock(mutex_);
    WifiStatus s = status_;
    if (s.state == WifiState::Backoff && attempt_due_) s.retry_at_ms = next_attempt_ms_;
    return s;
}

// ---------------------------------------------------------------------------
// Networks

Result<std::vector<ScanResult>> WifiService::scan() {
    std::lock_guard<os::Mutex> radio(radio_mutex_);
    {
        std::lock_guard<os::Mutex> lock(mutex_);
        if (radio_off_) return Result<std::vector<ScanResult>>::err(ErrorCode::Busy, "Wi-Fi off");
    }
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
        out.push_back({ssid, e.rssi, e.secured, find_locked(ssid) != nullptr, e.details});
    }
    std::stable_sort(out.begin(), out.end(),
                     [](const ScanResult& a, const ScanResult& b) { return a.rssi > b.rssi; });
    return Result<std::vector<ScanResult>>::ok(std::move(out));
}

Result<void> WifiService::add_network(std::string_view ssid, std::string_view password,
                                      uint64_t now_ms) {
    WISP_TRY(validate(ssid, password));

    std::lock_guard<os::Mutex> lock(mutex_);
    if (!enabled_) return Result<void>::err(ErrorCode::Busy, "Wi-Fi off");
    std::vector<Network> updated = networks_;
    auto it = std::find_if(updated.begin(), updated.end(),
                           [&](const Network& n) { return n.ssid == ssid; });
    if (it != updated.end()) {
        it->password = std::string(password);
        it->auth_failed = false;
        it->rejections = 0;
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

    try_directly_locked(ssid, now_ms);
    return Result<void>::ok();
}

Result<void> WifiService::connect_saved(std::string_view ssid, uint64_t now_ms) {
    std::lock_guard<os::Mutex> lock(mutex_);
    if (!enabled_) return Result<void>::err(ErrorCode::Busy, "Wi-Fi off");
    if (find_locked(ssid) == nullptr) {
        return Result<void>::err(ErrorCode::NotFound, "network not saved");
    }
    ++generation_;
    WLOG_I(kTag, "connecting to saved network '%.*s' on request", static_cast<int>(ssid.size()),
           ssid.data());
    try_directly_locked(ssid, now_ms);
    return Result<void>::ok();
}

Result<void> WifiService::disconnect() {
    std::lock_guard<os::Mutex> radio(radio_mutex_);
    std::lock_guard<os::Mutex> lock(mutex_);
    WISP_TRY(wifi_->disconnect());
    ++generation_;
    manually_disconnected_ = true;
    attempt_due_ = false;
    in_flight_ = false;
    in_flight_ssid_.clear();
    direct_ssid_.reset();
    set_state_locked(WifiState::Off, "", "", 0);
    return Result<void>::ok();
}

void WifiService::try_directly_locked(std::string_view ssid, uint64_t now_ms) {
    manually_disconnected_ = false;
    if (ap_) provisioned_ssids_.emplace(ssid);
    in_flight_ = false;
    in_flight_ssid_.clear();
    // A direct attempt connects to this network whatever its auth_failed
    // mark (finish_attempt_locked() only skips marked ones when scanning).
    direct_ssid_ = std::string(ssid);
    backoff_ms_ = config_.backoff_min_ms;
    // With the radio off the attempt waits for set_power_level() to turn
    // it back on, which connects to this network first.
    if (radio_off_) return;
    schedule_attempt_locked(now_ms);
    set_state_locked(WifiState::Connecting, std::string(ssid), "", 0);
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
    } else if (status_.ssid == ssid) {
        // A backoff (or a planned attempt) about the forgotten network:
        // keep the schedule but stop naming it.
        set_state_locked(status_.state, "", status_.ip, status_.rssi);
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

std::vector<SavedNetwork> WifiService::saved_networks() const {
    std::lock_guard<os::Mutex> lock(mutex_);
    std::vector<SavedNetwork> out;
    out.reserve(networks_.size());
    for (const auto& n : networks_) out.push_back({n.ssid, n.auth_failed, !n.password.empty()});
    return out;
}

void WifiService::forget_all() {
    std::lock_guard<os::Mutex> radio_lock(radio_mutex_);
    std::lock_guard<os::Mutex> lock(mutex_);
    ++generation_;
    networks_.clear();
    enabled_ = true;
    manually_disconnected_ = false;
    apply_radio_locked(os::now_ms());
    // On a failed write the networks come back after a reboot; the write
    // says so.
    (void)persist_locked(networks_, "factory reset of saved networks was not persisted");
    if (ap_) (void)close_ap_locked("factory reset");
    if (in_flight_ || status_.state != WifiState::Off) (void)wifi_->disconnect();
    attempt_due_ = false;
    in_flight_ = false;
    in_flight_ssid_.clear();
    direct_ssid_.reset();
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

    // After success, a hotspot nobody has been on for ap_empty_linger_ms
    // has no page waiting for the result over it. Shorter gaps are the
    // phone being dropped by the channel switch (see WifiServiceConfig).
    const bool empty_long_enough = ap_idle_since_ms_ && now_ms >= *ap_idle_since_ms_ &&
                                   now_ms - *ap_idle_since_ms_ >= config_.ap_empty_linger_ms;
    if (ap_close_at_ms_ && (now_ms >= *ap_close_at_ms_ || empty_long_enough)) {
        (void)close_ap_locked("provisioning done");
    } else if (ap_idle_since_ms_ && now_ms >= *ap_idle_since_ms_ &&
               now_ms - *ap_idle_since_ms_ >= config_.ap_idle_timeout_ms) {
        (void)close_ap_locked("no client joined");
    }
}

Result<ApSession> WifiService::start_provisioning(uint64_t now_ms) {
    std::lock_guard<os::Mutex> lock(mutex_);
    if (ap_) return Result<ApSession>::ok(*ap_);
    if (radio_off_) {
        return Result<ApSession>::err(ErrorCode::Busy, "Wi-Fi is off in deep standby");
    }

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

    ap_ = ApSession{ssid, std::move(password), ip.value(), ++last_ap_id_};
    ap_clients_ = 0;
    ap_idle_since_ms_ = now_ms;
    ap_close_at_ms_.reset();
    provisioned_.reset();
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

void WifiService::note_result_seen(std::string_view ssid, uint64_t now_ms) {
    std::lock_guard<os::Mutex> lock(mutex_);
    if (!ap_ || !ap_close_at_ms_ || !provisioned_ || provisioned_->session != ap_->id ||
        provisioned_->ssid != ssid) {
        return;
    }
    const uint64_t at = now_ms + config_.ap_seen_linger_ms;
    if (at < *ap_close_at_ms_) {
        ap_close_at_ms_ = at;
        WLOG_I(kTag, "setup page has the result; hotspot closes in %u ms",
               static_cast<unsigned>(config_.ap_seen_linger_ms));
    }
}

std::optional<ProvisioningResult> WifiService::provisioning_result() const {
    std::lock_guard<os::Mutex> lock(mutex_);
    return provisioned_;
}

ApStatus WifiService::ap_status() const {
    std::lock_guard<os::Mutex> lock(mutex_);
    if (!ap_) return ApStatus{};
    return ApStatus{true, ap_->ssid, ap_clients_};
}

}  // namespace wisp::net
