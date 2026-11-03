#pragma once

// Station connection manager and on-demand provisioning hotspot on top of
// a hal::Wifi device (spec 4.2-4.4, 5.1).
//
// Threading: every public method may be called from any thread (HTTP
// handlers, the serial console, the UI, the "wisp.net" tick task). One
// os::Mutex (`mutex_`) guards all state. The only device call that can
// block for seconds is hal::Wifi::scan(); it is never made while holding
// `mutex_`, so status()/ap_session() stay cheap for the UI even while a
// scan is in progress. A second mutex (`radio_mutex_`) serializes tick()
// bodies and public scan() calls against each other, so the driver never
// sees two overlapping scans. Lock order: radio_mutex_ before mutex_.
// Quick device calls (link, connect, disconnect, start_ap/stop_ap,
// ap_clients) are made under `mutex_` from whichever thread called in, so
// a hal::Wifi implementation must accept calls from several threads.
//
// Connection attempts (scan, connect) start only inside tick(): init()
// and add_network() merely schedule one. tick() is expected every
// 250 ms from a dedicated task; tests drive it with explicit timestamps.
//
// Secrets: Wi-Fi passwords and the hotspot password never appear in a
// log line, an event payload or an error message. The hotspot password is
// only handed out through start_provisioning()/ap_session() for the
// device's own screen.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <vector>

#include "wisp/base/result.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/core/settings.hpp"
#include "wisp/core/storage_manager.hpp"
#include "wisp/hal/wifi.hpp"
#include "wisp/os/os.hpp"

namespace wisp::net {

enum class WifiState { Off, Connecting, Backoff, Connected };

// "off" / "connecting" / "backoff" / "connected".
const char* to_string(WifiState s);

struct WifiStatus {
    WifiState state = WifiState::Off;
    std::string ssid;  // network being tried / connected; empty when none
    std::string ip;    // only in Connected
    int rssi = 0;      // only in Connected
};

struct ApSession {
    std::string ssid;
    std::string password;
    std::string ip;
};

struct ScanResult {
    std::string ssid;
    int rssi = 0;
    bool secured = true;
    bool saved = false;
};

struct WifiServiceConfig {
    uint32_t backoff_min_ms = 5000, backoff_max_ms = 300000;
    uint32_t ap_idle_timeout_ms = 600000, ap_linger_ms = 30000;
    size_t max_networks = 5;
    // An attempt that has not reached Connected this long after connect()
    // counts as a failure (the driver may otherwise sit in Connecting
    // indefinitely when the AP vanishes mid-handshake).
    uint32_t connect_timeout_ms = 15000;
};

using RandomFn = std::function<void(uint8_t* out, size_t len)>;

class WifiService {
public:
    // `settings` must be bound to namespace "net" (key "idle_off_min").
    // `random` fills the hotspot password bytes.
    WifiService(std::shared_ptr<hal::Wifi> wifi, core::StorageManager& storage, core::EventBus& bus,
                core::Settings settings, RandomFn random, WifiServiceConfig config = {});

    WifiService(const WifiService&) = delete;
    WifiService& operator=(const WifiService&) = delete;

    // Loads <System>/net/wifi.json. With at least one saved network the
    // state becomes Connecting and the first attempt runs on the next
    // tick(); never touches the radio itself, so boot never waits for
    // Wi-Fi. A missing file is not an error. A corrupt or unreadable file
    // is reported, but the service stays usable with no saved networks.
    Result<void> init(uint64_t now_ms);

    // Advances the state machine: starts due attempts (scan + connect),
    // polls the link, runs the standby and hotspot timers.
    void tick(uint64_t now_ms);

    // Mirrors power.state. After `net`/`idle_off_min` minutes of standby
    // the station is disconnected (state Off, no backoff); leaving standby
    // reconnects when networks are saved.
    void set_standby(bool standby, uint64_t now_ms);

    WifiStatus status() const;

    // Blocking device scan (up to ~3 s), deduplicated by SSID (strongest
    // entry wins) and sorted by descending RSSI. Hidden networks (empty
    // SSID) are dropped.
    Result<std::vector<ScanResult>> scan();

