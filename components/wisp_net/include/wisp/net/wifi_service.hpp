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
// and add_network() merely schedule one, and none starts while deep
// standby has the radio off (set_power_level()). tick() is expected every
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
#include <utility>
#include <vector>

#include "wisp/base/result.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/core/latest_save.hpp"
#include "wisp/core/storage_manager.hpp"
#include "wisp/hal/device.hpp"
#include "wisp/hal/wifi.hpp"
#include "wisp/net/random.hpp"
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
    // Why the last attempt failed ("not_found"/"auth_failed"/"other");
    // only set in Backoff, "" otherwise. Same value as net.wifi.state's
    // "reason". When every saved network is auth_failed (SavedNetwork),
    // the backoff reports "auth_failed" with `ssid` naming one of them:
    // no automatic attempt can succeed until new credentials arrive.
    std::string reason;
    // When the next attempt is due (now_ms timebase); only set in Backoff,
    // 0 otherwise. The device screen counts down to it.
    uint64_t retry_at_ms = 0;
};

struct ApSession {
    std::string ssid;
    std::string password;
    std::string ip;
};

// The hotspot as the admin page may see it: no password.
struct ApStatus {
    bool active = false;
    std::string ssid;
    int clients = 0;
};

struct ScanResult {
    std::string ssid;
    int rssi = 0;
    bool secured = true;
    bool saved = false;
};

// A saved network as callers may see it: no password.
struct SavedNetwork {
    std::string ssid;
    // This network rejected its password on three attempts in a row (with
    // no successful connect in between). Such a network is left out of
    // automatic selection until add_network() gives it credentials again,
    // so a wrong password on a stronger network cannot keep the device
    // away from one that works (spec 4.2). One rejection is not enough: a
    // lost handshake frame looks the same.
    bool auth_failed = false;
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

class WifiService {
public:
    // `random` fills the hotspot password bytes. `submit_io` queues the
    // writes of wifi.json on the flash thread (core::LatestSave); empty:
    // every change is written on the calling thread.
    WifiService(std::shared_ptr<hal::Wifi> wifi, core::StorageManager& storage, core::EventBus& bus,
                RandomFn random, WifiServiceConfig config = {},
                core::LatestSave::SubmitIo submit_io = {});

    WifiService(const WifiService&) = delete;
    WifiService& operator=(const WifiService&) = delete;

    // Loads <System>/net/wifi.json. With at least one saved network the
    // state becomes Connecting and the first attempt runs on the next
    // tick(); never touches the radio itself, so boot never waits for
    // Wi-Fi. A missing file is not an error. A corrupt or unreadable file
    // is reported, but the service stays usable with no saved networks.
    Result<void> init(uint64_t now_ms);

    // Advances the state machine: starts due attempts (scan + connect),
    // polls the link, runs the hotspot timers.
    void tick(uint64_t now_ms);

    // Follows PowerManager's level (scene-device spec 4.1). DeepStandby
    // closes the hotspot, disconnects and turns the radio off
    // (hal::Wifi::set_radio(false)): state Off, nothing retried. Any other
    // level turns the radio back on and, with networks saved, reconnects at
    // once. Standby keeps the station as it is. No-op when the level does
    // not change that.
    void set_power_level(hal::PowerLevel level, uint64_t now_ms);

    WifiStatus status() const;

    // Blocking device scan (up to ~3 s), deduplicated by SSID (strongest
    // entry wins) and sorted by descending RSSI. Hidden networks (empty
    // SSID) are dropped.
    Result<std::vector<ScanResult>> scan();

    // SSID 1-32 bytes; password empty (open network) or 8-63 bytes, else
    // InvalidArgument. Re-adding a saved SSID replaces its password and
    // clears its auth_failed flag and its count of rejections. A sixth distinct SSID fails with
    // QuotaExceeded. On success the list is persisted and the next tick()
    // connects to exactly this network (in deep standby, the first tick()
    // after the radio is back on).
    Result<void> add_network(std::string_view ssid, std::string_view password, uint64_t now_ms);

    // NotFound if not saved. Forgetting the network currently in use drops
    // the link (and reconnects to another saved network, if any).
    Result<void> forget_network(std::string_view ssid);

    std::vector<std::string> saved_ssids() const;
    // Saved networks in the order they were first added.
    std::vector<SavedNetwork> saved_networks() const;

