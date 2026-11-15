# Settings Schema: Single Source of Truth — Design

- Date: 2026-11-15
- Status: approved by the owner on 2026-11-15 (see "Approval" at the end)
- Scope: phase E of `docs/superpowers/plans/2026-11-14-architecture-optimization-roadmap.md`. Every system setting is declared once, in a constexpr table; validation, defaults, the snapshot in the settings cell, the REST and WebSocket shapes, the agent's settings tools, the scenes' `wisp.device.info()` fields, the web System page's ranges and the Shell settings page's rows derive from it. A CI guard keeps default and range literals out of the rest of the tree.
- Builds on: the foundation spec (Settings, the 15-character key rule), the connectivity spec (`GET`/`PUT /settings`, the `system.settings` event, sections 5.4 and 8.3), the flash-thread design (`2026-11-06-flash-thread-design.md`, the settings cache), the agent-core spec 4.4 (`settings.get`/`settings.set`), the scene-runtime-core spec 5.2 (`wisp.device.info()`), the MVP spec 6.4 (the settings page), and the state-cells design (`2026-11-14-state-cells-design.md`: the settings cell exists and every private settings cache is already gone).
- Serves: phase F (its new `power/hibernate_s` key is one table row; PowerManager reads every power key from the snapshot) and phase L (the schema is infrastructure that stays in the core part when `wisp_core` splits; nothing in it names a domain type).
- Evidence: the code on `feat/settings-schema` at `5e2dec3`. Every `file:line` below refers to it.

---

## 1. Background and problem

### 1.1 What exists today

`core::SystemSettings` (`system_settings.hpp:116-165`, `system_settings.cpp`) is the validated front for twelve keys. Each key is described three times inside `wisp_core` alone:

- a comment table (`system_settings.hpp:4-17`);
- `inline constexpr` defaults and ranges (`system_settings.hpp:60-87`), plus two range functions (`valid_screen_off_s`, `valid_deep_after_s`, `system_settings.hpp:77-78`, `system_settings.cpp:127-129`);
- a private `KeySpec` table with a validator lambda per key (`system_settings.cpp:19-93`);
- the `SystemSettingsValues` struct with one member initializer per key (`system_settings.hpp:90-105`);
- `get()`, which reads each key by name (`system_settings.cpp:160-175`), and `to_map()`, which lists each key again (`system_settings.cpp:131-146`).

The snapshot lives in a `StateCell<SystemSettingsValues>` (`system_settings.hpp:139`, phase D). It holds two `std::string`s, `language` and `ntp_servers` (`system_settings.hpp:94, 96`); the header says its `get()` allocates because `ntp_servers` outgrows the short-string buffer (`system_settings.hpp:135-137`). The state-cells design left the type for this phase (state-cells design 2.2, 3.3).

Two more keys are read straight from the store, bypassing `SystemSettings`: `power/low_batt_pct` (`power_monitor.cpp:21, 169`) and `ui/perf_overlay` (`system_duties.cpp:137`, `self_check_screen.cpp:163, 435`).

Outside the Settings store, and not settings in this sense: Wi-Fi networks and their passwords live in `/net/wifi.json` (`wifi_service.cpp:28, 127-131`), paired browsers in `/net/clients.json` (`pairing_service.cpp:212`), model profiles with their API keys in `/secrets/models.json` (`model_profiles.cpp:24, 546, 927`). They are structured documents with their own services and validation.

### 1.2 Inventory

Fourteen keys. "Surfaces" lists who reads or writes the key besides `SystemSettings` itself: R = REST `GET`/`PUT /settings` and the `system.settings` event (they share `to_map()`); A = agent `settings.get`/`settings.set`; L = Lua `wisp.device.info()`; S = Shell settings page; W = web System page. "Repeated at" lists every place outside `system_settings.{hpp,cpp}` that restates the key's default, range, choices, label or validation.

| # | Name | ns / key | Type | Range | Default | Surfaces | Repeated at |
|---|---|---|---|---|---|---|---|
| 1 | `brightness` | `display/brightness` | int | 10-255 | 200 | R A S W | boot read of the raw store with default 200 and clamp 0-255, `app.cpp:381-385`; PowerManager's initial level 200, `power_manager.hpp:336-337`; Shell fallback 200, `settings_page.cpp:103`; Shell slider 10-255, `settings_page.cpp:216-217`, max 255 `:406`; web 10/255, `system.js:80-81`; agent description, `tools_device.cpp:84`; `docs/architecture.md:51` "(default 200)"; labels `AgentSetBrightness` and `SettingsBrightness` in each of `strings_{zh,ja,en}.cpp`, web `sys.brightness` (`zh.js:358`) |
| 2 | `volume` | `audio/volume` | int | 0-100 | 60 | R A S W | loopback reads the raw store with default 60, `loopback.cpp:27-29, 94-97`; clamp 0-100, `voice_wiring.cpp:27`; 100 when no volume dep, `voice_service.cpp:374, 633, 640`; Shell fallback, slider, max, `settings_page.cpp:104, 218-219, 407`; web `VOLUME_MAX`, `system.js:82`; agent description, `tools_device.cpp:84-85`; the ESP driver's own start volume 80, `esp_audio.cpp:25`, documented as overridden by the setting, `esp_audio.hpp:21-31`; simulator output 100, `sim_devices.hpp:163`; labels as row 1 |
| 3 | `notify_vol` | `audio/notify_vol` | int | 0-100 | 60 | R A S W | `kDefaultVolume = 60`, `notification_router.cpp:16, 54`; clamp, `app.cpp:105-109`; Shell, `settings_page.cpp:105, 220-221, 408`; web `?? 60`, `system.js:317`; agent description, `tools_device.cpp:85`; labels `AgentSetNotifyVol`/`SettingsNotifyVol`, web `sys.notify_vol` |
| 4 | `screen_off_s` | `power/screen_off_s` | int | 0 (never) or 15-3600 | 60 | R A S W | comment default, `power_manager.hpp:10`; re-validation, `power_manager.cpp:219-226`; Shell choices `{15,30,60,300,600,0}`, `settings_page.hpp:88`; web choices `[0,15,30,60,120,300,600]`, `system.js:84`; agent description, `tools_device.cpp:85`; `app.cpp:53` comment |
| 5 | `deep_after_s` | `power/deep_after_s` | int | 0 (never) or 60-3600 | 300 | R S W | comment default, `power_manager.hpp:14`; re-validation, `power_manager.cpp:222-223`; Shell choices `{60,300,600,1800,3600,0}`, `settings_page.hpp:89`; web choices `[0,60,120,300,600,1800,3600]`, `system.js:86` |
| 6 | `language` | `sys/lang` | `zh`/`ja`/`en` | — | `"zh"` | R A L S W | the code list, `system_settings.cpp:31-37` (duplicates `ui::parse_lang()`, `strings.hpp:600`); Shell fallback and list, `settings_page.cpp:108`, `settings_page.hpp:91`; fallback `Lang::Zh`, `app.cpp:101`; Lua `lang[4] = "zh"`, `scene_api.hpp:189`; web uses the browser-language list `LANGS` for it, `system.js:484`, `index.js:34-38` |
| 7 | `utc_offset_min` | `sys/tz_min` | int | -720 to 840 | 480 | R L S W | `status_shell.cpp:27, 179`; `reminders.cpp:22, 159`; `agent/context.hpp:60`; `scene_runtime.hpp:530`; `scene_api.hpp:190`; `self_check_screen.hpp:195`; Shell fallback, `settings_page.cpp:109`; offset list with city names, `settings_page.cpp:64-82`; web offset list, `system.js:91-95` |
| 8 | `ntp_servers` | `sys/ntp` | 1-2 hosts, each 1-63 of `[A-Za-z0-9.-]` | — | `"pool.ntp.org,ntp.aliyun.com"` | R W (and `TimeSyncService`) | validator, `system_settings.cpp:66-86`; web normalisation only, `system.js:553-559`; split, `time_sync.hpp:93`; entry length restated, `time_sync_result.hpp:23`, `time_sync.cpp:22` |
| 9 | `tts_mute` | `voice/tts_mute` | bool | — | false | R S W | Shell shows it inverted as "Spoken replies", `settings_page.cpp:222`; web, `system.js:324` |
| 10 | `time_24h` | `sys/time_24h` | bool | — | true | R A L S W | `self_check_screen.hpp:196`; `scene_runtime.cpp:2255`; `scene_api.hpp:191`; Shell fallback, `settings_page.cpp:110`; web `!== false`, `system.js:526`; agent note text, `tools_device.cpp:49` |
| 11 | `agent_max_steps` | `agent/max_steps` | int | 8-200 | 60 | R W | `agent::kMaxSteps = 60`, `limits.hpp:14`, tied by `static_assert`, `agent_wiring.cpp:25`; re-clamp, `agent_wiring.cpp:28-39`; web 8/200 and `?? 60`, `system.js:76-77, 719` |
| 12 | `agent_turn_min` | `agent/turn_min` | int | 2-60 | 15 | R W | `agent::kTurnMs = 900000`, `limits.hpp:15`, `agent_wiring.cpp:26`; re-clamp, `agent_wiring.cpp:33-38`; web 2/60 and `?? 15`, `system.js:78-79, 720` |
| 13 | `low_batt_pct` | `power/low_batt_pct` | int | none checked | 15 | none (store only) | `power_monitor.cpp:21, 169`; `power_monitor.hpp:43, 102`; re-read every 20 samples, `power_monitor.cpp:23-29` |
| 14 | `perf_overlay` | `ui/perf_overlay` | bool | — | false | diagnostics toggle | `system_duties.cpp:137`; `self_check_screen.cpp:163`; written without `SystemSettings`, `self_check_screen.cpp:435` |

