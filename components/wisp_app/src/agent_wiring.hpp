#pragma once

// The agent's half of App's boot (agent-core spec 2, 7.1, 7.2): the
// AgentService with every tool built so far, its REST face (AgentApiAdapter,
// handed to the admin routes as AdminDeps::agent) and its WebSocket frames
// (AgentWsBridge, fed to the admin page's WsHub).
//
// App builds it (App::build_agent()) with a screen and a scene runtime,
// after the runtime and before networking (the routes take api()); it
// start()s once the runtime runs and stop()s before the scene runtime, the
// system layer and the storage worker go: after stop() nothing runs on
// wisp.agent any more (a run_sync() after the worker stops would run
// inline on a stack that may be in PSRAM). The object itself stays until
// networking is gone, so a late REST call still finds api() (it then
// answers "not running" or reads the conversation as it was left).
//
// Tool threads (ToolExecutors): "ui" posts to the UiRuntime, "fs" and "io"
// queue on the storage worker (wisp.fs, the one thread that touches
// flash).
//
// The per-turn limits follow Settings' agent_max_steps and agent_turn_min
// (the system.settings event), from the next turn on.
//
// Everything the agent reads about the device while it runs (the context
// source, device.status) comes from cached values: wisp.agent may have its
// stack in PSRAM and must never touch flash. Storage use is refreshed on
// the worker whenever device.status is read, so it lags by one call.

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "wisp/admin/agent_api.hpp"
#include "wisp/admin/ws_hub.hpp"
#include "wisp/agent/agent_service.hpp"
#include "wisp/agent/confirm.hpp"
#include "wisp/agent/context.hpp"
#include "wisp/agent/conversation.hpp"
#include "wisp/agent/scene_workspace.hpp"
#include "wisp/agent/tool_registry.hpp"
#include "wisp/base/result.hpp"
#include "wisp/base/value.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/core/memory_store.hpp"
#include "wisp/core/model_profiles.hpp"
#include "wisp/core/notification_service.hpp"
#include "wisp/core/reminders.hpp"
#include "wisp/core/scene_files.hpp"
#include "wisp/core/scene_repository.hpp"
#include "wisp/core/storage_manager.hpp"
#include "wisp/core/storage_worker.hpp"
#include "wisp/core/system_settings.hpp"
#include "wisp/core/wakelock.hpp"
#include "wisp/hal/display_geometry.hpp"
#include "wisp/net/http.hpp"
#include "wisp/net/llm_client.hpp"
#include "wisp/os/large.hpp"
#include "wisp/os/os.hpp"
#include "wisp/scene/scene_runtime.hpp"
#include "wisp/scene/ui_inspector.hpp"
#include "wisp/ui/system_layer.hpp"
#include "wisp/ui/ui_runtime.hpp"

namespace wisp::app {

// One message as REST and the WebSocket show it (wisp/admin/agent_api.hpp):
// agent::to_value() plus "hidden" and "dropped".
Value agent_message_value(const agent::Message& m, size_t max_result_chars);

// admin::AgentApi over an AgentService and its ConversationStore.
class AgentApiAdapter final : public admin::AgentApi {
public:
    struct Deps {
        agent::AgentService& service;
        agent::ConversationStore& store;
        core::ModelProfileService& models;
        // Runs a job on the storage worker and waits for it (the archives
        // live in files). Empty: on the calling thread.
        std::function<Result<void>(std::function<void()>)> run_io;
    };

    explicit AgentApiAdapter(Deps deps);

    // From the service's listener: a Cleared that changed something starts
    // the next conversation's epoch.
    void on_event(const agent::AgentEvent& e);
    uint64_t epoch() const { return epoch_.load(); }

    admin::JsonDoc conversation(size_t max_messages) override;
    Result<admin::JsonDoc> message(uint64_t seq) override;
    Result<bool> submit(std::string text) override;
    void stop() override;
    Result<void> new_conversation() override;
    Result<Value> archives() override;
    Result<admin::JsonDoc> archive(uint32_t n) override;
    Result<void> request_clear() override;

private:
    Result<void> io(std::function<void()> job);

    Deps deps_;
    std::atomic<uint64_t> epoch_{1};
    // Serializes submit(): whether a message waits is read just before it
    // is handed over.
    os::Mutex submit_mu_;
};

// Agent events -> {"t":"agent",...} frames (agent-core spec 7.2):
//   delta    {"k":"delta","text"}: reply text, coalesced so at most one
//            frame leaves per coalesce_ms (the rest follows from a timer);
//   tool     {"k":"tool","phase":"start"|"end","name","args"|"result"
//            (cut to 200 code points),"ok" (end)}, and while the model
//            still writes the call {"k":"tool","phase":"prep","name",
//            "bytes"} (the arguments' length so far, never their text);
//   state    {"k":"state","state","tool" (while "tool")};
//   confirm  {"k":"confirm","text":string|null};
//   msg      {"k":"msg","msg":agent_message_value(...)};
//   cleared  {"k":"cleared","ok","epoch"}.
// Text still waiting to go out leaves before any other frame, so the
// order is the events' order. Turn begin/end have no frame (the user's
// message arrives as "msg", the end as a state change and a marker
// "msg").
//
// on_event() runs on wisp.agent, the flush timer on the timer thread; both
// call `send` under the bridge's lock (so frames keep their order), which
// must therefore only queue (WsHub::send_agent()).
class AgentWsBridge {
public:
    static constexpr uint32_t kCoalesceMs = 100;

    using Send = std::function<void(std::string json)>;
    using Find = std::function<std::optional<agent::Message>(uint64_t seq)>;

