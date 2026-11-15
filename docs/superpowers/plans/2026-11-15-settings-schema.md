# Settings Schema — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Every system setting is declared once, in one X-macro list in `wisp/core/settings_schema.hpp`. `SystemSettings`, the settings cell's snapshot, `GET`/`PUT /settings`, the new `GET /settings/schema`, the agent's `settings.*` tools, `wisp.device.info()`, the Shell settings page and the web System page take keys, types, ranges, defaults, choices and labels from it, and `tools/check_settings.py` keeps default and range literals of known keys out of the rest of the tree. Stored values and the wire formats stay as they are (spec 3.10).

**Architecture:** `WISP_SETTINGS(X)` expands into `core::SettingId`, the constexpr table `core::kSettingsSchema` and the trivially copyable `core::SettingsSnapshot` held by the settings cell. Consumers read the snapshot (memory only) or walk the table filtered by a flag per surface; `SystemSettings::set()` takes the caller's surface and refuses keys that surface may not write. Presentation (card and row layout, widgets, city names) stays hand-written and is checked against the table by tests.

**Tech Stack:** C++20 (`-fno-exceptions -fno-rtti`), GoogleTest, the simulator build (`build/sim`), Python 3 standard library (the CI checker), Preact/htm (the web page).

**Spec:** `docs/superpowers/specs/2026-11-15-settings-schema-design.md` (approved 2026-11-15, all section 11 proposals; decision 6 as recorded in its "Approval" section).

## Global Constraints

- Branch `feat/settings-schema`; never push, merge or switch branches. Never `git stash`. Never add `.claude/` or `.playwright-mcp/`.
- AGENTS.md applies in full. In particular:
  - Layering (`tools/check_layering.py`): `wisp_core` names no `wisp_ui`/`wisp_admin`/later type; `wisp_admin` sits below `wisp_ui` and cannot name `ui::StrId`.
  - TDD: write the failing test first, run it, see it fail for the stated reason, then implement.
  - `bash tools/ci.sh` green before every commit; test output clean, no warnings.
  - Conventional Commits; every commit ends with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
  - Only `wisp.fs` touches internal flash. Settings reads go through the cell (memory). `SystemSettings::set()` writes the settings cache; callers that ran it inside a `submit_io`/`run_io` job keep doing so.
  - Cell callbacks do memory operations only and run on the dispatcher they were given (`wisp.bus` by default, the `UiRuntime` for UI code).
  - User-facing text in zh, ja and en (device: `strings_{zh,ja,en}.cpp`; web: `web/app/i18n/{zh,ja,en}.js`, `tools/check_i18n.py`).
  - Settings namespaces and keys at most 15 characters.
  - Comments say why the code is as it is; no review ids, task numbers, "round N", or `.superpowers/` paths (`tools/ci.sh` greps for them).
- `-Werror=missing-field-initializers` is on for host components and the firmware toolchain: every aggregate this plan adds or changes gives **every** member a default member initializer, so a brace or designated initializer that leaves one out still compiles.
- Snapshot integer fields are `int`, not `int32_t`: on the ESP-IDF toolchains `int32_t` is `long`, and existing code such as `std::clamp(v.notify_vol, 0, 100)` would stop compiling on the device only (see "Decisions" item 1).
- Stored values and wire formats do not change except where spec 3.10 says they do: every namespace, key and stored `Value` type; `GET`/`PUT /settings` keys, types and error bodies; the `system.settings` payload; `wisp.device.info()`'s `lang`, `utc_offset`, `time_24h`; the agent tools' argument and result shapes. The goldens of Task 1 run in every task that touches settings: `export GOLDEN='ForwardedStream|SettingsGolden|SystemSettings.StoreWrittenByTheOldCodeReadsTheSame|Routes.Settings|DeviceTools.Settings|SceneApiTest.(DeviceInfoShape|SettingsCellAtStartShowsInInfo)'` once per shell; the `-R` filters below use `$GOLDEN`. A task that must change a golden says so in its own text; otherwise a golden change is a bug.
- `components/wisp_port_esp/` is compiled only by `idf.py`. A task that edits it runs `idf.py build` for both boards when ESP-IDF v6.0.3 is available (`cd firmware && idf.py bmgr -c ../boards -b <board> && idf.py build`, boards `waveshare_esp32_s3_touch_amoled_2_16` and `waveshare_esp32_s3_touch_amoled_1_75c`); otherwise it records "firmware build not run" in its commit body, and the documents task lists it under the device checks.
- Device checks are never reported as done.

## Review Focus

The five failures most likely to reach a user that no single task's own feature tests would catch; each has a pinning test in the task named.

1. **A device upgraded from the current release reads different settings.** The snapshot type swap (language string → `LangCode`, `ntp_servers` → `FixedString<127>`, table-driven `get()`) must read a store written by today's code exactly as today. Pinned by `SystemSettings.StoreWrittenByTheOldCodeReadsTheSame` (Task 4), written against today's code in Task 1 and left unchanged.
2. **A surface gains or loses a key.** `PUT /settings` must still take exactly the twelve keys, the agent exactly its keys, the Shell its nine, and nobody the two store-only keys. Pinned by `SystemSettings.SurfaceRefusesKeysOutsideItsFlags` and `Routes.SettingsPutRefusesStoreOnlyKeys` (Task 5), `DeviceTools.SettingsGetListsNineKeys` (Task 8).
3. **The wrong language after the enum conversion.** `core::LangCode` and `ui::Lang` must agree in order and codes, or the screen, scenes and agent switch to the wrong language. Pinned by `SettingLabels.LangCodeMatchesUiLang` (Task 3) and `AppBoot.StoredLanguageReachesTheScreenScenesAndAgent` (Task 4).
4. **Brightness on the panel differs from the setting.** At boot (X1), after a burst of changes, after a failed rollback, and while dimmed. Pinned by `AppBoot.PanelStartsAtTheStoredBrightness`, `AppBoot.InvalidStoredBrightnessStartsAtTheDefault`, `PowerManager.BrightnessFollowsTheCellFlooded` and `PowerManager.ReseededBrightnessReachesThePanel` (Task 14).
5. **Speech gets quieter after the loopback self-check (X2), or louder without a volume dependency (X6).** Pinned by `Loopback.LeavesTheOutputVolumeAlone` (Task 15) and `VoiceServiceTest.MissingVolumeUsesTheSettingDefault` (Task 16).

## Decisions the spec left to the plan

