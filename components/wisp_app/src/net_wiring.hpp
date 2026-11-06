#pragma once

// The networking half of App's boot (connectivity spec 4.1; app.hpp step
// 4b), kept apart from app.cpp so App::Impl only holds one object for it.
//
// start() runs on App::start()'s thread, after PowerManager exists:
//   - builds PairingService (and, with a "net.wifi" device, WifiService)
//     and init()s them through run_io -- WifiService::init() only loads
//     the saved list, so nothing here touches the radio;
//   - subscribes to power.state, net.wifi.state and net.ap.state, only to
//     wake the task: each tick it applies PowerManager::level() to
//     WifiService::set_power_level() and reads which interfaces are up from
//     WifiService, so a dropped or late event cannot leave the radio or the
//     servers in the wrong state;
//   - with the scenes' SceneHttp, builds the NetworkWindow (scene-device
//     spec 4.4) that the task ticks with the same level before the Wi-Fi
//     service, so requests start waiting before the radio goes off; the
//     system.time event tells the window a sync attempt has finished;
//   - system wake locks (scene-device spec 4.3): the task holds
//     "system.hotspot" (network) while the setup hotspot is up, and the
//     "wisp.ntp" worker "system.ntp" (network) for each sync attempt;
//     AdminDeps::wakelocks takes the admin page's and the uploads' locks;
//   - with Platform::http_server, builds the AdminServer and, when
//     http_always_on, starts serving right away. Platform::captive_server
//     (if any) runs AdminServer::captive_handler() on port 80 exactly
//     while the hotspot is up;
//   - with Platform::sntp and a WifiService, builds the TimeSyncService
//     (connectivity spec 5.6) and its "wisp.ntp" worker task;
//   - spawns the "wisp.net" task.
//
// The "wisp.net" task is the only caller of WifiService::tick(),
// PairingService::tick() and FirmwareUpdateService::tick() (every
// kNetTickMs) and the only thread that
// starts or stops the HTTP servers once start() has returned. It has its
// own task rather than an os::Timer because tick() may block for a ~3 s
// scan, and on ESP every Timer shares one dispatch task. Event handlers
// only wake the task, so no blocking server call ever runs on the
// EventBus dispatcher. A server whose start() failed is retried with a
// backoff (kStartRetryMinMs doubling up to kStartRetryMaxMs) while its
// interface stays up.
//
// The "wisp.net" task also ticks the TimeSyncService with whether the
// station has an IP; the SNTP queries an attempt makes (seconds each when
// a server is unreachable) run on the "wisp.ntp" task so they never hold
// up a Wi-Fi tick. "wisp.ntp" never touches flash: tick() reads the server
// list on "wisp.net", and the worker only does sockets, settimeofday(),
// the RTC's I2C write, logging, a bus publish and its wake lock (which,
// with a UI, only queues PowerManager's reaction to the UI task; see
// PowerManager::schedule_lock_eval() for headless).
//
// The "wisp.net" task also runs AdminServer::tick() (the admin WebSocket
// keepalive and the REST requests' wake lock).
//
// stop() joins the task, stops both servers (their on_close callbacks run
// while they join connection threads; nothing here holds a lock then),
// unsubscribes and destroys the AdminServer before the services it
// routes to. App calls it before tearing down PowerManager.

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "wisp/admin/admin_server.hpp"
#include "wisp/app/app.hpp"
#include "wisp/app/power_manager.hpp"
#include "wisp/core/boot_report.hpp"
#include "wisp/core/device_registry.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/core/firmware_update.hpp"
#include "wisp/core/latest_save.hpp"
#include "wisp/core/model_profiles.hpp"
#include "wisp/core/occupancy.hpp"
#include "wisp/core/power_monitor.hpp"
#include "wisp/core/scene_files.hpp"
#include "wisp/core/scene_host.hpp"
#include "wisp/core/scene_repository.hpp"
#include "wisp/core/storage_manager.hpp"
#include "wisp/core/storage_worker.hpp"
#include "wisp/core/system_settings.hpp"
#include "wisp/core/wakelock.hpp"
#include "wisp/net/asset_pack.hpp"
#include "wisp/net/network_window.hpp"
#include "wisp/net/pairing_service.hpp"
#include "wisp/net/scene_http.hpp"
#include "wisp/net/time_sync.hpp"
#include "wisp/net/wifi_service.hpp"
#include "wisp/os/os.hpp"

