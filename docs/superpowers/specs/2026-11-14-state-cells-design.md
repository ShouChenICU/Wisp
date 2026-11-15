# State Cells and Event Bus Semantics — Design

- Date: 2026-11-14
- Status: approved by the owner on 2026-11-14 (see "Approval" at the end)
- Scope: phase D of `docs/superpowers/plans/2026-11-14-architecture-optimization-roadmap.md`. State that other components follow moves from the lossy event bus to `core::StateCell<T>`; commands and input edges get paths that cannot lose them; the bus keeps notifications and telemetry, with the wire formats of the admin WebSocket and the scenes unchanged.
- Builds on: the foundation spec (event bus, power levels), the connectivity spec (`system.settings`, `system.firmware`, `system.factory_reset`, WebSocket `events`), the flash-thread design (`2026-11-06-flash-thread-design.md`, threading and internal-RAM rules).
- Serves: phase E (the settings snapshot lives in a cell), phase F (`wisp.net` and `wisp.power` can wait on state changes instead of polling), phase L (commands are already off the bus; `core::Dispatcher` is the executor interface PowerManager needs).
- Evidence: the code on `main` at `85a99aa`. Every `file:line` below refers to it.

---

## 1. Background and problem

### 1.1 The one primitive

`core::EventBus` is the only way a component learns that another one changed:

- String topics with prefix subscriptions, `Value` payloads (`Event{topic, source, ts_ms, Properties payload}`, `event_bus.hpp:36-41`; `Properties` is `std::map<std::string, Value>`, `value.hpp:18-22`).
- One dispatch task `wisp.bus` (6144-byte stack, priority 5, `event_bus.cpp:84`), handlers called one by one in subscription order (`event_bus.cpp:202-228`).
- A queue of 64 (`event_bus.hpp:48`) that drops the **oldest** event when full and counts it (`event_bus.cpp:151-187`). `dropped()` is read by nothing but tests (its only definition, `event_bus.cpp:200`, has no caller in `components/`).
- `unsubscribe()` from another thread waits for *whatever* dispatch is in flight, holding one global `dispatch_mutex_` (`event_bus.hpp:99-132`, `event_bus.cpp:129-149`).
- Each publish allocates: the topic and source strings (topics over 15 characters, such as `power.charging.changed`, leave the small-string buffer), one map node per payload key and the strings inside the values. These are small blocks, so they land in internal RAM (AGENTS.md section 8), even though the queue slots themselves are in `Mem::Large` (`docs/architecture.md:136`).

The same primitive carries three different things:

- **State** that a subscriber mirrors: power level, Wi-Fi status, settings, agent and voice state, the scene in front.
- **Commands** that must happen exactly once: the power key, a factory-reset request, a firmware confirmation.
- **Telemetry and notifications** where a loss is harmless: the microphone level, wake-lock changes, `memory.changed`.

A dropped event is fine for the third group and wrong for the first two. The code knows this, so consumers carry their own resync code (section 1.3), and where they do not, a dropped event leaves them wrong until the next change (section 1.4).

### 1.2 How full the queue gets

The queue fills when publishers outrun `wisp.bus`. The largest steady publisher is `voice.state`: while listening or speaking, `publish_level()` republishes the whole state with a new `level` every 50 ms (`voice_service.cpp:446-450`, `kLevelIntervalMs`), 20 events per second, each a fresh map (`voice_service.cpp:424-435`). Everything else is human-rate or slower. So a voice turn plus any handler that stalls `wisp.bus` for about three seconds (64 / 20 per second) starts evicting the oldest events, and those can be a `button.vol_down.up`, a `power.state` or a `system.settings`. Whether that happens on a device is not measured; nothing reports `dropped()`.

### 1.3 Compensations in the code today

