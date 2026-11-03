#pragma once

// The networking half of App's boot (connectivity spec 4.1; app.hpp step
// 4b), kept apart from app.cpp so App::Impl only holds one object for it.
//
// start() runs on App::start()'s thread, after PowerManager exists:
//   - builds PairingService (and, with a "net.wifi" device, WifiService)
//     and init()s them -- WifiService::init() only loads the saved list,
//     so nothing here touches the radio;
//   - subscribes to power.state (-> WifiService::set_standby()) and to
//     net.wifi.state / net.ap.state (which interfaces are up);
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
// only record interface state and wake the task, so no blocking server
// call ever runs on the EventBus dispatcher.
//
// stop() joins the task, stops both servers (their on_close callbacks run
// while they join connection threads; nothing here holds a lock then),
// unsubscribes and destroys the AdminServer before the services it
// routes to. App calls it before tearing down PowerManager.

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
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
    void reconcile_http();
    void reconcile_captive();
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
    // A start() that failed is not retried until the interfaces go down
    // and come back, so a busy port does not spam the log every tick.
    bool start_failed_ = false;
    // The same pair for the captive (port-80) server.
    bool captive_serving_ = false;
    bool captive_failed_ = false;

    os::Semaphore wake_{1, 0};
    std::atomic<bool> stopping_{false};
    std::unique_ptr<os::Task> task_;
};

}  // namespace wisp::app