1. **Snapshot integers are `int`.** Spec 3.3 says `int32_t`; on the ESP-IDF toolchains `int32_t` is `long`, so mixed-type `std::clamp`/`std::min` calls and `%d` formats on snapshot fields would break the firmware build only. The table's `min`/`max`/`def_int` stay `int32_t` (constexpr data, never deduced against `int`).
2. **The surface parameter is its own enum with a default.** `SystemSettings::set(changes, SettingSurface from = SettingSurface::Internal)`, `enum class SettingSurface : uint8_t { Internal, Agent, Web, DevicePage }`. Spec 3.4 wrote `SettingFlag surface` with a `kInternal` value; a separate enum keeps access (who writes) apart from the row flags (which also carry `kSettingZeroNever`). The default keeps the 105 existing internal and test call sites unchanged; the three surfaces pass theirs explicitly and are pinned by tests.
3. **Flag names** are `core::kSettingAgent`, `kSettingScene`, `kSettingWeb`, `kSettingDevicePage`, `kSettingZeroNever` (`inline constexpr uint8_t`): plain `kScene`/`kInternal` already exist as locals and role constants elsewhere.
4. **The schema body of `GET /settings/schema` is built once**, at `AdminServer` construction, into a `std::shared_ptr<const os::LargeString>` served through `HttpResponse::static_body`/`body_owner`. Spec 3.7 said per request; the body depends on nothing at run time, so once is cheaper and still in PSRAM.
5. **The CI guard's matching rules** (spec section 4 left the token rule loose, and as written it floods):
   - Identifiers on a line are split into lower-case words (snake, camel and SCREAMING case). A row's tokens are its `name`, `key`, `label` and `scene_name` as word sequences, plus the `name` without a trailing `s`/`min`/`pct` word **only when at least two words remain** (`screen_off`, `deep_after`, `agent_turn`, `utc_offset`, `low_batt`; not `turn`, `tz`).
   - Rule 2 (default or range next to the key) applies to **Int rows only**, and never matches `0`, `1`, or `100` on a `Percent` row (full scale is not a setting fact: it keeps `pcm_player.cpp`'s and `tone_player.cpp`'s `volume`/`100` lines legal without a rename or marker). HostList `min`/`max` are list shape, not values, and are not matched.
   - Rule 3 matches the HostList default text anywhere, and an Enum default code only in an assignment or initializer (`= "zh"`, `{"zh"}`, `?? 'zh'`), so `parse_lang()`'s `== "zh"` and `lang_code()`'s `return "zh"` stay legal.
   - Expected markers after migration: `components/wisp_ui/src/wisp_shape.cpp:203` (the wisp's own glow parameter named `brightness`), `components/wisp_port_sim/include/wisp/port/sim/sim_board.hpp:86` (the simulated panel's power-on level) and `components/wisp_core/src/power_monitor.cpp:22` (`kLowBattHysteresis = 5`, a coincidence with `low_batt_pct`'s minimum). `power_manager.cpp:22`'s `kMinDimBrightness = 10` is the brightness minimum in meaning and becomes `setting_spec(SettingId::Brightness).min`.
6. **Without a settings cell, `wisp.device.info()` reports the schema defaults** (`zh`, 480, `true`), not `ui::lang()`. `SceneRuntimeDeps::utc_offset_min` goes; `SceneApiTest.InfoWithoutASettingsCell` changes to expect `zh\t480\ttrue` (App always passes the cell).
7. **The agent note for `tts_mute`** uses the settings page's label ("Spoken replies", decision 7) and shows the value as the page does, inverted: `语音播报 on → off` when mute goes from false to true. `text_of()` renders `tts_mute` as `on`/`off` of spoken replies, as it renders `time_24h` as `24h`/`12h`.
8. **`PowerManager::set_brightness()` becomes UI-thread-only (or no UI).** With a UI, brightness reaches PowerManager only through its cell follower on the UI dispatcher; the off-thread post path and its "UI queue full" drop go. Without a UI (headless with a display) PowerManager does not follow brightness: callbacks on `wisp.bus` must not call a device, and nobody sees a panel that has no UI. `SetBrightnessCoalescesWhileUiIsBusy` and `UiThreadBrightnessIsNotOverwrittenByAnOlderQueuedJob` are replaced by `BrightnessFollowsTheCellFlooded`.
9. **The Shell lists languages in its own order** (中文, English, 日本語, today's `kLanguages`), kept as `kLanguageOrder` in `wisp_shell` and tested to be a permutation of the schema's codes; the schema's order (`zh ja en`) is the stored meaning, not a display order.
10. **The agent's `settings.get` description** is generated by one function with the exact format of Task 7; `settings.set` keeps running inside `run_io` (without the settings cache `set()` waits for flash); `settings.get` reads the cell.
11. **`PowerMonitor` takes the settings cell** (`const StateCell<SettingsSnapshot>*`, null: the default threshold) instead of a `Settings`; its threshold cache and `kThresholdRefreshSamples` go.
12. **The `kDefault*`/`kMin*`/`kMax*` constants of `system_settings.hpp`** are re-expressed from the table in Task 4 (so consumers keep compiling) and deleted in Task 17, after every consumer moved.

---

### Task 1: Pin the settings goldens

**Files:**
- Test: `tests/core/system_settings_test.cpp` (append), `tests/admin/routes_test.cpp` (append)

**Interfaces:**
- Consumes (today's code, unchanged): `core::to_map(const SystemSettingsValues&)` (`system_settings.hpp:114`), `SystemSettings` (`system_settings.hpp:116-165`), `JsonFileSettingsBackend`, `AdminFixture` (`tests/admin/admin_fixture.hpp`), `to_json(const Value&)`.
- Produces: tests only. Test suite name `SettingsGolden` for the new core tests (in `system_settings_test`), `Routes.SettingsErrorBodiesAreUnchanged` for REST.

- [ ] **Step 1: Write the tests** against the current tree (characterisation: they pass now).

```cpp
// tests/core/system_settings_test.cpp
// The bytes GET /settings, PUT's reply and system.settings carry for the
// defaults: keys, value types and values (spec 3.10).
TEST(SettingsGolden, DefaultMapJson) {
    EXPECT_EQ(wisp::to_json(Value(wisp::core::to_map(SystemSettingsValues{}))),
              R"({"agent_max_steps":60,"agent_turn_min":15,"brightness":200,"deep_after_s":300,)"
              R"("language":"zh","notify_vol":60,"ntp_servers":"pool.ntp.org,ntp.aliyun.com",)"
              R"("screen_off_s":60,"time_24h":true,"tts_mute":false,"utc_offset_min":480,"volume":60})");
}
// Each value's type, for a non-default set.
TEST(SettingsGolden, NonDefaultMapTypes)
// v: brightness 10, volume 0, screen_off_s 0, language "ja", utc_offset_min -210,
// ntp_servers "10.0.0.1", deep_after_s 3600, notify_vol 100, tts_mute true, time_24h false,
// agent_max_steps 200, agent_turn_min 2. to_map(v): 12 keys; Int for the eight integers,
// String for language and ntp_servers, Bool for tts_mute and time_24h; each value as set.
// A store written the way the current release writes it (the JSON file
// backend's format) reads back as these values; Task 4 must keep it.
TEST(SystemSettings, StoreWrittenByTheOldCodeReadsTheSame)
// Write through core::Settings on a JsonFileSettingsBackend: display/brightness 90,
// audio/volume 35, power/screen_off_s 120, sys/lang "en", sys/tz_min 330,
// sys/ntp "a.example,b.example", power/deep_after_s 1800, audio/notify_vol 20,
// voice/tts_mute true, sys/time_24h false, agent/max_steps 100, agent/turn_min 30,
// plus two out-of-range ones in a second store: display/brightness 5, sys/lang "fr".
// A SystemSettings on the first store: get() equals each value (language via the
// field's current type); to_map(get()) equals the Map of those twelve values.
// On the second store: brightness 200, language "zh".
TEST(SettingsGolden, EventPayloadIsTheMapOfTheStoredValues)
// Fixture f; f.settings.set({{"volume", 30}}); f.changed().back() == to_map(f.settings.get()).
```

```cpp
// tests/admin/routes_test.cpp
TEST(Routes, SettingsErrorBodiesAreUnchanged)
// PUT {"color":1}       -> 400, body {"error":{"code":"invalid_argument","message":"unknown setting: color"}}
// PUT {"volume":"30"}   -> 400, body {"error":{"code":"invalid_argument","message":"invalid value: volume"}}
// PUT {"brightness":5}  -> 400, body {"error":{"code":"invalid_argument","message":"invalid value: brightness"}}
// PUT "[1]"             -> 400, error code "invalid_argument" (bad_request's message as it is now: paste it)
// Compare the raw body strings, not parsed JSON.
```

- [ ] **Step 2: Run** `ctest --test-dir build/sim -R 'SettingsGolden|SystemSettings.StoreWrittenByTheOldCodeReadsTheSame|Routes.SettingsErrorBodiesAreUnchanged' --output-on-failure` three times. Expected: PASS each time. If a literal above differs from what the current code produces, the literal is wrong only if the code's output contradicts spec 1.2 or `docs/architecture.md:131`; otherwise paste the code's output and note it in the commit body.
- [ ] **Step 3: Commit** `test(core): pin the settings wire format and stored-value reading`.

---

### Task 2: The schema table

**Files:**
- Create: `components/wisp_core/include/wisp/core/settings_schema.hpp`, `components/wisp_core/src/settings_schema.cpp`
- Modify: `components/wisp_core/CMakeLists.txt` (add `src/settings_schema.cpp` to `SRCS`, line 12 next to `src/system_settings.cpp`)
- Create: `tests/core/settings_schema_test.cpp`; Modify: `tests/CMakeLists.txt` (after line 63: `wisp_test(NAME settings_schema_test SRCS core/settings_schema_test.cpp LIBS wisp_core wisp_port_sim_os)`)

**Interfaces:**
- Consumes: `wisp::Value` (`wisp/base/value.hpp`), `wisp::FixedString<N>` (`wisp/base/fixed_string.hpp`), `core::validate_settings_name()` (`settings.hpp:35`).
- Produces (`namespace wisp::core`, all in `settings_schema.hpp` unless marked `.cpp`):

```cpp
enum class SettingType : uint8_t { Int, Bool, Enum, HostList };
enum class SettingUnit : uint8_t { None, Level, Percent, Seconds, Minutes, UtcOffsetMinutes, Steps };
inline constexpr uint8_t kSettingAgent = 1 << 0;       // settings.get / settings.set
inline constexpr uint8_t kSettingScene = 1 << 1;       // wisp.device.info(), under scene_name
inline constexpr uint8_t kSettingWeb = 1 << 2;         // GET/PUT /settings, system.settings, /settings/schema
inline constexpr uint8_t kSettingDevicePage = 1 << 3;  // the Shell settings page
inline constexpr uint8_t kSettingZeroNever = 1 << 4;   // 0 is valid ("never") besides [min, max]

enum class LangCode : uint8_t { Zh, Ja, En };
inline constexpr std::string_view kLangCodeValues[] = {"zh", "ja", "en"};  // index = LangCode
constexpr std::string_view lang_code(LangCode c);
constexpr std::optional<LangCode> parse_lang_code(std::string_view code);

inline constexpr int32_t kScreenOffChoices[] = {0, 15, 30, 60, 120, 300, 600};
inline constexpr int32_t kDeepAfterChoices[] = {0, 60, 120, 300, 600, 1800, 3600};
inline constexpr int32_t kUtcOffsetChoices[] = {
    -720, -660, -600, -570, -540, -480, -420, -360, -300, -240, -210, -180, -120,
    -60, 0, 60, 120, 180, 210, 240, 270, 300, 330, 345, 360, 390, 420, 480, 525,
    540, 570, 600, 630, 660, 720, 765, 780, 840};
inline constexpr std::span<const int32_t> kNoIntChoices{};
inline constexpr std::span<const std::string_view> kNoEnumValues{};

struct SettingSpec {   // every member defaulted (missing-field-initializers)
    std::string_view name{};        // wire name: REST, event, agent; also the snapshot field
    std::string_view ns{};          // store namespace, 1-15 chars
    std::string_view key{};         // store key, 1-15 chars
    SettingType type = SettingType::Int;
    int32_t min = 0;                // Int: lowest; HostList: max entries
    int32_t max = 0;                // Int: highest; HostList: max entry length
    int32_t def_int = 0;            // Int value; Bool 0/1; Enum index into enum_values
    std::string_view def_text{};    // HostList only
    std::span<const int32_t> int_choices{};
    std::span<const std::string_view> enum_values{};
    SettingUnit unit = SettingUnit::None;
    uint8_t flags = 0;
    std::string_view label{};       // label id (spec 3.1); empty: no surface shows it
    std::string_view scene_name{};  // wisp.device.info() field; set exactly with kSettingScene
};

enum class SettingId : uint8_t { /* one per row, in row order */ Count };
inline constexpr SettingSpec kSettingsSchema[] = { /* expanded */ };
constexpr const SettingSpec& setting_spec(SettingId id);
constexpr SettingId setting_id_of(const SettingSpec& spec);     // its index
constexpr int32_t setting_default_int(SettingId id);
constexpr std::string_view setting_default_text(SettingId id);  // Enum: the default code; HostList: def_text; else ""
constexpr const SettingSpec* find_setting(std::string_view name);  // nullptr if none

struct SettingsSnapshot {  // generated; each member's initializer is its row's default
    /* int brightness = ...; ... LangCode language = ...; FixedString<127> ntp_servers = ...; ... */
    bool operator==(const SettingsSnapshot&) const = default;
};

// .cpp
bool valid_host_list(std::string_view list, int32_t max_entries, int32_t max_entry_len);
bool valid_setting(const SettingSpec& spec, const Value& v);   // type and range, as SystemSettings::set() checks
Value setting_value(const SettingsSnapshot& s, SettingId id);  // Int -> Value(int64_t), Bool, Enum -> code string, HostList -> string
bool assign_setting(SettingsSnapshot& s, SettingId id, const Value& v);  // false (s unchanged) unless valid_setting
// Calls f with int, bool or std::string_view (Enum: the code; HostList: the text). No allocation.
template <class F> decltype(auto) visit_setting(const SettingsSnapshot& s, SettingId id, F&& f);
```

The list, exactly these rows in this order, inside `// clang-format off` / `// clang-format on` so each `X(...)` stays on one line (Task 17's checker parses one row per line). Columns: `Id, name, C++ field type, SettingType, ns, key, min, max, def_int, def_text, int_choices, enum_values, SettingUnit, flags, label, scene_name`:

```cpp
#define WISP_SETTINGS(X) \
X(Brightness,    brightness,      int,              Int,      "display", "brightness",   10,   255,  200, "",                            kNoIntChoices,     kNoEnumValues,   Level,            kSettingAgent | kSettingWeb | kSettingDevicePage,                     "brightness",    "") \
X(Volume,        volume,          int,              Int,      "audio",   "volume",       0,    100,  60,  "",                            kNoIntChoices,     kNoEnumValues,   Percent,          kSettingAgent | kSettingWeb | kSettingDevicePage,                     "volume",        "") \
X(ScreenOffS,    screen_off_s,    int,              Int,      "power",   "screen_off_s", 15,   3600, 60,  "",                            kScreenOffChoices, kNoEnumValues,   Seconds,          kSettingAgent | kSettingWeb | kSettingDevicePage | kSettingZeroNever, "screen_off",    "") \
X(Language,      language,        LangCode,         Enum,     "sys",     "lang",         0,    0,    0,   "",                            kNoIntChoices,     kLangCodeValues, None,             kSettingAgent | kSettingScene | kSettingWeb | kSettingDevicePage,     "lang_label",    "lang") \
X(UtcOffsetMin,  utc_offset_min,  int,              Int,      "sys",     "tz_min",       -720, 840,  480, "",                            kUtcOffsetChoices, kNoEnumValues,   UtcOffsetMinutes, kSettingScene | kSettingWeb | kSettingDevicePage,                     "tz_label",      "utc_offset") \
X(NtpServers,    ntp_servers,     FixedString<127>, HostList, "sys",     "ntp",          2,    63,   0,   "pool.ntp.org,ntp.aliyun.com", kNoIntChoices,     kNoEnumValues,   None,             kSettingWeb,                                                          "time_servers",  "") \
X(DeepAfterS,    deep_after_s,    int,              Int,      "power",   "deep_after_s", 60,   3600, 300, "",                            kDeepAfterChoices, kNoEnumValues,   Seconds,          kSettingWeb | kSettingDevicePage | kSettingZeroNever,                 "deep_after",    "") \
X(NotifyVol,     notify_vol,      int,              Int,      "audio",   "notify_vol",   0,    100,  60,  "",                            kNoIntChoices,     kNoEnumValues,   Percent,          kSettingAgent | kSettingWeb | kSettingDevicePage,                     "notify_vol",    "") \
X(TtsMute,       tts_mute,        bool,             Bool,     "voice",   "tts_mute",     0,    1,    0,   "",                            kNoIntChoices,     kNoEnumValues,   None,             kSettingWeb | kSettingDevicePage,                                     "tts_mute",      "") \
X(Time24h,       time_24h,        bool,             Bool,     "sys",     "time_24h",     0,    1,    1,   "",                            kNoIntChoices,     kNoEnumValues,   None,             kSettingAgent | kSettingScene | kSettingWeb | kSettingDevicePage,     "time_24h",      "time_24h") \
X(AgentMaxSteps, agent_max_steps, int,              Int,      "agent",   "max_steps",    8,    200,  60,  "",                            kNoIntChoices,     kNoEnumValues,   Steps,            kSettingWeb,                                                          "agent_steps",   "") \
X(AgentTurnMin,  agent_turn_min,  int,              Int,      "agent",   "turn_min",     2,    60,   15,  "",                            kNoIntChoices,     kNoEnumValues,   Minutes,          kSettingWeb,                                                          "agent_minutes", "") \
X(LowBattPct,    low_batt_pct,    int,              Int,      "power",   "low_batt_pct", 5,    50,   15,  "",                            kNoIntChoices,     kNoEnumValues,   Percent,          0,                                                                    "",              "") \
X(PerfOverlay,   perf_overlay,    bool,             Bool,     "ui",      "perf_overlay", 0,    1,    0,   "",                            kNoIntChoices,     kNoEnumValues,   None,             0,                                                                    "",              "")
```

(The three keys the agent gains, `tts_mute`, `deep_after_s`, `utc_offset_min`, get `kSettingAgent` in Task 8, after the table has reproduced today's six.)

Snapshot members are generated as `ctype name = setting_init<ctype>(SettingId::Id);`, with `setting_init` overloads: `int` → `def_int`; `bool` → `def_int != 0`; `LangCode` → `static_cast<LangCode>(def_int)`; `FixedString<127>` → `FixedString<127>(def_text)`.

`static_assert`s in the header, over a `consteval bool` walk of `kSettingsSchema`: row count equals `SettingId::Count`; names unique; `(ns, key)` unique; `ns`/`key` 1-15 characters; each default valid by the row's rule (Int: in range or 0 with `kSettingZeroNever`; Bool 0/1; Enum index in range; HostList `def_text` passes the same syntax rule, written as a `constexpr` helper the `.cpp`'s `valid_host_list()` calls); each Int choice valid; `kSettingScene` set exactly when `scene_name` is non-empty; a row with any of `kSettingAgent | kSettingWeb | kSettingDevicePage` has a non-empty label; `std::is_trivially_copyable_v<SettingsSnapshot>`; `sizeof(SettingsSnapshot) <= 256`.

`valid_host_list()` is today's `valid_ntp_servers()` (`system_settings.cpp:66-86`) with `2` and `63` taken from its parameters.

- [ ] **Step 1: Write the failing tests** (`tests/core/settings_schema_test.cpp`):

```cpp
TEST(SettingsSchema, RowsInTableOrder)
// names == {"brightness","volume","screen_off_s","language","utc_offset_min","ntp_servers",
//           "deep_after_s","notify_vol","tts_mute","time_24h","agent_max_steps",
//           "agent_turn_min","low_batt_pct","perf_overlay"}
TEST(SettingsSchema, StoreNamesAreValidAndUnique)
// every ns and key passes validate_settings_name(); no (ns,key) twice; no name twice.
TEST(SettingsSchema, DefaultsMatchTodaysValues)
// SettingsSnapshot d{}; SystemSettingsValues old{};  (the hand-written struct still exists)
// d.brightness == old.brightness ... for the twelve; d.language == LangCode::Zh;
// d.ntp_servers == std::string_view(old.ntp_servers); d.low_batt_pct == 15; d.perf_overlay == false.
TEST(SettingsSchema, ValidationPerRow)
// valid_setting(spec, Value):
//   brightness: 9 F, 10 T, 255 T, 256 F, "200" F;   volume: -1 F, 0 T, 100 T, 101 F, 30.5 F
//   screen_off_s: 0 T, 14 F, 15 T, 3600 T, 3601 F;  deep_after_s: 0 T, 59 F, 60 T, 3601 F
//   utc_offset_min: -721 F, -720 T, 840 T, 841 F;   language: "zh" "ja" "en" T, "fr" F, 1 F
//   ntp_servers: "a.b" T, "a,b" T, "a,b,c" F, "" F, "a b" F, 64 x 'a' F, 63 x 'a' T, 1 F
//   tts_mute: true T, 1 F;  time_24h: false T, "true" F
//   agent_max_steps: 7 F, 8 T, 200 T, 201 F;  agent_turn_min: 1 F, 2 T, 60 T, 61 F
//   low_batt_pct: 4 F, 5 T, 50 T, 51 F;  perf_overlay: true T, 0 F
TEST(SettingsSchema, FlagsReproduceTodaysExposure)
// agent: {brightness, volume, screen_off_s, language, notify_vol, time_24h}
// web: the first twelve rows; scene: {language, utc_offset_min, time_24h} with scene names
// {"lang","utc_offset","time_24h"}; device page: {brightness, volume, screen_off_s, language,
// utc_offset_min, deep_after_s, notify_vol, tts_mute, time_24h}; zero-never: {screen_off_s, deep_after_s}.
TEST(SettingsSchema, ValuesAndVisitAgree)
// default snapshot: setting_value(.., Language) == Value("zh"); (.., NtpServers) ==
// Value("pool.ntp.org,ntp.aliyun.com"); (.., Brightness) == Value(int64_t{200});
// visit_setting(.., Language, f) calls f with string_view "zh"; assign_setting(s, Language,
// Value("ja")) -> true and s.language == LangCode::Ja; assign_setting(s, Brightness, Value(5)) -> false, unchanged.
TEST(SettingsSchema, LangCodes)
// lang_code(LangCode::Ja) == "ja"; parse_lang_code("en") == LangCode::En; parse_lang_code("fr") == nullopt.
TEST(SettingsSchema, DefaultAccessors)
// setting_default_int(SettingId::UtcOffsetMin) == 480; setting_default_text(SettingId::Language) == "zh";
// setting_default_text(SettingId::NtpServers) == "pool.ntp.org,ntp.aliyun.com"; find_setting("nope") == nullptr.
```

- [ ] **Step 2: Run** `cmake --build build/sim && ctest --test-dir build/sim -R SettingsSchema --output-on-failure`. Expected: build fails (no header).
- [ ] **Step 3: Implement** the header and `.cpp` as specified. The header comment states: the list is the one place a setting is declared (spec 3.2); presentation lives with each surface; Task 17's checker parses one `X(` per line.
- [ ] **Step 4: Run** the same filter plus `-R "$GOLDEN"`. Expected: PASS.
- [ ] **Step 5: Commit** `feat(core): declare every system setting in one schema table`.

---

### Task 3: Device labels and the language enum bridge

**Files:**
- Create: `components/wisp_ui/include/wisp/ui/setting_labels.hpp`, `components/wisp_ui/src/setting_labels.cpp`; Modify: `components/wisp_ui/CMakeLists.txt` (`WISP_UI_SRCS`, lines 28-37)
- Modify: `components/wisp_ui/include/wisp/ui/strings.hpp` (near `parse_lang`, line 600)
- Create: `tests/ui/setting_labels_test.cpp`; Modify: `tests/CMakeLists.txt` (after `strings_test`, line 119):
  `wisp_test(NAME setting_labels_test SRCS ui/setting_labels_test.cpp LIBS wisp_ui wisp_port_sim_os)` and `target_compile_definitions(setting_labels_test PRIVATE WISP_WEB_I18N_DIR="${WISP_ROOT}/web/app/i18n")`

**Interfaces:**
- Consumes: `core::kSettingsSchema`, `core::LangCode`, `core::lang_code()` (Task 2); `ui::StrId`, `ui::Lang`, `ui::lang_code(Lang)` (`strings.hpp:600-601`).
- Produces:

```cpp
// wisp/ui/setting_labels.hpp
namespace wisp::ui {
// The device text id for a schema label id (core::SettingSpec::label).
std::optional<StrId> setting_label(std::string_view label);
}
// wisp/ui/strings.hpp
constexpr Lang to_lang(core::LangCode c);       // static_cast, guarded by static_asserts below
constexpr core::LangCode to_lang_code(Lang l);
```

  Mapping (the settings page's ids, decision 7): `brightness`→`SettingsBrightness`, `volume`→`SettingsVolume`, `notify_vol`→`SettingsNotifyVol`, `tts_mute`→`SettingsSpeak`, `screen_off`→`SettingsScreenOff`, `deep_after`→`SettingsDeepStandby`, `lang_label`→`SettingsLanguage`, `tz_label`→`SettingsTimeZone`, `time_24h`→`SettingsTimeFormat`. The web-only labels (`time_servers`, `agent_steps`, `agent_minutes`) have no device text.

- [ ] **Step 1: Write the failing tests:**

```cpp
TEST(SettingLabels, EveryDeviceShownKeyHasALabel)
// for each row with kSettingAgent | kSettingDevicePage: setting_label(row.label).has_value()
// (strings_test's EveryIdHasTextInEveryLanguage already checks the text in zh/ja/en).
TEST(SettingLabels, EveryWebKeyHasAWebLabel)
// read WISP_WEB_I18N_DIR "/en.js"; for each row with kSettingWeb: the text contains "'sys." + label + "':".
TEST(SettingLabels, LangCodeMatchesUiLang)
// for each i in 0..2: lang_code(kAllLangs[i]) == core::lang_code(core::LangCode(i));
// to_lang(core::LangCode::Ja) == Lang::Ja; to_lang_code(Lang::En) == core::LangCode::En.
```

- [ ] **Step 2: Run** `ctest --test-dir build/sim -R SettingLabels --output-on-failure`. Expected: build fails.
- [ ] **Step 3: Implement.** `static_assert`s beside `to_lang()`: `Lang::Zh/Ja/En` equal `LangCode::Zh/Ja/En` numerically.
- [ ] **Step 4: Run** `-R 'SettingLabels|Strings'`. Expected: PASS.
- [ ] **Step 5: Commit** `feat(ui): map schema label ids to device text`.

---

### Task 4: `SystemSettings` on the table; the snapshot type changes

**Files:**
- Modify: `components/wisp_core/include/wisp/core/system_settings.hpp` (lines 3-17 header comment, 60-105 constants and struct, 110-114 `to_map`), `components/wisp_core/src/system_settings.cpp` (lines 19-175 rewritten as loops)
- Modify (language/ntp readers): `components/wisp_app/src/system_duties.cpp:330, 398`; `components/wisp_scene/src/scene_runtime.cpp:220-235, 2257-2261`; `components/wisp_app/src/app.cpp:99-103, 375-377`; `components/wisp_shell/src/settings_page.cpp:32-36, 108, 265, 318-326`; `components/wisp_app/src/net_wiring.cpp:108`; `components/wisp_net/include/wisp/net/time_sync.hpp:93` and its definition (take `std::string_view`, unchanged if it already does)
- Modify (tests, mechanical): every test line that assigns or compares `SystemSettingsValues::language` with a string (35 lines across `tests/`; `grep -rn "\.language\b" tests | grep -vi model`) and `ntp_servers` with `std::string`
- Test: `tests/core/system_settings_test.cpp`, `tests/app/app_boot_test.cpp`

**Interfaces:**
- Consumes: Task 2 (`SettingsSnapshot`, `kSettingsSchema`, `valid_setting`, `assign_setting`, `setting_value`, `setting_default_int/text`), Task 3 (`ui::to_lang`).
- Produces:
  - `using SystemSettingsValues = SettingsSnapshot;` (removed in Task 17).
  - The `kDefault*`, `kMin*`, `kMax*` constants and `kDefaultNtpServers`, `kDefaultLanguage` stay as names, defined from the table (`inline constexpr int64_t kDefaultBrightness = setting_default_int(SettingId::Brightness);`, `kDefaultLanguage = setting_default_text(SettingId::Language)` as `std::string_view`), until Task 17 deletes them. `valid_screen_off_s()`/`valid_deep_after_s()` call `valid_setting()`.
  - `Map to_map(const SettingsSnapshot&)`: every `kSettingWeb` row, `setting_value()`. (Spec 3.4's "in table order" is moot: `Map` is a `std::map`, so the JSON is alphabetical, as today.)
  - `SystemSettings::get()`: each row from the store, `assign_setting()`; a missing or invalid value keeps the default. `seed_cell()`: as today, plus one `WLOG_W(kTag, "stored %s/%s is not valid; using the default", ns, key)` per row whose stored value is present but invalid.
  - `set()`: validation by `find_setting(name)` + `valid_setting()`; messages unchanged (`"unknown setting: " + name`, `"invalid value: " + name`); rollback, `apply_brightness_` and the event exactly as today (lines 177-236).
  - `SettingsSnapshot::language` is `LangCode`; `ntp_servers` is `FixedString<127>`. Readers: `ui::parse_lang(v.language)` → `ui::to_lang(v.language)`; `settings.get().ntp_servers` → `.str()` where a `std::string` is needed.

- [ ] **Step 1: Write the failing tests:**

```cpp
TEST(SystemSettings, InvalidStoredValueLogsOnceAtSeed)
// store display/brightness 5 and sys/lang "fr"; CaptureSink (as edge_inbox_test.cpp:37);
// seed_cell(); exactly two lines containing "not valid", one naming "display/brightness",
// one "sys/lang"; cell().get().brightness == 200; a second seed_cell() logs them again
// (once per seed, not per read: get() alone logs nothing).
TEST(AppBoot, StoredLanguageReachesTheScreenScenesAndAgent)
// make_app({.with_display = true}); store sys/lang "ja" before start(); after start():
// ui::lang() == Lang::Ja; app.system_settings().cell().get().language == LangCode::Ja.
```
  Update `SettingsSchema.DefaultsMatchTodaysValues` (Task 2) to compare with literal values instead of the old struct, which is now the same type.
- [ ] **Step 2: Run** `ctest --test-dir build/sim -R 'SystemSettings|SettingsSchema|AppBoot' --output-on-failure`. Expected: FAIL (no log; type mismatches until the readers move).
- [ ] **Step 3: Implement** the loops, the alias and the reader changes listed under Files. In `settings_page.cpp`, the minimum to compile: `lang_name()` takes `core::LangCode`; `now_.language` comparisons and assignments use `core::parse_lang_code(kLanguages[i])` (Task 11 replaces `kLanguages`).
- [ ] **Step 4: Run** `bash tools/ci.sh` and `ctest --test-dir build/sim -R "$GOLDEN|SystemSettings|AppBoot|SystemDuties|SceneApiTest|SettingsPageTest|NetWiring|FactoryReset" --output-on-failure`. Expected: PASS; `SystemSettings.StoreWrittenByTheOldCodeReadsTheSame` and every golden unchanged (only its comparison of `language` may switch from `"en"` to `LangCode::En`).
- [ ] **Step 5: Commit** `refactor(core): derive SystemSettings and its snapshot from the schema`. Body lists the tests edited mechanically.

---

### Task 5: Each surface writes only its keys

**Files:**
- Modify: `components/wisp_core/include/wisp/core/system_settings.hpp` (`set()` at lines 145-154), `components/wisp_core/src/system_settings.cpp` (`set()`)
- Modify: `components/wisp_admin/src/routes_device.cpp:63-74` (`put_settings`; also fix the stale comment at 63-66), `components/wisp_agent/src/tools_device.cpp:119` (`d->settings.set(...)`), `components/wisp_app/src/app.cpp:819-823` (the Shell's `set_settings`)
- Test: `tests/core/system_settings_test.cpp`, `tests/admin/routes_test.cpp`

**Interfaces:**
- Produces:

```cpp
namespace wisp::core {
enum class SettingSurface : uint8_t { Internal, Agent, Web, DevicePage };
constexpr uint8_t surface_flag(SettingSurface s);  // Agent->kSettingAgent, Web->kSettingWeb, DevicePage->kSettingDevicePage, Internal->0
}
Result<void> SystemSettings::set(const std::map<std::string, Value>& changes,
                                 SettingSurface from = SettingSurface::Internal);
```
  A key whose row lacks `surface_flag(from)` (when `from != Internal`) fails the whole batch with `InvalidArgument`, message `"unknown setting: " + name`, before anything is written.
- Callers: `put_settings` passes `SettingSurface::Web`; `settings.set` passes `SettingSurface::Agent`; App's Shell lambda passes `SettingSurface::DevicePage`.

- [ ] **Step 1: Write the failing tests:**

```cpp
TEST(SystemSettings, SurfaceRefusesKeysOutsideItsFlags)
// Web: {"low_batt_pct":20} and {"perf_overlay":true} -> InvalidArgument "unknown setting: <key>";
//      all twelve web keys in one batch (non-default valid values) -> ok.
// Agent: {"deep_after_s":600}, {"agent_max_steps":100}, {"ntp_servers":"a.b"} -> refused;
//        {"brightness":120} -> ok.
// DevicePage: {"agent_max_steps":100}, {"ntp_servers":"a.b"} -> refused; {"tts_mute":true} -> ok.
// Internal: {"perf_overlay":true}, {"low_batt_pct":20} -> ok.
// A refused batch writes nothing and publishes no system.settings.
TEST(Routes, SettingsPutRefusesStoreOnlyKeys)
// PUT {"low_batt_pct":20} -> 400, body {"error":{"code":"invalid_argument","message":"unknown setting: low_batt_pct"}};
// GET /settings unchanged and has no low_batt_pct / perf_overlay key.
```

- [ ] **Step 2: Run** `-R 'SystemSettings|Routes.Settings'`. Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `-R "$GOLDEN|SystemSettings|Routes|DeviceTools|SettingsPageTest"`. Expected: PASS.
- [ ] **Step 5: Commit** `feat(core): let each settings surface write only its own keys`.

---

### Task 6: The store-only keys read from the cell

**Files:**
- Modify: `components/wisp_core/include/wisp/core/power_monitor.hpp` (constructor 96-112, members 163-197, header lines 43 and 100-112), `components/wisp_core/src/power_monitor.cpp` (21-29, 75-76, 166-171)
- Modify: `components/wisp_app/src/app.cpp:425-426` (pass `&impl_->system_settings.cell()`)
- Modify: `components/wisp_app/src/system_duties.cpp:135-139`; `components/wisp_app/src/self_check_screen.hpp:42, 163`; `components/wisp_app/src/self_check_screen.cpp:80, 163, 432-436`
- Test: `tests/core/power_monitor_test.cpp` (fixture 40-60; tests 298-404), `tests/app/net_wiring_test.cpp:146-147`, `tests/app/power_manager_test.cpp:168`, `tests/app/self_check_screen_test.cpp:1013-1060`

**Interfaces:**
- Produces:

```cpp
PowerMonitor(EventBus& bus, std::shared_ptr<hal::Power> power,
             const StateCell<SettingsSnapshot>* settings);  // null: the schema default threshold
```
  `sample()` reads `low_batt_pct` with `settings->read(...)` on every successful sample (memory only; it runs on the esp_timer task). `cached_low_batt_pct_`, `samples_until_threshold_refresh_`, `kThresholdRefreshSamples`, `kDefaultLowBattPct` and the `Settings` member go. `kLowBattHysteresis = 5` stays, with `// settings-literal-ok: hysteresis, not the threshold` (Decisions item 5).
- `SystemDuties::start()` reads `app_.system_settings().cell().get().perf_overlay`; `SelfCheck` reads it the same way at build and writes with `app_.system_settings().set({{"perf_overlay", Value(overlay_enabled_)}})` (Internal). `SelfCheck::ui_settings_` goes.

- [ ] **Step 1: Write the failing tests:**

```cpp
TEST(PowerMonitor, ThresholdFollowsTheSettingsCell)
// cell with low_batt_pct 30; device at 25 %: sample() -> power.battery.low {battery_percent:25}.
// cell set to 20 (device back to 40 then 25): the next crossing uses 20 (25 > 20: no event).
TEST(PowerMonitor, NoCellUsesTheSchemaDefault)
// settings = nullptr; 15 % -> one power.battery.low; 16 % from a fresh monitor -> none.
TEST(SystemDutiesTest, PerfOverlaySettingPersistsAcrossRestarts)  // existing, rewritten
// toggled through the diagnostics: stored as ui/perf_overlay true via SystemSettings;
// a new App on the same store starts with the overlay on.
```
  Delete `SettingsOverrideLowBattThreshold`, `ThresholdCacheIsNotRereadOnTheVeryNextSample`, `ThresholdCacheIsRereadAfterRefreshWindow`, `ThresholdCacheIsRereadAfterStart` (the cache is gone; the first test above replaces them).
- [ ] **Step 2: Run** `-R 'PowerMonitor|SystemDutiesTest|SelfCheck'`. Expected: build fails.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `-R "$GOLDEN|PowerMonitor|PowerManager|NetWiring|SystemDuties|SelfCheck|AppBoot"`. Expected: PASS.
- [ ] **Step 5: Commit** `refactor(core): read the low-battery threshold and the overlay from the settings cell`.

---

### Task 7: Agent settings tools from the table

**Files:**
- Modify: `components/wisp_agent/src/tools_device.cpp` (1-5 header, 27-53 `kKeys`/`agent_view`/`text_of`, 82-136 the two tools), `components/wisp_agent/include/wisp/agent/tools.hpp:59-68`
- Modify: `components/wisp_ui/include/wisp/ui/strings.hpp:261-271` and `components/wisp_ui/src/strings_{zh,ja,en}.cpp` (delete the six `AgentSet*` ids and their cases; no character leaves the font: every character of those strings is used elsewhere in its table, checked when this plan was written)
- Test: `tests/agent/device_tools_test.cpp` (90-148)

**Interfaces:**
- Consumes: `kSettingsSchema`, `kSettingAgent`, `setting_value`, `SystemSettings::cell()`, `SettingSurface::Agent`, `ui::setting_label`.
- Produces in `tools_device.cpp` (file-local, but its format is pinned by the test):

```cpp
// "<name> (<unit prefix><zero-never>min to max)" for Int; unit prefix: Seconds "seconds, ",
// Minutes "minutes, ", UtcOffsetMinutes "minutes east of UTC, ", Percent "percent, ", else "";
// zero-never "0 never, or ". Enum: "<name> (zh, ja, en)". Bool: "<name> (<meaning>)" with
// meanings time_24h "true: 24-hour clock, false: 12-hour", tts_mute "true: replies are shown
// but not spoken", any other "true or false".
std::string describe_setting(const core::SettingSpec& spec);
```
  `settings.get` description: `"The settings the agent may change: "` + the agent rows' clauses joined by `", "` + `"."`. `settings.set` schema: `{"type":"object","properties":{"key":{"type":"string","enum":[<agent names in table order>]},"value":{"type":["integer","string","boolean"]}},"required":["key","value"]}`; description unchanged. `settings.get` reads `d->settings.cell().get()` without `run_io`; `settings.set` keeps `run_io` and calls `set({{key, value}}, SettingSurface::Agent)`. The note label is `ui::setting_label(spec.label)`; `text_of()` keeps `time_24h` → `24h`/`12h` and renders `tts_mute` as `on`/`off` of spoken replies (`!mute`; Decisions item 7). `DeviceToolDeps::settings`'s comment changes to "read from its cell; written inside submit_io jobs".

- [ ] **Step 1: Write the failing tests** (rewrite the existing ones where noted):

```cpp
TEST_F(DeviceTools, SettingsGetDescriptionIsGenerated)
// the registry's settings.get description ==
// "The settings the agent may change: brightness (10 to 255), volume (percent, 0 to 100), "
// "screen_off_s (seconds, 0 never, or 15 to 3600), language (zh, ja, en), notify_vol "
// "(percent, 0 to 100), time_24h (true: 24-hour clock, false: 12-hour)."
TEST_F(DeviceTools, SettingsSetSchemaListsTheAgentKeys)
// the parsed schema's properties.key.enum ==
// ["brightness","volume","screen_off_s","language","notify_vol","time_24h"]
TEST_F(DeviceTools, SettingsSetValidatesLikeTheWebPage)  // existing; one expectation changes:
// language note "语言 zh → en" becomes "系统语言 zh → en" (decision 7); others unchanged.
TEST_F(DeviceTools, SettingsRunOnTheWorker)  // existing; now: settings_get submits nothing,
// each settings_set submits one job: EXPECT_EQ(submitted, 2) after one get and two sets.
```
- [ ] **Step 2: Run** `-R DeviceTools`. Expected: FAIL.
- [ ] **Step 3: Implement**; delete the `AgentSet*` ids.
- [ ] **Step 4: Run** `-R "$GOLDEN|DeviceTools|Strings|ToolRegistry|AgentLoop"` and `bash tools/ci.sh` (the CJK font test runs on string-table changes). Expected: PASS.
- [ ] **Step 5: Commit** `refactor(agent): derive the settings tools from the schema`.

---

### Task 8: The agent gets mute, deep standby and time zone

**Files:**
- Modify: `components/wisp_core/include/wisp/core/settings_schema.hpp` (add `kSettingAgent` to rows `UtcOffsetMin`, `DeepAfterS`, `TtsMute`)
- Test: `tests/core/settings_schema_test.cpp`, `tests/agent/device_tools_test.cpp`

**Interfaces:** none new. The agent list becomes, in table order: `brightness, volume, screen_off_s, language, utc_offset_min, deep_after_s, notify_vol, tts_mute, time_24h`. No confirmation (spec decision 6).

- [ ] **Step 1: Write the failing tests:**

```cpp
TEST_F(DeviceTools, SettingsGetListsNineKeys)  // replaces SettingsGetListsSixKeys
// keys (sorted Map) == {"brightness","deep_after_s","language","notify_vol","screen_off_s",
//                       "time_24h","tts_mute","utc_offset_min","volume"}
TEST_F(DeviceTools, AgentChangesMuteDeepStandbyAndTimeZone)
// settings_set tts_mute true -> ok, note "语音播报 on → off";
// deep_after_s 600 -> note "深度待机 300 → 600"; deep_after_s 0 -> ok; 30 -> invalid_value;
// utc_offset_min 540 -> note "时区 480 → 540"; 900 -> invalid_value.
TEST_F(DeviceTools, SettingsGetDescriptionIsGenerated)  // expected text updated:
// "... language (zh, ja, en), utc_offset_min (minutes east of UTC, -720 to 840), deep_after_s "
// "(seconds, 0 never, or 60 to 3600), notify_vol (percent, 0 to 100), tts_mute (true: replies "
// "are shown but not spoken), time_24h (true: 24-hour clock, false: 12-hour)."
```
  Update `SettingsSchema.FlagsReproduceTodaysExposure` → `SettingsSchema.AgentFlags` with the nine; `SystemSettings.SurfaceRefusesKeysOutsideItsFlags`' agent case uses `agent_max_steps` and `ntp_servers` only.
- [ ] **Step 2: Run** `-R 'DeviceTools|SettingsSchema|SystemSettings'`. Expected: FAIL.
- [ ] **Step 3: Implement** (three flags).
- [ ] **Step 4: Run** `-R "$GOLDEN|DeviceTools|SettingsSchema|SystemSettings|AgentLoop"`. Expected: PASS.
- [ ] **Step 5: Commit** `feat(agent): let the agent change mute, deep standby and the time zone`.

---

### Task 9: `wisp.device.info()` from the scene rows

**Files:**
- Modify: `components/wisp_scene/src/scene_api.hpp:186-200` (`DeviceInfo`), `components/wisp_scene/src/bind_device.cpp:342-346`, `components/wisp_scene/src/scene_runtime.cpp:2245-2262`, `components/wisp_scene/include/wisp/scene/scene_runtime.hpp:204-214` (comment), `:528-530` (delete `utc_offset_min`), `:554-558` (comment); every other reader of `DeviceInfo::lang`/`utc_offset_min`/`time_24h` (`grep -rn "\.lang\b\|utc_offset_min\|time_24h" components/wisp_scene`)
- Test: `tests/scene/scene_api_test.cpp:628-645` and new tests there

**Interfaces:**
- `DeviceInfo` loses `lang`, `utc_offset_min`, `time_24h` and gains `core::SettingsSnapshot settings{};` (trivially destructible: keep the `static_assert` at line 200).
- `device_info()` copies the cell's value into `settings` (memory read), or leaves the default-constructed snapshot without a cell (Decisions item 6).
- `bind_device.cpp` pushes one field per row with `kSettingScene`, named `scene_name`, through `core::visit_setting()` (int → integer, bool → boolean, string_view → string): the same three fields with the same values as today.

- [ ] **Step 1: Write the failing tests:**

```cpp
TEST_F(SceneApiTest, InfoSettingsFieldsAreTheSceneRows)
// a scene prints the keys of wisp.device.info() that are settings: exactly lang, utc_offset,
// time_24h (iterate a fixed list of every info key name and check the others are not settings).
TEST_F(SceneApiTest, InfoWithoutASettingsCell)  // existing; expected becomes {"zh\t480\ttrue"}
// and the configure_deps line setting d.utc_offset_min goes (the member is deleted).
```
- [ ] **Step 2: Run** `-R SceneApiTest`. Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `-R "$GOLDEN|SceneApiTest|SceneRuntimeTest|Examples|CheckBuild"`. Expected: PASS; `DeviceInfoShape` and `SettingsCellAtStartShowsInInfo` unchanged.
- [ ] **Step 5: Commit** `refactor(scene): fill wisp.device.info()'s settings from the schema`.

---

### Task 10: `GET /api/v1/settings/schema`

**Files:**
- Modify: `components/wisp_admin/src/routes_device.cpp:59-84` (new handler and route), `components/wisp_admin/src/admin_internal.hpp` or `admin_server.cpp` (where `AdminServer` builds per-server state; hold the body there)
- Test: `tests/admin/routes_test.cpp`

**Interfaces:**
- Produces: route `{Method::Get, "/settings/schema", false, get_settings_schema}` (paired, as `/settings`). Body, built once at `AdminServer` construction into `std::shared_ptr<const os::LargeString>` and served via `HttpResponse::static_body` + `body_owner` (Decisions item 4): `{"keys":[...]}`, one object per `kSettingWeb` row in table order, members in this order:
  - all: `"name"`, `"type"` (`"int"`, `"bool"`, `"enum"`, `"hosts"`), `"label"`;
  - Int: `"min"`, `"max"`, `"default"`, `"unit"` (`"none"`, `"level"`, `"percent"`, `"seconds"`, `"minutes"`, `"utc_offset_minutes"`, `"steps"`), `"zero_never": true` only with the flag, `"choices": [...]` only when non-empty;
  - Bool: `"default"` (`true`/`false`);
  - Enum: `"values": [...]`, `"default"` (the code);
  - HostList: `"max_entries"`, `"max_length"`, `"default"`.
  Serialised with the JSON writer used by `to_json` (string escaping), not by hand-concatenating unescaped strings.

- [ ] **Step 1: Write the failing tests:**

```cpp
TEST(Routes, SettingsSchemaListsTheWebKeys)
// GET /api/v1/settings/schema -> 200; parsed "keys" names == the first twelve rows in table order;
// brightness == {name, type "int", label "brightness", min 10, max 255, default 200, unit "level"};
// screen_off_s has zero_never true and choices [0,15,30,60,120,300,600];
// language == {name, type "enum", label "lang_label", values ["zh","ja","en"], default "zh"};
// ntp_servers == {name, type "hosts", label "time_servers", max_entries 2, max_length 63,
//                 default "pool.ntp.org,ntp.aliyun.com"};
// tts_mute == {name, type "bool", label "tts_mute", default false}.
TEST(Routes, SettingsSchemaNeedsAToken)   // 401 without a token, like /settings
TEST(Routes, SettingsSchemaIsTheSameBytesEachTime)  // two GETs: identical bodies
```
- [ ] **Step 2: Run** `-R 'Routes.SettingsSchema'`. Expected: FAIL (404).
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `-R "$GOLDEN|Routes|admin_smoke"` and `bash tools/ci.sh`. Expected: PASS.
- [ ] **Step 5: Commit** `feat(admin): describe the settings at GET /settings/schema`.

---

### Task 11: The Shell settings page takes values, ranges, choices and labels from the table

**Files:**
- Modify: `components/wisp_shell/include/wisp/shell/settings_page.hpp` (header comment 1-37, 86-95 constants, 168-176 members), `components/wisp_shell/src/settings_page.cpp` (28-36, 47-55, 99-112, 125-167, 212-269, 295-361, 394-441)
- Test: `tests/shell/settings_page_test.cpp`

**Interfaces:**
- Produces (public, for the test):

```cpp
namespace wisp::shell {
enum class SettingGroup : uint8_t { DisplaySound, ScreenPower, LangTime };
enum class SettingWidget : uint8_t { Slider, Switch, InvertedSwitch, Choice };
struct SettingRow {
    SettingGroup group = SettingGroup::DisplaySound;
    core::SettingId id = core::SettingId::Brightness;
    SettingWidget widget = SettingWidget::Slider;
};
class SettingsPage {
    // The settings rows, in page order (spec 3.8's hand-written layout list).
    static std::span<const SettingRow> setting_rows();
    // The languages in their display order (Decisions item 9).
    static constexpr std::array<core::LangCode, 3> kLanguageOrder = {
        core::LangCode::Zh, core::LangCode::En, core::LangCode::Ja};
    // (value - min) * 100 / (max - min), rounded to nearest: spec decision 3.
    static int percent(int value, const core::SettingSpec& spec);
    ...
};
}
```
  `setting_rows()`: `{DisplaySound, Brightness, Slider}`, `{DisplaySound, Volume, Slider}`, `{DisplaySound, NotifyVol, Slider}`, `{DisplaySound, TtsMute, InvertedSwitch}`, `{ScreenPower, ScreenOffS, Choice}`, `{ScreenPower, DeepAfterS, Choice}`, `{LangTime, Language, Choice}`, `{LangTime, UtcOffsetMin, Choice}`, `{LangTime, Time24h, Choice}`. The Wi-Fi, models and system groups and rows stay where they are (`settings_page.cpp:228-250`).
  - Deleted: `kScreenOffChoices`, `kDeepStandbyChoices`, `kLanguages`; the fallback values of `settings_page.cpp:103-110` (a default-constructed `core::SettingsSnapshot` instead); the slider literals of `:216-221` and `:405-409` (from `setting_spec(id).min/max`).
  - Choice lists: an Int row's `int_choices` with `0` moved to the end (shown "never" last, as today), then `with_current()`. Time-zone rows keep their city labels (`time_zones()`), now built over `kUtcOffsetChoices`.
  - Labels: `ui::setting_label(spec.label)` for each row's name and choice title.
  - Brightness and both volumes show `percent(value, spec)`.

- [ ] **Step 1: Write the failing tests:**

```cpp
TEST(SettingsPageLayout, EveryDevicePageKeyHasOneRow)
// the ids in setting_rows() are exactly the rows with kSettingDevicePage, each once.
TEST(SettingsPageLayout, EveryTimeZoneChoiceHasACity)
// for each kUtcOffsetChoices value: one time_zones() entry with that offset.
TEST(SettingsPageLayout, LanguageOrderIsAPermutationOfTheCodes)
TEST(SettingsPageLayout, PercentFollowsTheRange)
// percent(10, brightness) == 0; percent(255, brightness) == 100; percent(200, brightness) == 78;
// percent(60, volume) == 60.
TEST_F(SettingsPageTest, ControlsWriteSettings)  // existing; the brightness label check becomes
// text_of(brightness_value()) == std::to_string(SettingsPage::percent(v, brightness spec)) + "%".
// kChoiceCases (existing): ScreenOff labels become
//   {"15 秒","30 秒","1 分钟","2 分钟","5 分钟","10 分钟","从不"}, current 2, pick 6 -> 0 "从不";
// DeepStandby labels {"1 分钟","2 分钟","5 分钟","10 分钟","30 分钟","1 小时","从不"},
//   current 2, pick 5 -> 3600 "1 小时". The others unchanged.
TEST_F(SettingsPageTest, StoredValueOutsideTheListIsListed)  // existing; screen_off_s 120 is now
// in the list: use 45 ("45 秒") instead, expected list {"15 秒","30 秒","45 秒","1 分钟","2 分钟",
// "5 分钟","10 分钟","从不"}, current 2.
```
- [ ] **Step 2: Run** `-R 'SettingsPage'`. Expected: FAIL.
- [ ] **Step 3: Implement.** Build the settings rows by walking `setting_rows()`; keep `brightness()`, `volume()`, `notify_volume()`, `speak()` and `Row` accessors returning the same objects so the other tests stay.
- [ ] **Step 4: Run** `-R 'SettingsPage|SettingsAddress|Shell|Bench'`; render the page with the existing settings-page screenshot preview on rect 480, circle 466 and rounded_rect 480 and look at slider and row spacing (AGENTS.md section 7). Expected: PASS; layout as before apart from the two longer choice pages.
- [ ] **Step 5: Commit** `refactor(shell): take the settings page's ranges, choices and labels from the schema`.

---

### Task 12: The Shell settings page follows the cell

**Files:**
- Modify: `components/wisp_shell/include/wisp/shell/settings_page.hpp` (`SettingsDeps` 61-79, header comment 26-31), `components/wisp_shell/src/settings_page.cpp` (constructor 99-112, `write()` 363-374, destructor)
- Modify: `components/wisp_shell/include/wisp/shell/shell.hpp:245-246`, `components/wisp_shell/src/shell.cpp:443-446`, `components/wisp_app/src/app.cpp:818`
- Test: `tests/shell/settings_page_test.cpp` (fixture 31-80), `tests/shell/page_fixture.hpp` / `shell_fixture.hpp` if they set `settings`

**Interfaces:**
- `SettingsDeps::settings` becomes `const core::StateCell<core::SettingsSnapshot>* settings = nullptr;` (null: the schema defaults, nothing followed) plus `core::Dispatcher* dispatcher = nullptr;` (the Shell passes `&deps_.ui`). `ShellDeps::settings` becomes the same cell pointer; App passes `&impl->system_settings.cell()`.
- The page subscribes in `build()` on `dispatcher`, keeps the `CellSubscription` as a member released in the destructor (UI thread), and on each delivery stores the value and redraws every settings control (slider positions and labels, switch state, value rows). A write still goes through `submit_io`; the optimistic local value stays until the next delivery replaces it (closes `docs/backlog.md:195`).

- [ ] **Step 1: Write the failing tests** (fixture: a `StateCell<SettingsSnapshot>` on a test bus, set by the test; `set_settings` stores into it with `set()` to stand in for `SystemSettings`):

```cpp
TEST_F(SettingsPageTest, ChangeWhileOpenIsShown)
// page open; cell.set(volume 25, screen_off_s 300, language ja handled below); one UI iteration:
// slider value 25, label "25%", ScreenOff row "5 分钟".
TEST_F(SettingsPageTest, RefusedWriteShowsTheStoredValue)
// set_settings drops the write (does not touch the cell); pick screen-off "从不":
// row shows "从不"; then cell.set(same value as before, revision moves via another field):
// row shows the stored "1 分钟" again.
TEST_F(SettingsPageTest, ClosingThePageReleasesItsSubscription)
// open then leave the page; cell.set(...) afterwards runs no page code (cell subscription count back).
```
- [ ] **Step 2: Run** `-R SettingsPage`. Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `-R "SettingsPage|Shell|Bench|AppBoot|$GOLDEN"`. Expected: PASS.
- [ ] **Step 5: Commit** `fix(shell): show settings changed elsewhere while the settings page is open`.

---

### Task 13: The web System page reads the schema

**Files:**
- Modify: `web/app/pages/system.js` (75-95 constants, 119-133 typedef, 154-209 load/put, 243-345 `RangeField`/`percent`/`DisplayCard`, 374-435 `PowerCard`, 466-535 language and time-zone cards, 554-559 `tidyServers`, 707-770 `wholeIn`/`AgentLimitsCard`, the card list in `SystemPage`'s render)
- Modify: `web/app/i18n/en.js`, `ja.js`, `zh.js` (one key `sys.more_title`: "More settings" / "その他の設定" / "更多设置")

**Interfaces:**
- Consumes: `GET /api/v1/settings/schema` (Task 10) and `GET`/`PUT /settings` (unchanged).
- Produces (JS, file-local):
  - `loadSettings()` fetches `/settings` and `/settings/schema` together; `schema` state `{keys: [...]}`; `spec(name)` returns a key's entry.
  - Every constant of lines 75-95 except `POWER_REFRESH_MS`, `REBOOT_*`, `MIN_REAL_UNIX` is deleted; their uses read `spec(...)`: brightness and volume ranges, `SCREEN_OFF`/`DEEP_AFTER`/`UTC_OFFSETS` from `choices`, agent limits from `min`/`max`, `wholeIn()` against the spec's range. The `?? 60`, `?? 15` and `!== false` fallbacks (lines 317, 526, 719-720) go: the values always come from `/settings`.
  - `percent(v, min, max)` stays `(v - min) / (max - min)` (spec decision 3, the same as the Shell now).
  - The typedef comment lists the keys without ranges ("see GET /settings/schema").
  - `MoreCard`: every schema key the other cards do not place, one control by type (`int` number input with min/max and `zero_never`, `bool` checkbox, `enum` select, `hosts` text input), label `t('sys.' + key.label)`; not rendered when there is no such key (none today).
  - Until the schema has loaded the cards show `SkeletonCard`, as they do for settings today.

- [ ] **Step 1:** `python3 tools/check_i18n.py` and `python3 tools/pack_assets.py web -o build/ci/web-budget.wpak --budget-kb 170` fail before the i18n keys exist only if the page references them; add the keys first, run both, expect PASS.
- [ ] **Step 2: Implement** the page changes.
- [ ] **Step 3: Check by hand in the simulator** (frontend tests are out of scope): start `build/sim/wisp_sim`, open the admin page in a browser, pair, open System. Check: every card loads; brightness at 10 shows 0 %; the screen-off select lists 0, 15, 30, 60, 120, 300, 600 s; the time-zone select lists 38 offsets; agent limits refuse 7 and 201 steps; a change on the device's settings page shows after reload. Then make a test build in which one extra `kSettingWeb` row exists (a local, uncommitted edit of the schema list) and check it appears in "More settings" and saves; revert that edit.
- [ ] **Step 4: Run** `bash tools/ci.sh`. Expected: PASS (budget, i18n, layering, tests).
- [ ] **Step 5: Commit** `refactor(web): take setting ranges and choices from the device's schema`. Body records the hand checks of step 3.

---

### Task 14: Brightness follows the cell (X1, backlog 353)

**Files:**
- Modify: `components/wisp_core/include/wisp/core/system_settings.hpp:116-124, 159` and `components/wisp_core/src/system_settings.cpp:148-153, 223-226` (delete the `apply_brightness` parameter, member and call)
- Modify: `components/wisp_app/src/app_impl.hpp:85-97` (construct `SystemSettings{*platform.settings, bus}`), `components/wisp_app/src/app.cpp:378-386` (boot brightness from the cell), `:494-500` (delete the `set_brightness` call), `components/wisp_app/include/wisp/app/app.hpp:23-25, 367-369` (comments)
- Modify: `components/wisp_app/include/wisp/app/power_manager.hpp:7-8, 133-138, 193-201, 271-272, 336-337`, `components/wisp_app/src/power_manager.cpp:22, 55-72, 577-628`
- Modify: every `SystemSettings` construction in tests (`tests/core/system_settings_test.cpp`, `tests/admin/admin_fixture.hpp`, `tests/admin/routes_test.cpp:376`, `tests/agent/device_tools_test.cpp:40`, `tests/app/net_wiring_test.cpp`)
- Test: `tests/app/power_manager_test.cpp` (841-960), `tests/app/app_boot_test.cpp`, `tests/app/app_net_test.cpp:493-501`, `tests/core/system_settings_test.cpp:274-284, 411-421`

**Interfaces:**
- `SystemSettings(SettingsBackend& backend, EventBus& bus);`
- `PowerManagerDeps::system_settings` keeps its type (`const core::StateCell<core::SettingsSnapshot>*`). With it and a UI, PowerManager subscribes a second follower on the UI dispatcher (`ui_`) that calls `apply_brightness(v.brightness)`; without a UI it does not follow brightness (Decisions item 8). The constructor sets `user_brightness_` and `shown_brightness_` from `system_settings->get().brightness` (the panel already shows it), else the schema default. `kMinDimBrightness` becomes `core::setting_spec(core::SettingId::Brightness).min`.
- `void set_brightness(uint8_t level);` stays for the no-UI case and tests: on the UI thread, or with no UI, it applies; called off the UI thread with a UI it logs `WLOG_E` once and returns. The post path, `requested_brightness_`, `brightness_job_queued_` and the "UI queue full" warning go.
- App: `boot_brightness = impl_->system_settings.cell().get().brightness` (valid by construction; no clamp, no `core::Settings` read).

- [ ] **Step 1: Write the failing tests:**

```cpp
TEST(AppBoot, PanelStartsAtTheStoredBrightness)
// store display/brightness 90 before start(); with_display: first set_brightness on the
// FakeDisplay is 90 and display->brightness() == 90 after the first frame.
TEST(AppBoot, InvalidStoredBrightnessStartsAtTheDefault)
// store display/brightness 5: display->brightness() == 200 (X1).
TEST(PowerManager, BrightnessFollowsTheCellFlooded)
// Fixture with a settings cell; BusFlood + the UI held; 10 sets of brightness 100..109;
// release; drain the UI: display shows 109, set_brightness_calls() counts at most 2.
TEST(PowerManager, ReseededBrightnessReachesThePanel)
// cell.seed(brightness 70) (what SystemSettings does after a failed rollback): drain UI -> 70.
TEST(PowerManager, DimIsRelativeToTheCellsBrightness)
// cell brightness 200, then Dim: panel 60 (30 %); cell brightness 20 while dimmed: panel 10 (min).
TEST(AppNet, BrightnessSettingReachesTheDisplay)  // existing, unchanged: must still pass.
```
  Rewrite `SetBrightnessWhileActiveAppliesImmediately` and `BrightnessChangedInStandbyAppliesOnWake` to call `set_brightness()` inside `f.run_on_ui(...)`; delete `SetBrightnessCoalescesWhileUiIsBusy` and `UiThreadBrightnessIsNotOverwrittenByAnOlderQueuedJob`; in `system_settings_test.cpp` delete `AppliesBrightnessImmediately` and `BrightnessIsAppliedBeforeTheCellMoves` (no hook) and drop the `applied` vector from the fixture.
- [ ] **Step 2: Run** `-R 'PowerManager|AppBoot|AppNet.Brightness|SystemSettings'`. Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `-R "$GOLDEN|PowerManager|AppBoot|AppNet|SystemSettings|Routes|SelfCheck|FactoryReset"`. Expected: PASS.
- [ ] **Step 5: Commit** `fix(app): apply brightness from the settings cell, at boot too`.

---

### Task 15: Loopback scales its samples; the ports' gains are named (X2)

**Files:**
- Modify: `components/wisp_app/src/loopback.cpp` (25-30, 31-32 signature, 80-99, 151-157)
- Modify: `components/wisp_port_esp/dev/esp_audio.cpp:25, 135` and `components/wisp_port_esp/include/wisp/port/esp/esp_audio.hpp:18-31` (rename and rewrite the comment); `components/wisp_port_sim/dev/sim_devices.hpp:163`
- Modify (test support): `tests/support/fake_devices.hpp:492-567`
- Test: `tests/app/loopback_test.cpp` (1-3, 55-75, 116-150, 174-190)

**Interfaces:**
- `void record_and_play(App& app, const std::function<void(const LoopbackStatus&)>& on_status);` reads `app.system_settings().cell().read(...)` for `volume` once before playing, scales each recorded sample `s * volume / 100` (as `PcmPlayer`, `pcm_player.cpp:211-214`), and never calls `AudioOut::set_volume()`.
- `esp_audio.cpp`: `constexpr uint8_t kOutputGainPercent = 80;` (the codec's output gain; a board fact to tune, not the setting); `sim_devices.hpp`: `std::atomic<uint8_t> volume_{kOutputGainPercent};` with `static constexpr uint8_t kOutputGainPercent = 100;` in `SimAudioOut`.
- `FakeAudioIn(std::string id, std::string role = "main", int16_t fill = 0)`; `FakeAudioOut::peak() const -> int` (largest `|sample|` written since construction).

- [ ] **Step 1: Write the failing tests** (replacing `AppliesDefaultVolumeWhenSettingsHasNone`, `AppliesVolumeFromSettings`, `ClampsOutOfRangeVolumeFromSettings`):

```cpp
TEST(Loopback, LeavesTheOutputVolumeAlone)
// audio_in fill 1000; run: audio_out->set_volume_calls() == 0.
TEST(Loopback, PlaysAtTheDefaultVolume)       // no setting: peak() == 600
TEST(Loopback, PlaysAtTheStoredVolume)        // audio/volume 35: peak() == 350
TEST(Loopback, InvalidStoredVolumeIsTheDefault)  // audio/volume 250: peak() == 600
// UsesTheRoleMicAndSpeaker: its set_volume_calls() == 1 expectation becomes == 0.
```
- [ ] **Step 2: Run** `-R Loopback`. Expected: FAIL.
- [ ] **Step 3: Implement.** ESP files: run the firmware build per the Global Constraints.
- [ ] **Step 4: Run** `-R "Loopback|SimAudio|SelfCheck"` and `bash tools/ci.sh`. Expected: PASS.
- [ ] **Step 5: Commit** `fix(app): keep the loopback test from changing the output volume`.

---

### Task 16: The remaining consumers read the snapshot (X6, X7)

**Files:**
- Modify: `components/wisp_app/src/agent_wiring.cpp:24-40, 473-483`; `components/wisp_agent/include/wisp/agent/limits.hpp:14-15`
- Modify: `components/wisp_app/src/power_manager.cpp:24-30, 219-226` (`valid_or`, `apply_settings`), `components/wisp_app/include/wisp/app/power_manager.hpp:10, 14, 136-137, 309-315`
- Modify: `components/wisp_app/src/notification_router.cpp:16, 53-55`; `components/wisp_app/src/voice_wiring.cpp:27, 59-63`; `components/wisp_voice/src/voice_service.cpp:374, 633, 640`; `components/wisp_app/src/app.cpp:53, 105-109`
- Modify: `components/wisp_app/src/status_shell.cpp:27, 179`; `components/wisp_core/src/reminders.cpp:22, 158-160`; `components/wisp_agent/include/wisp/agent/context.hpp:52, 60`; `components/wisp_app/src/self_check_screen.hpp:193-196`
- Test: `tests/app/agent_wiring_test.cpp`, `tests/app/app_agent_test.cpp:401-460`, `tests/voice/voice_service_test.cpp`, `tests/app/notification_router_test.cpp`, `tests/core/reminders_test.cpp`, `tests/app/status_shell_test.cpp`

**Interfaces:**
- `agent::kMaxSteps = static_cast<uint32_t>(core::setting_default_int(core::SettingId::AgentMaxSteps))`, `kTurnMs = ... AgentTurnMin ... * 60000u`; the two `static_assert`s and `apply_turn_limits()`'s clamps and zero cases go: `service.set_turn_limits(v.agent_max_steps, v.agent_turn_min * 60000u)`.
- PowerManager: `apply_settings()` stores the delivered values (they are valid); `valid_or()` goes; the atomics start from `setting_default_int(...) * 1000`.
- Every fallback for a missing dependency uses the schema default: `NotificationRouter::volume()` → `setting_default_int(SettingId::NotifyVol)`; `VoiceService` → `setting_default_int(SettingId::Volume)` (was 100, X6); `StatusShell`, `ReminderScheduler` → `UtcOffsetMin`; `DeviceSnapshot::lang = std::string(setting_default_text(SettingId::Language))`, `utc_offset_min = setting_default_int(SettingId::UtcOffsetMin)`; `SelfCheck`'s `utc_offset_min_`/`time_24h_` from a default-constructed snapshot.
- The clamps of `voice_wiring.cpp:27` and `app.cpp:105-109` go (the snapshot is valid); the casts to `uint8_t` stay.

- [ ] **Step 1: Write the failing tests:**

```cpp
TEST_F(VoiceServiceTest, MissingVolumeUsesTheSettingDefault)
// VoiceDeps without .volume: the PcmPlayer begin() and the cue tone get 60, not 100.
TEST(NotificationRouter, MissingVolumeUsesTheSettingDefault)   // 60
TEST(AgentLimits, DefaultsAreTheSchemas)
// agent::kMaxSteps == 60; agent::kTurnMs == 900000.
```
  (`AppAgent.TurnLimitsFollowTheSettings*`, `PowerManager.OutOfRangeStoredSettingsReadAsDefaults` and the reminders and status-shell suites stay green unchanged.)
- [ ] **Step 2: Run** the filter of step 4. Expected: the two `MissingVolume` tests FAIL (the voice one gets 100).
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `-R "$GOLDEN|VoiceServiceTest|AppVoice|NotificationRouter|AgentLimits|AppAgent|AgentWiring|PowerManager|Reminder|StatusShell|SelfCheck|Context"`. Expected: PASS.
- [ ] **Step 5: Commit** `refactor: read setting defaults from the schema everywhere`.

---

### Task 17: The guard

**Files:**
- Create: `tools/check_settings.py`, `tools/check_settings_test.py`
- Modify: `tools/ci.sh` (after the `check_topics` block, lines 136-139: `run_tool_test check_settings_test`, then `python3 tools/check_settings.py`), `tools/ci_tool_tests.py:14-26` (`"check_settings_test": ()`)
- Modify: `components/wisp_core/include/wisp/core/system_settings.hpp` (delete `kDefault*`, `kMin*`, `kMax*`, `kDefaultNtpServers`, `kDefaultLanguage`, `valid_screen_off_s`, `valid_deep_after_s`, the `SystemSettingsValues` alias) and `system_settings.cpp:127-129`; rename every remaining `SystemSettingsValues` to `SettingsSnapshot` (`grep -rn SystemSettingsValues components simulator tests`)
- Modify: the lines the checker reports at this point (expected: the comment copies at `power_manager.hpp:10, 14`, `app.hpp:24`, `system_settings.hpp` header, `settings_page.hpp:5-6`; the three markers of Decisions item 5)

**Interfaces:**
- `tools/check_settings.py [--root DIR]`: prints `path:line: message` per finding and `settings-literal-ok markers: <n>`; exits 1 on any finding. Standard library only. Module functions for the test: `load_rows(root) -> list[Row]` (parses `X(` lines of `settings_schema.hpp`, 16 comma-separated fields), `check(root) -> list[str]`.
- Scan: `components/`, `simulator/`, `boards/`, `drivers/` (`.cpp .hpp .c .h`), `web/app/` (`.js`); skipped: `settings_schema.hpp`, `tests/`, `third_party/`. Comments are scanned.
- Rules (spec section 4 as refined by Decisions item 5):
  1. A row's `ns` as a string literal in a `core::Settings(`/`Settings(` call or backend `get(`/`set(`/`erase(` call, or a row's `key` as the first argument of `get_int(`/`get_bool(`/`get_string(`.
  2. A line holding a row token and an integer literal equal to that Int row's default, min or max (not `0`, `1`; not `100` on a `Percent` row).
  3. The HostList default text, or `pool.ntp.org`, anywhere; an Enum default code in `= "zh"`, `{"zh"}`, `?? 'zh'` form on a line holding the row's token.
  A line ending in `// settings-literal-ok: <reason>` (non-empty reason) is skipped; an empty reason is itself a finding.

- [ ] **Step 1: Write the failing tool tests** (`tools/check_settings_test.py`, scratch trees as `check_topics_test.py` builds them):

```python
def test_raw_store_read_fails(self)                 # get_int("brightness", 200) -> finding
def test_default_constant_fails(self)               # constexpr int kDefaultUtcOffsetMin = 480; -> finding
def test_number_without_a_token_passes(self)        # int x = 480; -> none
def test_single_word_suffix_strip_is_not_a_token(self)  # "turn" + 2 on one line -> none
def test_percent_full_scale_passes(self)            # volume_ = std::min<uint8_t>(volume, 100); -> none
def test_percent_default_fails(self)                # kDefaultVolume = 60; -> finding
def test_marker_with_reason_passes(self)
def test_marker_without_reason_fails(self)
def test_enum_default_assignment_fails_comparison_passes(self)  # lang = "zh" vs code == "zh"
def test_hostlist_default_anywhere_fails(self)
def test_js_fallback_fails(self)                    # settings.notify_vol ?? 60 in web/app
def test_tests_dir_and_schema_header_not_scanned(self)
def test_new_row_is_picked_up(self)                 # a fixture header with an extra X( row
```
- [ ] **Step 2: Run** `python3 tools/check_settings_test.py`. Expected: FAIL (no module).
- [ ] **Step 3: Implement** the checker; delete the constants and the alias; run `python3 tools/check_settings.py` and fix every finding (rewrite comments to name the schema instead of a number; add the three markers). Expected final output: no findings, `settings-literal-ok markers: 3`.
- [ ] **Step 4: Run** `bash tools/ci.sh`. Expected: PASS.
- [ ] **Step 5: Commit** `ci: keep setting defaults and ranges in the schema only`.

---

### Task 18: Documents

**Files:**
- Modify: `AGENTS.md:104` ("Settings keys" bullet) — separate commit `docs(agents): declare settings only in the schema`: add "Every system setting is declared once, in `WISP_SETTINGS` (`wisp/core/settings_schema.hpp`); no other file restates its default or range, which `tools/check_settings.py` enforces."
- Modify: `docs/architecture.md`: `:51` (boot brightness comes from the settings cell), `:60` (stop order: "HTTP handlers call `PowerManager::set_brightness()` through `SystemSettings`" becomes PowerManager's brightness follower), `:126` (`low_batt_pct` from the settings cell, range 5-50), `:131` (`system.settings`: the snapshot is `core::SettingsSnapshot`; payload unchanged), `:180` (cell type), the `wisp.tone` row `:71` if it names a cached volume; the REST route list gains `GET /settings/schema`.
- Modify: `docs/superpowers/specs/2026-11-03-connectivity-admin-design.md:303` (route table: add `GET /settings/schema`, "设置的描述：键、类型、范围、默认值、选项").
- Modify: `docs/backlog.md`: remove `:195` (settings page now follows the cell) and `:353` (brightness after a failed rollback); add a section "Settings schema: device checks not run" with the list below.
- Modify: `CHANGELOG.md`, under `## Unreleased`:
  - Features: "`GET /api/v1/settings/schema` describes every setting (type, range, default, choices); the web System page builds its fields from it, and a setting the page has no card for appears under "More settings"." / "The agent can also mute spoken replies, change when deep standby starts and set the time zone (`settings.set`), without asking."
  - Changes: "Screen-off and deep-standby choices are the same on the device and the web page (2 minutes added on the device)." / "Brightness shows 0 % at its lowest level on the device as on the web page." / "The agent's settings notes use the settings page's names." / "Internal: every setting is declared once (`wisp/core/settings_schema.hpp`); `tools/check_settings.py` keeps defaults and ranges out of the rest of the code."
  - Fixes: "The screen starts at the stored brightness even when the store holds a value out of range (it uses the default, as the settings page shows)." / "Running the loopback self-check no longer makes speech and sounds quieter until the next restart." / "The settings page shows changes made by voice or on the web page while it is open." / "A brightness left in storage by a failed settings save reaches the screen." / "A stored low-battery threshold outside 5-50 % is ignored."
  - Extend the closing "Device checks are pending ..." paragraph with "the settings schema" and its backlog section name.
- Modify: `README.md:173`, `README.zh-CN.md`, `README.ja.md` (the same table row): the agent can also "mute spoken replies, deep standby, time zone".

- [ ] **Step 1:** Edit the documents; `python3 tools/check_topics.py` and `python3 tools/check_settings.py` still pass.
- [ ] **Step 2:** Run `bash tools/ci.sh --full`. Expected: exit 0, no warnings in test output.
- [ ] **Step 3: Commit** `docs: record the settings schema` (and the separate `docs(agents): ...` commit first).

## Device checks (not run; for the owner)

Copied into `docs/backlog.md` by Task 18.

1. Upgrade from the current release with non-default settings stored (all twelve, plus `power/low_batt_pct` if set by hand): every value reads back the same on the device and in `GET /settings`.
2. Speech, cue tones and notification sounds at volume 30, 60 and 100 before and after running the loopback self-check: the same loudness before and after; the loudness at 60 acceptable with the output gain at 80 on each board, or a new gain recorded in `docs/hardware/<board>.md`.
3. Brightness at boot equals the stored value; at 10 the panel is still readable; dim (30 % of the setting) and wake behave as before; a burst of slider moves on the settings page ends at the last value.
4. The Shell settings page on the 2.16 and the round 1.75 boards: rows, sliders, the longer screen-off and deep-standby choice pages, and values following a change made on the web page while the page is open.
5. The web System page against the device: every card loads its ranges from `/settings/schema`; saving each card works.
6. `mem` at idle: internal free and largest block against `ram_budget.hpp` (the settings cell grows by about 60 bytes; while the settings page is open its object is about 200 bytes larger and it holds one small shared block).
7. A factory reset ends with the defaults and the performance overlay off.
8. The firmware builds for both boards (if a task recorded "firmware build not run").

## Deviations

Rulings made during execution; each is narrower than or equal to the task text it adjusts.

1. **The upgrade golden and the `language` type.** `SystemSettings.StoreWrittenByTheOldCodeReadsTheSame` compares `language` as `LangCode` (`LangCode::En`, and `LangCode::Zh` for the refused `"fr"`) once the field changed type; the expected stored and wire values did not change, and its `to_map()` check still pins the stored string.
2. **The table refuses a mismatched field type at compile time.** Each row asserts that its C++ field type agrees with its `SettingType` (a field-type trait), and the primary `setting_init` template is deleted, so a wrong type fails on the host build rather than at link time on the device. A further check makes sure the largest valid host list fits its `FixedString` (`max_entries * max_entry_len + (max_entries - 1)`, exactly 127 today).
3. **`SettingSurface` and `surface_flag()` live in `system_settings.hpp`**, next to `set()`, not in the schema header.
4. **The agent's settings note for a HostList row** has a description text, although no agent key is a HostList today.
5. **`Slot::lang` in the scene runtime became `core::LangCode`**, a reader of the language the plan's grep did not list.
6. **`GET /settings/schema`**: `AdminServer` holds the body and the handler reaches it through `RequestContext::settings_schema`; a missing body answers 500; the route's test pins the whole body text.
7. **Shell settings page layout.** "Never" moves to the end of a choice list only on a zero-means-never row. The row mapping, the ascending order of each row's choices and the rows with worded choices (24 h / 12 h, language names) are checked at compile time. A manual preview, `SettingsPreview.DISABLED_PageAndChoice`, renders the page and the screen-off choice page on the three shapes (`docs/testing.md`), and the shapes test also checks the seven-row screen-off choice page at both scroll ends.
8. **Shell settings page following the cell.** A per-key count of writes in flight keeps the page's value for a key while its own write is in flight; a write that did not store (refused, failed, storage queue full) puts the row back when it completes; the page's 1 s refresh picks up a completion whose UI post was dropped; a delivery redraws only the rows whose value changed; a held slider is left alone.
9. **Web System page.** The schema is fetched with every settings load, not once (after a firmware update the ranges may differ); a schema that lacks a key a card places is a failed load (error card and retry) instead of a render that throws.
10. **Brightness.** PowerManager's follower is `follow_brightness()`, which skips a level already applied (so boot makes one panel call); the subscription is released in `~PowerManager()`, as PowerManager has no `stop()`; `Routes.SettingsPutValidatesAndApplies` reads the cell instead of the removed hook.
11. **Loopback** plays back only the frames it recorded; before, a short recording also played the uninitialised tail of its buffer.
12. **Tests of the consumers.** PowerManager's out-of-range test fills its cell through `SystemSettings::get()` and is renamed `StoredOutOfRangeTimeoutsReachPowerManagerAsDefaults`; AgentWiring reads its first turn limits from the cell.
13. **The guard** (`tools/check_settings.py`): a comment line also counts as next to the code line it documents; every server of the NTP default is matched, not only the whole default text; JavaScript `|| 'zh'` is caught as well as `?? 'zh'`; the raw-store rule also catches a `set(` whose first argument is a row's key, but only with a second argument; `firmware/main/` is scanned too. Three markers remain, the ones "Decisions" item 5 expected. A row of the list the guard cannot parse fails the check instead of being skipped.
14. **Test hygiene.** Tests that register a log sink or set the process-wide UI language release them through a scope guard, so a failed assertion cannot leave a destroyed sink registered or later tests in another language.
15. **Neutral initializers.** The scene runtime's local settings struct beside its settings follower starts from neutral values (`false`, `{}`), not restated setting defaults; it is always fully assigned before use.
16. **The Shell page's fallback snapshot is gone.** The hard-coded values the page filled in without a settings source were deleted: its snapshot member already holds the table's defaults.
17. **Two cases pinned by tests.** A refused batch leaves the settings cell where it was (`SystemSettings.ValidatesWholeBatchAtomically`), and with a UI a `PowerManager::set_brightness()` call from another thread is ignored (`PowerManager.OffThreadSetBrightnessIsIgnored`).
18. **Two store readers moved to the cell.** The boot language (`App::start()`, right after the cell is seeded) and the time servers of the time sync read the settings cell, not the store; pinned by `AppBoot.BootLanguageIsTheCells` and `NetWiring.TimeSyncAsksTheServersOfTheSettingsCell`.
19. **Follow-ups.** The Shell page follows the cell field by field into its own snapshot, with no snapshot copy on the UI stack beyond the cell's delivery copy; a direct `PowerManager::set_brightness()` (no production caller) also moves the follower's last applied level; `valid_host_list()` went, `valid_setting()` calls `host_list_syntax_ok()` directly.