    AgentWsBridge(Send send, Find find, std::function<uint64_t()> epoch,
                  std::function<uint64_t()> now_ms = os::now_ms,
                  uint32_t coalesce_ms = kCoalesceMs);
    // stop().
    ~AgentWsBridge();

    AgentWsBridge(const AgentWsBridge&) = delete;
    AgentWsBridge& operator=(const AgentWsBridge&) = delete;

    void on_event(const agent::AgentEvent& e);
    // Sends the text still waiting and stops the timer; later events are
    // dropped. Not from inside `send`. Idempotent.
    void stop();

private:
    void flush_locked();
    void on_timer();

    Send send_;
    Find find_;
    std::function<uint64_t()> epoch_;
    std::function<uint64_t()> now_ms_;
    const uint32_t coalesce_ms_;
    std::unique_ptr<os::Timer> timer_;  // null: every delta goes out at once

    os::Mutex mu_;
    std::string pending_;  // delta text not sent yet
    uint64_t last_delta_ms_ = 0;
    bool sent_any_ = false;
    bool timer_armed_ = false;
    bool stopped_ = false;
};

struct AgentWiringDeps {
    core::EventBus& bus;
    core::StorageManager& storage;
    core::StorageWorker& worker;
    core::ModelProfileService& models;
    core::SystemSettings& settings;
    core::NotificationService& notes;
    // The todos (todo.* tools); null: no todo tools.
    core::ReminderScheduler* reminders = nullptr;
    core::WakeLockRegistry* locks = nullptr;
    ui::UiRuntime& ui;
    ui::SystemLayer& layer;
    hal::ScreenShape shape{};
    scene::SceneRuntime& runtime;
    core::SceneRepository& repo;
    core::SceneFiles* files = nullptr;
    // <System>/agent, <System>/memory/notes.json and <scene data
    // root>/.drafts.
    std::string conversation_dir{};
    std::string memory_path{};
    std::string drafts_root{};
    // Platform::http_client; null: every model request fails as a
    // transport error.
    net::HttpClient* http = nullptr;
    // StorageWorker::run_sync() with an unbounded wait.
    std::function<Result<void>(std::function<void()>)> run_io{};
    // SceneWiring::delete_scene.
    std::function<void(const std::string& id, bool with_data,
                       std::function<void(Result<void>)> done)>
        delete_scene{};
    // Turns the screen on before a device confirmation (UI thread).
    std::function<void()> wake{};
    // AgentDeps' (cached values only).
    std::function<bool()> screen_off{};
    std::function<bool()> network_up{};
    std::function<std::string()> lang{};
    std::function<int64_t()> wall_s{};
    // AgentWiring fills DeviceSnapshot::memory in from its own MemoryStore
    // before handing the snapshot to AgentService; this need not (and, as
    // cached values only, cannot) set it.
    agent::ContextSource context{};
    // device.status without "storage", which AgentWiring adds from its
    // cache. Cached values only.
    std::function<Value()> device_status{};
    agent::AgentConfig config{};
};

class AgentWiring {
public:
    // Builds everything and registers every tool; nothing runs yet.
    explicit AgentWiring(AgentWiringDeps deps);
    // stop().
    ~AgentWiring();

    AgentWiring(const AgentWiring&) = delete;
    AgentWiring& operator=(const AgentWiring&) = delete;

    // Whether every tool set registered (a failure is logged; the agent
    // then runs with what did).
    bool tools_ok() const { return tools_ok_; }

    // Loads the conversation (on the worker) and starts wisp.agent.
    Result<void> start();
    // Stops wisp.agent (a turn in progress ends) and the WebSocket frames.
    // Idempotent.
    void stop();

    admin::AgentApi* api() { return adapter_.get(); }
    agent::AgentService* service() { return service_.get(); }
    // The agent's long-term memory notes (mvp spec 5.1); the admin page's
    // /memory routes read and delete through this same instance rather
    // than opening a second one on the same file.
    core::MemoryStore* memory() { return memory_.get(); }
    // Sends the agent frames to `hub` from now on (null: nowhere). The hub
    // must outlive stop() or the next attach_ws().
    void attach_ws(admin::WsHub* hub);

private:
    Value device_status();
    void refresh_storage();

    AgentWiringDeps deps_;
    // Members in construction order; destroyed in reverse.
    std::unique_ptr<net::HttpClient> no_http_;
    std::unique_ptr<net::LlmClient> llm_;
    std::unique_ptr<agent::ConversationStore> store_;
    std::unique_ptr<core::MemoryStore> memory_;
    std::unique_ptr<agent::ToolRegistry> registry_;
    std::unique_ptr<scene::UiInspector> inspector_;
    std::unique_ptr<agent::ConfirmService> confirm_;
    std::unique_ptr<agent::AgentService> service_;
    std::unique_ptr<agent::SceneWorkspace> workspace_;
    std::unique_ptr<AgentApiAdapter> adapter_;
    std::unique_ptr<AgentWsBridge> bridge_;
    bool tools_ok_ = true;
    core::SubscriptionId settings_sub_ = 0;

    os::Mutex hub_mu_;
    admin::WsHub* hub_ = nullptr;
    uint32_t listener_ = 0;
    bool listening_ = false;

    // The last StorageManager::status(), read on the worker. Shared with
    // the refresh job, which may still be queued when this object goes.
    struct StorageCache {
        os::Mutex mu;
        Value value;
        std::atomic<bool> pending{false};
    };
    std::shared_ptr<StorageCache> storage_cache_ = std::make_shared<StorageCache>();
};

}  // namespace wisp::app