Counting the rows: brightness alone is restated in 8 places in C++, JavaScript and docs, plus 9 label entries (two device ids in three languages, one web key in three); most keys are restated in 3 to 8 places. No consumer outside `wisp_core` reads a key's range from `wisp_core`, except PowerManager (`valid_screen_off_s`/`valid_deep_after_s`) and AgentWiring (`kMin/kMaxAgent*`).

### 1.3 Disagreements found

| # | What | Evidence | Effect |
|---|---|---|---|
| X1 | Boot brightness bypasses validation | `app.cpp:381-385` reads `display/brightness` from the raw store and clamps to 0-255; `SystemSettings::get()` reads a stored value outside 10-255 as 200 (`system_settings.cpp:102-110`); `app.cpp:499` hands the raw value to PowerManager | A stored 5 (hand-edited or from an older release) lights the panel at 5 and PowerManager dims relative to 5, while the cell, `GET /settings` and the Shell report 200 |
| X2 | Two volume stages, and the loopback test changes one for good | PcmPlayer and TonePlayer scale samples by the setting and leave the device volume alone (`pcm_player.hpp:14-16`, `voice_service.cpp:374, 633, 640`); the ESP output opens at its own 80 % (`esp_audio.cpp:25, 135, 169`); the only caller of `set_volume()` in the tree is the loopback self-check, which sets the device volume to the setting (`loopback.cpp:94-97`), and the driver keeps it for every later open (`esp_audio.cpp:219-221`) | Before a loopback test, speech at volume 60 plays at 0.8 × 0.6 = 48 % of full scale; after it, at 0.6 × 0.6 = 36 %, until reboot. `esp_audio.hpp:21-31` says the setting "overrides" the driver default "on the very next open", which is true only for the loopback. The simulator's output starts at 100 (`sim_devices.hpp:163`), so the simulator plays 60 % where the device plays 48 % |
| X3 | Screen-off and deep-standby choice lists | Shell `{15,30,60,300,600,0}` and `{60,300,600,1800,3600,0}` (`settings_page.hpp:88-89`); web `[0,15,30,60,120,300,600]` and `[0,60,120,300,600,1800,3600]` (`system.js:84, 86`) | 120 s can be picked on the web only; the Shell then lists it as an extra "current" entry (`settings_page.cpp:47-55`) |
| X4 | Brightness shown as a percentage two ways | Shell `value * 100 / 255` (`settings_page.cpp:28-30, 165`); web `(value - 10) / 245` (`system.js:274-276, 298`) | At 10 the Shell shows 4 %, the web 0 %; they agree only near the top |
| X5 | Labels for the same key | zh agent note "提示音量" (`strings_zh.cpp:322-323`) vs settings row "提醒音量" (`:594-595`) vs web "通知音量" (`zh.js:550`); "熄屏时间" (`strings_zh.cpp:324-325`) vs "自动熄屏" (`:440-441`); "语言" (`:326-327`) vs "系统语言" (`:602-603`) | The same setting has two device names in Chinese; one string id per key per surface (`AgentSet*` and `Settings*`, `strings.hpp:266-271, 358-360, 446-454`) |
| X6 | Defaults used when a dependency is absent | voice volume 100 (`voice_service.cpp:374, 633, 640`) against the setting's 60; the others (480, `true`, `"zh"`, 60) agree today but are copies (rows 3, 7, 10 above) | A test or a build without the dependency plays at a volume no user chose; any later change of a default leaves these behind |
| X7 | Validation repeated downstream | PowerManager re-validates both timeouts (`power_manager.cpp:219-226`); AgentWiring re-clamps both limits (`agent_wiring.cpp:28-39`); App re-clamps `notify_vol` (`app.cpp:105-109`) and VoiceWiring `volume` (`voice_wiring.cpp:27`) | Harmless today (the snapshot is already valid) but each is a second copy of a range |
| X8 | `low_batt_pct` is not validated | `power_monitor.cpp:169` takes any integer | A stored 0 or 200 disables or permanently raises the low-battery notice; nothing can set it except a hand edit, so it is latent |
| X9 | Stale descriptions | `routes_device.cpp:63-66` says `PUT /settings` takes five keys (it takes twelve); `esp_audio.hpp:21-31` (X2) | Misleads the next reader |
| X10 | The Shell page does not follow the cell | it copies the snapshot once at build (`settings_page.cpp:99-102`, `settings_page.hpp:28-31`), already in `docs/backlog.md:195` | A change made by voice or on the web while the page is open shows only after reopening |