    // Starts the hotspot "Wisp-XXXX" (last two MAC bytes, uppercase hex)
    // with a fresh 8-character [A-Za-z0-9] password. Calling it while a
    // session is already active returns that session unchanged; Busy
    // while deep standby has the radio off.
    Result<ApSession> start_provisioning(uint64_t now_ms);
    // Idempotent: OK when no session is active.
    Result<void> stop_provisioning();
    // Only for the device's own screen -- carries the hotspot password.
    std::optional<ApSession> ap_session() const;
    // Password-free view of the hotspot, for remote callers (admin API).
    ApStatus ap_status() const;

    // Called once after LatestSave::kFailuresToReport failed writes in a
    // row (see core::LatestSave). Set it before the first change.
    void set_on_persist_failed(std::function<void()> fn) {
        saver_.set_on_persist_failed(std::move(fn));
    }

    // Factory reset: drops and persists an empty network list, closes the
    // hotspot and disconnects.
    void forget_all();

private:
    struct Network {
        std::string ssid;
        std::string password;
        bool auth_failed = false;  // see SavedNetwork::auth_failed
        // Rejections since the last successful connect or add_network();
        // memory only, so a reboot starts the count over.
        uint8_t rejections = 0;
    };

    // What tick() decided to do with the radio, computed under mutex_ and
    // executed after releasing it (the scan) or re-checked afterwards.
    struct AttemptPlan {
        bool scan = false;
        std::string direct_ssid;  // set: connect to this one without scanning
        uint64_t generation = 0;
    };

    std::string wifi_path() const;
    // Saves `networks` as wifi.json (see core::LatestSave): Ok once the
    // write is queued, or, without submit_io, once it is done. `lost`,
    // when set, is logged as well if the write fails.
    Result<void> persist_locked(const std::vector<Network>& networks, const char* lost = nullptr);
    const Network* find_locked(std::string_view ssid) const;
    // Counts a rejected password; the third in a row flags the network
    // auth_failed and persists the flag. No-op for unsaved or already
    // flagged networks.
    void note_rejection_locked(const std::string& ssid);

    void set_state_locked(WifiState state, std::string ssid, std::string ip, int rssi);
    void publish_ap_locked(bool active, const std::string& ssid);
    Result<void> close_ap_locked(const char* why);
    void schedule_attempt_locked(uint64_t at_ms);
    void fail_attempt_locked(uint64_t now_ms, const char* why, const char* reason);
    void start_connect_locked(const Network& net, uint64_t now_ms);
    void poll_link_locked(uint64_t now_ms);
    void tick_ap_locked(uint64_t now_ms);
    std::optional<AttemptPlan> plan_attempt_locked(uint64_t now_ms);
    void finish_attempt_locked(const AttemptPlan& plan,
                               const Result<std::vector<hal::WifiScanEntry>>* scan,
                               uint64_t now_ms);

    std::shared_ptr<hal::Wifi> wifi_;
    core::StorageManager& storage_;
    core::EventBus& bus_;
    RandomFn random_;
    WifiServiceConfig config_;
    std::string source_;  // event source: the device id
    core::LatestSave saver_;

    os::Mutex radio_mutex_;  // see file header
    mutable os::Mutex mutex_;

    std::vector<Network> networks_;
    WifiStatus status_;

    // Bumped whenever the connection plan changes from outside tick()
    // (add/forget/power level/factory reset), so a tick() that released
    // mutex_ for a scan can tell its plan went stale.
    uint64_t generation_ = 0;

    bool attempt_due_ = false;  // Connecting/Backoff: an attempt is scheduled
    uint64_t next_attempt_ms_ = 0;
    uint32_t backoff_ms_ = 0;  // wait applied after the next failure
    // Cause of the last failed attempt ("not_found"/"auth_failed"/"other"),
    // published as net.wifi.state's "reason" (and WifiStatus::reason)
    // while in Backoff.
    const char* fail_reason_ = "";
    std::optional<std::string> direct_ssid_;  // next attempt skips the scan

    bool in_flight_ = false;  // connect() issued, waiting for the link
    std::string in_flight_ssid_;
    uint64_t in_flight_deadline_ms_ = 0;

    // Deep standby turned the radio off (set_power_level()).
    bool radio_off_ = false;

    std::optional<ApSession> ap_;
    int ap_clients_ = 0;
    std::optional<uint64_t> ap_idle_since_ms_;
    std::optional<uint64_t> ap_close_at_ms_;
    // SSIDs added through add_network() while the hotspot is up; only one
    // of these connecting means provisioning succeeded.
    std::set<std::string> provisioned_ssids_;
};

}  // namespace wisp::net
