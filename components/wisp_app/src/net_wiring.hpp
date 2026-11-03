#pragma once

// The networking half of App's boot (connectivity spec 4.1; app.hpp step
// 4b), kept apart from app.cpp so App::Impl only holds one object for it.
//
// start() runs on App::start()'s thread, after PowerManager exists:
//   - builds PairingService (and, with a "net.wifi" device, WifiService)
//     and init()s them -- WifiService::init() only loads the saved list,
//     so nothing here touches the radio;
//   - subscribes to power.state (-> WifiService::set_standby()) and to
//     net.wifi.state / net.ap.state (only to wake the task: which
//     interfaces are up is read from WifiService each tick, so a dropped
//     event cannot leave the servers in the wrong state);
//   - with Platform::http_server, builds the AdminServer and, when
//     http_always_on, starts serving right away. Platform::captive_server
//     (if any) runs AdminServer::captive_handler() on port 80 exactly
//     while the hotspot is up;
//   - spawns the "wisp.net" task.
//
// The "wisp.net" task is the only caller of WifiService::tick() and
// PairingService::tick() (every kNetTickMs) and the only thread that
// starts or stops the HTTP servers once start() has returned. It has its
// own task rather than an os::Timer because tick() may block for a ~3 s
// scan, and on ESP every Timer shares one dispatch task. Event handlers
// only wake the task, so no blocking server call ever runs on the
// EventBus dispatcher. A server whose start() failed is retried with a
// backoff (kStartRetryMinMs doubling up to kStartRetryMaxMs) while its
// interface stays up.
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
#include <vector>

#include "wisp/admin/admin_server.hpp"
#include "wisp/app/app.hpp"
#include "wisp/core/boot_report.hpp"
#include "wisp/core/device_registry.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/core/llm_config.hpp"
#include "wisp/core/power_monitor.hpp"
#include "wisp/core/storage_manager.hpp"
#include "wisp/core/system_settings.hpp"
#include "wisp/net/asset_pack.hpp"
#include "wisp/net/pairing_service.hpp"
#include "wisp/net/wifi_service.hpp"
#include "wisp/os/os.hpp"

namespace wisp::app {

struct NetWiringDeps {
    Platform& platform;
    core::DeviceRegistry& registry;
    core::BootReport& report;
    core::EventBus& bus;
    core::StorageManager& storage;
    core::LlmConfigService& llm;
    core::SystemSettings& system_settings;
    // GET /device reports its last battery sample; null: battery unknown.
    const core::PowerMonitor* power_monitor = nullptr;
    // Shows the factory-reset confirmation on the device (AdminDeps::
    // request_factory_reset). Empty when there is no screen to confirm on:
    // the route then answers 503.
    std::function<void()> request_factory_reset;
};

class NetWiring {
public:
    static constexpr uint32_t kNetTickMs = 250;
    // Where phones' captive-portal probes go.
    static constexpr uint16_t kCaptivePort = 80;
    static constexpr uint32_t kStartRetryMinMs = 2000;
    static constexpr uint32_t kStartRetryMaxMs = 60000;

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
    net::PairingService* pairing() const { return pairing_.get(); }

private:
    void run();
    // Starts or stops the HTTP servers to match the interface state. Runs
    // on the "wisp.net" task, or on start()/stop()'s thread while that
    // task is not running.
    void reconcile_http(uint64_t now_ms);
    void reconcile_captive(uint64_t now_ms);
    Value device_status() const;

    NetWiringDeps deps_;

    std::unique_ptr<net::WifiService> wifi_;
    std::unique_ptr<net::PairingService> pairing_;
    net::AssetPack assets_;
    bool has_assets_ = false;
    std::unique_ptr<admin::AdminServer> admin_;

    std::vector<core::SubscriptionId> subscriptions_;
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
};

}  // namespace wisp::app