The roadmap's two examples are X1 (`app.cpp:357` in the roadmap, now `:381-385`) and X2 (`loopback.cpp:28` / `esp_audio.cpp:24`, now `:29` / `:25`); the volume case is worse than a differing constant, because the two numbers are two gain stages and one of them is changed by a test.

Related open items in `docs/backlog.md`:

- `:353` — after a batch whose rollback failed, the reseeded cell follows storage, but a brightness that reached storage that way is not applied to the panel. Cause: brightness is applied by a hook inside `set()` (`system_settings.cpp:223-226`), not by following the cell.
- `:311` — the factory reset's reseed moves PowerManager to the default timeouts just before the reboot. Unchanged by this phase (harmless; section 9).
- `:195` — X10.

## 2. Goals and non-goals

### 2.1 Goals

1. Each setting is declared once: namespace, key, type, range, default, choices, unit, label id, surfaces. Nothing else in `components/`, `simulator/` or `web/` restates a default or a range of a known key, and CI enforces it.
2. The snapshot in the settings cell is generated from the table, is trivially copyable and allocates nothing when copied.
3. Every surface (REST, WebSocket event, agent, Lua, web, Shell) takes its key list, types, ranges and choices from the table; a new scalar key reaches every surface its flags name by adding one table row, one label per language, and nothing else, except where section 3.8 says presentation is hand-written.
4. Stored values and the external wire formats stay compatible (section 3.10).
5. X1-X10 are resolved.

### 2.2 Non-goals

