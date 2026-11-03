#include "factory_reset.hpp"

#include <algorithm>
#include <mutex>
#include <utility>

#include "app_impl.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/core/log.hpp"
#include "wisp/net/pairing_service.hpp"
#include "wisp/net/wifi_service.hpp"
#include "wisp/os/os.hpp"

namespace wisp::app {

namespace {

constexpr const char* kTag = "reset";
constexpr const char* kTopic = "system.factory_reset";
// How long the "done" event gets to reach the browser before the
// revocation closes its socket. Bounded: a stalled socket must not keep
// the device from resetting.
constexpr uint32_t kFlushMs = 500;
// The reset writes flash (LittleFS / NVS) through several layers.
constexpr size_t kResetStackBytes = 6144;

void publish_state(App& app, const char* state) {
    app.bus().publish(core::Event{kTopic, "app", 0, Properties{{"state", Value(state)}}});
}

}  // namespace

void request_factory_reset(App& app) { publish_state(app, "requested"); }

void cancel_factory_reset(App& app) {
    WLOG_I(kTag, "factory reset cancelled on the device");
    publish_state(app, "cancelled");
}

void factory_reset(App& app) {
    WLOG_W(kTag, "factory reset: erasing settings, Wi-Fi, pairings and the model config");
    publish_state(app, "done");

    const uint64_t deadline = os::now_ms() + kFlushMs;
    auto remaining = [deadline] {
        const uint64_t now = os::now_ms();
        return static_cast<uint32_t>(now < deadline ? deadline - now : 0);
    };
    (void)app.bus().wait_idle(remaining());
    if (app.impl_->net && !app.impl_->net->flush_admin_ws(remaining())) {
        WLOG_W(kTag, "admin WebSocket not flushed in time; resetting anyway");
    }

    if (auto r = app.impl_->platform.settings->erase_all(); !r) {
        WLOG_E(kTag, "erasing settings failed: %s", r.error().message.c_str());
    }
    if (net::WifiService* wifi = app.wifi()) wifi->forget_all();
    if (net::PairingService* pairing = app.pairing()) pairing->revoke_all();
    app.llm().clear();

    if (app.impl_->platform.reboot) {
        WLOG_W(kTag, "factory reset complete; restarting");
        app.impl_->platform.reboot();
    } else {
        WLOG_E(kTag, "factory reset complete but this platform cannot restart");
    }
}

bool start_factory_reset(App& app) {
    App::Impl& impl = *app.impl_;
    std::lock_guard<os::Mutex> lock(impl.reset_mutex);
    if (impl.reset_task) {
        WLOG_W(kTag, "factory reset already running");
        return true;
    }
    const os::TaskConfig config{.name = "wisp.reset", .stack_bytes = kResetStackBytes};
    auto body = [&app] { factory_reset(app); };
    impl.reset_task =
        impl.reset_spawner ? impl.reset_spawner(config, body) : os::Task::spawn(config, body);
    if (!impl.reset_task) {
        WLOG_E(kTag, "factory reset task could not start; nothing was erased");
        publish_state(app, "cancelled");
        return false;
    }
    return true;
}

void set_reset_spawner(App& app, TaskSpawner spawner) {
    std::lock_guard<os::Mutex> lock(app.impl_->reset_mutex);
    app.impl_->reset_spawner = std::move(spawner);
}

void join_factory_reset(App& app) {
    std::unique_ptr<os::Task> task;
    {
        std::lock_guard<os::Mutex> lock(app.impl_->reset_mutex);
        task = std::move(app.impl_->reset_task);
    }
    if (task) task->join();
}

}  // namespace wisp::app