| # | Where | What it compensates | Mechanism |
|---|---|---|---|
| C1 | `PowerManager::tick()`, `power_manager.cpp:173-184`; `kSettingsRefreshTicks = 60`, `power_manager.hpp:123-125` | a lost `system.settings` | the event only sets `settings_dirty_` (`power_manager.cpp:47-50`); every 60 ticks the power keys are read from Settings again on the timer task (`read_settings()`, `power_manager.cpp:164-171`) |
| C2 | `SystemDuties::refresh_timer_cb()`, `system_duties.cpp:265-280`; `kNetResyncMs = 1000`, `system_duties.cpp:33-39` | lost `net.wifi.state`, `net.ap.state`, `net.pairing.code`, `system.factory_reset`, `system.firmware` | a 100 ms `lv_timer` re-reads Wi-Fi, pairing and both pending requests once a second |
| C3 | same timer, `sync_system_language()`, `system_duties.cpp:336-343` | a lost `system.settings` (language) | `SystemSettings::revision()` compared every 100 ms |
| C4 | `reset_pending`, `app_impl.hpp:267-271`; `request_factory_reset()` / `take_factory_reset_request()`, `factory_reset.cpp:42-48` | a request made before the screen subscribed, or whose event was dropped | an atomic flag beside the event, taken on the UI thread at start, on the event and in the C2 resync |
| C5 | `firmware_pending`, `app_impl.hpp:286-288`; `firmware_confirm.cpp:25-27`; `app.cpp:496-505` | the same for a received firmware image | same as C4, plus `Deps::request_confirm` (`firmware_update.cpp:318`) |
| C6 | `SelfCheck::refresh_timer_cb()`, `self_check_screen.cpp:380-401`; `kNetResyncMs`, `kSceneResyncMs = 30000`, `self_check_screen.cpp:36-49` | lost `net.*` and `scene.state`; and a foreground change that publishes nothing (closing a faulted scene's error panel, comment at `self_check_screen.cpp:391-395`) | network section re-read every second; scene list relisted and foreground re-followed every 30 s |
| C7 | `SelfCheck::sync_settings()`, `self_check_screen.cpp:317-327` | a lost `system.settings` (time zone, 24 h) | `revision()` compared every 100 ms |
| C8 | `SystemSettings::revision()`, `system_settings.hpp:114-118` | exists only for C3 and C7 ("a poller that may have missed `on_changed`'s event", and `system_settings.hpp:38-40`) | — |
| C9 | `NetWiring::run()`, `net_wiring.cpp:259-283`; subscriptions `net_wiring.cpp:85-90`, header `net_wiring.hpp:10-14` | lost `power.state`, `net.wifi.state`, `net.ap.state` | events only wake the task; every 250 ms (`kNetTickMs`, `net_wiring.hpp:162`) it reads `PowerManager::level()` and `WifiService` itself |
| C10 | `NetWiring::reconcile_external_power()`, `net_wiring.cpp:313-324` | there is no event it trusts for USB power | polls `PowerMonitor::last_status()` every 250 ms |
| C11 | `SceneWiring::start_runtime()`, `scene_wiring.cpp:199-204`; `SceneRuntime::set_visible()`, `scene_runtime.cpp:421-429` | a late or lost `power.state` | the handler ignores the payload and reads `standby()`; the scene task follows `want_visible_` |
| C12 | `StatusShell`, `status_shell.cpp:245-256, 273-287` | a burst of events | events only queue one refresh, which reads the services |
| C13 | `Home::request()`, `home.cpp:437` | a full UI post queue | "the timer's next tick picks the change up" — but only for what the timer re-reads (section 1.4) |

C9, C11 and C12 use the right pattern (the event is a doorbell; the state is read from its owner). C1-C8 are periodic work that exists only because the bus may drop.

### 1.4 Mirrors with no compensation

These fold event payloads into private copies and have nothing that repairs a lost event:

| Mirror | File | Consequence of one lost event |
|---|---|---|
| `notify_vol`, `utc_offset_min`, `lang`, `time_24h`, `settings_now` (`merge_settings()`) | `app.cpp:89-115, 414-434`; `app_impl.hpp:158-173` | notification volume, status-bar clock time zone, agent language and the shell's settings page stay at the old value until the next settings change |
| `tts_mute`, `volume` | `voice_wiring.cpp:34-43` | spoken replies stay muted or unmuted; TTS volume stale |
| agent turn limits | `agent_wiring.cpp:484-494` | old step/minute limits until the next change |
| scene runtime `utc_offset_min_`, `time_24h_`, `lang_` | `scene_runtime.cpp:193-221` | scenes keep the old time zone and language; the "next minute" fallback re-delivers the stale value |
| `SensorService::set_standby()` from the `power.state` payload | `app.cpp:455-461` | the IMU keeps polling in standby, or stays paused on a lit screen |
| self-check sampler standby | `self_check_screen.cpp:296-302` | same, for the diagnostics sampler |
| Home `agent_state_`, `voice_state_`, `standby_`, `level_` | `home.cpp:470-496` | the wisp shows "thinking" after the turn ended, or a stale standby flag ends a press (`home.cpp:508-512`) |
| Shell talk overlay `voice_state_` | `shell.cpp:248-262` | the overlay keeps a listening indicator |
| agent `net_state` | `agent_service.cpp:160-173` | after a lost `connected`, `network_up()` stays false and turns are refused as offline |
| status panel close on standby | `status_shell.cpp:277-287` | the panel is still open when the screen wakes |
| battery notice from `power.battery.low` / `power.charging.changed` | `app.cpp:268-286` | no low-battery notice; or a stale one while charging |
| scene recents and notices from `scene.state` | `app.cpp:287-304` | a deleted scene's notifications stay forever; the scene order misses an open |
| scene tool index | `agent_wiring.cpp:376-397` | the agent keeps a deleted scene's tools, or misses an install's new ones |
| SD recount | `scene_wiring.cpp:104-110` | scene file usage on the card is not recounted |
| reminders and the network window from `system.time` | `reminders.cpp:172-179`; `network_window.cpp:38` | due reminders after a clock jump are not re-evaluated until the next timer; the window waits for its timeout |
| conversation `error` / `not_heard` / deferred hold | `conversation.cpp:481-496` | the error hint is not shown; a hold made while a capture was being cancelled never starts |
| talk key `down`/`up` | `talk_key.cpp:51-91` | a lost `up` keeps recording until the capture limit |
| power key | `system_duties.cpp:181-216` | a press does nothing |
| button wake from standby | `power_manager.cpp:67-77` | the screen does not wake |

Two more defects of the same kind: UI subscribers hop to the UI thread with `UiRuntime::post()`, whose queue holds 32 and returns false when full (`ui_runtime.hpp:125-129`, `ui_runtime.cpp:39`); every such handler logs "post(...) dropped" and loses the work (`system_duties.cpp:194-215, 227-262`, `self_check_screen.cpp:280-312`, `power_manager.cpp:117-121`). And transient states are published as states: `voice.state` `error` is "immediately followed by `idle`" (`voice_service.cpp:452-456`, `docs/architecture.md:130`), and the agent goes `error` then `idle` at the end of the same turn (`agent_service.cpp:488` then `:441`). Any coalescing primitive must keep those transients or the error hints disappear.

### 1.5 Who consumes each topic

Grepped `publish(` / `subscribe(` across `components/` and `simulator/`, excluding tests. "WS" means forwarded by `WsHub` (`kForwardedPrefixes = memory. net. power. scene. storage. system. todo.`, `ws_hub.cpp:24-25`).

| Topic | Publisher | Device subscribers | Off device | Kind |
|---|---|---|---|---|
| `button.<id>.down/up/short/long` | `ButtonService::publish_event()`, `button_service.cpp:448-453` | PowerManager `:67`, SystemDuties `:74`, SelfCheck `:274`, TalkKey `talk_key.cpp:27` | none | input edge |
| `power.state` | `PowerManager::publish_state()`, `power_manager.cpp:590-596` | Home `home.cpp:372`, StatusShell `:277`, SelfCheck `:292`, App sensors `app.cpp:455`, SceneWiring `:202`, NetWiring `:87` | WS | state |
| `power.charging.changed` | `PowerMonitor`, `power_monitor.cpp:138-150` | App notice `app.cpp:279`, SceneRuntime `:190` | WS | state |
| `power.battery.low` | `PowerMonitor`, `power_monitor.cpp:157-168` | App notice `app.cpp:268` | WS | edge (threshold crossing) |
| `power.wakelock` | `WakeLockRegistry::publish()`, `wakelock.cpp:159-169` | none | WS | telemetry |
| `notification.posted/dismissed` | `NotificationService`, `notification_service.cpp:135-157` | StatusShell `:275` (doorbell) | none | notification |
| `storage.sd.inserted/removed` | `StorageManager::refresh()`, `storage_manager.cpp:1067-1078` | SceneWiring `:105` | WS | state |
| `system.boot.completed` | `App::start()`, `app.cpp:876` | none | WS | notification |
| `system.factory_reset` | `factory_reset.cpp:36-38` | SystemDuties `:91` | WS (System page, `web/app/pages/system.js:1239`) | command + progress |
| `system.firmware` | `FirmwareUpdateService::publish()`, `firmware_update.cpp:136-141` via `app.cpp:502-504` | SystemDuties `:93` | WS (`system.js:993`) | command + progress |
| `system.settings` | `SystemSettings` `on_changed` → `app_impl.hpp:94-96` | App `app.cpp:414`, AgentWiring `:484`, VoiceWiring `:34`, SceneRuntime `:193`, PowerManager `:49`, SystemDuties `:88` | WS | state |
| `system.time` | `TimeSyncService`, `time_sync.cpp:141-143`; simulator `clock-advance`, `simulator/main.cpp:219` | Reminders `reminders.cpp:172`, SceneRuntime `:224`, NetWiring window `:92` | WS (`system.js:640`) | notification (attempt result) |
| `net.wifi.enabled` | `WifiService`, `wifi_service.cpp:496` | StatusShell (prefix `net.wifi.`) | WS | state |
| `net.wifi.state` | `WifiService::set_state_locked()`, `wifi_service.cpp:238-258` | SystemDuties, SelfCheck, StatusShell, NetWiring, AgentService `agent_service.cpp:1012` | WS | state |
| `net.ap.state` | `WifiService::publish_ap_locked()`, `wifi_service.cpp:727-731` | SystemDuties, SelfCheck, NetWiring | WS | state |
| `net.pairing.code` | `PairingService`, `pairing_service.cpp:255-260, 294-296` | SystemDuties | WS | state |
| `net.pairing.failed` | `PairingService`, `pairing_service.cpp:375` | none | WS | notification |
| `scene.state` | `SceneRuntime::publish()`, `scene_runtime.cpp:659-668`; `SceneWiring::publish()` (`deleted`), `scene_wiring.cpp:428-438` | App recents/notices `app.cpp:287`, tool index `agent_wiring.cpp:343`, SelfCheck `:137`, Shell `shell.cpp:122`, StatusShell `:274` | WS (`web/app/pages/scenes.js:110`) | state + lifecycle edges |
| `model.profiles` | `ModelProfileService::publish()`, `model_profiles.cpp:957-961` | **none** | **none** (not forwarded) | notification |
| `agent.state` | `AgentService` `set_state()`, `agent_service.cpp:245-261` | Home `home.cpp:370` | **none** (the web uses the WS `agent` channel, `docs/architecture.md:129`) | state + transient |
| `voice.state` | `VoiceService::publish()`, `voice_service.cpp:424-444` | Home `:371`, Shell `shell.cpp:138`, ConversationController `conversation.cpp:64` | **none** | state + transient + telemetry (`level`) |
| `memory.changed` | `MemoryStore`, `memory_store.cpp:180` | none | WS (`memory.js:90`) | notification |
| `todo.changed` | `ReminderScheduler`, `reminders.cpp:372` | none | WS (`memory.js:203`) | notification |

One documentation gap the registry will catch: `scene.state` also carries `headless` (`scene_runtime_tools.cpp:292`, handled at `agent_wiring.cpp:394`), which `docs/architecture.md:127` does not list.

## 2. Goals and non-goals

### 2.1 Goals

1. A subscriber to state always converges to the latest value: however slow it is, however busy its thread, whenever it subscribes. Nothing a subscriber depends on is "dropped".
2. Commands and input edges reach their handler exactly once, in order, or the loss is counted and reported (never silent).
3. Every compensation in section 1.3 that exists only because of drops is deleted, and every mirror in section 1.4 reads the owner's state instead.
4. The admin WebSocket `events` stream and the scenes' `wisp.device` events keep their wire formats.
5. No new periodic work, no new task, no new allocation in internal RAM per change.
6. One registry of topics; `docs/architecture.md` cannot drift from it.

### 2.2 Non-goals

- **Settings schema** (phase E): this phase puts today's `core::SystemSettingsValues` in a cell; E replaces that type with the schema-generated snapshot, the derived JSON schema, the web form and the defaults guard.
- **Power optimisation** (phase F): the 250 ms `wisp.net` loop, the 1 s `wisp.power` tick and the 2 s SD probe stay. This phase makes it *possible* for F to remove them (every input they poll becomes a cell or a non-lossy wake), but does not remove them.
- **Composition and UI moves** (phase L): no code moves between components; `SystemDuties`, `SelfCheck` and the status shell stay in `wisp_app`. PowerManager stays in `wisp_app`.
- The scene runtime's own message queue and completions (phase C), scene input APIs (phase I), the Lua API and the REST API.
- UI timers that only run while a page is visible (the settings page's 1 s refresh of Wi-Fi and model names, `settings_page.hpp:28-33`; the diagnostics storage measurement, `self_check_screen.cpp:46-49`). They are not drop compensations and do not run in standby.
- Removing the event bus, changing its capacity or its drop policy.

## 3. Design

### 3.1 Three paths, one rule each

| Path | For | Delivery | Loss |
|---|---|---|---|
| `core::StateCell<T>` | state other components follow | latest value, coalesced, on the subscriber's dispatcher | none: a subscriber always ends at the latest value |
| `core::EdgeInbox<E, N>` | input edges and lifecycle edges whose every occurrence matters | every edge, in order, on the consumer's dispatcher | bounded; overflow counted, logged, shown by `mem` |
| `core::EventBus` | notifications and telemetry; the legacy mirror of every cell | best effort, drop-oldest | allowed by definition |

The rule that replaces the compensations: **no component folds bus events into a copy of another component's state.** State is read from its cell (`get()`/`read()`) and followed by subscribing to the cell; the bus is for things nobody's correctness depends on. This goes into AGENTS.md section 3 (section 6).

### 3.2 `core::Dispatcher`: where callbacks run

Callbacks never run on the setter's thread. Setters often hold their own locks: `SystemSettings::set()` calls `on_changed` with its lock held (`system_settings.hpp:102-105`, `system_settings.cpp:174-208`); `WifiService` publishes from `*_locked` functions (`wifi_service.cpp:238-258`, `:727-731`); `PairingService` under `mutex_` (`pairing_service.cpp:255-296`); PowerManager inside a transition holding `transition_mutex_` (`power_manager.cpp:325-431`). Calling subscribers inline would run their code under those locks.

A small interface in `wisp/core/dispatcher.hpp`:

```cpp
namespace wisp::core {

// A unit of work owned by its scheduler (a cell subscription, an inbox).
// Intrusive: scheduling never allocates.
struct DispatchSlot {
    void (*run)(DispatchSlot&) = nullptr;
    DispatchSlot* next = nullptr;   // the dispatcher's ready list
    uint8_t flags = 0;              // queued / running; the dispatcher's lock guards it
};

class Dispatcher {
public:
    virtual ~Dispatcher() = default;
    // Any thread. Queues `slot` to run once on this dispatcher's thread. Never
    // blocks on the dispatcher, never allocates, never fails. A slot already
    // queued stays queued once (this is the coalescing).
    virtual void schedule(DispatchSlot& slot) = 0;
    // Removes `slot`. Off the dispatcher's thread, returns once a run of
    // `slot` in progress has returned; on it, returns at once.
    virtual void cancel(DispatchSlot& slot) = 0;
    virtual bool on_dispatch_thread() const = 0;
};

}  // namespace wisp::core
```

Two implementations:

- **`EventBus`** runs slots on `wisp.bus`. The dispatch loop changes from "one token, one event" to "on every wake, run every queued slot, then every queued event, until both are empty"; `ready_` tokens become hints. This is required, not cosmetic: `ready_` saturates at `capacity + 1` (`event_bus.cpp:30`), so with slot tokens added a token-per-event count could strand an event (the comment at `event_bus.cpp:244-253` describes exactly that failure). `wait_idle()` also waits for queued and running slots, so the factory-reset and firmware restarts that wait for the bus (`factory_reset.cpp:80`, `firmware_confirm.cpp:70`) still see their last mirror event delivered.
- **`UiRuntime`** runs slots on the UI task at the top of every loop iteration. In the active loop that is at most 10 ms later (`ui_runtime.cpp:264-265`). In the paused loop the task blocks on its post queue (`ui_runtime.cpp:206-226`), so `schedule()` from another thread while paused also pushes a no-op post; if that push fails the queue is non-empty and wakes the task anyway, the argument `UiRuntime::stop()` already relies on (`ui_runtime.cpp:680-687`). Slots run in the paused branch too.

Default: a cell subscription without a dispatcher runs on `wisp.bus` (the roadmap's decision). A UI subscriber passes the UI runtime, so it is called on the UI thread directly instead of posting from `wisp.bus` into the lossy 32-entry post queue. Headless (no `UiRuntime`), the same subscribers pass nothing and run on `wisp.bus`.

Alternatives rejected:
- *Call subscribers inline in `set()`*: runs subscriber code under the setter's locks (above); a deadlock waiting to happen.
- *Every subscriber on `wisp.bus`, UI subscribers post as today*: keeps the 32-entry post queue as a lossy hop for state, so the "post(...) dropped" paths of section 1.4 stay.
- *A per-cell task*: a task per cell costs a stack each and gives nothing over one dispatcher.

Phase L's PowerManager needs "run this on the UI" without `ui::UiRuntime*` (roadmap L). `core::Dispatcher` is that interface; this spec recommends L reuse it rather than add `os::Executor`.

### 3.3 `core::StateCell<T>`

`wisp/core/state_cell.hpp`:

```cpp
namespace wisp::core {

template <class T>
class StateCell {
    static_assert(std::is_copy_constructible_v<T>);
    static_assert(sizeof(T) <= 256, "a callback copies T onto the dispatcher's stack");
public:
    using Callback = std::function<void(const T&)>;
    using Mirror = void (*)(EventBus&, const T* before, const T& after);

    // `bus` runs subscriptions that name no dispatcher. `mirror`, when set,
    // publishes the legacy bus event(s) for each change (3.6).
    StateCell(EventBus& bus, T initial, Mirror mirror = nullptr);
    ~StateCell();  // live subscriptions: abort in the simulator, WLOG_E on the device

    // --- readers: any thread -------------------------------------------
    T get() const;                                    // a copy under the cell's lock
    template <class F> auto read(F&& f) const;        // f(const T&) under the lock; short, no calls out
    uint32_t revision() const;                        // atomic; moves on every change

    // Calls `cb` with the current value once, soon, then after every change,
    // on `on` (nullptr: the bus's dispatcher). Coalescing: a callback always
    // gets the latest value; values set while it was queued or running are
    // skipped, never delivered late. Same subscription: never concurrent,
    // always in revision order.
    [[nodiscard]] CellSubscription subscribe(Callback cb, Dispatcher* on = nullptr) const;

    // --- owner only ----------------------------------------------------
    bool set(const T& v);                             // false (and nothing happens) if v == current
    template <class F> bool update(F&& mutate);       // read-modify-write under the lock
    bool seed(const T& v);                            // set() without the legacy event (3.6)
};

// Move-only. Destruction or reset() unsubscribes: off the dispatcher thread
// it waits for this subscription's callback in progress (only this one);
// inside its own callback it returns at once and no further call happens.
class CellSubscription { /* ... */ };

}  // namespace wisp::core
```

Semantics:

- **Revision.** `uint32_t`, starts at 1, increments on every `set`/`update`/`seed` that changes the value. 32 bits because 64-bit atomics are not lock-free on the ESP32-S3; wrap-around only matters for equality and is harmless.
- **Delivery.** Each subscription holds a `DispatchSlot` and the revision it last delivered (0 at first). `set()` stores the value, bumps the revision and calls `schedule()` on each subscription's dispatcher. The slot, when it runs, copies `(value, revision)` under the cell's lock, returns if the revision equals the one delivered, else records it and calls the callback with the copy, outside every lock. A `set()` during the callback re-queues the slot (it was unlinked before running), so the last change is always followed by one more run that reads it. That is the convergence argument, and it holds whatever the dispatcher's delay.
- **Equality.** `T` needs `operator==` (defaulted). An equal `set()` is a no-op, so owners may call `set()` from a periodic sample without causing work. Owners keep their own change conditions where today's events have them (for example `WifiService` publishes on state/ssid/ip/reason changes but not on RSSI, `wifi_service.cpp:242-243`; it keeps calling `set()` only then).
- **A callback means "changed since your last call".** With A→B→A between two runs, the subscriber is called with A again. Subscribers must be idempotent; all of today's are (they re-read or store).
- **Subscribing late.** The first run delivers the current value, so "subscribe, then read the seed" (`app.cpp:410-444`, `voice_wiring.cpp:32-46`, `agent_wiring.cpp:480-496`) collapses to one `subscribe()`. Constructors that need the value synchronously call `get()` as well.
- **Reentrancy.** From a callback: `get`/`read` of any cell, `set` of any cell (including its own — the subscription runs again; a subscriber that always sets a new value loops, which is a bug), `subscribe`, and releasing any subscription, including its own.
- **Locks.** A cell's lock and a dispatcher's ready-list lock are leaf locks: nothing is called while either is held, except the mirror (3.6), which only calls `EventBus::publish()` (itself a leaf: `publish_mutex_` and a non-blocking queue push, `event_bus.cpp:151-187`). The documented order is `cell → bus publish_mutex_`, and `cell → dispatcher ready list`. No path takes them the other way round.
- **Writers.** A cell is written only by its owner. Every other component receives `const StateCell<T>&`, which exposes no `set`.

Alternatives rejected for the type:

- *`StateCell<Value>`*: every get would be a map copy in internal RAM, and subscribers would keep parsing strings, the problem this phase removes.
- *A seqlock for trivially copyable `T`*: lock-free reads are not needed (all readers are tasks, reads are rare) and `read(F)` would have no safe equivalent.
- *One lock shared by all cells* to save kernel objects: makes `read(F)` and mirrors in different cells nest on one non-recursive lock. Per-cell locks cost about 100 bytes of internal RAM each (an `os::Mutex` is a FreeRTOS mutex created with `xSemaphoreCreateMutex()`, `os_esp.cpp:150-151`); with the 14 cells of section 3.4 that is about 1.4 KB, measured on the device (section 7).

**Memory.** The cell object lives inside its owner, so in `Mem::Large` wherever the owner is (most are `os::make_large` or members of `App::Impl`). Subscriptions are allocated with `os::LargeAllocator`. `set()` copies `T` and touches intrusive lists: no allocation. Snapshot structs that change often use fixed-capacity strings (`core::FixedString<N>`, a new trivially copyable helper in `wisp_base`) so a copy allocates nothing: SSIDs (32 bytes), IPv4 addresses (15), scene ids (`kMaxSceneIdBytes = 32`, `scene_manifest.hpp:79`), dotted tool names (72: `scene.` + 32 + `.` + 32 by `valid_tool_name()`, `scene_manifest.cpp:34, 208-220`; a longer unknown name from the model is truncated in the status only). The one exception is the settings snapshot, which keeps `std::string` for `language` and `ntp_servers` until phase E replaces the type; it changes only when a user saves settings, and readers on hot paths use `read()` instead of `get()`.

### 3.4 Which state becomes which cell

| Cell | Type (header) | Owner, set where today's event is published | Legacy topic(s) mirrored |
|---|---|---|---|
| power level | `hal::PowerLevel` | `PowerManager::set_level()`, `power_manager.cpp:427-431`; `PowerManager::level()` becomes a read of it | `power.state` |
| power supply | `core::PowerSupply{charging, usb_present, battery_percent, low}` (`power_monitor.hpp`) | `PowerMonitor` every sample (equal values are no-ops); the first sample is `seed()` | `power.charging.changed` when charging/USB changed; `power.battery.low` when `low` rises |
| Wi-Fi | `net::WifiStatus{enabled, state, ssid, ip, rssi, reason}` (`wifi_service.hpp`) | `WifiService` where it publishes `net.wifi.state` and `net.wifi.enabled` | both, each when its fields changed |
| hotspot | `net::ApStatus{active, ssid, clients}` | `WifiService::publish_ap_locked()` | `net.ap.state` |
| pairing | `net::PairingStatus{active, expires_ms}` | `PairingService` (code issued / lapsed) | `net.pairing.code` |
| settings | `core::SystemSettingsValues` (`system_settings.hpp`) | `SystemSettings::set()`, replacing `on_changed`; seeded by App after the settings cache is attached (`app.cpp:309-321`) | `system.settings` |
| time sync | `net::TimeSyncStatus{result, server, time, attempts}` | `TimeSyncService` at the end of each attempt (`attempts` increments) | `system.time` |
| SD card | `core::SdStatus{online, changes}` | `StorageManager::refresh()` | `storage.sd.inserted` / `storage.sd.removed` |
| firmware | `core::FirmwareStatus{phase, version, error, seq}` (`firmware_update.hpp`) | `FirmwareUpdateService` where it publishes | `system.firmware` (`received`/`confirmed`/`rejected`/`failed`) |
| factory reset | `app::ResetStatus{phase, seq}` (`factory_reset.hpp`) | `factory_reset.cpp` | `system.factory_reset` |
| scenes | `scene::SceneOverview{foreground, background_count, catalog}` (`scene_runtime.hpp`) | `SceneRuntime` whenever the foreground or background set changes, and on install/replace/delete (`catalog` increments; SceneWiring's delete calls `SceneRuntime::note_catalog_changed()`) | none (scene.state stays as it is, 3.5) |
| notifications | `core::NoticeSummary{unread, changes}` | `NotificationService` on post/dismiss | none (`notification.*` stays) |
| agent | `agent::AgentStatus{state, tool, errors}` | `AgentService` `set_state()` | `agent.state` (`errors` not on the wire) |
| voice | `voice::VoiceStatus{state, outcome, reason, outcomes}` | `VoiceService::publish()` for state changes only | `voice.state` without `level` |

> In the code: the scenes cell is `scene::SceneOverview{foreground, background_count, headless, catalog}`. `background_count` counts the entries of the background list only (never the scene in front), and `headless` counts the running headless scene (0 or 1). A headless scene sent to the background changes both fields, so the cell moves; with one summed count it would stay equal and a follower of the cell would keep a stale background list.

Decisions in this table that are not mechanical:

- **Transients become counted outcomes.** `AgentStatus::errors` increments each time the agent enters `error`; `VoiceStatus::outcomes` increments with each `error` (with `reason`) or `idle` with `result = not_heard`. A subscriber that coalesced `error → idle` into `idle` still sees the counter move and shows the hint (Home `error_at_`, `home.cpp:476-483`; ConversationController, `conversation.cpp:481-496`). Rejected: an inbox per transient (two mechanisms for one status) and "never coalesce these cells" (that is the bus again).
- **The microphone level leaves the bus entirely.** `VoiceService::level()` returns the latest level (an atomic set where `publish_level()` runs today). Home and the talk overlay read it in their existing `lv_timer`s, which run at their quick period (`kRefreshMs = 50`, `home.hpp:229`) while the voice state is listening or speaking. The roadmap proposed a `voice.level` bus topic; rejected because `voice.state` is not forwarded (`ws_hub.cpp:24-25`), its only level consumers are two UI animators that already have timers, and 20 events per second is the bus's main load (section 1.2). Those timers are UI timers, paused with the screen (`ui_runtime.cpp:206-212`), so no standby wake is added.
- **`scene.state` is one overview cell plus lifecycle edges, not a cell per scene.** The roadmap proposed per-scene cells. Rejected: the consumers that cannot lose a `scene.state` need *edges* — `deleted` (notices, recents, tool index) and `running` as "opened" (recents order) — which a per-scene coalescing cell loses by design (open then close → `stopped`), and per-scene cells would be created and destroyed with scenes. The consumers that need state need the whole runtime (what is in front, how many in the background, whether the catalog changed), which one cell gives. Edges go through an inbox (3.5).
- **Pairing, time sync, SD card, notifications, power supply** are not in the roadmap's list. Each has a consumer in section 1.4 that is wrong after one lost event, or a periodic resync (C2, C10). Each is a one-struct cell set where the event is published today.
- **`model.profiles` stays a plain bus event** (the roadmap listed it as a cell). It has no subscriber on the device and is not forwarded (section 1.5); `ModelProfileService::get()` already is the readable state, and Home reads readiness from it on its timer (`voice_wiring.cpp:80-91`, `home.cpp:663-694`). A cell can be added when a subscriber appears.
- **Factory reset and firmware are cells, not "direct calls with no state".** The roadmap said their pending flags go away. They become the cells: a request made before the screen exists has to be held somewhere (C4's reason), and a cell holds it and delivers it at subscription. `reset_pending`, `firmware_pending`, `take_*_request()`, `Deps::request_confirm` and the 1 s resync go; `FirmwareUpdateService` keeps a `bool confirmable` in place of `request_confirm` for its "no screen" refusal (`firmware_update.cpp:148-150`). The request itself is a direct call on the service (`request_factory_reset()`, the upload route), as the roadmap wanted; only the hand-over to the dialog is state.

### 3.5 Edges: `core::EdgeInbox<E, N>`

`wisp/core/edge_inbox.hpp`: a fixed ring of `N` trivially copyable `E` in `Mem::Large`, a `DispatchSlot`, and a callback. `push(e)` (any thread, never blocks) appends and schedules the slot; when full it drops the oldest, increments `overflows()` and logs a throttled `WLOG_W`. The slot drains every queued edge in order on the consumer's dispatcher. Overflow requires the consumer's dispatcher to stall for `N` edges; it is never silent.

Two uses in this phase:

1. **Buttons.** `ButtonService::add_listener(fn)` calls `fn(const ButtonEdge&)` on `wisp.button`, in order, for every `down`/`up`/`short`/`long`, with the edge's own timestamp. Listeners only push into their inbox:
   - SystemDuties: inbox on the UI dispatcher; drains into `PowerKeyController` (today a lossy post, `system_duties.cpp:194-215`).
   - PowerManager: notes activity (an atomic, as today) and, for a wake from standby, sets a wake latch and schedules a UI slot that calls `exit_standby()` (today a lossy post, `power_manager.cpp:76`, `:112-122`).
   - TalkKey: inbox on `wisp.bus` (where its logic runs today); the hold is timed from the edge's timestamp instead of the dispatch time (`talk_key.cpp:63-68`).
   - SelfCheck's button test: listener with a plain post (a diagnostics display; a loss is harmless).
   `N = 8` per consumer. The `button.*` bus events stay as an observable mirror (no system consumer); phase I decides how scenes get key input.
2. **Scene lifecycle.** `SceneRuntimeDeps::on_lifecycle` receives `SceneTransition{id, state}` for each state the runtime publishes, and SceneWiring adds its `deleted`. App feeds one inbox (`N = 32`, enough for restoring background scenes at boot) drained on `wisp.bus`, which runs today's handlers: recents `opened`/`forget` and `dismiss_source` (`app.cpp:287-304`), tool index refresh/remove (`agent_wiring.cpp:376-397`). Rejected: reconciling from the overview cell's `catalog` counter (reread every manifest and the notice list on each change): costs a full manifest walk per install and still cannot express "opened".

Rejected for edges in general: a second, non-lossy bus lane (publishers would have to block or fail when it is full, and the publishers hold locks, section 3.2); synchronous listeners only (they would run consumer code, such as the talk key's microphone priming, on `wisp.button` and delay debouncing of the other keys).

### 3.6 The legacy mirror: wire formats unchanged

A cell with a `Mirror` calls it inside `set()`/`update()`, under the cell's lock, with the previous and new value; the mirror builds the same `Event` today's code builds and calls `EventBus::publish()`. Under the lock so that concurrent setters produce events in revision order; `publish()` never blocks (`event_bus.hpp:134-142`). `seed()` does not call the mirror, which is how "the first sample is not a change" (`docs/architecture.md:114`) and "nothing is published at boot" stay true.

Consequences:

- `WsHub` keeps subscribing by prefix (`ws_hub.cpp:91-94`) and sees the same topics, payloads and order as today, still best effort as today; the web pages (`scenes.js:110`, `network.js:100-106`, `system.js:640, 993, 1239`, `memory.js:90, 203`) need no change.
- The scene runtime's `wisp.device` events (minute, battery, lang) are produced by the runtime from cell callbacks instead of bus events; what scenes receive is unchanged.
- The mirror is per change, not coalesced. Coalescing the mirror would change what the web sees (for example `done` must precede the wipe, `factory_reset.cpp:73-83`).
- `voice.state` loses its `level` field. It is not forwarded and its only readers move to `VoiceService::level()`, so this changes no external format; `docs/architecture.md:130` is updated.
- Payload builders live next to the snapshot types (`net::to_payload(const WifiStatus&)` in `wisp_net`, and so on), not in `wisp/core/topics.hpp`: core sits below net, voice, agent and scene and cannot name their types (AGENTS.md section 3). The roadmap's "parsers" are dropped: after this phase no component on the device parses a legacy payload (section 3.8), and tests compare payload maps directly.

### 3.7 Topic registry: `wisp/core/topics.hpp`

```cpp
namespace wisp::core::topics {
enum class Kind : uint8_t { CellMirror, Notification, Telemetry, InputEdge };
struct Topic { std::string_view name; Kind kind; bool forwarded; };

inline constexpr std::string_view kPowerState = "power.state";
// ... one constant per topic; "button.*" as the pattern "button.<id>.<edge>"
inline constexpr Topic kAll[] = {
    {kPowerState, Kind::CellMirror, true},
    // ...
};
}  // namespace wisp::core::topics
```

- Every `publish()` and `subscribe()` in `components/` uses a constant (no topic string literal elsewhere).
- `WsHub::kForwardedPrefixes` stays as it is; a unit test checks that `forwarded` in `kAll` matches what the prefixes forward, so the two cannot disagree.
- `tools/check_topics.py` (run by `tools/ci.sh` next to `check_layering.py`, with its own `check_topics_test.py`) checks that the set of names in `kAll` equals the first column of the "Event topics" table in `docs/architecture.md`, and that no string literal that looks like a registered topic appears in a `publish(`/`subscribe(` call outside `topics.hpp`. Checked rather than generated: the table's notes column is prose that a generator could not produce.

### 3.8 Every subscriber, after

| Today | After |
|---|---|
| PowerManager `system.settings` + C1 | settings cell on `wisp.bus`: stores `screen_off_ms_`/`deep_after_ms_` from the value (no Settings read at all); `settings_dirty_`, `ticks_since_read_`, `kSettingsRefreshTicks` and the Settings read in `tick()` deleted |
| PowerManager `button.` | button listener (3.5) |
| SystemDuties `button.` | button inbox on the UI dispatcher |
| SystemDuties `system.settings` + C3 | settings cell on the UI dispatcher → `sync_system_language()`; the per-100 ms revision check deleted |
| SystemDuties `net.` + C2 (Wi-Fi, pairing) | Wi-Fi, hotspot and pairing cells on the UI dispatcher → `sync_wifi()`/`sync_pairing()` (which still read `ap_session()` and `active_code()` for the password and code, never in a payload); the 1 s resync deleted. The 100 ms timer stays for the cards' countdowns (`system_duties.cpp:278-279`) |
| SystemDuties `system.factory_reset`, `system.firmware` + C2, C4, C5 | reset and firmware cells on the UI dispatcher: `requested`/`awaiting` with a new `seq` opens the dialog, any other phase closes it; flags, `take_*`, start-time sync and resync deleted |
| SelfCheck `power.state` | power cell → `sampler_.set_standby()` |
| SelfCheck `net.` + C6 | Wi-Fi and hotspot cells on the UI dispatcher → `sync_net()`; 1 s resync deleted |
| SelfCheck `scene.state` + C6 | scenes cell on the UI dispatcher: `catalog` changed → relist; `foreground` changed → `follow_foreground()`; 30 s resync deleted (the error-panel close now changes `foreground`) |
| SelfCheck `sync_settings()` + C7 | settings cell on the UI dispatcher; poll deleted |
| SelfCheck `button.` | button listener |
| App `notify_vol`, `utc_offset_min`, `lang`, `time_24h`, `settings_now`, `merge_settings()` | deleted; readers use `settings.read(...)` |
| App `power.state` → sensors | power cell |
| App `power.battery.low`, `power.charging.changed` | power-supply cell: `low` rising posts the notice, charging rising dismisses it |
| App `scene.state` | lifecycle inbox |
| AgentWiring `system.settings` | settings cell |
| SceneToolIndexObserver `scene.state` | lifecycle inbox |
| VoiceWiring `tts_mute_`, `volume_` | deleted; reads of the settings cell |
| SceneRuntime `power.charging.changed`, `system.settings`, `system.time` | power-supply, settings and time-sync cells; the callbacks set bits in an atomic `pending_device_` mask and push a wake, so a full scene queue delays but no longer loses a change (the same pattern as `want_visible_`, `scene_runtime.cpp:421-429`); `utc_offset_min_`/`time_24h_`/`lang_` read from the cell |
| SceneWiring `power.state` (C11) | power cell; unchanged logic |
| SceneWiring `storage.sd.*` | SD cell |
| NetWiring `power.state`, `net.wifi.state`, `net.ap.state`, `system.time` (C9) | the four cells, each callback only `wake_.give()` as today (plus `note_time_result()` when `attempts` moved); the `wifi_state_` atomic snapshot (`net_wiring.cpp:70, 271`) is replaced by reads of the Wi-Fi cell. The 250 ms loop and the level read stay (phase F) |
| NetWiring C10 | the power-supply cell wakes the loop; the read in `reconcile_external_power()` reads the cell. The poll's removal is phase F |
| AgentService `net.wifi.state` | Wi-Fi cell. A coalesced `connected → connecting → connected` no longer aborts the request; the stall check (`agent_service.cpp:153-158`) still ends a stream on a dead link |
| Reminders `system.time` | time-sync cell (`attempts` moved and `result == synced` → `queue_pass()`) |
| StatusShell (C12) | scenes, notifications, power and Wi-Fi cells on the UI dispatcher → `refresh()` / `close_panel()`; `refresh_queued` coalescing and the "UI queue full" path deleted |
| Home `agent.state`, `voice.state`, `power.state` | agent, voice and power cells on the UI dispatcher; level from `VoiceService::level()` |
| Shell `scene.state`, `voice.state` | scenes and voice cells on the UI dispatcher |
| ConversationController `voice.state` | voice cell on `wisp.bus` (outcomes by counter) |
| TalkKey `button.<talk>.` | button inbox on `wisp.bus` |
| WsHub (all forwarded prefixes) | unchanged |

Also deleted: `SystemSettings::revision()` and the `on_changed` constructor parameter (C8; the cell replaces both); `FirmwareUpdateService::Deps::publish` (the cell's mirror publishes). After this table the only device-side bus subscriber is `WsHub`.

### 3.9 Diagnostics

The serial `mem` command prints `bus dropped=<n>`, each inbox's `overflows`, and the number of live cell subscriptions. `dropped()` is otherwise never surfaced (section 1.1), so today nobody can tell whether drops happen on a device.

## 4. Migration

Each step leaves `bash tools/ci.sh` green and the device behaviour unchanged unless stated.

1. **Primitives.** `FixedString`, `Dispatcher`, `EventBus` as a dispatcher (drain-all loop, `wait_idle()` covering slots), `UiRuntime` as a dispatcher, `StateCell`, `CellSubscription`, `EdgeInbox`, `topics.hpp` with today's topics, `tools/check_topics.py`, the `mem` counters. No caller changes.
2. **Cells beside events.** Each owner gets its cell and sets it where it publishes today; the hand-written publish becomes the cell's mirror, payload builders move next to the types. Existing tests that assert published events stay green unchanged, which is the proof the wire format did not move.
3. **Subscribers move**, one file per commit, following section 3.8. Tests that drive a subscriber by publishing a topic (for example `power.state` 13 references, `system.settings` 19, `scene.state` 24 in `tests/`) switch to setting the owner's cell through its existing API or a test seam. Folded mirrors (section 1.4) are deleted in the same commit as their subscription.
4. **Edges.** `ButtonService` listeners and the four button consumers; the scene lifecycle inbox and its two consumers. Tests that drive the power key with `button.power.*` events (about 140 references to `button.power*` in `tests/`) use a listener seam.
5. **Compensations removed** (last, once nothing depends on them): C1, C2's resync and C3, C4, C5, C6's resyncs, C7, C8, `merge_settings()`, the `voice.state` `level` field, the `wifi_state_` snapshot. Tests that proved a compensation ("a dropped event is caught up within a second") are rewritten to prove convergence without one (section 5).
6. **Documents** in the same commits as the behaviour they describe: `docs/architecture.md` (event-topics table gains a Kind column and the cell for each topic; the bus paragraph; the `voice.state` payload; `headless` in `scene.state`; the `mem` fields), AGENTS.md section 3 (the rule of 3.1) with a `docs(agents): ...` commit, and the connectivity spec's note on `system.settings` resync.

## 5. Testing

GoogleTest in the simulator, written before the code (AGENTS.md section 5).

`StateCell` / `Dispatcher` / `EdgeInbox` unit tests:

- Late subscriber: subscribing after 5 sets delivers the current value once.
- Slow subscriber: the callback blocks on a semaphore while another thread sets 1000 values; after release the subscriber receives exactly one more call, with the last value; revisions it sees increase strictly.
- Two dispatchers (the bus and a test dispatcher standing in for the UI): each subscription runs only on its own; the test dispatcher stalled does not delay the bus's subscribers.
- Equal set: no revision change, no callback, no mirror event.
- Reentrancy: set/subscribe/unsubscribe from inside a callback, including unsubscribing itself (no further call).
- Unsubscribe from another thread while the callback runs: returns only after it returns; does not wait for an unrelated slow subscription (the difference from `EventBus::unsubscribe()`).
- Lock order: the setter holds mutex M while calling `set()`; the subscriber's callback takes M. No deadlock (the callback runs after `set()` returns).
- Destroying a cell with a live subscription aborts in the simulator (death test); so does calling `get()` on a cell from inside its own `read(F)`.
- Mirror order: 4 threads set concurrently; the mirror events on the bus are in revision order and equal in number to the changes.
- Saturated bus: a publisher floods 10 000 telemetry events (the bus drops) while a cell changes 100 times; every cell subscriber ends at the last value, and `wait_idle()` returns only after the last slot ran.
- `EventBus` drain-all: with the token count saturated, no event is stranded (the case of `event_bus.cpp:244-253`).
- `UiRuntime` paused: a slot scheduled while paused runs without any post from the test; with a full post queue it still runs.
- `EdgeInbox`: order kept; overflow drops the oldest, counts it, logs once per throttle window.

Integration tests (App in the simulator), each with the bus flooded during the action:

- Factory reset requested before `SystemDuties::start()` → the dialog opens at start; requested while flooded → opens on the next UI iteration; web events `requested`/`cancelled`/`done` unchanged.
- Firmware received → dialog; declined via timeout → dialog closes; events unchanged.
- Language change → system UI, scenes and agent language follow; no 100 ms poll exists.
- Power level to standby and back → Home, status panel, sensors, sampler, scenes and `wisp.net` follow.
- Talk key down/up with the bus flooded → capture starts and ends; the hold is timed from the edge timestamp.
- Scene delete → notices dismissed, tool index entry removed, recents forgotten.
- Golden payloads: for every `CellMirror` topic in `kAll`, the mirror's payload equals today's (captured before step 2).
- `tools/check_topics_test.py`: a missing table row, an extra row and a literal topic in a `publish(` each fail.

## 6. Documents to change (with the implementation)

- AGENTS.md section 3, a new bullet: state that other components follow is a `core::StateCell`; the bus carries notifications and telemetry, which may be lost; never fold bus events into a copy of another component's state; callbacks follow the dispatcher they were given (`wisp.bus` by default, the UI runtime for UI code) and must not touch flash.
- `docs/architecture.md`: event-topics table (Kind column, cell per topic), the event-bus paragraph (`docs/architecture.md:136`), the `wisp.bus` task row (`docs/architecture.md:77`: it also runs cell callbacks), `voice.state` payload, `scene.state` `headless`, the `mem` output.
- This spec's status line once approved.

## 7. Risks

- **Reentrancy and lock order.** A mirror runs under the cell's lock; if a future mirror calls anything but `EventBus::publish()`, the leaf-lock rule breaks. The mirror type is a plain function pointer taking `EventBus&` to keep it narrow; review guards the rest. `read(F)` runs caller code under the cell's lock; the header states "short, no calls out", and in the simulator the cell's lock records its owner thread so that re-entering the same cell from inside `read(F)` aborts with the call site instead of deadlocking (a death test covers it).
- **Shutdown.** Subscriptions must be released before their cells' owners are destroyed. Today the same order is needed for bus subscriptions, but a mistake was silent (the bus outlives everyone); now it aborts in the simulator. App's 46 hand-ordered stops (roadmap L) are the place where this can go wrong; the plan walks `App::stop()` against the owner/subscriber pairs of section 3.8. After `EventBus::stop()` or `UiRuntime::stop()`, no slot runs and `cancel()` never waits.
- **A dispatcher busy for a long time.** Cell subscribers wait, then get one call with the latest value; nothing piles up (one slot per subscription). Inbox edges wait and, past `N`, overflow visibly. Telemetry drops as today. A UI subscriber now waits for the UI loop rather than for `wisp.bus`; a long LVGL frame delays it, which is also true of today's posts.
- **Memory.** Per cell: `T` plus a FreeRTOS mutex (about 100 bytes of internal RAM), so about 1.4 KB internal for 14 cells. Per subscription: about 48 bytes in PSRAM (slot, revision, `std::function`). Per inbox: `N * sizeof(E)` in PSRAM. Against that, the 20-per-second `voice.state` allocations leave internal RAM during voice turns, and the bus carries far fewer events. Needs `mem` on the device (owner).
- **Coalescing hides a transition someone needed.** Section 3.4 reviews every subscriber: transients are counters; edges are inboxes; the agent's network abort is backed by the stall check. A future subscriber that needs every transition must use an inbox, which the AGENTS.md rule states.
- **Callbacks on `wisp.bus` that block** delay every default subscriber (as today for bus handlers). The rule "callbacks do only memory operations" (`docs/architecture.md:77`) stays.

Cannot be verified in the simulator (device checks for the owner, written into the plan):

1. `mem` at idle and in the worst case (STA+AP, two httpd, WebSocket log stream, TLS, OTA) against `ram_budget.hpp`: internal free and largest block, `wisp.bus` and UI stack high-water marks.
2. `WISP_PERF` during a spoken turn: Home and talk overlay animation are timer-driven now; frame rate not below the baseline.
3. Power key, talk key and button wake from standby and deep standby, including during a voice turn.
4. Factory reset and firmware update from the web page, with the dialog on the device and the page following the events.
5. `bus dropped=` and inbox overflows after a day of normal use.

## 8. Acceptance

- `StateCell`, `Dispatcher` (bus and UI), `EdgeInbox`, `topics.hpp` and `check_topics.py` exist with the tests of section 5.
- The cells of section 3.4 exist; every subscriber of section 3.8 uses them; `WsHub` is the only device-side bus subscriber.
- C1-C8, the folded mirrors of section 1.4, `merge_settings()`, `SystemSettings::revision()`, `reset_pending`, `firmware_pending` and the `voice.state` `level` are gone.
- The admin WebSocket event stream is byte-for-byte what the golden tests captured.
- `bash tools/ci.sh` green; documents of section 6 updated; device checks of section 7 listed for the owner.

## 9. Changes to the roadmap's decisions

| Roadmap (phase D) | This spec | Why |
|---|---|---|
| Dispatch on `wisp.bus` | default `wisp.bus`; a subscriber may name the UI runtime | UI subscribers otherwise hop through the lossy 32-entry post queue (section 1.4) |
| `scene.state` per scene | one `SceneOverview` cell plus a lifecycle inbox | the critical consumers need edges (`deleted`, opened), which a coalescing cell loses (3.4) |
| `agent.state`, `voice.state` (state only) as cells | as cells, with `error`/`not_heard` as counted outcomes | `error` is followed at once by `idle`; coalescing would erase it (1.4) |
| `voice.level` topic | `VoiceService::level()` read by the UI timers; no topic | no off-device consumer; it is the bus's main load (1.2, 3.4) |
| `model.profiles` cell | stays a bus event | no subscriber, not forwarded (1.5) |
| commands call services; pending flags go away | requests are direct calls; the hand-over to the dialog is a cell (`ResetStatus`, `FirmwareStatus`) | a request before the screen exists must be held; a cell holds it and delivers it at subscription (3.4) |
| power key calls service APIs directly | button listener + inbox for every button consumer | the power key, the talk key and the wake share one non-lossy path (3.5) |
| cells listed: power, Wi-Fi, AP, settings, scene, agent, voice, models | adds power supply, pairing, time sync, SD card, notifications, firmware, factory reset | each has a consumer wrong after one lost event or a periodic resync (1.3, 1.4) |
| builders and parsers in `wisp/core/topics.hpp` | names and kinds in `topics.hpp`; builders beside the types; no parsers | layering (core cannot name net/agent/voice types); no device consumer parses payloads afterwards |
| compensations removed: `kSettingsRefreshTicks`, self-check resync, `reset_pending` | also SystemDuties' 1 s resync and 100 ms language poll, self-check's settings poll, `SystemSettings::revision()`, `firmware_pending` | same cause (1.3) |
| (phase E) private settings caches replaced by cell reads | done here | they are the lossy mirrors this phase removes; each is a one-line read; E changes only the snapshot type |

## 10. Open questions for the owner

1. **UI delivery by the cell.** Should `UiRuntime` implement `core::Dispatcher` so UI subscribers are called on the UI thread directly? *Recommendation: yes.* Without it, every UI subscriber keeps a lossy post and its own "dropped" path, and C2/C6-style resyncs would be needed again.
2. **Button edges in this phase.** Move every button consumer to `ButtonService` listeners with per-consumer inboxes now, or leave buttons on the bus (much less loaded once `voice.state` levels are gone) and only move the power key? *Recommendation: now, all four consumers.* A lost talk-key `up` keeps recording; one path for all keys is simpler than two.
3. **Microphone level.** Accept a polled `VoiceService::level()` instead of the roadmap's `voice.level` topic? *Recommendation: yes* (section 3.4).
4. **Legacy topics without consumers.** After migration, `agent.state`, `voice.state`, `notification.*`, `button.*` and `model.profiles` have no device subscriber and are not forwarded. Keep publishing them (cheap, documented, useful in logs and for phase I) or delete them? *Recommendation: keep in this phase*; deleting documented topics is a separate decision with no benefit beyond a few events per minute.
5. **Settings caches in D, not E.** Accept moving the replacement of `merge_settings()` and the private caches into this phase? *Recommendation: yes* (section 9).

## Approval

The owner approved this spec on 2026-11-14, answering each question of section 10 with its recommendation:

1. UI delivery by the cell: yes. `UiRuntime` implements `core::Dispatcher`; UI subscribers' callbacks run on the UI thread, delivered by the UI runtime.
2. Button edges in this phase: yes. All four button consumers move to `ButtonService` listeners with per-consumer inboxes now.
3. Microphone level: yes. Home and the talk overlay read `VoiceService::level()`; there is no `voice.level` topic.
4. Legacy topics without a device subscriber: keep publishing them in this phase.
5. Settings caches: yes. `merge_settings()` and the private settings caches are removed in this phase, not in the settings-schema phase.