- Model, ASR and TTS profiles, Wi-Fi networks, paired browsers and API keys. They are documents with nested lists and secrets, edited through their own services (`ModelProfileService`, `WifiService`, `PairingService`); a key/value schema does not describe them. The profiles' own option table (`model_profiles.hpp:99-124`) is a separate, smaller instance of the same pattern and stays as it is.
- Secrets of any kind. No key in the table is secret; `GET /settings` and the event never carry one. A future secret setting belongs with the write-only secrets of phase I, not here.
- New settings or new behaviour of existing ones, except the fixes of section 1.3. Phase F adds `power/hibernate_s` with its own default and range.
- Scenes writing settings. `wisp.device` stays read-only for settings.
- Generating the web page or the Shell page layout entirely from the table (section 3.8 explains what is derived and what is not).
- Unifying the web's and the device's label wording. They are separate dictionaries by design (the web follows the browser's language, connectivity spec 7.4); only their presence is checked.

## 3. Design

### 3.1 Where the table lives

`components/wisp_core/include/wisp/core/settings_schema.hpp`, in `wisp::core`. `SystemSettings` is in `wisp_core` and is the only writer; every consumer is in `wisp_core` or above it, so all can read the header.

The roadmap gives each row an "i18n label id". The device's strings are `ui::StrId` values in `wisp_ui` (`strings.hpp:35`), which sits above `wisp_core` (AGENTS.md section 3), and `wisp_admin` sits below `wisp_ui`, so neither the table nor the REST layer can name a `StrId`. The label id is therefore a string (`std::string_view label`), and each dictionary maps it:

- device: `wisp_ui` gains `ui::setting_label(std::string_view label) -> std::optional<StrId>`, one constexpr array in `components/wisp_ui/src/setting_labels.cpp`;
- web: the key `'sys.' + label` in `web/app/i18n/*.js`.

The label ids are chosen to be today's web keys, so the web dictionaries do not change: `brightness`, `volume`, `notify_vol`, `tts_mute`, `screen_off`, `deep_after`, `lang_label`, `tz_label`, `time_24h`, `time_servers`, `agent_steps`, `agent_minutes` (`en.js:359-388, 551-554`). Keys with no surface (13, 14) have no label.

### 3.2 The table

One list, expanded into the table, the snapshot struct and an id enum (the X-macro form is open decision 1; the alternative is described there):

```cpp
namespace wisp::core {

enum class SettingType : uint8_t { Int, Bool, Enum, HostList };

// Who sees a key. Each surface filters by its bit (3.5-3.9).
enum SettingFlag : uint8_t {
    kAgent      = 1 << 0,  // settings.get / settings.set
    kScene      = 1 << 1,  // wisp.device.info(), under scene_name
    kWeb        = 1 << 2,  // GET/PUT /settings, system.settings, /settings/schema
    kDevicePage = 1 << 3,  // the Shell settings page
    kZeroNever  = 1 << 4,  // 0 is valid and means "never", besides [min, max]
};

enum class SettingUnit : uint8_t { None, Level, Percent, Seconds, Minutes, UtcOffsetMinutes, Steps };

// The device UI languages. Same order as ui::Lang (wisp_ui checks it).
enum class LangCode : uint8_t { Zh, Ja, En };

struct SettingSpec {
    std::string_view name;        // the wire name: REST, event, agent
    std::string_view ns, key;     // the store's names, each 1-15 characters
    SettingType type;
    int32_t min, max;             // Int; for HostList: max entries, max entry length
    int32_t def_int;              // Int, Bool (0/1), Enum (index into choices)
    std::string_view def_text;    // HostList only
    std::span<const int32_t> int_choices;           // Int: suggested values, may be empty
    std::span<const std::string_view> enum_values;  // Enum: the codes, in index order
    SettingUnit unit;
    uint8_t flags;
    std::string_view label;       // label id (3.1); empty: no surface shows it
    std::string_view scene_name;  // the wisp.device.info() field; empty unless kScene
};

#define WISP_SETTINGS(X) \
  X(brightness,      int32_t,  Int,      "display", "brightness", 10, 255, 200, ...) \
  ...
inline constexpr SettingSpec kSettingsSchema[] = { /* WISP_SETTINGS expanded */ };
enum class SettingId : uint8_t { /* WISP_SETTINGS expanded */ };

}  // namespace wisp::core
```

The rows, in today's `to_map()` order (`system_settings.cpp:131-146`), then the two keys that have no surface:

| Name | ns/key | Type | min/max | Default | Choices | Unit | Flags | Label | Scene name |
|---|---|---|---|---|---|---|---|---|---|
| `brightness` | display/brightness | Int | 10/255 | 200 | — | Level | A W D | brightness | — |
| `volume` | audio/volume | Int | 0/100 | 60 | — | Percent | A W D | volume | — |
| `screen_off_s` | power/screen_off_s | Int | 15/3600 | 60 | decision 4 | Seconds | A W D 0 | screen_off | — |
| `language` | sys/lang | Enum | — | `zh` | `zh ja en` | None | A S W D | lang_label | `lang` |
| `utc_offset_min` | sys/tz_min | Int | -720/840 | 480 | the 38 offsets of `system.js:91-95` | UtcOffsetMinutes | S W D | tz_label | `utc_offset` |
| `ntp_servers` | sys/ntp | HostList | 2/63 | `pool.ntp.org,ntp.aliyun.com` | — | None | W | time_servers | — |
| `deep_after_s` | power/deep_after_s | Int | 60/3600 | 300 | decision 4 | Seconds | W D 0 | deep_after | — |
| `notify_vol` | audio/notify_vol | Int | 0/100 | 60 | — | Percent | A W D | notify_vol | — |
| `tts_mute` | voice/tts_mute | Bool | — | false | — | None | W D | tts_mute | — |
| `time_24h` | sys/time_24h | Bool | — | true | — | None | A S W D | time_24h | `time_24h` |
| `agent_max_steps` | agent/max_steps | Int | 8/200 | 60 | — | Steps | W | agent_steps | — |
| `agent_turn_min` | agent/turn_min | Int | 2/60 | 15 | — | Minutes | W | agent_minutes | — |
| `low_batt_pct` | power/low_batt_pct | Int | 5/50 (decision 2) | 15 | — | Percent | — | — | — |
| `perf_overlay` | ui/perf_overlay | Bool | — | false | — | None | — | — | — |

(A agent, S scene, W web, D device page, 0 `kZeroNever`.) The flags reproduce today's exposure exactly; open decision 6 is whether the agent gets more keys.

Types:

- **Int** is `int32_t` in the snapshot. All ranges fit; `Value` carries `int64_t` and `set()` rejects anything outside `[min, max]` (or 0 with `kZeroNever`) before narrowing, as `get()` already guards (`system_settings.cpp:102-105`).
- **Bool** is `bool`.
- **Enum** is stored as its code string, as today (`"zh"`), and held in the snapshot as a typed enum (`LangCode` for `language`). The table's `enum_values` are the codes; the stored value must be one of them. `language` is the only Enum key; a second Enum key would get its own C++ enum type in the list.
- **HostList** is stored as today's comma-separated string and held as `FixedString<127>` (2 entries × 63 + 1 comma; `fixed_string.hpp:19-20` allows up to 255). Its syntax check is today's `valid_ntp_servers()` (`system_settings.cpp:66-86`), moved next to the table and parameterised by `min`/`max` (entries, entry length). It is the one validator that stays a function, because the syntax is not a range.

`kZeroNever` replaces `valid_screen_off_s()`/`valid_deep_after_s()` (`system_settings.cpp:127-129`).

Compile-time checks in the header (`static_assert` over a `consteval` walk of `kSettingsSchema`): names unique; `(ns, key)` unique; `ns` and `key` 1-15 characters (`validate_settings_name()`'s rule, `settings.hpp:29-35`); every default valid by the row's own rule; every Int choice valid; Enum default index within `enum_values`; `kScene` set exactly when `scene_name` is non-empty; a key with any of `kAgent | kWeb | kDevicePage` has a label. A bad row does not build.

### 3.3 The snapshot

```cpp
struct SettingsSnapshot {          // generated from WISP_SETTINGS
    int32_t brightness = 200;      // each initializer is the row's default
    ...
    LangCode language = LangCode::Zh;
    FixedString<127> ntp_servers{"pool.ntp.org,ntp.aliyun.com"};
    ...
    bool operator==(const SettingsSnapshot&) const = default;
};
static_assert(std::is_trivially_copyable_v<SettingsSnapshot>);
```

- Field names are the wire names, so the 43 non-test lines naming `SystemSettingsValues` (grep at `5e2dec3`) keep compiling for every int and bool field; only `language` (about a dozen uses, e.g. `ui::parse_lang(v.language)` at `system_duties.cpp:330, 398`, `scene_runtime.cpp:225, 232, 2258`, `app.cpp:101`) and `ntp_servers` (`net_wiring.cpp:108`) change type. `wisp_ui` adds `ui::to_lang(core::LangCode)`, a cast guarded by a `static_assert` that the two enums agree, and `parse_lang()` stays for the agent's context string.
- Generic access for the surfaces: `Value core::setting_value(const SettingsSnapshot&, SettingId)` and `to_map()` built on it.
- `SystemSettingsValues` becomes an alias of `SettingsSnapshot` for the length of the migration and is then removed.
- Size: 9 × `int32_t` + 3 × `bool` + 1-byte enum + 128-byte `FixedString` ≈ 168 bytes, within `StateCell`'s 256-byte limit (`state_cell.hpp:227`).

### 3.4 `SystemSettings`

Its public API stays (`get()`, `set()`, `cell()`, `seed_cell()`); its body becomes loops over `kSettingsSchema`:

- `get()` reads each row from the store and falls back to the default for a missing value or one `set()` would refuse, as today (`system_settings.hpp:18`). New: when a stored value is present but invalid, `seed_cell()` logs one `WLOG_W` naming the key (a range narrowed by a later release, or a hand edit), so the fallback is no longer silent.
- `set(changes)` keeps its batch semantics (validate all, write, roll back on failure, one event per stored batch, `system_settings.hpp:145-154`). It gains an origin: `set(changes, SettingFlag surface)`. A key whose row lacks `surface`'s bit is rejected as `unknown setting: <name>`, the message today's unknown keys get (`system_settings.cpp:181`). REST passes `kWeb`, the agent `kAgent`, the Shell `kDevicePage`; App's internal writers (the diagnostics' overlay toggle) pass `kInternal`, a value outside the flag bits for which the check is skipped (the two store-only keys have no surface bit at all). This is how `PUT /settings` keeps rejecting `low_batt_pct` and `perf_overlay`, and how the agent's key list is enforced in the service instead of only in the tool's JSON enum (`tools_device.cpp:102-105`).
- `to_map()` emits the rows flagged `kWeb`, in table order: the twelve keys of today, in today's order. It is the body of `GET /settings`, of `PUT`'s reply and of the `system.settings` payload, as today.
- The `apply_brightness` hook goes (open decision 5): PowerManager follows `brightness` in the cell.
- The constants `kDefault*`, `kMin*`, `kMax*`, `valid_screen_off_s()`, `valid_deep_after_s()` and `kDefaultNtpServers` (`system_settings.hpp:60-87`) are deleted. Code that needs a default reads `kSettingsSchema[id]` through a `constexpr` accessor (`setting_default_int(SettingId::Brightness)`), which is the only way the CI guard allows (section 4).

### 3.5 Agent

`tools_device.cpp`:

- `kKeys` (`:28-36`) is deleted. `settings.get` returns the rows flagged `kAgent` from the cell (a memory read: the `run_io` hop of `:90-92` goes, as the cache already serves reads from memory, `cached_settings.hpp:3-10`).
- `settings.get`'s description is generated once at registration (the description is an `os::LargeString`, `tool_registry.hpp:71`): one clause per agent key, "name (min-max[, 0 never])", "(zh, ja, en)" for Enum, "(true/false)" for Bool, with a fixed lead sentence and the `time_24h` explanation kept as hand-written prose.
- `settings.set`'s schema is generated: `key` is an `enum` of the agent keys in table order; `value` stays `{"type": ["integer","string","boolean"]}`. Per-key value schemas (`oneOf`) are not used: the argument validator supports `enum`, `minimum` and `maximum` on one schema (`args_schema.cpp:80-96`), and `SystemSettings::set()` reports a bad value with today's message.
- The note uses `ui::setting_label(spec.label)` (one label per key, decision 7); `text_of()` (`:48-53`) stays (it formats a value for the note).
- `device.status` is unchanged. It carries no setting today (`tools_device.cpp:73-74`, `app.cpp:1210-1242`); `settings.get` is the agent's read path. The roadmap listed "`device.status` fields" as derived; there is nothing to derive (section 10).

### 3.6 Lua

`wisp.device.info()` fills its settings fields from the rows flagged `kScene`, under `scene_name`: `lang` (the code string), `utc_offset`, `time_24h` (`bind_device.cpp:342-346`, `scene_runtime.cpp:2245-2262`). The names, types and values are unchanged. `DeviceInfo`'s three hand-kept fields and their defaults (`scene_api.hpp:189-191`) and the runtime's fallbacks (`scene_runtime.hpp:528-530`, `scene_runtime.cpp:2254-2255`) go; without a settings cell (tests) the runtime uses a default-constructed `SettingsSnapshot`, whose initializers are the table's defaults.

What stays hand-written: which changes fire `"minute"` (time zone, `time_24h`) and `"lang"` (`scene_runtime.cpp:220-235`), and the API documentation prose (`schema.cpp:437-446`). Both describe behaviour, not values.

### 3.7 REST and the web page

- New route `GET /api/v1/settings/schema`, paired like `/settings` (`routes_device.cpp:82-83`). Body:

  ```json
  {"keys": [
    {"name": "brightness", "type": "int", "min": 10, "max": 255, "default": 200, "unit": "level"},
    {"name": "screen_off_s", "type": "int", "min": 15, "max": 3600, "default": 60, "unit": "seconds",
     "zero_never": true, "choices": [0, 15, 30, 60, 120, 300, 600]},
    {"name": "language", "type": "enum", "values": ["zh", "ja", "en"], "default": "zh"},
    {"name": "ntp_servers", "type": "hosts", "max_entries": 2, "max_length": 63,
     "default": "pool.ntp.org,ntp.aliyun.com"},
    ...
  ]}
  ```

  Only rows flagged `kWeb`, in table order, each with its `label` id. The body depends on nothing at run time; it is serialised into an `os::LargeString` per request (about 1.5 KB, PSRAM) rather than built as a `Value` tree, whose map nodes would be small internal-RAM allocations on the httpd task.
- `GET`/`PUT /settings` are unchanged in shape and error messages; `PUT` calls `set(changes, kWeb)`.
- The web System page loads the schema once with the settings (`system.js:161-170`). Its constants `AGENT_STEPS_*`, `BRIGHTNESS_*`, `VOLUME_MAX`, `SCREEN_OFF`, `DEEP_AFTER`, `UTC_OFFSETS` (`system.js:75-95`) and the `?? 60` / `?? 15` / `!== false` fallbacks (`:317, 526, 719-720`) are replaced by schema reads; `wholeIn()` checks against the schema's range; the brightness percentage uses one formula with the Shell (decision 3). The cards, their order and their special controls (the time-server draft and `tidyServers`, the agent card's combined save) stay hand-written. A key flagged `kWeb` that no card places is rendered in a generic card at the end with a control chosen by type (number field, switch, select), so a new key is editable on the web before anyone writes a card for it.

