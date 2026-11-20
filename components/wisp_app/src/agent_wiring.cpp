#include "agent_wiring.hpp"

#include <cstddef>
#include <cstdint>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "wisp/agent/limits.hpp"
#include "wisp/agent/tools.hpp"
#include "wisp/base/json.hpp"
#include "wisp/core/log.hpp"

namespace wisp::app {

namespace {

constexpr const char* kTag = "agent";

// Settings' agent_max_steps and agent_turn_min as the cell holds them: the
// cell only ever holds values within the rows' ranges (SystemSettings maps
// an invalid stored value to the default), so they pass as they are.
void apply_turn_limits(agent::AgentService& service, const core::SettingsSnapshot& v) {
    service.set_turn_limits(static_cast<uint32_t>(v.agent_max_steps),
                            static_cast<uint32_t>(v.agent_turn_min) * 60000u);
}

// What a model request gets when the platform has no HTTP client: a
// transport error, which the agent reports as "no connection".
class NoHttpClient final : public net::HttpClient {
public:
    net::HttpClientResponse request(const net::HttpClientRequest&) override {
        net::HttpClientResponse res;
        res.transport = net::TransportError::Other;
        return res;
    }
};

// Appends `"key":<json>,` to `out`.
void put(os::LargeString& out, const char* key, const Value& v) {
    out += '"';
    out += key;
    out += "\":";
    const std::string json = to_json(v);
    out.append(json.data(), json.size());
    out += ',';
}

// `[...]` of the messages, one serialized at a time so no tree of the whole
// list is ever built in (internal) RAM.
void put_messages(os::LargeString& out, std::span<const agent::Message> msgs, size_t max_chars) {
    out += "\"messages\":[";
    bool first = true;
    for (const agent::Message& m : msgs) {
        if (!first) out += ',';
        first = false;
        const std::string json = to_json(agent_message_value(m, max_chars));
        out.append(json.data(), json.size());
    }
    out += ']';
}

admin::JsonDoc finish(os::LargeString out) {
    return std::make_shared<const os::LargeString>(std::move(out));
}

Value model_value(const core::ModelProfileService& models) {
    const core::ProfilesState st = models.get();
    const std::string& id = st.active_for(core::ModelKind::Llm);
    if (id.empty()) return Value();
    for (const core::ModelProfile& p : st.profiles) {
        if (p.id != id) continue;
        return Value(Map{{"name", Value(p.name)},
                         {"model", Value(p.model)},
                         {"vision", Value(core::profile_vision(p.options))}});
    }
    return Value();
}

}  // namespace

Value agent_message_value(const agent::Message& m, size_t max_result_chars) {
    Value v = agent::to_value(m, max_result_chars);
    if (m.role != agent::Role::Summary && m.role != agent::Role::Marker &&
        m.role != agent::Role::Carry)
        return v;
    Map out = v.as_map();
    out["hidden"] = Value(true);
    if (m.role == agent::Role::Summary && m.text.empty()) out["dropped"] = Value(true);
    return Value(std::move(out));
}

// --- AgentApiAdapter ---

AgentApiAdapter::AgentApiAdapter(Deps deps) : deps_(std::move(deps)) {}

void AgentApiAdapter::on_event(const agent::AgentEvent& e) {
    if (e.kind == agent::AgentEvent::Kind::Cleared && e.ok) ++epoch_;
}

Result<void> AgentApiAdapter::io(std::function<void()> job) {
    if (!deps_.run_io) {
        job();
        return Result<void>::ok();
    }
    return deps_.run_io(std::move(job));
}

admin::JsonDoc AgentApiAdapter::conversation(size_t max_messages) {
    // A clear between reading the epoch and the snapshot would pair the old
    // epoch with the new messages; read until both agree.
    uint64_t epoch = 0;
    agent::ConversationView view;
    for (int attempt = 0; attempt < 3; ++attempt) {
        epoch = epoch_.load();
        view = deps_.service.view();
        if (epoch_.load() == epoch) break;
    }
    const agent::History& all = *view.messages;
    const size_t first = all.size() > max_messages ? all.size() - max_messages : 0;

    os::LargeString out;
    out += '{';
    put(out, "epoch", Value(static_cast<int64_t>(epoch)));
    put(out, "state", Value(agent::to_string(view.state)));
    put(out, "tool",
        view.state == agent::AgentState::Tool && !view.tool.empty() ? Value(view.tool) : Value());
    put(out, "model", model_value(deps_.models));
    put(out, "turn", view.running_turn ? Value(static_cast<int64_t>(*view.running_turn)) : Value());
    put(out, "queued", Value(view.queued));
    put(out, "confirm", view.confirm ? Value(*view.confirm) : Value());
    put(out, "total", Value(static_cast<int64_t>(all.size())));
    put_messages(out, std::span<const agent::Message>(all).subspan(first), agent::kSummaryChars);
    out += '}';
    return finish(std::move(out));
}

Result<admin::JsonDoc> AgentApiAdapter::message(uint64_t seq) {
    std::optional<agent::Message> m = deps_.store.find(seq);
    if (!m) return Result<admin::JsonDoc>::err(ErrorCode::NotFound, "not_found");
    const std::string json = to_json(agent_message_value(*m, SIZE_MAX));
    return Result<admin::JsonDoc>::ok(finish(os::LargeString(json.data(), json.size())));
}

Result<bool> AgentApiAdapter::submit(std::string text) {
    std::lock_guard<os::Mutex> lock(submit_mu_);
    const agent::ConversationView before = deps_.service.view();
    const bool waits = before.running_turn.has_value() || before.queued;
    if (auto r = deps_.service.submit(std::move(text), agent::Source::Web); !r) {
        return Result<bool>::err(r.error().code, r.error().message);
    }
    return Result<bool>::ok(waits);
}

void AgentApiAdapter::stop() { deps_.service.stop_turn(); }

Result<void> AgentApiAdapter::new_conversation() { return deps_.service.new_conversation(); }

Result<Value> AgentApiAdapter::archives() {
    Result<std::vector<agent::ArchiveInfo>> list =
        Result<std::vector<agent::ArchiveInfo>>::err(ErrorCode::IoError, "not run");
    if (auto r = io([&] { list = deps_.store.archives(); }); !r) {
        return Result<Value>::err(r.error().code, r.error().message);
    }
    if (!list) return Result<Value>::err(list.error().code, list.error().message);
    List items;
    for (const agent::ArchiveInfo& a : list.value()) {
        items.push_back(Value(Map{{"n", Value(static_cast<int64_t>(a.n))},
                                  {"ts", Value(a.ts)},
                                  {"first", Value(a.first)}}));
    }
    return Result<Value>::ok(Value(Map{{"archives", Value(std::move(items))}}));
}

Result<admin::JsonDoc> AgentApiAdapter::archive(uint32_t n) {
    Result<agent::History> read = Result<agent::History>::err(ErrorCode::IoError, "not run");
    if (auto r = io([&] { read = deps_.store.read_archive(n); }); !r) {
        return Result<admin::JsonDoc>::err(r.error().code, r.error().message);
    }
    if (!read) {
        if (read.error().code == ErrorCode::NotFound) {
            return Result<admin::JsonDoc>::err(ErrorCode::NotFound, "not_found");
        }
        return Result<admin::JsonDoc>::err(read.error().code, read.error().message);
    }
    os::LargeString out;
    out += '{';
    put(out, "n", Value(static_cast<int64_t>(n)));
    put_messages(out, read.value(), agent::kSummaryChars);
    out += '}';
    return Result<admin::JsonDoc>::ok(finish(std::move(out)));
}

Result<void> AgentApiAdapter::request_clear() {
    // The outcome reaches the page as the "cleared" frame.
    return deps_.service.request_clear([](bool) {});
}

// --- AgentWsBridge ---

AgentWsBridge::AgentWsBridge(Send send, Find find, std::function<uint64_t()> epoch,
                             std::function<uint64_t()> now_ms, uint32_t coalesce_ms)
    : send_(std::move(send)),
      find_(std::move(find)),
      epoch_(std::move(epoch)),
      now_ms_(std::move(now_ms)),
      coalesce_ms_(coalesce_ms) {
    timer_ = os::Timer::create("wisp.agent.ws", coalesce_ms_, /*periodic=*/false,
                               [this] { on_timer(); });
    if (!timer_) WLOG_W(kTag, "no timer for the reply stream; every piece is sent at once");
}

AgentWsBridge::~AgentWsBridge() { stop(); }

void AgentWsBridge::stop() {
    {
        std::lock_guard<os::Mutex> lock(mu_);
        if (stopped_) return;
    }
    // Outside mu_: the callback takes it.
    if (timer_) timer_->stop();
    std::lock_guard<os::Mutex> lock(mu_);
    flush_locked();
    stopped_ = true;
}

void AgentWsBridge::flush_locked() {
    if (pending_.empty()) return;
    std::string text = std::move(pending_);
    pending_.clear();
    sent_any_ = true;
    last_delta_ms_ = now_ms_();
    send_(to_json(Value(
        Map{{"t", Value("agent")}, {"k", Value("delta")}, {"text", Value(std::move(text))}})));
}

void AgentWsBridge::on_timer() {
    std::lock_guard<os::Mutex> lock(mu_);
    timer_armed_ = false;
    if (!stopped_) flush_locked();
}

void AgentWsBridge::on_event(const agent::AgentEvent& e) {
    using Kind = agent::AgentEvent::Kind;
    if (e.kind == Kind::Delta) {
        bool arm = false;
        {
            std::lock_guard<os::Mutex> lock(mu_);
            if (stopped_) return;
            pending_ += e.text;
            const uint64_t now = now_ms_();
            if (!timer_ || !sent_any_ || now - last_delta_ms_ >= coalesce_ms_) {
                flush_locked();
            } else if (!timer_armed_) {
                timer_armed_ = true;
                arm = true;
            }
        }
        // Outside mu_ (os::Timer::start() may wait for a callback that
        // takes it).
        if (arm) timer_->start();
        return;
    }

    Map frame{{"t", Value("agent")}};
    switch (e.kind) {
        case Kind::ToolPrep:
            frame["k"] = Value("tool");
            frame["phase"] = Value("prep");
            frame["name"] = Value(e.tool);
            frame["bytes"] = Value(static_cast<int64_t>(e.bytes));
            break;
        case Kind::ToolStart:
            frame["k"] = Value("tool");
            frame["phase"] = Value("start");
            frame["name"] = Value(e.tool);
            frame["args"] = Value(e.args);
            break;
        case Kind::ToolEnd:
            frame["k"] = Value("tool");
            frame["phase"] = Value("end");
            frame["name"] = Value(e.tool);
            frame["result"] = Value(e.result);
            frame["ok"] = Value(e.ok);
            break;
        case Kind::State:
            frame["k"] = Value("state");
            frame["state"] = Value(agent::to_string(e.state));
            if (e.state == agent::AgentState::Tool) frame["tool"] = Value(e.tool);
            break;
        case Kind::Confirm:
            frame["k"] = Value("confirm");
            frame["text"] = e.text.empty() ? Value() : Value(e.text);
            break;
        case Kind::Message: {
            std::optional<agent::Message> m = find_ ? find_(e.seq) : std::nullopt;
            if (!m) return;
            frame["k"] = Value("msg");
            frame["msg"] = agent_message_value(*m, agent::kSummaryChars);
            break;
        }
        case Kind::Cleared:
            frame["k"] = Value("cleared");
            frame["ok"] = Value(e.ok);
            frame["epoch"] = Value(static_cast<int64_t>(epoch_ ? epoch_() : 0));
            break;
        default:
            return;
    }
    const std::string json = to_json(Value(std::move(frame)));
    std::lock_guard<os::Mutex> lock(mu_);
    if (stopped_) return;
    // Reply text still waiting goes first, so the page sees the events'
    // order.
    flush_locked();
    send_(json);
}

// --- SceneToolIndexObserver ---

SceneToolIndexObserver::SceneToolIndexObserver(core::StorageWorker& worker,
                                               core::SceneRepository& repo, agent::ToolIndex& index)
    : worker_(worker),
      repo_(repo),
      index_(index),
      refresh_state_(std::make_shared<RefreshState>()) {
    refresh_state_->repo = &repo_;
    refresh_state_->index = &index_;
}

SceneToolIndexObserver::~SceneToolIndexObserver() {
    std::lock_guard<os::Mutex> lock(refresh_state_->mutex);
    refresh_state_->repo = nullptr;
    refresh_state_->index = nullptr;
}

void SceneToolIndexObserver::on_manifest(const core::SceneManifest& manifest) {
    index_.replace_scene(manifest.id, manifest.name, manifest.tools);
}

Result<void> SceneToolIndexObserver::load_installed() {
    // manifests() reads each scene.json in a job of its own and parses it
    // here, on the caller (the agent's start), which may wait. An install
    // or remove can land between two of those jobs, and its lifecycle
    // edge may be handled before this applies the older manifest, so
    // whatever the repository says changed meanwhile is read again by a
    // refresh() job queued after this.
    uint64_t since = 0;
    (void)repo_.changed_since(0, since);
    auto manifests = repo_.manifests();
    if (!manifests) return Result<void>::err(manifests.error().code, manifests.error().message);
    for (const core::SceneManifest& m : manifests.value()) {
        index_.replace_scene(m.id, m.name, m.tools);
    }
    uint64_t now = 0;
    for (const std::string& id : repo_.changed_since(since, now)) refresh(id);
    return Result<void>::ok();
}

void SceneToolIndexObserver::on_transition(const scene::SceneTransition& t) {
    const std::string scene_id = t.id.str();
    if (t.state == scene::SceneTransition::State::Deleted) {
        index_.remove_scene(scene_id);
        return;
    }
    // An install arrives as Stopped (a scene that was not running) or
    // Running/Background/Error after a scene that was reopened following
    // its replace. A normal open and close report the same states, so
    // refresh() reads the manifest back from flash and is a no-op when
    // nothing changed.
    refresh(scene_id);
}

void SceneToolIndexObserver::refresh(const std::string& scene_id) {
    // The worker owns flash access. Queued jobs share a guarded state rather
    // than this observer, so destruction can invalidate it before its refs go.
    const std::shared_ptr<RefreshState> state = refresh_state_;
    if (!worker_.submit([state, scene_id] {
            std::lock_guard<os::Mutex> lock(state->mutex);
            if (state->repo == nullptr || state->index == nullptr) return;
            auto manifest = state->repo->manifest(scene_id);
            if (manifest) {
                state->index->replace_scene(manifest.value().id, manifest.value().name,
                                            manifest.value().tools);
            } else {
                state->index->remove_scene(scene_id);
            }
        })) {
        WLOG_W(kTag, "scene tool index refresh not queued for %s", scene_id.c_str());
    }
}

// --- AgentWiring ---

AgentWiring::AgentWiring(AgentWiringDeps deps) : deps_(std::move(deps)) {
    net::HttpClient* http = deps_.http;
    if (http == nullptr) {
        no_http_ = std::make_unique<NoHttpClient>();
        http = no_http_.get();
    }
    llm_ = std::make_unique<net::LlmClient>(*http, deps_.models);
    store_ = std::make_unique<agent::ConversationStore>(deps_.storage, deps_.conversation_dir);
    memory_ = std::make_unique<core::MemoryStore>(deps_.storage, deps_.memory_path, &deps_.bus,
                                                  deps_.wall_s);
    core::StorageWorker* worker = &deps_.worker;
    ui::UiRuntime* ui = &deps_.ui;
    auto on_worker = [worker](std::function<void()> fn) { return worker->submit(std::move(fn)); };
    io_runner_ = std::make_unique<agent::IoRunner>();
    agent::IoRunner* io = io_runner_.get();
    registry_ = std::make_unique<agent::ToolRegistry>(agent::ToolExecutors{
        .ui = [ui](std::function<void()> fn) { return ui->post(std::move(fn)); },
        .fs = on_worker,
        .io = [io](std::function<void()> fn) { return io->post(std::move(fn)); },
        .fs_interactive =
            [worker](std::function<void()> fn) {
                return worker->submit(std::move(fn), core::IoLane::Interactive);
            },
    });
    inspector_ = std::make_unique<scene::UiInspector>(scene::InspectorDeps{
        .ui = deps_.ui,
        .layer = deps_.layer,
        .scenes = deps_.runtime,
        .shape = deps_.shape,
    });
    confirm_ = std::make_unique<agent::ConfirmService>(agent::ConfirmDeps{
        .ui = deps_.ui,
        .layer = deps_.layer,
        .wake = deps_.wake,
    });
    // The MCP provider exists only where http.fetch does (a SceneHttp);
    // it connects to no server until a search or call needs one.
    if (deps_.mcp != nullptr && deps_.scene_http != nullptr) {
        mcp_client_ = std::make_unique<net::McpClient>(
            *deps_.scene_http, *deps_.mcp,
            net::McpClientConfig{.client_version = deps_.firmware_version,
                                 .max_text_bytes = agent::kHttpMaxMaxBytes});
        mcp_tools_ = std::make_unique<agent::McpToolProvider>(*mcp_client_, *deps_.mcp);
    }
    // DeviceSnapshot::memory and ::mcp come from here, not from
    // `deps_.context` itself: list() and summary_lines() are memory-only
    // (no flash, no request) and this runs on wisp.agent, exactly where it
    // is safe to call.
    core::MemoryStore* memory = memory_.get();
    agent::McpToolProvider* mcp_tools = mcp_tools_.get();
    agent::ContextSource outer_context = deps_.context;
    agent::ContextSource context_with_memory = [outer_context, memory, mcp_tools](agent::Source s) {
        agent::DeviceSnapshot d = outer_context ? outer_context(s) : agent::DeviceSnapshot{};
        for (const core::MemoryNote& n : memory->list()) {
            d.memory.push_back("[" + std::to_string(n.id) + "] " +
                               std::string(n.text.data(), n.text.size()));
        }
        if (mcp_tools) d.mcp = mcp_tools->summary_lines();
        return d;
    };
    service_ = std::make_unique<agent::AgentService>(
        agent::AgentDeps{
            .bus = deps_.bus,
            .store = *store_,
            .tools = *registry_,
            .llm = *llm_,
            .run_io = deps_.run_io,
            .run_io_interactive = deps_.run_io_interactive,
            .locks = deps_.locks,
            .notes = &deps_.notes,
            .screen_off = deps_.screen_off,
            .network_up = deps_.network_up,
            .lang = deps_.lang,
            .now_ms = os::now_ms,
            .wall_s = deps_.wall_s,
            .context = std::move(context_with_memory),
        },
        deps_.config);
    // The per-turn limits from Settings, applied now so the first turn
    // has them, then whatever the settings page stores (each change applies
    // from the next turn on). The subscription's first call delivers the
    // same values again; coalesced changes deliver only the latest, which
    // is all the next turn reads.
    apply_turn_limits(*service_, deps_.settings.cell().get());
    settings_sub_ = deps_.settings.cell().subscribe(
        [service = service_.get()](const core::SettingsSnapshot& v) {
            apply_turn_limits(*service, v);
        });
    service_->set_confirm(confirm_.get());
    const agent::ConfirmFn confirm = service_->confirm_fn();
    tool_index_observer_ =
        std::make_unique<SceneToolIndexObserver>(deps_.worker, deps_.repo, tool_index_);
    workspace_ = std::make_unique<agent::SceneWorkspace>(agent::SceneWorkspaceDeps{
        .repo = deps_.repo,
        .runtime = deps_.runtime,
        .storage = deps_.storage,
        .worker = deps_.worker,
        .confirm = confirm,
        .drafts_root = deps_.drafts_root,
        .inspector = inspector_.get(),
        .on_tools_changed =
            [this](const core::SceneManifest& manifest) {
                tool_index_observer_->on_manifest(manifest);
            },
    });

    scene_tools_ = std::make_unique<agent::SceneToolProvider>(tool_index_, deps_.runtime);
    auto check = [this](const char* what, Result<void> r) {
        if (r) return;
        tools_ok_ = false;
        WLOG_E(kTag, "agent tools not registered (%s): %s", what, r.error().message.c_str());
    };
    check("scene authoring", agent::add_scene_authoring_tools(*registry_, *workspace_));
    check("ui", agent::add_ui_tools(*registry_, *inspector_));
    // Without a panel's shape there is no screen to picture.
    if (deps_.shape.width > 0) {
        check("screenshot",
              agent::add_screenshot_tool(*registry_, {.ui = deps_.ui, .shape = deps_.shape}));
    }
    check("scene ops",
          agent::add_scene_ops_tools(*registry_, agent::SceneOpsDeps{
                                                     .runtime = deps_.runtime,
                                                     .repo = deps_.repo,
                                                     .files = deps_.files,
                                                     .storage = deps_.storage,
                                                     .submit_io = on_worker,
                                                     .delete_scene = deps_.delete_scene,
                                                     .confirm = confirm,
                                                 }));
    check("device",
          agent::add_device_tools(*registry_, agent::DeviceToolDeps{
                                                  .status = [this] { return device_status(); },
                                                  .settings = deps_.settings,
                                                  .notes = deps_.notes,
                                                  .submit_io = on_worker,
                                              }));
    // Offered only when some device describes itself: elsewhere the tools
    // could only list nothing, yet cost every request their JSON. Decided
    // once, so the list stays byte-stable: no service re-resolves devices
    // after boot (device model spec 1.4).
    bool described = false;
    if (deps_.devices != nullptr) {
        deps_.devices->for_each([&](Value) {
            described = true;
            return false;
        });
    }
    if (described) {
        check("device call", agent::add_device_call_tools(
                                 *registry_, agent::DeviceCallToolDeps{.devices = *deps_.devices}));
    }
    check("docs", agent::add_docs_tools(*registry_));
    if (deps_.scene_http != nullptr) {
        check("http",
              agent::add_http_tools(*registry_, agent::HttpToolDeps{.http = *deps_.scene_http}));
    }
    // Decided once at boot, like the others, so the tool list stays
    // byte-stable: a card inserted later shows in media.state, not here.
    if (deps_.media_library != nullptr || deps_.media_player != nullptr) {
        check("media", agent::add_media_tools(*registry_, agent::MediaToolDeps{
                                                              .library = deps_.media_library,
                                                              .player = deps_.media_player,
                                                              .http = deps_.scene_http,
                                                          }));
    }
    check("lua", agent::add_lua_tools(*registry_, agent::LuaToolDeps{.runtime = deps_.runtime}));
    registry_->add_provider(*scene_tools_);
    if (mcp_tools_) registry_->add_provider(*mcp_tools_);
    check("find tools", agent::add_find_tools(*registry_));
    if (mcp_tools_) {
        check("mcp", agent::add_mcp_tools(*registry_, agent::McpToolDeps{
                                                          .servers = *deps_.mcp,
                                                          .provider = *mcp_tools_,
                                                      }));
    }
    check("memory", agent::add_memory_tools(*registry_, agent::MemoryToolDeps{.store = *memory_}));
    if (deps_.reminders != nullptr) {
        check("todo", agent::add_todo_tools(*registry_,
                                            agent::TodoToolDeps{.scheduler = *deps_.reminders}));
    }

    adapter_ = std::make_unique<AgentApiAdapter>(AgentApiAdapter::Deps{
        .service = *service_,
        .store = *store_,
        .models = deps_.models,
        .run_io = deps_.run_io,
    });
    bridge_ = std::make_unique<AgentWsBridge>(
        [this](std::string json) {
            std::lock_guard<os::Mutex> lock(hub_mu_);
            if (hub_ != nullptr) hub_->send_agent(std::move(json));
        },
        [store = store_.get()](uint64_t seq) { return store->find(seq); },
        [adapter = adapter_.get()] { return adapter->epoch(); });
}

AgentWiring::~AgentWiring() {
    stop();
    // Before service_ goes; the cell's owner (App::Impl::system_settings)
    // outlives this wiring.
    settings_sub_.reset();
}

Result<void> AgentWiring::start() {
    if (!listening_) {
        listener_ = service_->add_listener([this](const agent::AgentEvent& e) {
            adapter_->on_event(e);
            bridge_->on_event(e);
        });
        listening_ = true;
    }
    // Loaded here (on the worker, like ConversationStore::load() inside
    // AgentService::start()) rather than lazily on first use: a missing or
    // corrupt file only degrades to an empty memory, so this never blocks
    // the agent from starting.
    core::MemoryStore* memory = memory_.get();
    Result<void> loaded = Result<void>::err(ErrorCode::Busy, "storage worker refused");
    if (deps_.run_io) {
        deps_.run_io([memory, &loaded] { loaded = memory->load(); });
    } else {
        loaded = memory->load();
    }
    if (!loaded) WLOG_W(kTag, "memory notes not loaded: %s", loaded.error().message.c_str());
    if (Result<void> indexed = tool_index_observer_->load_installed(); !indexed) {
        WLOG_W(kTag, "scene tool index not loaded: %s", indexed.error().message.c_str());
    }
    refresh_storage();
    return service_->start();
}

void AgentWiring::stop() {
    service_->stop();
    // Io calls the registry abandoned may still run; nothing calls tools now.
    io_runner_->join_all();
    if (listening_) {
        service_->remove_listener(listener_);
        listening_ = false;
    }
    bridge_->stop();
    attach_ws(nullptr);
}

void AgentWiring::on_scene_transition(const scene::SceneTransition& t) {
    tool_index_observer_->on_transition(t);
}

void AgentWiring::attach_ws(admin::WsHub* hub) {
    std::lock_guard<os::Mutex> lock(hub_mu_);
    hub_ = hub;
}

void AgentWiring::refresh_storage() {
    std::shared_ptr<StorageCache> cache = storage_cache_;
    if (cache->pending.exchange(true)) return;
    core::StorageManager* storage = &deps_.storage;
    const bool queued = deps_.worker.submit([cache, storage] {
        List media;
        for (const core::MediaStatus& m : storage->status().media) {
            media.push_back(Value(Map{{"id", Value(m.id)},
                                      {"online", Value(m.online)},
                                      {"removable", Value(m.removable)},
                                      {"total_bytes", Value(static_cast<int64_t>(m.total_bytes))},
                                      {"free_bytes", Value(static_cast<int64_t>(m.free_bytes))}}));
        }
        {
            std::lock_guard<os::Mutex> lock(cache->mu);
            cache->value = Value(std::move(media));
        }
        cache->pending.store(false);
    });
    if (!queued) cache->pending.store(false);
}

Value AgentWiring::device_status() {
    Value base = deps_.device_status ? deps_.device_status() : Value();
    Map out = base.type() == Value::Type::Map ? base.as_map() : Map{};
    {
        std::lock_guard<os::Mutex> lock(storage_cache_->mu);
        out["storage"] = storage_cache_->value;
    }
    // For the next call: the read happens on the worker.
    refresh_storage();
    return Value(std::move(out));
}

}  // namespace wisp::app