namespace wisp::app {

struct NetWiringDeps {
    Platform& platform;
    core::DeviceRegistry& registry;
    core::BootReport& report;
    core::EventBus& bus;
    core::StorageManager& storage;
    core::ModelProfileService& models;
    core::SystemSettings& system_settings;
    // GET /device reports its last battery sample; null: battery unknown.
    const core::PowerMonitor* power_monitor = nullptr;
    // The power level, applied to WifiService and the NetworkWindow every
    // tick; null: always Active.
    const PowerManager* power_manager = nullptr;
    // The system locks' registry (see the file header); null: no locks are
    // taken, so deep standby can turn Wi-Fi off under anything.
    core::WakeLockRegistry* wakelocks = nullptr;
    // The scenes' proxy, whose requests wait in deep standby; null: no
    // network window.
    net::SceneHttp* scene_http = nullptr;
    // Shows the factory-reset confirmation on the device (AdminDeps::
    // request_factory_reset). Empty when there is no screen to confirm on:
    // the route then answers 503.
    std::function<void()> request_factory_reset;
    // The firmware upload's service (AdminDeps::firmware); ticked by the
    // "wisp.net" task so an unanswered update times out. Null: no upload.
    core::FirmwareUpdateService* firmware = nullptr;
    // AdminDeps::scenes / scene_host / request_scene_delete (see there).
    core::SceneRepository* scenes = nullptr;
    core::SceneHost* scene_host = nullptr;
    std::function<Result<void>(const std::string& id, bool with_data)> request_scene_delete;
    // AdminDeps::occupancy / files (see there); null: their routes answer
    // 503.
    core::OccupancyRegistry* occupancy = nullptr;
    core::SceneFiles* files = nullptr;
    // AdminDeps::worker (see there).
    core::StorageWorker* worker = nullptr;
    // AdminDeps::agent (see there); null: the /agent routes answer 503.
    admin::AgentApi* agent = nullptr;
    // The clock the "wisp.net" loop hands WifiService, the NetworkWindow
    // and the admin services; empty: os::now_ms. Tests move it to reach
    // the network window's 15 minutes.
    std::function<uint64_t()> now_ms;
    // Queues the Wi-Fi and pairing file writes on the flash thread (their
    // submit_io); empty: they write on the calling thread.
    core::LatestSave::SubmitIo submit_io;
    // Their set_on_persist_failed(); empty: failed writes are only logged.
    std::function<void()> on_persist_failed;
    // Runs their init() (which reads flash) on the flash thread and waits;
    // empty: on start()'s thread.
    std::function<Result<void>(std::function<void()>)> run_io;
    // AdminDeps::reboot; empty: Platform::reboot.
    std::function<void()> reboot;
};

class NetWiring {
public:
    static constexpr uint32_t kNetTickMs = 250;
    // Where phones' captive-portal probes go.
    static constexpr uint16_t kCaptivePort = 80;
    static constexpr uint32_t kStartRetryMinMs = 2000;
    static constexpr uint32_t kStartRetryMaxMs = 60000;
    // The "wisp.ntp" worker's stack: a name lookup, one UDP exchange, a
    // log line and a publish.
    static constexpr size_t kNtpStackBytes = 4096;

    explicit NetWiring(NetWiringDeps deps);
    // Calls stop().
    ~NetWiring();

    NetWiring(const NetWiring&) = delete;
    NetWiring& operator=(const NetWiring&) = delete;

    // See the file header. Records the "net" boot-report note.
    void start();
    // Idempotent.
    void stop();

    // Waits up to timeout_ms for the admin WebSocket sender to put every
    // queued message on the wire (AdminServer::flush_ws()); true at once
    // without an admin server. Not concurrently with stop().
    bool flush_admin_ws(uint32_t timeout_ms);

    net::WifiService* wifi() const { return wifi_.get(); }
    // The Wi-Fi state as of the "wisp.net" task's last tick (nullopt
    // without a WifiService): one atomic load, never a lock, for readers
    // that must not wait (the scene task's wisp.device.info()).
    std::optional<net::WifiState> wifi_state() const;
    net::PairingService* pairing() const { return pairing_.get(); }
    // The admin WebSocket's hub (the agent's frames go there); null without
    // an admin server. Lives until stop().
    admin::WsHub* ws_hub() const { return admin_ ? &admin_->ws_hub() : nullptr; }
    net::TimeSyncService* time_sync() const { return time_sync_.get(); }

private:
    void run();
    void run_ntp();
    // Holds "system.hotspot" exactly while `up`. On the "wisp.net" task.
    void hold_hotspot_lock(bool up);
    // Starts or stops the HTTP servers to match the interface state. Runs
    // on the "wisp.net" task, or on start()/stop()'s thread while that
    // task is not running.
    void reconcile_http(uint64_t now_ms);
    void reconcile_captive(uint64_t now_ms);
    // Logs internal free RAM and its largest block after a server starts
    // or fails to (wisp/app/ram_budget.hpp); a warning under the budget.
    void log_internal_ram() const;
    Value device_status() const;
    // AdminDeps::power_status: PowerManager's level and light sleep, and
    // how many scene requests wait for the network. Atomics and one short
    // lock, so an HTTP thread never waits on a power transition.
    Value power_status() const;

    NetWiringDeps deps_;

    std::unique_ptr<net::WifiService> wifi_;
    std::unique_ptr<net::PairingService> pairing_;
    std::unique_ptr<net::TimeSyncService> time_sync_;
    // Ticked and owned by the "wisp.net" task once start() has returned.
    std::unique_ptr<net::NetworkWindow> window_;
    core::WakeLock hotspot_lock_;
    net::AssetPack assets_;
    bool has_assets_ = false;
    std::unique_ptr<admin::AdminServer> admin_;

    std::vector<core::SubscriptionId> subscriptions_;
    // static_cast<int>(net::WifiState), or -1 without a WifiService.
    std::atomic<int> wifi_state_{-1};
    std::atomic<bool> sta_up_{false};
    std::atomic<bool> ap_up_{false};

    // Owned by whoever runs reconcile_http() (see there).
    bool serving_ = false;
    // After a failed start(): when to try again and the wait after the
    // next failure, so a busy port is neither given up on nor retried
    // (and logged) every tick. Reset once the interfaces go down.
    struct StartRetry {
        std::optional<uint64_t> at_ms;
        uint32_t wait_ms = kStartRetryMinMs;
        bool due(uint64_t now_ms) const { return !at_ms || now_ms >= *at_ms; }
        void failed(uint64_t now_ms);
        void reset() { *this = StartRetry{}; }
    };
    StartRetry start_retry_;
    // The same pair for the captive (port-80) server.
    bool captive_serving_ = false;
    StartRetry captive_retry_;

    os::Semaphore wake_{1, 0};
    std::atomic<bool> stopping_{false};
    std::unique_ptr<os::Task> task_;
    // The "wisp.ntp" worker: woken when tick() has prepared an attempt.
    os::Semaphore ntp_wake_{1, 0};
    std::unique_ptr<os::Task> ntp_task_;
};

}  // namespace wisp::app