### 3.8 The Shell settings page

What is derived, what is not:

- **Derived from the row:** slider range, choice list (Int choices plus the current value when the list lacks it, as `with_current()` does today, `settings_page.cpp:47-55`), Enum choices, the default used before the first cell delivery, the label (`ui::setting_label()`), the key written, and value formatting by `unit` (`Seconds` → `duration_text()`, `UtcOffsetMinutes` → `time_zone_text()`, `Level`/`Percent` → one percentage formula).
- **Hand-written in `wisp_shell`:** a layout list of `{group, key, widget}` entries, where `widget` is `Slider`, `Switch`, `InvertedSwitch` (`tts_mute` shown as "Spoken replies", `settings_page.cpp:222, 421`) or `Choice` (`time_24h` is a choice row with 24 h / 12 h, `:347-356`, not a switch). The non-setting rows (Wi-Fi, models, version, address, diagnostics, restart, power off) are where they are today. Per-choice labels that are not numbers (language names `LangFull*`, the time-zone cities `Tz*` of `settings_page.cpp:64-82`) stay in `wisp_shell`/`wisp_ui`.
- **Checked by a test:** every key flagged `kDevicePage` appears in the layout list exactly once, and no other key does; every Int choice of `utc_offset_min` has a city entry.

The page subscribes to the settings cell on the UI dispatcher while it is open and redraws its values from each delivery (closes X10 and `docs/backlog.md:195`). A write still goes through `submit_io` (`settings_page.cpp:363-374`) with `kDevicePage`; a failed write is corrected by the next delivery instead of leaving the optimistic value on screen.

The same rule applies to the diagnostics' performance-overlay toggle: it writes `perf_overlay` with `set(..., kInternal)` and SystemDuties reads it from the cell at start (`system_duties.cpp:137`, `self_check_screen.cpp:163, 435`).

### 3.9 Other consumers

Each reads the snapshot; each local default named in section 1.2 goes:

| Consumer | Today | After |
|---|---|---|
| App boot brightness | raw `display/brightness`, default 200, clamp 0-255 (`app.cpp:381-385`) | `system_settings.cell().get().brightness` (X1) |
| PowerManager timeouts | re-validates (`power_manager.cpp:219-226`) | takes the values as delivered; initial atomics from the schema defaults (`power_manager.hpp:314-315`) |
| PowerManager brightness | `apply_brightness` hook, `user_brightness_ = 200` (`power_manager.hpp:336`) | follows `brightness` in the cell on the UI dispatcher (decision 5); the "UI queue full; brightness change dropped" path (`power_manager.cpp:588-601`) goes with the post |
| PowerMonitor | reads `low_batt_pct` from the store every 20 samples (`power_monitor.cpp:166-171`) | reads the cell (memory); `kThresholdRefreshSamples` and the `Settings` member go; range from the table (X8) |
| AgentWiring | `static_assert`s against `agent::kMaxSteps`/`kTurnMs` and re-clamps (`agent_wiring.cpp:25-39`) | `agent::kMaxSteps`/`kTurnMs` are defined from the table's defaults; the clamps go (X7) |
| NotificationRouter, VoiceWiring, App helpers | `kDefaultVolume = 60`, clamps, `: 100` (`notification_router.cpp:16, 54`; `voice_wiring.cpp:27`; `voice_service.cpp:374, 633, 640`; `app.cpp:105-109`) | read the cell; a missing dependency uses the schema default, never 100 (X6) |
| StatusShell, Reminders, agent context, SelfCheck | `480`/`true` fallbacks (`status_shell.cpp:27`, `reminders.cpp:22`, `context.hpp:60`, `self_check_screen.hpp:195-196`) | schema default accessor |
| Loopback self-check | raw `audio/volume`, default 60, sets the device volume (`loopback.cpp:27-29, 94-97`) | reads the cell; scales its samples like PcmPlayer and no longer calls `set_volume()` (open decision 5; X2) |
| ESP and simulator outputs | `kDefaultVolumePercent = 80` (`esp_audio.cpp:25`); `volume_{100}` (`sim_devices.hpp:163`) | a port output gain, not a setting: renamed `kOutputGainPercent`; the header comment `esp_audio.hpp:21-31` corrected. Whether 80 is the right gain for each board is a device check |
| TimeSyncService | `settings.get().ntp_servers` per attempt (`net_wiring.cpp:108`) | a cell read; `split_servers()` takes the `FixedString`'s view |

### 3.10 Compatibility

Unchanged:

- Stored values: every namespace, key and stored `Value` type is today's (the NVS backend stores each value as its JSON text, `settings_nvs.cpp:93, 111-124`; `language` stays the string `"zh"`, `ntp_servers` the comma-separated string). A device upgraded from the current release reads the same settings. A stored value outside a range reads as the default, as today, now with a log line.
- `GET /settings`, `PUT /settings` (keys, order, types, error messages; it still rejects any key it rejects today).
- The WebSocket `system.settings` payload (the same `to_map()`; the golden payload test of the state-cells design keeps it byte-for-byte).
- Lua `wisp.device.info()` fields and events.
- The agent's `settings.get` result keys and `settings.set` argument shape and accepted keys.

Changed:

- New `GET /api/v1/settings/schema` (additive).
- The agent's `settings.get` description text is generated (same facts, possibly different wording), and the `enum` of `settings.set` follows table order (`brightness, volume, screen_off_s, language, notify_vol, time_24h` instead of `brightness, volume, notify_vol, screen_off_s, language, time_24h`). Neither affects a conversation stored on the device: tool definitions are sent fresh each turn.
- Shell: the choice lists of decision 4; one label per key in zh (decision 7); brightness percentage formula (decision 3).
- Device sound after a loopback test (X2), and in the simulator (its output gain).
- The C++ type of the snapshot and of `SystemSettings::set()`'s signature (internal).

## 4. The CI guard

`tools/check_settings.py`, run by `tools/ci.sh` next to `check_topics.py`, with its own `check_settings_test.py` (as the other checkers, `tools/ci.sh:131-139`).

It reads the rows from `settings_schema.hpp` (the `WISP_SETTINGS` list has one `X(...)` per line, parsed with a regular expression, as `check_i18n.py` parses the dictionaries without a JavaScript engine). For each row it knows the name, `ns`, `key`, default, min, max, Int choices, label and scene name. It then scans `components/`, `simulator/`, `boards/`, `drivers/` (`*.cpp`, `*.hpp`, `*.c`, `*.h`) and `web/app/` (`*.js`), excluding `settings_schema.hpp` itself and `tests/`, and fails on:

1. **Raw store access to a schema key.** A string literal equal to a row's `ns` passed to `core::Settings(` or to a backend `get(`/`set(`/`erase(`, or a `get_int(`/`get_bool(`/`get_string(` whose first argument is a row's `key`. Every read goes through the snapshot. (Catches `app.cpp:383-384`, `loopback.cpp:94-96`, `power_monitor.cpp:169`, `system_duties.cpp:137`.)
2. **A default or range literal next to its key.** A line containing an identifier token derived from a row (its name, its key, its label, its scene name, and the name with a unit suffix removed: `brightness`, `screen_off`, `utc_offset`, `notify_vol`, `max_steps`, `turn_min`, `low_batt`, ...; case-insensitive, also inside `kDefault...` and `SCREAMING_CASE` identifiers) together with an integer literal equal to that row's default, min or max, or a string literal equal to its default text or one of its Int choices lists' bracketed form. Literal `0`, `1`, `true` and `false` are not matched (too common to mean anything; booleans are covered by rule 1 and review).
3. **A default text anywhere.** The `HostList` default (`pool.ntp.org`) and every Enum default code inside a line that also names the key (`"zh"` next to `lang`/`language`).

False positives:

- Comments are scanned too: a default restated in a comment drifts as surely as one in code (`power_manager.hpp:10, 14`, `routes_device.cpp:63-66`, `esp_audio.hpp:26-28` are all comment copies).
- A genuine coincidence is allowed by an end-of-line marker `// settings-literal-ok: <reason>` (`// settings-literal-ok:` in JavaScript too), with a non-empty reason. The checker prints the count of markers so a growing count is visible in CI output.
- Expected markers after migration: none known. The port output gains are renamed so that no setting token appears on their lines; `pcm_player.cpp`'s `volume = 100` is "full scale" on a line that names `volume`, so it either gets a marker or its local is renamed `scale` — the plan picks one.
- The checker's own test covers: a raw `get_int("brightness", 200)` fails; `kDefaultUtcOffsetMin = 480` fails; `x = 480` on a line with no setting token passes; `volume_{100}` fails without a marker and passes with one; a marker without a reason fails; a schema row added to the fixture header is picked up.

What the guard does not catch: a default copied under an unrelated name on a line with no setting token (`int fallback = 60;`). The table-walking tests (section 6) and review cover that; the roadmap asks for the known keys only.

## 5. Costs

**Flash.** The table: 14 rows of about 80 bytes (string views, three `int32_t`, two spans, small enums) plus their strings, about 1.5 KB of `.rodata`. Reading it is a read of constants mapped into the address space, which the flash-thread rule allows from any task (AGENTS.md section 5). Generated code for `get()`/`set()`/`to_map()` loops replaces today's per-key code; net code size is expected to shrink slightly (not measured). The web page gains the schema fetch and a generic card and loses its constants: a few hundred bytes of the 170 KB gzip budget (`tools/ci.sh` `pack_assets.py --budget-kb 170`).