    // SSID 1-32 bytes; password empty (open network) or 8-63 bytes, else
    // InvalidArgument. Re-adding a saved SSID replaces its password. A
    // sixth distinct SSID fails with QuotaExceeded. On success the list is
    // persisted and the next tick() connects to exactly this network.
    Result<void> add_network(std::string_view ssid, std::string_view password, uint64_t now_ms);

    // NotFound if not saved. Forgetting the network currently in use drops
    // the link (and reconnects to another saved network, if any).
    Result<void> forget_network(std::string_view ssid);

    std::vector<std::string> saved_ssids() const;

    // Starts the hotspot "Wisp-XXXX" (last two MAC bytes, uppercase hex)
    // with a fresh 8-character [A-Za-z0-9] password. Calling it while a
    // session is already active returns that session unchanged.
    Result<ApSession> start_provisioning(uint64_t now_ms);
    // Idempotent: OK when no session is active.
    Result<void> stop_provisioning();
    // Only for the device's own screen -- carries the hotspot password.
    std::optional<ApSession> ap_session() const;

    // Factory reset: drops and persists an empty network list, closes the
    // hotspot and disconnects.
    void forget_all();

private:
    struct Network {
        std::string ssid;
        std::string password;
    };

    // What tick() decided to do with the radio, computed under mutex_ and
    // executed after releasing it (the scan) or re-checked afterwards.
    struct AttemptPlan {
        bool scan = false;
        std::string direct_ssid;  // set: connect to this one without scanning
        uint64_t generation = 0;
    };

    std::string wifi_path() const;
    Result<void> persist_locked(const std::vector<Network>& networks);
    const Network* find_locked(std::string_view ssid) const;

    void set_state_locked(WifiState state, std::string ssid, std::string ip, int rssi);
    void publish_ap_locked(bool active, const std::string& ssid);
    Result<void> close_ap_locked(const char* why);
    void schedule_attempt_locked(uint64_t at_ms);
    void fail_attempt_locked(uint64_t now_ms, const char* why, const char* reason);
    void start_connect_locked(const Network& net, uint64_t now_ms);
    void poll_link_locked(uint64_t now_ms);
    void tick_standby_locked(uint64_t now_ms);
    void tick_ap_locked(uint64_t now_ms);
    std::optional<AttemptPlan> plan_attempt_locked(uint64_t now_ms);
    void finish_attempt_locked(const AttemptPlan& plan,
                               const Result<std::vector<hal::WifiScanEntry>>* scan,
                               uint64_t now_ms);

    std::shared_ptr<hal::Wifi> wifi_;
    core::StorageManager& storage_;
    core::EventBus& bus_;
    core::Settings settings_;
    RandomFn random_;
    WifiServiceConfig config_;
    std::string source_;  // event source: the device id

    os::Mutex radio_mutex_;  // see file header
    mutable os::Mutex mutex_;

    std::vector<Network> networks_;
    WifiStatus status_;

    // Bumped whenever the connection plan changes from outside tick()
    // (add/forget/standby/factory reset), so a tick() that released
    // mutex_ for a scan can tell its plan went stale.
    uint64_t generation_ = 0;

    bool attempt_due_ = false;  // Connecting/Backoff: an attempt is scheduled
    uint64_t next_attempt_ms_ = 0;
    uint32_t backoff_ms_ = 0;  // wait applied after the next failure
    // Cause of the last failed attempt ("not_found"/"auth_failed"/"other"),
    // published as net.wifi.state's "reason" while in Backoff.
    const char* fail_reason_ = "";
    std::string published_reason_;            // "reason" of the last published event
    std::optional<std::string> direct_ssid_;  // next attempt skips the scan

    bool in_flight_ = false;  // connect() issued, waiting for the link
    std::string in_flight_ssid_;
    uint64_t in_flight_deadline_ms_ = 0;

    bool standby_ = false;
    bool standby_off_ = false;  // disconnected because of standby
    uint64_t standby_since_ms_ = 0;
    uint64_t standby_off_after_ms_ = 0;

    std::optional<ApSession> ap_;
    int ap_clients_ = 0;
    std::optional<uint64_t> ap_idle_since_ms_;
    std::optional<uint64_t> ap_close_at_ms_;
    // SSIDs added through add_network() while the hotspot is up; only one
    // of these connecting means provisioning succeeded.
    std::set<std::string> provisioned_ssids_;
};

}  // namespace wisp::net