**Internal RAM.** The settings cell's body is `T` inside `SystemSettings` inside `App::Impl`. Today `T` is about 84 bytes on the device (eight `int`, two 24-byte `std::string`, two `bool`; the `std::string` size is the 32-bit libstdc++ layout, not measured) plus a heap block for `ntp_servers` (27 characters, past the 15-byte short-string buffer) and for any `language` copy that allocates. After: about 168 bytes inline, no heap. Net static change about +60 bytes wherever `App::Impl` lives (not verified whether that is internal RAM). Per delivery and per `get()`: today a heap allocation in internal RAM (a small malloc, AGENTS.md section 8); after, a 168-byte stack copy on the dispatcher (`wisp.bus` 6144 bytes, the UI task) and no allocation. Subscriptions added: the Shell page while open, PowerManager's brightness follower (one each, about 48 bytes in PSRAM per subscription, state-cells design 7). Subscriptions removed: none.

**PSRAM.** The schema response, about 1.5 KB per request, freed after sending; the generated agent descriptions, a few hundred bytes for the life of the registry.

**Threading.** Unchanged rules. Readers read the cell (memory, any thread). Writers call `SystemSettings::set()`, which writes the settings cache in memory; the cache writes to flash on `wisp.fs` (`cached_settings.hpp:3-12`, `settings_front.hpp:1-33`). Without a cache, `SettingsFront` routes each call to `wisp.fs` as today. Cell callbacks remain memory-only: PowerManager's brightness follower calls the display's `set_brightness()` on the UI thread (a device call, as `PowerManager::show_brightness()` does today from a UI post, `power_manager.cpp:618-628`); the Shell page's follower updates labels. The agent's `settings.get` drops its `run_io` hop (memory read). PowerMonitor's sampling on the esp_timer task no longer calls into the settings store.

## 6. Testing

GoogleTest in the simulator, written before the code (AGENTS.md section 5).

Table walk (`tests/core/settings_schema_test.cpp`), besides the `static_assert`s of 3.2 (kept as runtime assertions too so a failure names the row):

- names, `(ns, key)` pairs and scene names unique; `ns`/`key` pass `validate_settings_name()`;
- each default valid by `SystemSettings`' own validation (not only the `consteval` rule): a fresh store reads every default back;
- for each Int row: `min - 1` and `max + 1` refused, `min`, `max` and each choice accepted; 0 accepted exactly when `kZeroNever`; wrong type refused; for Enum: each code accepted, `"fr"` refused; for HostList: today's cases from `system_settings_test.cpp` (three entries, a 64-character entry, a space).

Labels (`tests/ui/setting_labels_test.cpp`, in `wisp_ui` because it needs `StrId`):

- every row with `kAgent | kWeb | kDevicePage` has `ui::setting_label()` and a non-empty text in each of zh, ja, en read from that language's own table (not through the Chinese fallback of `strings.hpp:8-10`);
- `LangCode` and `ui::Lang` agree on order and codes.
- every `kWeb` row's `'sys.' + label` exists in `web/app/i18n/en.js` (the directory passed to the test by CMake as a compile definition, the way `WISP_FIXTURE_WEB_DIR` is, `tests/CMakeLists.txt:230`); `check_i18n.py` already makes zh and ja carry the same keys as en.

Compatibility goldens (captured from the code at `5e2dec3` before the change):

- `to_map()` of the default snapshot and of a non-default one: keys, order, value types;
- `GET /settings`, `PUT /settings` replies and error bodies for an unknown key, a wrong type, an out-of-range value;
- the `system.settings` payload (the state-cells golden test stays green unchanged);
- `wisp.device.info()`'s `lang`, `utc_offset`, `time_24h`;
- the agent's `settings.get` result and `settings.set` result and note text for each agent key.

Behaviour:

- `set(changes, surface)`: `PUT` with `low_batt_pct` refused as unknown; the agent with `deep_after_s` refused; the Shell with `agent_max_steps` refused; internal with `perf_overlay` accepted.
- An out-of-range stored value reads as the default and logs once at `seed_cell()`.
- Boot with a stored brightness of 5: the panel gets 200 (X1).
- Brightness: a batch whose rollback failed with a brightness left in storage reaches the panel (`docs/backlog.md:353`).
- Loopback: plays at the setting's volume by sample scaling and leaves the output's volume untouched (a fake `AudioOut` records `set_volume()` calls; X2).
- Shell page: every `kDevicePage` key has one row; a cell change while the page is open updates the row; a refused write shows the stored value after the next delivery; choice lists come from the table. Rendered on the three screen shapes with the existing settings-page screenshot preview (manual, AGENTS.md section 5).
- `GET /settings/schema`: golden body; every `kWeb` row present and nothing else.
- `tools/check_settings_test.py` (section 4).

The web page's use of the schema has no automated test (frontend tests are out of the roadmap's scope); it is checked by hand in the simulator: every card loads, a value outside a field's range is refused by the field, a fake extra `kWeb` row in a test build appears in the generic card.

## 7. Device checks (not run; for the owner)

1. Upgrade from the current release with non-default settings stored: every value reads back the same on the device and in `GET /settings`.
2. Speech, cue tones and notification sounds at volume 30, 60 and 100 before and after running the loopback test: same loudness before and after; the loudness at 60 acceptable with the output gain at 80 on each board (or a new gain recorded in `docs/hardware/<board>.md`).
3. Brightness at boot equals the stored value; at 10 the panel is still readable; dim (30 % of the setting) and wake behave as before.
4. The Shell settings page on the 2.16 board and the round 1.75 board: rows, sliders, choice pages, values following a change made on the web while the page is open.
5. `mem` at idle: internal free and largest block against `ram_budget.hpp` (the cell grows by about 60 bytes; no other internal-RAM change is expected).
6. A factory reset ends with defaults and the overlay off.

## 8. Migration

Each step leaves `bash tools/ci.sh` green; steps 1-3 change no behaviour.

1. **Table.** `settings_schema.hpp` with the list, the table, `SettingId`, `SettingsSnapshot` (as an alias target), the `consteval` checks and the table-walk tests. `ui::setting_label()` and its test. No caller changes.
2. **`SystemSettings` on the table.** `get()`, `set()`, `to_map()` and validation loop over the rows; the compatibility goldens are captured first and stay green. `SystemSettingsValues` becomes an alias; `language`/`ntp_servers` change type with their readers in the same commit.
3. **`set()` origins.** Each caller passes its surface; tests for refusals.
4. **Surfaces**, one per commit: agent tools; Lua info; `GET /settings/schema`; the Shell page (layout list, cell follower); the web page.
5. **Consumers** of section 3.9, one per commit, including the two store-only keys (PowerMonitor, the overlay) and the brightness follower (removing `apply_brightness`). The loopback and port-gain change.
6. **Guard.** Delete the `kDefault*`/`kMin*`/`kMax*` constants and the alias; add `tools/check_settings.py` and its test to `tools/ci.sh`; the remaining hits are fixed in this commit.
7. **Documents**, in the commits of the behaviour they describe: AGENTS.md section 5 "Settings keys" (one more sentence: settings are declared in `settings_schema.hpp`, nowhere else, enforced by `check_settings.py`) in a `docs(agents): ...` commit; `docs/architecture.md` (`:51` boot brightness, `:131` and `:180` the snapshot type, the new route); the connectivity spec's route table (`:303`); `docs/porting-guide.md` (adding a setting, if the owner wants it there); `docs/backlog.md` (close `:195`, `:353`).

## 9. Out of scope

- The factory reset's reseed moving PowerManager to default timeouts before the reboot (`docs/backlog.md:311`): harmless, unchanged.
- Making `ui::Lang` an alias of `core::LangCode` (one enum instead of two kept in step by a `static_assert`): a mechanical change across `wisp_ui`'s callers, no user effect.
- Schema-driven model-profile options, Wi-Fi and pairing documents (section 2.2).
- Settings writable by scenes (`wisp.device.set`), per-scene settings, or settings with per-surface write permissions beyond the flags.
- A "reset this setting to its default" control on the web or the Shell (the schema makes it easy; nobody asked).
- Volume on a hardware key (`docs/backlog.md:27`).
- Generating the web dictionaries' label entries or the device's `StrId`s from the table.

## 10. Changes to the roadmap's decisions

| Roadmap (phase E) | This spec | Why |
|---|---|---|
| per key an "i18n label id" | a string label id, mapped by `wisp_ui` and by the web dictionaries | `wisp_core` and `wisp_admin` sit below `wisp_ui` and cannot name `StrId` (3.1) |
| `device.status` fields derive from the table | `device.status` unchanged; `settings.get` derives | `device.status` carries no setting today; adding settings there would duplicate `settings.get` (3.5) |
| the web page "builds its form" from the schema | values, ranges, choices and unknown keys from the schema; card layout hand-written | the cards have controls a generic form cannot express (time-server draft, combined agent save, city names) (3.7) |
| Shell rows for keys flagged `device_page` | derived values and choices; a hand-written layout list checked by a test | widget kind is not a function of type (`time_24h` is a choice row, `tts_mute` an inverted switch) (3.8) |
| flags agent, scene, web, device_page | the same, and they also gate `SystemSettings::set()` per caller; plus `kZeroNever` | one place decides which surface may write which key (3.4) |
| (not listed) | `power/low_batt_pct` and `ui/perf_overlay` join the table | they are settings read from the store with literal defaults; the guard would otherwise have to exempt them (1.1) |
| (not listed) | brightness follows the cell; loopback stops changing the output volume | X1, X2 and `docs/backlog.md:353` are disagreements this phase exists to remove |

## 11. Open decisions for the owner

1. **How the snapshot is generated.** *Proposal: one X-macro list `WISP_SETTINGS(X)` expanded into the table, the struct and the id enum.* The compiler then guarantees that the struct and the table agree. Alternative: a hand-written struct plus a table of member pointers (`std::variant<int32_t S::*, bool S::*, ...>`), more readable, but nothing can check that every struct field has a row (C++20 has no reflection). If wrong: the switch either way is mechanical and local to one header and its checker's parser.
2. **The two store-only keys.** *Proposal: both join the table without surface flags; `low_batt_pct` gets the range 5-50.* The range is new (today any integer is taken); 5-50 covers every sensible threshold. If wrong: a stored value outside the chosen range on some device reads as 15; nobody can set the key except by hand today, so no user is affected.
3. **Brightness as a percentage.** *Proposal: both the Shell and the web show `(value - min) / (max - min)`, so 10 is 0 % and 255 is 100 %.* 0 % at the lowest level is what the slider position already shows. If wrong: one formula, changed in one helper per side.
4. **Screen-off and deep-standby choices.** *Proposal: the web's lists for both surfaces: screen off `0, 15, 30, 60, 120, 300, 600`; deep standby `0, 60, 120, 300, 600, 1800, 3600` (shown with "never" last on the device, as today).* If wrong: one more row on the Shell's choice page, or a value the web can no longer pick; neither changes stored values.
5. **Brightness follows the cell, and the loopback test stops setting the output volume.** *Proposal: yes to both.* Brightness then applies after `set()` returns, on the UI thread, in the cell's coalescing order instead of inside `set()`'s lock; a burst of slider writes shows the last value, which is what the user wants. The loopback then plays at the setting by scaling samples, and the port's output gain (80 on the ESP, 100 in the simulator) becomes a documented port constant. If wrong: loudness after this change is fixed at what users heard before any loopback test; if 48 % at volume 60 is too quiet on a board, the gain is a board fact to raise in that board's adapter, not a setting.
6. **Agent reach.** *Proposal: keep today's six agent keys in this phase, and add `tts_mute`, `deep_after_s` and `utc_offset_min` as a separate one-line change if the owner agrees* (the agent should have as much control as possible; mute and time zone are things a user asks for by voice). `ntp_servers` and the agent's own turn limits stay off the agent's list. If wrong: one flag per key, either way; the agent's writes need no confirmation in any case.
7. **One device label per key.** *Proposal: the settings page's text (`Settings*`) for both the page and the agent's note; the `AgentSet*` ids are deleted.* zh then reads "提醒音量", "自动熄屏", "系统语言" in the agent's note too. If wrong: a wording change in three zh strings.
8. **Stored values outside a range.** *Proposal: keep "read as the default" and add one log line per key at boot.* Alternative: clamp into range (a narrowed range then keeps the nearest value instead of the default). Clamping would make X1's stored 5 read as 10 instead of 200; for `kZeroNever` keys and Enum keys clamping has no meaning, so the rule would differ by type. If wrong: one function in `SystemSettings`.

## Approval

The owner approved this spec on 2026-11-15, answering each question of section 11 with its proposal:

1. Snapshot generation: one X-macro list `WISP_SETTINGS(X)` expanded into the table, the struct and the id enum.
2. Store-only keys: `low_batt_pct` and `perf_overlay` join the table without surface flags; `low_batt_pct` has the range 5-50.
3. Brightness percentage: `(value - min) / (max - min)` on the Shell and the web.
4. Choices: screen off `0, 15, 30, 60, 120, 300, 600`; deep standby `0, 60, 120, 300, 600, 1800, 3600`, on both surfaces.
5. Brightness follows the cell; the loopback test scales samples and no longer sets the output volume; the ports' 80 and 100 are output gains.
6. Agent reach: the agent also gets `tts_mute`, `deep_after_s` and `utc_offset_min` (nine keys), in a change of its own after the table reproduces today's six. No confirmation for any of them.
7. One device label per key: the settings page's text; the `AgentSet*` ids are deleted.
8. Stored values outside a range read as the default, with one log line per key at boot.
