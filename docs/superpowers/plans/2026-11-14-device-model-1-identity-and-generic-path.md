# Device Model H1: Identity and the Generic Access Path Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Portable code finds devices by interface and role instead of fixed ids, typed lookups are sound without RTTI, and a peripheral no core service knows (the simulator's `SimHaptic`) is usable by the agent (`device.list`, `device.call`) and by scenes (`wisp.device.list/get/call`) with a driver and a board line only.

**Architecture:** `hal::Device` gains `interface()` (a `final` virtual in each typed interface), `describe()`/`get()`/`invoke()` and a declarative `hal::DeviceSchema`. `DeviceRegistry` checks the interface on every typed lookup, adds `by_role<T>()`, and rejects mislabelled devices, duplicate roles and bad schemas at `add()`. Every literal id in portable code becomes a role lookup, an injected id or a flag. `core::DeviceService` is the one service the agent tools and the Lua binding adapt: schema validation, typed-device boundary, occupancy, standby, per-command `min_interval_ms`, the 50 ms budget and the log line.

**Tech Stack:** C++20 (`-fno-exceptions -fno-rtti`), GoogleTest, Lua 5.5 bindings, Python (`tools/check_layering.py`, `tests/smoke/sim_smoke.py`), the simulator build (`build/sim`).

**Spec:** `docs/superpowers/specs/2026-11-14-device-model-design.md` (approved 2026-11-14, section 20), sections 2, 3, 4, 5 and the H1 part of section 13. Sections 6-11 belong to H2 and H3 and are not touched here.

## Global Constraints

- Branch: `feat/device-model`. No commits on `main`.
- AGENTS.md applies: `wisp::Result<T>`, no exceptions/RTTI/`dynamic_cast`, comments explain why (no review ids, no change history), user-facing strings in zh/ja/en through `ui::StrId`, layering (`tools/check_layering.py`).
- Every task ends with `cmake --build build/sim && ctest --test-dir build/sim --output-on-failure -R '<the task's filter>'` green; Tasks 8 and 15 also run `bash tools/ci.sh`.
- Commit messages: Conventional Commits, ending with the `Co-Authored-By` line of the session's attribution rules. AGENTS.md edits are separate `docs(agents): ...` commits inside the task that makes the rule true (Tasks 8 and 14).
- `components/wisp_port_esp/` and `boards/*/board.cpp` are not compiled in this environment. Their edits (Task 3 only) are minimal and mechanical, listed line by line, and re-read twice; the owner builds both boards (device checks at the end).
- No internal-RAM change: no new task, no new stack, no new hot path. `DeviceService`'s tables are fixed-size and live in `Mem::Large` containers (`os::LargeVector`); results larger than a few KB are serialized into `os::LargeString`.
- Threads: `DeviceService::get/call` and every driver `get()`/`invoke()` run on the caller's task: `wisp.agent` (a `ToolThread::Agent` tool) or `wisp.scene` (a Lua binding). Never on the UI thread, never in an ISR. Drivers must not touch flash and get no `Settings` access; they are internally thread-safe (foundation spec 5.3); the service does not serialise calls.
- A missing device or one without the role never blocks boot: every role lookup that finds nothing behaves exactly as today's `get<T>("<id>")` returning null.
- `WISP_MANIFEST` stays one line of JSON; its changes are additive only (Task 3: caps `role`; Task 9: caps `schema`, class `"other"`; Task 11: device `haptic.main`). `WISP_BOOT_REPORT` does not change in H1.
- The agent's tool list changes exactly once (Task 12: `device.list`, `device.call` right after `notify.dismiss`). The fixed prompt does not change.

## Review Focus

1. A device that is not of interface `T` is never returned as a `T`: an `Other` device whose id is `display.x` is not a `Display`, and a device whose class does not match its interface never registers. Tests in Task 1.
2. A board whose power key is `button.side` (role `power`): standby polling, PowerManager's power-key ownership and SystemDuties' short/long handling all follow it; a key without the role is not polled in standby. Tests in Tasks 5 and 7.
3. An out-of-range argument, an unknown argument, a call closer than `min_interval_ms`, an exclusive holder, a typed device's command: each refused before the driver is called (the driver's invocation count stays 0). Tests in Task 10.
4. A scene without `permissions.device` for the id gets `nil, "permission_denied"` and the driver is not invoked; the agent's `lua.run` is granted without a declaration. Tests in Task 13.
5. A call while the device is off for standby answers `busy` and does not wake it (no `set_power` from the service). Tests in Tasks 10 and 11.

## Deviations from the spec

Recorded per AGENTS.md section 7 ("Deviations"); all small, none changes the design's intent.

1. **ESP port ids stay where they are in H1.** Spec 2.4/13 has every `Esp*` constructor take id and role from its config. H2 rewrites exactly these constructors (adapters move to `drivers/*`, the board kit's `Slot` carries id and role), and the port is not compiled here. H1 therefore only adds caps `role` inside the ESP devices (today's ids unchanged, the port is excluded from the grep guard) and a `role` parameter on `EspPmuPowerKey`; the boards give their power keys role `power`. Moving ids into configs is H2's.
2. **The power key is found per button, not injected into `ButtonService`.** `ButtonService` is a member of `App::Impl` constructed before the board registers buttons, so a constructor argument cannot carry the id. `refresh_buttons()` marks the state whose device has role `power` (unique per interface by Task 2). `PowerManager` gets the id through `PowerManagerDeps::power_key`; `SystemDuties` resolves it at construction.
3. **Topics derived from the power key's id.** `system_duties.cpp:205,207` compares topics `"button.power.short"`/`"button.power.long"` (missing from spec 1.1.2). They become `<power key id> + ".short"`/`".long"`.
4. **`MediaStatus` gains `removable`.** Spec 2.4 item 3 says `StorageManager` already reports the medium's `removable` flag; it does not (`storage_manager.hpp:162-167`). Task 4 adds `bool removable` (true for the medium found by role `removable`). The topics `storage.sd.inserted`/`storage.sd.removed` stay fixed system topic names (`docs/architecture.md` "Event topics"); their `source` becomes the card's id.
5. **Role uniqueness for generic devices is per kind.** Spec 2.3 scopes generic roles to interface `None`, which would forbid `haptic.main` and `led.main` together. For interface `None` the uniqueness key is (kind, role), the kind being the id's prefix before the dot. A generic device's id must match `^[a-z][a-z0-9_]*\.[a-z0-9_]+$` and be at most 32 bytes (the `device.call` id limit).
6. **The schema's default is a `constexpr` tagged scalar, not `wisp::Value`.** Spec 3.2 declares `Value def`; `Value` is not a literal type, so every table would be a dynamically initialised static in internal RAM with a static constructor. `ParamSpec::def` is a `hal::ParamDefault` instead (none, bool, integer, number or string literal; 16 bytes), all schema types are literal types, and drivers declare their tables `constexpr`, so they live in flash (`ParamSpec` 64 bytes instead of 88 on the 32-bit targets). `check_schema()` requires the default's kind to match the argument's type (Int: Int; Number: Int or Number; Bool: Bool; String and Enum: String). What consumers see is unchanged: `validate_args()` fills a missing optional argument with the `Value` of the default's own kind (an Int default stays Int, also for a Number argument, which a driver reads with `as_double()`), and `render_schema()` renders the default as before.
7. **More rejected at `add()`.** Besides spec 2.1/3.2's checks, `add()` rejects a typed device whose schema declares commands, a `Display` with any schema (spec 3.3 says a Display never declares one), and any schema failing `hal::check_schema()` (names, counts, ranges). `DeviceService` still refuses commands on typed devices (`PermissionDenied`) as a second line.
8. **Order inside `DeviceService::call()`.** Occupancy and the standby check come before `min_interval_ms`, and the interval is checked and stamped in one critical section right before `invoke()`, so a call refused for another reason does not start the interval, and two racing calls cannot both pass. A call that reaches `invoke()` stamps the interval whatever `invoke()` returns (a failing actuator is not retried at full speed).
9. **Who decides "off for standby".** `DeviceService` keeps the power level from `power.state` events (`set_power_level()`, fed by `App` like `SensorService::set_standby`) and refuses calls and reads on `standby: "off"` devices while not `Active`/`Dim`. Drivers also refuse when off (they are authoritative across the race with `PowerManager`).
10. **Agent error codes follow the agent's existing convention.** Spec 4.2 says snake_case `ErrorCode` names; every other agent tool goes through `detail::tool_code()` (`InvalidArgument` → `invalid_value`, `PermissionDenied` → `denied`). `device.call` uses `detail::fail()` too, and `tool_code()` gains `RateLimited` → `rate_limited` and `DeviceFailure` → `device_failure`. The Lua binding returns snake_case `ErrorCode` names, as spec 4.3 says and `wisp.sensor` does.
11. **The 1 KiB result cap lives in `DeviceService`** (both adapters get it) and is reported as `DeviceFailure` "result too large (N bytes)", not as a separate `result_too_large` code: an oversized result is a driver bug.
12. **Lua details.** `wisp.device.get` needs the same `permissions.device` entry as `call` (sensor readings are data of the device); `list()` needs none. A command whose result is null returns `true`, so success is never `nil`. Background and headless scenes may call declared devices (no view is needed; a device that is off for standby answers `busy` anyway).
13. **The id-literal guard is in `tools/check_layering.py`**, which already strips comments and has a self-test, run by `tools/ci.sh`; it also covers `simulator/*.cpp` (migrated in Task 7).
14. **Admin `GET /api/v1/devices` needs one change.** Spec 4.4 says it already returns the schema; it returns `info.caps` (`routes_device.cpp:36`), so it switches to `hal::caps_of()` like the manifest. The web page shows class `other` with a translated name; caps (role, schema) already render generically.
15. **Test accessor.** `AgentWiring::tools()` and `App::agent_tools()` (const pointers to the agent's `ToolRegistry`, null without an agent) are added so the tool order and the end-to-end path are tested through the real wiring.
16. **Schema names are not Lua keywords.** Spec 3.2 allows any `^[a-z][a-z0-9_]{0,31}$` name; a scene passes arguments as a table constructor (`wisp.device.call(id, "pulse", {ms = 150})`), where a field named `end` or `repeat` does not parse. `check_schema()` therefore also refuses the 22 Lua 5.4 reserved words as property, command and argument names.
17. **A full rate table refuses instead of evicting a running interval.** The table of `min_interval_ms` stamps has `kMaxRateEntries` (32) fixed slots. When a rate-limited command needs a slot, the table is full and no entry has expired, `DeviceService::call()` returns `RateLimited` `"'<id>.<cmd>' cannot be rate-tracked now; retry_after_ms=<n>"` (`<n>`: the soonest a running interval ends) without calling the driver. Evicting the oldest stamp would let a long interval (a 60 s command) be bypassed at once by a burst of short ones. Entries without a running interval (budget warnings only) stay replaceable; commands without an interval are unaffected. While 32 rate-limited pairs keep being re-stamped by their callers, a 33rd pair may be refused repeatedly; `retry_after_ms` says when the earliest slot frees, not that the retry will succeed.
18. **The scene-API text names `list()`'s cap.** `wisp.device.list()` returns at most 32 devices, as the agent's `device.list` does; the brief's fixed text omitted it, so the `wisp.device.list()` bullet says "(at most 32)". Unlike the agent's tool, nothing in the result signals that the list was cut.
19. **A schema string parameter is at most 256 bytes.** The spec sets no limit on `max_len`; `check_schema()` refuses a String parameter whose `max_len` exceeds `hal::kMaxStringParamBytes` (256), so every argument set a schema allows fits the Lua binding's 16 KiB cap on a call's argument JSON (`bind_device.cpp`, a `static_assert` over `kMaxCmdArgs` and `kMaxStringParamBytes`).
20. **`interface()` is a stored tag, not a `final` virtual.** Spec 2.1 makes `interface()` virtual and `final` in each interface class; a class deriving from `Device` directly could still override it and claim an interface it does not implement. `hal::Device` stores the tag instead, set through a constructor only the interface classes reach (each passes its own `kInterface`), and `interface()` is not virtual (Task 1).
21. **A test clock for the App's `DeviceService`.** The end-to-end test (Task 14) moves the clock past `min_interval_ms` between the agent's pulse and the Lua pulse; `App` built the service on `os::now_ms()` only. `Platform::device_clock_ms` (empty: `os::now_ms()`) is that clock, and `ToolRegistry::call()` is `const` so tests call the wired tools through `App::agent_tools()`.
22. **A button's id must be `button.<name>`.** The spec leaves a button's id to the board, but `ButtonService` publishes `<id>.<edge>` and `PowerManager`, `SystemDuties` and the self-check screen subscribe to `button.`: a power key named `key.pwr` would be polled and heard by no one. `DeviceRegistry::add()` refuses a Button whose id is not `button.<name>` (`<name>` non-empty, the generic id's character rule and 32-byte limit), and `ButtonService`'s warning for such an id is gone, being unreachable.

---

### Task 1: Interface identity and interface-checked lookups

**Files:**
- Modify: `components/wisp_hal/include/wisp/hal/device.hpp`, `components/wisp_hal/src/device.cpp`
- Modify: `components/wisp_hal/include/wisp/hal/{display,touch,imu,power,clock,button,audio,storage,wifi}.hpp` (one `kInterface` and one `final` override each; `audio.hpp` has two classes)
- Modify: `components/wisp_core/include/wisp/core/device_registry.hpp`, `components/wisp_core/src/device_registry.cpp`
- Modify: `components/wisp_port_sim/sim_board.cpp` (`matches_disable` already works by class name; nothing else)
- Modify: `tests/support/fake_devices.hpp` (new `FakeGeneric`)
- Test: `tests/hal/device_identity_test.cpp` (new; `wisp_test(NAME device_identity_test ... LIBS wisp_hal)`), `tests/core/device_registry_test.cpp`

**Interfaces:**
- Produces in `wisp/hal/device.hpp`:
  - `enum class Interface : uint8_t { None, Display, Touch, Imu, Power, Clock, Button, AudioOut, AudioIn, Storage, Wifi };`
  - `const char* to_string(Interface i);` → `"none"`, `"display"`, `"touch"`, `"imu"`, `"power"`, `"clock"`, `"button"`, `"audio_out"`, `"audio_in"`, `"storage"`, `"wifi"`.
  - `DeviceClass` gains `Other` (last value); `to_string(DeviceClass::Other) == "other"`, `class_from_string("other")`.
  - `DeviceClass class_of(Interface i);` — the class a device of interface `i` must report: same name, except `Wifi` → `Network` and `None` → `Other`.
  - `class Device { ... virtual Interface interface() const { return Interface::None; } ... };`
  - In each typed interface: `static constexpr Interface kInterface = Interface::<X>;` and `Interface interface() const final { return kInterface; }`. `kClass` stays (boards and tests write `info_.cls = kClass`).
  - The file header's "No RTTI" paragraph now says lookups compare `interface()`, which a concrete class cannot change because it is `final`.
- Produces in `DeviceRegistry`:
  - `get<T>(id)` and `list<T>()` compare `device->interface() == T::kInterface` (not `info().cls`).
  - `add()` returns `InvalidArgument` (registry unchanged) for: a null device; `info().cls != hal::class_of(device->interface())` (message `"<id>: class <cls> does not match interface <iface>"`); interface `None` with an id not matching `^[a-z][a-z0-9_]*\.[a-z0-9_]+$` or longer than 32 bytes (message `"<id>: a generic device id is <kind>.<name>, at most 32 bytes"`). Duplicate id stays `AlreadyExists`.
- Produces in `tests/support/fake_devices.hpp`: `class FakeGeneric : public wisp::hal::Device` with `explicit FakeGeneric(std::string id, wisp::hal::DeviceClass cls = wisp::hal::DeviceClass::Other, wisp::Properties caps = {})` (Task 9 extends it).

- [ ] **Step 1: Write the failing tests.**

`tests/hal/device_identity_test.cpp`:

```cpp
TEST(DeviceIdentity, InterfaceComesFromTheType)
// FakeDisplay("display.main", 480, 480).interface() == Interface::Display;
// FakeWifi("net.wifi") seen through a const hal::Device& reports Interface::Wifi;
// FakeGeneric("haptic.main").interface() == Interface::None.
TEST(DeviceIdentity, ClassOfEveryInterface)
// class_of() for all 11 values; class_of(Wifi) == Network, class_of(None) == Other.
TEST(DeviceIdentity, NamesRoundTrip)
// to_string(Interface::AudioIn) == "audio_in"; to_string(DeviceClass::Other) == "other";
// class_from_string("other") == DeviceClass::Other.
```

`tests/core/device_registry_test.cpp`:

```cpp
TEST(DeviceRegistry, TypedLookupChecksTheInterfaceNotTheId)
// add FakeGeneric("display.x") -> get<hal::Display>("display.x") == nullptr,
// list<hal::Display>() empty, find("display.x") non-null.
TEST(DeviceRegistry, RejectsAClassThatDoesNotMatchTheInterface)
// a FakeImu subclass that writes info_.cls = DeviceClass::Display in its constructor:
// add() -> InvalidArgument; find() == nullptr.
TEST(DeviceRegistry, RejectsAGenericDeviceWithATypedClass)
// FakeGeneric("haptic.main", DeviceClass::Button) -> InvalidArgument.
TEST(DeviceRegistry, GenericDeviceIdsNeedAKind)
// "haptic" and "Haptic.main" and a 33-byte "h.<31 x>" -> InvalidArgument; "led.status" -> ok.
```

Extend `DeviceClass.StringRoundTrip` with `Other`.

- [ ] **Step 2: Run** `ctest --test-dir build/sim -R 'DeviceIdentity|DeviceRegistry|DeviceClass' --output-on-failure`. Expected: compile failure (no `Interface`), then FAIL.
- [ ] **Step 3: Implement.** No existing device changes: every one already writes `info_.cls = kClass`, which matches `class_of(kInterface)`. Update the `device_registry.hpp` comments on `get`/`list`.
- [ ] **Step 4: Run** `ctest --test-dir build/sim --output-on-failure` (the whole suite: every device in every test goes through `add()`). Expected: PASS.
- [ ] **Step 5: Commit** `feat(hal): tie device lookups to a final interface tag`.

---

### Task 2: Roles, `by_role<T>()` and role uniqueness

**Files:**
- Create: `components/wisp_hal/include/wisp/hal/roles.hpp`, `components/wisp_hal/src/roles.cpp` (add to `wisp_hal`'s sources)
- Modify: `components/wisp_core/include/wisp/core/device_registry.hpp`, `components/wisp_core/src/device_registry.cpp`
- Modify: `components/wisp_hal/include/wisp/hal/button.hpp` (caps comment: `role` may be `power`, `talk` or a list)
- Modify: `tests/support/fake_devices.hpp`, `tests/support/fake_audio.hpp`, `tests/support/fake_wifi.hpp` (default roles)
- Test: `tests/hal/device_identity_test.cpp`, `tests/core/device_registry_test.cpp`

**Interfaces:**
- Produces `wisp/hal/roles.hpp`:

```cpp
namespace wisp::hal::role {
inline constexpr const char* kMain = "main";
inline constexpr const char* kInternal = "internal";    // Storage
inline constexpr const char* kRemovable = "removable";  // Storage
inline constexpr const char* kPower = "power";          // Button
inline constexpr const char* kTalk = "talk";            // Button
inline constexpr const char* kNavPrev = "nav_prev";     // Button, used from H3
inline constexpr const char* kNavNext = "nav_next";
inline constexpr const char* kNavOk = "nav_ok";
inline constexpr const char* kNavBack = "nav_back";
}  // namespace wisp::hal::role
namespace wisp::hal {
// caps "role": a string, or a list of strings; anything else is no role.
bool has_role(const DeviceInfo& info, std::string_view role);
std::vector<std::string> roles_of(const DeviceInfo& info);
}
```

  The header comment holds spec 2.3's vocabulary table (interface, role, used by) and says a board may use other names for devices only it or the generic path cares about.
- Produces in `DeviceRegistry`:
  - `template <class T> std::shared_ptr<T> by_role(std::string_view role) const;` — the device whose `interface() == T::kInterface` and `has_role(info, role)`, or null.
  - `add()` returns `AlreadyExists` with message `"role <role> already taken by <other id>"` when one of the new device's roles is already held by a registered device of the same interface (for interface `None`: of the same kind, deviation 5). `remove()` frees the role.
- Fakes: `FakeDisplay`, `FakeTouch`, `FakeImu`, `FakePower`, `FakeClock`, `FakeAudioIn`, `FakeAudioOut` (both headers) and `FakeWifi` gain a trailing `std::string role = "main"` and write caps `role` when non-empty (the two `fake_audio.hpp` classes, which have fixed ids, always write `"main"`). `FakeStorage` writes `"removable"` or `"internal"` from its `removable` flag. `FakeButton` is unchanged (roles come through its `caps` argument).

- [ ] **Step 1: Write the failing tests.**

```cpp
TEST(DeviceRoles, HasRoleAcceptsAStringOrAList)
// caps role "talk" -> has_role(talk) true, has_role(power) false;
// caps role ["power","talk"] -> both true; roles_of() == {"power","talk"}.
TEST(DeviceRoles, OtherTypesAreNoRole)
// caps role 3 -> false; ["power", 3] -> has_role(power) true, roles_of() == {"power"}.

TEST(DeviceRegistry, ByRoleReturnsOnlyThatInterface)
// FakeImu("imu.accel") (role main), FakeGeneric("haptic.main", Other, {{"role","main"}}):
// by_role<hal::Imu>("main") == the imu; by_role<hal::Display>("main") == nullptr;
// by_role<hal::Imu>("talk") == nullptr.
TEST(DeviceRegistry, SecondDeviceWithTheSameRoleIsRejected)
// FakeImu("imu.a"), FakeImu("imu.b") -> second add(): AlreadyExists,
// message "role main already taken by imu.a"; FakeImu("imu.c", "") -> ok.
TEST(DeviceRegistry, SameRoleOnDifferentInterfacesIsFine)
// FakeDisplay("display.main"...), FakeTouch("touch.main") both "main" -> both ok.
TEST(DeviceRegistry, RoleListsCountEveryRole)
// FakeButton("button.a", {{"role", List{"power","talk"}}}); FakeButton("button.b", {{"role","talk"}})
// -> AlreadyExists; after remove("button.a") the second add succeeds.
TEST(DeviceRegistry, GenericRolesAreUniquePerKind)
// haptic.main (main) + led.status (main) -> ok; haptic.side (main) -> AlreadyExists.
```

Update `DeviceRegistry.ManifestJson`'s expected JSON: the imu caps now include `"role":"main"`.

- [ ] **Step 2: Run** `ctest --test-dir build/sim -R 'DeviceRoles|DeviceRegistry' --output-on-failure`. Expected: FAIL.
- [ ] **Step 3: Implement.** Then run the whole suite: a test that registers two typed devices of one interface now gets `AlreadyExists` for the second; pass `""` as the second one's role there (search for it by the failures, there is no other way to list them). Tests that compare a fake's whole caps map add `"role"`.
- [ ] **Step 4: Run** `ctest --test-dir build/sim --output-on-failure`. Expected: PASS.
- [ ] **Step 5: Commit** `feat(core): find devices by role, one device per interface and role`.

---

### Task 3: The simulator, the ESP port and the boards declare roles

**Files:**
- Modify: `components/wisp_port_sim/dev/sim_devices.hpp`, `components/wisp_port_sim/dev/sim_{display,touch,imu,power,clock,audio,storage}.cpp`, `components/wisp_port_sim/include/wisp/port/sim/sim_wifi.hpp` and its source, `components/wisp_port_sim/sim_board.cpp`
- Modify (not compiled here): `components/wisp_port_esp/dev/esp_{display,touch,power,clock,imu,audio,storage}.cpp`, `components/wisp_port_esp/net/esp_wifi.cpp` (where `info_` is filled from `EspWifiConfig::id`), `components/wisp_port_esp/include/wisp/port/esp/esp_button.hpp`, `components/wisp_port_esp/dev/esp_button.cpp`
- Modify (not compiled here): `boards/waveshare_esp32_s3_touch_amoled_2_16/board.cpp:362`, `boards/waveshare_esp32_s3_touch_amoled_1_75c/board.cpp:333,341`
- Test: `tests/sim/sim_board_test.cpp`, `tests/smoke/sim_smoke.py`

**Interfaces:**
- Every `Sim*` device constructor gains a trailing `std::string role = {}` (as `SimButton` has) that, when non-empty, sets caps `role`. `SimBoard::init()` passes: `main` for display, touch, imu, power, clock, `net.wifi`, both audio devices; `internal`/`removable` for `storage.int`/`storage.sd`; `power` for `button.power`; `talk` for `button.vol_down` (as now); none for `button.vol_up`.
- `SimBoard::init()` records `DeviceState::Failed` with the `add()` error message when `add()` fails (today the result is dropped).
- ESP (exact edits): each of `EspDisplay`, `EspTouch`, `EspPower`, `EspClock`, `EspImu`, `EspAudioOut`, `EspAudioIn`, `EspWifi` adds `info_.caps["role"] = wisp::Value(wisp::hal::role::kMain);` next to where it fills `info_.caps`; `EspInternalStorage` adds `kInternal`, `EspSdStorage` adds `kRemovable`. `EspPmuPowerKey(std::shared_ptr<EspPower> power, std::string id, std::string model, std::string role)` sets caps `role` when non-empty. Boards: the 2.16's `kButtons` power entry role `""` → `"power"`; the 1.75C's `EspGpioButton(... "button.power", "gpio-button", "" ...)` → `"power"` and `EspPmuPowerKey(std::move(power), "button.power", "axp2101-pwrkey")` → add `"power"`. Ids do not change (deviation 1).

- [ ] **Step 1: Write the failing tests.**

```cpp
TEST_F(SimBoard, EveryTypedDeviceIsFoundByRole)
// by_role<Display>(main)->info().id == "display.main"; likewise Touch, Imu, Power, Clock,
// Wifi -> "net.wifi"; Storage internal -> "storage.int", removable -> "storage.sd";
// Button power -> "button.power", talk -> "button.vol_down";
// AudioOut/AudioIn main: equal to registry_->find("audio_out.main"/"audio_in.main")
// (null on a host without SDL audio).
```

`tests/smoke/sim_smoke.py` `case_default`: for every manifest device except `button.vol_up`, `caps.role` equals the role above (`expect(...)` per device with the id in the message).

- [ ] **Step 2: Run** `ctest --test-dir build/sim -R 'SimBoard|sim_smoke' --output-on-failure`. Expected: FAIL.
- [ ] **Step 3: Implement.** Re-read each ESP and board edit against the list above; nothing else in those files changes.
- [ ] **Step 4: Run** the same filter. Expected: PASS.
- [ ] **Step 5: Commit** `feat(port): devices declare their roles`.

---

### Task 4: `StorageManager` and `SensorService` without literals

**Files:**
- Modify: `components/wisp_core/src/storage_manager.cpp` (lines 23-24, 492-505, 1068, 1090), `components/wisp_core/include/wisp/core/storage_manager.hpp` (file header, `MediaStatus`)
- Modify: `components/wisp_core/include/wisp/core/sensor_service.hpp` (`kDevice` removed), `components/wisp_core/src/sensor_service.cpp` (lines 155, 175), the occupancy header comment (`occupancy.hpp`: "takes the IMU's id")
- Test: `tests/core/storage_manager_test.cpp`, `tests/core/sensor_service_test.cpp`

**Interfaces:**
- Produces: `struct MediaStatus { std::string id; bool online; uint64_t total_bytes, free_bytes; bool removable = false; };` (`removable` true for the medium found by role `removable`).
- `StorageManager::init()` uses `registry_.by_role<hal::Storage>(hal::role::kInternal)` and `(kRemovable)`. No internal medium: `StorageUnavailable` with message `"no storage device with role internal"`. The card's insert/remove events keep topics `storage.sd.inserted`/`storage.sd.removed` with `source = sd_dev_->info().id`.
- `SensorService` keys occupancy by `imu_->info().id` (stored once in the constructor as `std::string device_id_`; empty when `imu` is null, and then nothing is acquired, as now nothing can subscribe).

- [ ] **Step 1: Write the failing tests.**

```cpp
TEST(StorageManager, FindsMediaByRoleWhateverTheirIds)
// FakeStorage("storage.flash", in, false), FakeStorage("storage.card", sd, true):
// init() ok; status().media ids {"storage.flash","storage.card"},
// removable {false,true}; roots(Area::Library).front() is under sd's mount.
TEST(StorageManager, CardEventsCarryTheCardsId)
// the card goes offline, refresh() -> one Event topic "storage.sd.removed", source "storage.card".
TEST(StorageManager, NoInternalRoleIsUnavailable)
// FakeStorage("storage.int", in, false) whose caps role was replaced by "" (set via a
// subclass) -> init(): StorageUnavailable, message "no storage device with role internal".
TEST(SensorService, OccupancyUsesTheImusOwnId)
// FakeImu("imu.accel"); subscribe(Accel, 25, "scene.a", cb) ->
// occupancy.list()[0].device == "imu.accel"; unsubscribe -> list() empty.
```

- [ ] **Step 2: Run** `ctest --test-dir build/sim -R 'StorageManager|SensorService' --output-on-failure`. Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** the same filter plus `-R 'SceneRepository|Routes|AppBoot'` (users of `status()`). Expected: PASS.
- [ ] **Step 5: Commit** `refactor(core): storage and sensors find their devices by role`.

---

### Task 5: `ButtonService` polls the key with role `power`

**Files:**
- Modify: `components/wisp_core/src/button_service.cpp` (`refresh_buttons`, `poll_interval_ms` line 305), `components/wisp_core/include/wisp/core/button_service.hpp` (`ButtonState::power_key`)
- Modify (fixtures): every test construction of a button with id `"button.power"` gets caps `role: "power"`: `tests/core/button_service_test.cpp` (`FakeButton`, and the file's own `EdgeButton`/`PollButton`, which gain a role parameter), `tests/app/ui_net_harness.hpp:101`, `tests/app/app_boot_test.cpp` (`FakeBoard`), `tests/app/power_manager_test.cpp`, `tests/app/system_duties_test.cpp`, `tests/app/self_check_screen_test.cpp`, and any other hit of `grep -rn '"button.power"' tests`
- Test: `tests/core/button_service_test.cpp`

**Interfaces:**
- Produces: `ButtonState::power_key` (bool), set in `refresh_buttons()` from `hal::has_role(button->info(), hal::role::kPower)`. `poll_interval_ms()` in standby returns 0 for every state without it.

- [ ] **Step 1: Write the failing tests.**

```cpp
TEST(ButtonService, StandbyPollsTheKeyWithThePowerRole)
// copy of the existing PollButton standby test (line ~236) with id "button.side",
// role "power": same assertions (polled at its standby_poll_ms, events "button.side.short").
TEST(ButtonService, WithoutThePowerRoleNoKeyIsPolledInStandby)
// PollButton("button.power", 100) with no role; set_standby(true); poll(now + 1000):
// the button's pressed() call count does not grow.
```

- [ ] **Step 2: Run** `ctest --test-dir build/sim -R 'ButtonService|EdgeEvent|MixedInputs|PollOnly' --output-on-failure`. Expected: the new tests FAIL.
- [ ] **Step 3: Implement**, then update the fixtures listed above (without them the power-key standby tests elsewhere would stop polling).
- [ ] **Step 4: Run** `ctest --test-dir build/sim -R 'ButtonService|EdgeEvent|MixedInputs|PollOnly|PowerManager|SystemDutiesTest|AppBoot' --output-on-failure`. Expected: PASS.
- [ ] **Step 5: Commit** `refactor(core): poll the power-role key in standby`.

---

### Task 6: `App`, net, voice and loopback wiring look devices up by role

**Files:**
- Modify: `components/wisp_app/src/app.cpp` (lines 76, 351, 360, 398, 406, 451), `components/wisp_app/src/net_wiring.cpp` (31, 40, 176), `components/wisp_app/src/voice_wiring.cpp` (56, 59), `components/wisp_app/src/loopback.cpp` (36-37)
- Test: `tests/app/app_boot_test.cpp`, `tests/app/net_wiring_test.cpp`, `tests/app/loopback_test.cpp`, `tests/app/voice_wiring_test.cpp`

**Interfaces:**
- Each literal becomes `registry.by_role<hal::X>(hal::role::kMain)` (Display, Touch, Power, AudioOut, Imu, Wifi, Clock, AudioIn). `display_shape()`'s warning names `display.info().id` instead of the literal.
- `MakeAppOptions` (app_boot_test) gains `bool renamed_ids = false;`: `FakeBoard` then registers `display.panel`, `imu.accel`, `storage.flash` and `button.side` (role `power`) instead of today's ids.

- [ ] **Step 1: Write the failing tests.**

```cpp
TEST(AppBoot, DevicesAreFoundByRoleNotById)
// make_app({.renamed_ids = true}) -> app.ui() != nullptr; report has no "mode" note;
// no "storage" note; app.sensors()->available(SensorKind::Accel).
TEST(AppBoot, ADisplayWithoutTheRoleIsLikeNoDisplay)
// FakeDisplay("display.main", 480, 480, 1, "rounded_rect", /*role=*/"") ->
// boot completes, note "mode" == "headless", app.ui() == nullptr.
TEST(NetWiring, FindsTheRadioAndClockByRole)
// registry with FakeWifi("net.radio") and FakeClock("clock.rtc"): start() ->
// wifi() != nullptr and the net card's clock reads come from clock.rtc
// (FakeClock set to a known unix time; the observable the fixture already uses for clock.main).
TEST(Loopback, UsesTheRoleMicAndSpeaker)
// AudioOnlyBoard with ids "audio_in.mic"/"audio_out.spk" (role main) ->
// the status sequence reaches the playback state, not LoopFailed/NotFound.
TEST(AppVoice, FindsTheMicByRole)
// mic registered as "audio_in.mic" -> voice starts and a capture opens that mic.
```

Write each with its fixture's existing helpers and the assertions in the comment.

- [ ] **Step 2: Run** `ctest --test-dir build/sim -R 'AppBoot|NetWiring|Loopback|AppVoice' --output-on-failure`. Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `ctest --test-dir build/sim -R 'App|NetWiring|Loopback|AppVoice|SelfCheck|Routes' --output-on-failure`. Expected: PASS.
- [ ] **Step 5: Commit** `refactor(app): look the main devices up by role`.

---

### Task 7: The power key, self-check, talk key and simulator tools

**Files:**
- Modify: `components/wisp_app/include/wisp/app/power_manager.hpp` (`PowerManagerDeps::power_key`), `components/wisp_app/src/power_manager.cpp:73`, `components/wisp_app/src/app.cpp` (fills `power_key`)
- Modify: `components/wisp_app/src/system_duties.cpp` (47-48, 182, 205, 207) and its header (`std::string power_key_`)
- Modify: `components/wisp_app/src/self_check_screen.cpp` (62-64, 246, 499-501), `components/wisp_app/src/self_check_readouts.cpp` (146-148)
- Modify: `components/wisp_shell/src/conversation.cpp` (`find_talk_button`)
- Modify: `simulator/main.cpp` (215, 418, 801)
- Test: `tests/app/power_manager_test.cpp`, `tests/app/system_duties_test.cpp`, `tests/app/self_check_screen_test.cpp`, `tests/shell/talk_key_test.cpp`

**Interfaces:**
- Produces: `std::string power_key;` in `PowerManagerDeps` (empty: no key is owned; events from any source wake as today). `App` fills it from `by_role<hal::Button>(hal::role::kPower)`.
- `SystemDuties`: `display_`/`power_` by role `main`; `power_key_` resolved at construction; `on_button_event` ignores everything when it is empty, else compares `e.source == power_key_` and topics `power_key_ + ".short"` / `".long"`.
- Self-check: display/power/clock by role; `sd_id_` = id of `by_role<hal::Storage>(kRemovable)` (empty: no live row, exactly today's "device == nullptr" case); the live row matches `media.removable && media.online`. `StorageSection::update` picks `internal` = first medium with `!removable`, `sd` = first with `removable`.
- `shell::find_talk_button(reg)` keeps its signature and returns the id of `reg.by_role<hal::Button>(hal::role::kTalk)`.
- `simulator/main.cpp`: `by_role<Clock>`/`by_role<Display>(kMain)`.

- [ ] **Step 1: Write the failing tests.**

```cpp
TEST(PowerManager, PowerKeyOwnershipFollowsTheRoleKey)
// copy of PowerKeyScreenOffThenVolumeKeyWake... with the key "button.side" (role power)
// and deps.power_key = "button.side": same assertions.
TEST_F(SystemDutiesTest, PowerKeyWithAnotherIdTurnsTheScreenOff)
// board key "button.side" (role power): publish "button.side.short" with source
// "button.side" -> the screen goes to standby as for button.power today;
// "button.vol_up.short" does nothing.
TEST_F(SelfCheckScreen, CardRowFollowsTheRemovableMediumWhateverItsId)
// FullFakeBoard variant with "storage.card" (removable): the row follows online changes
// (Missing -> Ok) exactly as the existing storage.sd test asserts.
TEST(FindTalkButton, UsesTheTalkRole)  // existing UsesRoleCaps, unchanged assertions, stays green
```

- [ ] **Step 2: Run** `ctest --test-dir build/sim -R 'PowerManager|SystemDutiesTest|SelfCheckScreen|SelfCheckModel|FindTalkButton|ShellTalk' --output-on-failure`. Expected: the new tests FAIL.
- [ ] **Step 3: Implement.** Confirm with `grep -rnE '"(display|touch|imu|power|clock|audio_out|audio_in)\.main"|"net\.wifi"|"button\.power"|"storage\.(int|sd)"' components/wisp_{base,os,hal,core,net,voice,admin,ui,scene,agent,shell,app} simulator/*.cpp` that only comments remain.
- [ ] **Step 4: Run** the same filter plus `-R 'StandbyWalksDevicesByPolicy|AudioGoesDownInputFirstAndUpOutputFirst'` (the standby drive order is unchanged). Expected: PASS.
- [ ] **Step 5: Commit** `refactor(app): find the power key, the card and the talk key by role`.

---

### Task 8: The id-literal guard and the AGENTS.md identity rule

**Files:**
- Modify: `tools/check_layering.py` (new check), `tools/check_layering_test.py`
- Modify: `AGENTS.md` section 3 "Device model" (separate commit)

**Interfaces:**
- Produces in `check_layering.py`: `check_device_id_literals(root, errors)`, run by `run()`. Scans `components/wisp_*/**/*.{cpp,hpp,h,c}` except `components/wisp_port_esp/` and `components/wisp_port_sim/`, plus `simulator/*.cpp`, after `strip_comments()`, for the regex `"(?:[a-z_]+\.main|net\.wifi|button\.power|storage\.int|storage\.sd)"`. Error: `"<path>:<line>: device id literal <lit>: look the device up by role (AGENTS.md section 3)"`.

- [ ] **Step 1: Write the failing tests** in `check_layering_test.py` (temp trees, as the file's other cases):
  - `test_device_id_literal_in_portable_code_fails` (`components/wisp_app/src/x.cpp` with `get<hal::Display>("display.main")`);
  - `test_device_id_literal_in_a_comment_passes`;
  - `test_device_id_literal_in_the_ports_passes` (`components/wisp_port_sim/...`, `components/wisp_port_esp/...`);
  - `test_topic_with_an_id_prefix_passes` (`"storage.sd.inserted"`, `"net.wifi.state"`, `"button.power.short"`).
- [ ] **Step 2: Run** `python3 tools/check_layering_test.py`. Expected: FAIL.
- [ ] **Step 3: Implement**; run `python3 tools/check_layering.py` on the tree. Expected: no finding (Tasks 4-7 removed them).
- [ ] **Step 4: Run** `bash tools/ci.sh`. Expected: exit 0.
- [ ] **Step 5: Commit** `test(tools): forbid device id literals in portable code`.
- [ ] **Step 6:** Replace the first bullet of AGENTS.md section 3 "Device model" with three bullets:
  - "Devices are registered in `DeviceRegistry` under ids of the form `<class>.<name>`. An id is a name the board chooses (event topics and the manifest use it); code does not find devices by id."
  - "Portable code finds a device by interface and role: `registry.by_role<hal::Display>(hal::role::kMain)` (vocabulary in `wisp/hal/roles.hpp`, caps `role`). A role is unique per interface. `tools/check_layering.py` rejects device-id literals outside the ports."
  - "One typed interface per device object: `interface()` is `final` in each interface, so typed lookups need no RTTI. `DeviceClass` is a display category (`other` for generic devices)."
  Commit `docs(agents): devices are found by role, not by id`.

---

### Task 9: Device schemas in the HAL and the manifest

**Files:**
- Create: `components/wisp_hal/include/wisp/hal/device_schema.hpp`, `components/wisp_hal/src/device_schema.cpp`
- Modify: `components/wisp_hal/include/wisp/hal/device.hpp` (`describe/get/invoke`), `components/wisp_hal/src/device.cpp`
- Modify: `components/wisp_core/src/device_registry.cpp` (`add()` checks, `manifest()`)
- Modify: `components/wisp_admin/src/routes_device.cpp:36`
- Modify: `web/app/pages/devices.js` (`KNOWN_CLASSES` gains `'other'`), `web/app/i18n/{en,zh,ja}.js` (`'class.other'`: "Other" / "其他" / "その他")
- Modify: `tests/support/fake_devices.hpp` (`FakeGeneric` gains a schema; `kFakeGenericSchema`)
- Test: `tests/hal/device_schema_test.cpp` (new, `LIBS wisp_hal wisp_base`), `tests/core/device_registry_test.cpp`, `tests/admin/routes_test.cpp`

**Interfaces:**
- Produces in `wisp/hal/device_schema.hpp` (spec 3.2, verbatim types):

```cpp
namespace wisp::hal {
enum class ParamType : uint8_t { Bool, Int, Number, String, Enum };
struct ParamSpec {
    const char* name;                 // ^[a-z][a-z0-9_]{0,31}$
    ParamType type;
    const char* desc = "";            // English, one line, for the agent
    double min = 0, max = 0;          // Int/Number, inclusive; both 0: no range
    const char* unit = "";
    std::span<const char* const> choices = {};   // Enum
    uint16_t max_len = 64;            // String, bytes of UTF-8
    bool optional = false;            // args only
    Value def = {};                   // default for an optional arg (see deviation 6)
};
struct CmdSpec {
    const char* name;
    const char* desc;
    std::span<const ParamSpec> args;  // at most 8
    bool changes_state = true;
    uint32_t min_interval_ms = 0;
};
struct DeviceSchema {
    std::span<const ParamSpec> props; // read-only, at most 16
    std::span<const CmdSpec> cmds;    // at most 16
};
inline constexpr uint32_t kDeviceCallBudgetMs = 50;
inline constexpr size_t kMaxSchemaProps = 16, kMaxSchemaCmds = 16, kMaxCmdArgs = 8;
const char* to_string(ParamType t);   // "bool", "int", "number", "string", "enum"
Result<void> check_schema(const DeviceSchema&);           // InvalidArgument naming the field
Value render_schema(const DeviceSchema&);                 // caps "schema"
Result<Map> validate_args(const CmdSpec&, const Value& args);
const ParamSpec* find_prop(const DeviceSchema&, std::string_view name);
const CmdSpec* find_cmd(const DeviceSchema&, std::string_view name);
// info().caps plus "schema" (render_schema(*describe())) when the device describes itself.
Properties caps_of(const Device& device);
}
```

- `Device` gains `virtual const DeviceSchema* describe() const { return nullptr; }`, `virtual Result<Value> get(std::string_view prop);`, `virtual Result<Value> invoke(std::string_view cmd, const Map& args);` (defaults: `Unsupported`). Comment: drivers may assume `prop`/`cmd` are in the schema and `args` already validated and defaulted (only `DeviceService` calls them), still clamp, return within `kDeviceCallBudgetMs`, finish longer actions themselves, never touch flash, return small values.
- `check_schema` rules: every name matches `^[a-z][a-z0-9_]{0,31}$` and is unique within its list; counts within the limits; `cmd.desc` non-null; Enum has at least one choice; `min <= max`; `optional` only on args, its non-null `def` passes the arg's own validation; props are never `optional`.
- `render_schema` shape (lists keep declared order): `{"props":[P...],"cmds":[{"name","desc","changes_state","min_interval_ms","args":[P...]}]}` where P is `{"name","type"}` plus `"desc"` when non-empty, `"min"`/`"max"` when a range is set (int64 for Int, double for Number), `"unit"` when non-empty, `"choices"` for Enum, `"max_len"` for String, `"optional": true` and `"default"` (when non-null) for optional args.
- `validate_args` (args null means `{}`; anything but a Map: `InvalidArgument "args must be an object"`): unknown key → `"unknown argument '<n>'"`; missing required → `"missing argument '<n>'"`; Bool must be a bool; Int accepts an Int or an integral Double within int64 (stored as Int); Number accepts Int or Double (stored as given); String must be valid UTF-8 of at most `max_len` bytes; Enum must be one of `choices`; ranges inclusive: `"'<n>' must be between <min> and <max>"`; wrong type: `"'<n>' must be <type>"`. Optional args missing get `def` when non-null. Returns the complete argument map.
- `DeviceRegistry::add()` additionally returns `InvalidArgument` for: caps that already contain `"schema"`; `check_schema()` failing (message prefixed with the id); a typed device whose schema has commands (`"<id>: commands are only for generic devices"`); a `Display` with any schema. `manifest()` writes `caps_of(*device)`.
- `routes_device.cpp` writes `hal::caps_of(*dev)`.
- `FakeGeneric(std::string id, const hal::DeviceSchema* schema, Properties caps = {}, std::string role = {})` (the Task 1 constructor stays); records each `invoke` (`std::vector<std::pair<std::string, Map>> invocations()`), each `get`, each `set_power`; `std::function<Result<Value>(std::string_view, const Map&)> on_invoke` (default: ok, null) and `on_get` (default: `Value(1)`); caps `standby: "off"` when constructed with `standby_off = true`. `kFakeGenericSchema`: prop `level` (Int 0..100); cmds `pulse` (`ms` Int 10..2000 unit "ms", `strength` Int 0..100 optional default 80; `min_interval_ms` 100), `set_mode` (`mode` Enum {"soft","hard"}), `read_raw` (`reg` Int 0..255; `changes_state` false).

- [ ] **Step 1: Write the failing tests.**

```cpp
// device_schema_test.cpp
TEST(DeviceSchema, RangeEdgesAreInclusive)        // pulse ms 10 and 2000 ok; 9 and 2001 InvalidArgument
TEST(DeviceSchema, IntegralDoublesAreInts)         // ms 150.0 -> Int 150 in the result; 150.5 -> InvalidArgument
TEST(DeviceSchema, DefaultsAreFilled)              // {ms:150} -> strength == 80
TEST(DeviceSchema, MissingAndUnknownArgumentsAreNamed)
// {} -> message "missing argument 'ms'"; {ms:150, speed:1} -> "unknown argument 'speed'"
TEST(DeviceSchema, TypesAreChecked)                // ms "150" -> "'ms' must be int"; mode "x" -> InvalidArgument
TEST(DeviceSchema, StringLengthIsInBytes)          // max_len 4: "abcd" ok, "日本" (6 bytes) refused, invalid UTF-8 refused
TEST(DeviceSchema, NullArgsMeanEmptyObject)        // read_raw with null -> "missing argument 'reg'"
TEST(DeviceSchema, RenderKeepsOrderAndShape)
// to_json(render_schema(kFakeGenericSchema)) equals the exact expected string (write it out in the test).
TEST(DeviceSchema, CheckRejectsBadTables)
// a bad name "Pulse", 17 props, 9 args, duplicate cmd names, min > max, empty Enum, a bad default.
TEST(DeviceSchema, CapsOfAddsTheSchema)            // FakeGeneric: caps_of has "schema"; FakeImu: caps_of == info().caps

// device_registry_test.cpp
TEST(DeviceRegistry, RejectsHandWrittenSchemaCaps)
TEST(DeviceRegistry, RejectsCommandsOnTypedDevices)     // a FakeImu subclass describing a cmd
TEST(DeviceRegistry, AcceptsPropertiesOnTypedDevices)   // a FakeImu subclass with prop temp_c (Number)
TEST(DeviceRegistry, RejectsAnySchemaOnADisplay)
TEST(DeviceRegistry, ManifestRendersTheSchema)
// FakeGeneric("haptic.main", &kFakeGenericSchema): manifest entry class "other", caps.schema present.

// routes_test.cpp
TEST(Routes, DevicesCarryRoleAndSchema)  // GET /api/v1/devices: haptic.main caps.schema.cmds[0].name == "pulse"
```

- [ ] **Step 2: Run** `ctest --test-dir build/sim -R 'DeviceSchema|DeviceRegistry|Routes' --output-on-failure`. Expected: FAIL.
- [ ] **Step 3: Implement.** Run `python3 tools/check_i18n.py` for the web strings.
- [ ] **Step 4: Run** the same filter. Expected: PASS.
- [ ] **Step 5: Commit** `feat(hal): devices can describe properties and commands`.

---

### Task 10: `core::DeviceService`

**Files:**
- Create: `components/wisp_core/include/wisp/core/device_service.hpp`, `components/wisp_core/src/device_service.cpp`
- Modify: `components/wisp_app/include/wisp/app/app.hpp`, `components/wisp_app/src/app.cpp` (construct next to `SensorService`; feed the power level from the existing `power.state` subscription; `core::DeviceService* devices();`)
- Test: `tests/core/device_service_test.cpp` (new, `LIBS wisp_core wisp_port_sim_os`)

**Interfaces:**
- Produces:

```cpp
namespace wisp::core {
class DeviceService {
public:
    static constexpr size_t kMaxIdBytes = 32;
    static constexpr size_t kMaxResultBytes = 1024;  // JSON of a get/invoke result
    static constexpr size_t kMaxRateEntries = 32;    // (id, cmd) pairs with min_interval_ms
    DeviceService(DeviceRegistry& registry, OccupancyRegistry* occupancy,
                  std::function<uint64_t()> now_ms = os::now_ms);
    // Devices with a schema, by id: [{"id","class","model","role","schema"}];
    // "role" is the caps value (string or list) or null.
    List list() const;
    Result<Value> get(const std::string& id, const std::string& prop) const;
    // holder: "agent" or "scene.<id>"; used for occupancy and the log line.
    Result<Value> call(const std::string& id, const std::string& cmd, const Value& args,
                       const std::string& holder);
    // From power.state: Standby/DeepStandby make "standby: off" devices unreachable.
    void set_power_level(hal::PowerLevel level);
};
}
```

- `call()` in order, each failure returned at once without calling the driver:
  1. id longer than `kMaxIdBytes`, not registered, or no `describe()` → `NotFound` (`"no device '<id>'"` / `"'<id>' describes nothing"`).
  2. `find_cmd` fails → `NotFound` `"'<id>' has no command '<cmd>'"`.
  3. `interface() != None` → `PermissionDenied` `"commands are only for generic devices"`.
  4. `validate_args` → its `InvalidArgument`.
  5. Occupancy: an `Exclusive` entry for `id` with another holder → `Busy` `"'<id>' is held by <holder>"`.
  6. `standby_policy == Off` and `!powered_at(Off, level)` → `Busy` `"'<id>' is off while the screen is off"`.
  7. `min_interval_ms > 0`: under the mutex, an entry (id, cmd) younger than the interval → `RateLimited` `"'<id>.<cmd>' called again too soon; retry_after_ms=<n>"`; else stamp `now` (deviation 8). The table holds at most `kMaxRateEntries` entries in an `os::LargeVector` of `{std::array<char, 33> id, cmd; uint64_t at_ms; bool warned;}`; when full, an expired entry is replaced, else the oldest.
  8. `invoke()`; measure with `now_ms`; over `hal::kDeviceCallBudgetMs` → one `WLOG_W("device", "%s.%s took %u ms (budget %u ms)")` per (id, cmd) (the `warned` flag; pairs without an interval use the same table).
  9. Result JSON larger than `kMaxResultBytes` → `DeviceFailure` `"result too large (<n> bytes)"`.
  10. `changes_state` → `WLOG_I("device", "device %s.%s by %s", ...)`.
  A driver error is returned as the driver gave it.
- `get()`: steps 1, `find_prop` (`NotFound` `"'<id>' has no property '<prop>'"`), 6, `device->get()`, 8 (keyed by the prop name), 9. No occupancy, no interval: reads are side-effect free.
- The service holds a `shared_ptr` from the registry for the whole call, so a device removed meanwhile stays alive until the call returns; the next call answers `NotFound`.
- `App`: `impl_->devices = std::make_unique<core::DeviceService>(impl_->registry, &impl_->occupancy);` right after `SensorService`; the `power.state` handler also calls `devices->set_power_level()` (`"active"`, `"dim"`, `"standby"`, `"deep_standby"`).

- [ ] **Step 1: Write the failing tests** (fake clock `uint64_t t`, `now_ms = [&]{ return t; }`; registry with `FakeGeneric("haptic.main", &kFakeGenericSchema)`):

```cpp
TEST(DeviceService, ListsOnlyDevicesThatDescribeThemselves)
// plus FakeImu("imu.main"): list() has one entry, id "haptic.main", class "other", schema present.
TEST(DeviceService, CallValidatesThenInvokes)
// call("haptic.main","pulse",{ms:150},"agent") ok; invocations()[0] == {"pulse",{ms:150,strength:80}}.
TEST(DeviceService, RefusalsNeverReachTheDriver)
// unknown id, unknown cmd, ms 5, extra arg: NotFound/NotFound/InvalidArgument/InvalidArgument;
// invocations() empty after all four.
TEST(DeviceService, MinIntervalIsPerCommandAcrossHolders)
// pulse at t=0 by "agent" ok; t=50 by "scene.a" -> RateLimited, message contains "retry_after_ms=50";
// t=100 ok; set_mode at t=100 ok (no interval); exactly 3 invocations.
TEST(DeviceService, ARefusedCallDoesNotStartTheInterval)
// occupancy Exclusive by "scene.b"; pulse by "agent" -> Busy; release; pulse at the same t -> ok.
TEST(DeviceService, ExclusiveHolderMayCallOthersAreBusy)
// Exclusive by "scene.b": "scene.b" ok, "agent" Busy.
TEST(DeviceService, OffForStandbyIsBusyAndNotWoken)
// FakeGeneric with standby off; set_power_level(Standby): call and get -> Busy;
// set_power_calls() == 0; invocations() empty; set_power_level(Active) -> ok.
TEST(DeviceService, CommandsOnTypedDevicesAreRefused)
// add() rejects a typed device with commands (Task 9), so the second line is tested with a
// FakeImu subclass that describes only prop temp_c when added and a cmd "calibrate" after a
// flag is set: get("imu.main","temp_c") ok; flag set; call("imu.main","calibrate",{}) ->
// PermissionDenied; the subclass's invoke count stays 0.
TEST(DeviceService, TypedDeviceWithoutSchemaIsNotFound)  // FakeClock("clock.main") -> NotFound
TEST(DeviceService, ResultLargerThan1KiBIsAFailure)  // on_invoke returns a 2000-byte string
TEST(DeviceService, SlowCallIsLoggedOnce)
// on_invoke advances t by 80; two calls (t moved past the interval between them) ->
// log::ring_snapshot() contains "haptic.main.pulse took 80 ms" exactly once.
TEST(DeviceService, StateChangesAreLoggedWithTheHolder)
// pulse by "scene.demo" -> ring contains "device haptic.main.pulse by scene.demo";
// read_raw (changes_state false) adds no such line.
TEST(DeviceService, RemovedDeviceStaysAliveForTheRunningCall)
// on_invoke removes "haptic.main" from the registry and returns ok -> the call returns ok;
// the next call -> NotFound.
TEST(DeviceService, DriverErrorsPassThrough)  // on_invoke returns DeviceFailure -> DeviceFailure
```

- [ ] **Step 2: Run** `ctest --test-dir build/sim -R DeviceService --output-on-failure`. Expected: FAIL.
- [ ] **Step 3: Implement**, then the `App` wiring.
- [ ] **Step 4: Run** `ctest --test-dir build/sim -R 'DeviceService|AppBoot' --output-on-failure`. Expected: PASS.
- [ ] **Step 5: Commit** `feat(core): add the device service for self-describing devices`.

---

### Task 11: `SimHaptic`, the simulator's generic device

**Files:**
- Create: `components/wisp_port_sim/include/wisp/port/sim/sim_haptic.hpp`, `components/wisp_port_sim/dev/sim_haptic.cpp` (in `wisp_port_sim_dev`)
- Modify: `components/wisp_port_sim/sim_board.cpp`
- Test: `tests/sim/sim_board_test.cpp`, `tests/app/power_manager_test.cpp`, `tests/smoke/sim_smoke.py`

**Interfaces:**
- Produces: `class SimHaptic : public wisp::hal::Device` (interface `None`, class `Other`), `SimHaptic(std::string id, std::string role, std::function<uint64_t()> now_ms = os::now_ms)`; model `"sim-haptic"`; caps `standby: "off"`, `role`. Schema (`constexpr`, deviation 6): prop `active` (Bool, "Motor running"); cmds `pulse` ("Vibrate once"; `ms` Int 10..2000 "ms"; `strength` Int 0..100 "%" optional default 80; `min_interval_ms` 100) and `effect` ("Play a built-in effect"; `id` Int 1..123). `pulse` sets `until_ms = now + ms` and logs `WLOG_I("sim", "haptic pulse %d ms %d%%")`; `effect` logs `"haptic effect %d"` and runs 100 ms; `active` is `now < until_ms` (a bounded action needs no timer in the simulator). It clamps in `invoke()` too. `set_power(Standby|DeepStandby)` stops it and marks it off; `invoke`/`get` while off → `Busy`.
- `SimBoard::init()` registers `haptic.main` with role `main` (via `make_or_fail`, so `--disable haptic.main` and `--disable other` work).

- [ ] **Step 1: Write the failing tests.**

```cpp
TEST(SimHaptic, PulseRunsForItsDurationThenStops)
// fake clock t: invoke("pulse",{ms:150,strength:80}) ok; get("active") true at t+149, false at t+150.
TEST(SimHaptic, ClampsWhenCalledDirectly)  // invoke("pulse",{ms:999999,strength:500}) -> active false at t+2000
TEST(SimHaptic, StandbyStopsItAndRefusesCalls)
// pulse, set_power(Standby) -> active false; invoke -> Busy; set_power(Active) -> ok.
TEST(SimHaptic, SchemaPassesTheChecks)   // check_schema(*describe()) ok; names and ranges as listed
TEST_F(SimBoard, RegistersTheHaptic)     // find("haptic.main") class Other, caps_of has schema
TEST(PowerManager, GenericDevicesGoDownWithTheOthers)
// FakeGeneric (standby off) + the existing audio fakes: enter_standby -> its set_power(Standby)
// is called, before the audio input's (order log), as StandbyWalksDevicesByPolicy records.
```

`sim_smoke.py`: `EXPECTED_DEVICE_IDS` gains `"haptic.main"` (comment: 14 devices); `case_default` asserts its `class == "other"`, `caps.schema.cmds[0].name == "pulse"`, and that the manifest line still parses as one JSON line.

- [ ] **Step 2: Run** `ctest --test-dir build/sim -R 'SimHaptic|SimBoard|PowerManager|sim_smoke' --output-on-failure`. Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** the same filter plus `-R 'SelfCheck|SimOptions'`. Expected: PASS.
- [ ] **Step 5: Commit** `feat(sim): add a haptic motor that describes itself`.

---

### Task 12: Agent tools `device.list` and `device.call`

**Files:**
- Create: `components/wisp_agent/src/tools_device_call.cpp`
- Modify: `components/wisp_agent/include/wisp/agent/tools.hpp`, `components/wisp_agent/src/tool_io.hpp` (`tool_code`), `components/wisp_agent/CMakeLists.txt`
- Modify: `components/wisp_ui/include/wisp/ui/strings.hpp`, `components/wisp_ui/src/strings_{zh,ja,en}.cpp` (`StrId::AgentNoteDevice` after `AgentNoteSetting`)
- Modify: `components/wisp_app/src/agent_wiring.{hpp,cpp}` (`AgentWiringDeps::devices`, registration, `tools()`), `components/wisp_app/include/wisp/app/app.hpp`, `components/wisp_app/src/app.cpp` (`agent_tools()`, pass `devices`)
- Test: `tests/agent/device_call_tools_test.cpp` (new, LIBS as `device_tools_test`), `tests/app/app_agent_test.cpp`

**Interfaces:**
- Produces: `struct DeviceCallToolDeps { core::DeviceService& devices; };` and `Result<void> add_device_call_tools(ToolRegistry& reg, DeviceCallToolDeps deps);`
- `tool_code()`: `RateLimited` → `"rate_limited"`, `DeviceFailure` → `"device_failure"` (deviation 10).
- `device.list`: `ToolRisk::Normal`, `ToolThread::Agent`, no arguments. Result `{"ok":true,"devices":[...]}` with at most 32 entries (`"truncated":true` beyond), serialized into `ToolOutput::result_json` (an `os::LargeString`, as `tools_lua.cpp` does). Description, exactly: `"Peripherals that describe themselves (a vibration motor, a light, a sensor, ...): id, class, model, role and schema. schema.props are read-only values: read one with device_call op \"get\". schema.cmds are commands: run one with op \"invoke\" and its args (type; min and max inclusive; unit; choices; optional ones have a default). A command with changes_state false only reads. The screen, microphone, speaker, Wi-Fi, storage and battery are not here; see device_status."`
- `device.call`: `ToolRisk::Note`, `ToolThread::Agent`, `timeout_ms` 5000, schema exactly spec 4.2's (`id` maxLength 32, `op` enum get/invoke, `name` maxLength 32, `args` object; required id, op, name). `get` → `{"ok":true,"value":...}`; `invoke` → `{"ok":true,"result":...}`; errors through `detail::fail()`. Holder `"agent"`. When the command `changes_state` (asked through `DeviceService::changes_state()`, below), the output's note is `tr_fill_n(StrId::AgentNoteDevice, lang, {id, cmd})`. Description, exactly: `"Reads a property (op \"get\", name = the property) or runs a command (op \"invoke\", name = the command, args = its arguments) of a device listed by device_list. Arguments are checked against the schema before the device is touched. Errors: not_found (no such device, property or command), invalid_value (the message names the argument and the rule), rate_limited (called again too soon; the message gives retry_after_ms), busy (in use elsewhere, or off while the screen is off), device_failure."`
- `StrId::AgentNoteDevice`: zh `"设备 {}：{}"`, ja `"デバイス {}：{}"`, en `"Device {}: {}"`.
- `AgentWiringDeps` gains `core::DeviceService* devices = nullptr;`; `AgentWiring` registers `check("device call", add_device_call_tools(...))` immediately after the `"device"` group when it is non-null. Tool order becomes `... notify.dismiss, device.list, device.call, docs.read, ...`: a one-time change of the provider's cached prefix. `const agent::ToolRegistry* AgentWiring::tools() const;`, `const agent::ToolRegistry* App::agent_tools() const;` (deviation 15).
- `DeviceService` (Task 10's file) gains `bool changes_state(const std::string& id, const std::string& cmd) const;` (false for anything unknown).

- [ ] **Step 1: Write the failing tests** (`DeviceCallTools` fixture: registry with `FakeGeneric("haptic.main", &kFakeGenericSchema)` and `FakeImu("imu.main")`, a `DeviceService`, a `ToolRegistry` with empty executors, `ctx.lang = "zh"`):

```cpp
TEST_F(DeviceCallTools, ListShowsOnlyDescribedDevices)   // devices.size() == 1, [0].id "haptic.main", schema.cmds present
TEST_F(DeviceCallTools, ListStopsAt32)                    // 40 generic devices -> 32 entries, truncated true
TEST_F(DeviceCallTools, InvokeRunsTheCommandWithANote)
// device_call {"id":"haptic.main","op":"invoke","name":"pulse","args":{"ms":150}} ->
// ok, result null, note == "设备 haptic.main：pulse"; invocations()[0].second strength 80.
TEST_F(DeviceCallTools, QueriesHaveNoNote)                // read_raw {"reg":3} -> ok, note empty
TEST_F(DeviceCallTools, GetReturnsTheValue)               // op get name level -> value 1
TEST_F(DeviceCallTools, ErrorsUseTheAgentsCodes)
// unknown id -> "not_found"; ms 5 -> "invalid_value" with message naming 'ms';
// second pulse at once -> "rate_limited"; on_invoke DeviceFailure -> "device_failure";
// op "set" -> "bad_args" (registry validation, field "op").
TEST_F(DeviceCallTools, ArgsMustBeAnObject)               // "args":[1] -> bad_args, field "args"
TEST(AppAgent, DeviceToolsFollowTheNotificationTools)
// UiNetHarness with display: in agent_tools()->tools_json(), the positions of
// "notify_dismiss" < "device_list" < "device_call" < "docs_read".
```

- [ ] **Step 2: Run** `ctest --test-dir build/sim -R 'DeviceCallTools|AppAgent|Strings' --output-on-failure`. Expected: FAIL.
- [ ] **Step 3: Implement.** The `Strings.EveryGlyphIsInTheBuiltInFont` test proves the new text renders.
- [ ] **Step 4: Run** the same filter plus `-R 'DeviceTools|ToolRegistry|AgentLoop|Context'`. Expected: PASS.
- [ ] **Step 5: Commit** `feat(agent): list and call self-describing devices`.

---

### Task 13: `permissions.device` and `wisp.device.list/get/call`

**Files:**
- Modify: `components/wisp_core/include/wisp/core/scene_manifest.hpp`, `components/wisp_core/src/scene_manifest.cpp`
- Modify: `components/wisp_scene/src/scene_api.hpp` (`SceneApi` fields, file header list), `components/wisp_scene/src/bind_device.cpp`, `components/wisp_scene/include/wisp/scene/scene_runtime.hpp` (`SceneRuntimeDeps::devices`), `components/wisp_scene/src/scene_runtime.cpp` (`wire_sensors` sets the device fields), `components/wisp_scene/src/scene_runtime_lua.cpp` (agent VM grant)
- Modify: `components/wisp_app/src/scene_wiring.{hpp,cpp}` (`build_runtime(..., core::DeviceService* devices)`), `components/wisp_app/src/app.cpp`
- Modify: `components/wisp_scene/src/schema.cpp` (the `wisp.device` and `scene.json` sections); regenerate `docs/scene-api.md` (from the `Schema.DocIsUpToDate` failure output) and `components/wisp_agent/src/docs_data.cpp` (`python3 tools/gen_agent_docs.py`)
- Test: `tests/core/scene_manifest_test.cpp`, `tests/scene/device_binding_test.cpp` (new, LIBS as `sensor_binding_test`), `tests/scene/schema_test.cpp`

**Interfaces:**
- `SceneManifest` gains `std::vector<std::string> devices;` (`permissions.device`): an array of at most 8 strings, each `^[a-z][a-z0-9_]*\.[a-z0-9_]+$` and at most 32 bytes; errors `"permissions.device must be an array of device ids"`, `"permissions.device lists more than 8 devices"`. Existence is not checked at install. No install prompt (spec 20, answer 3).
- `SceneApi` gains `core::DeviceService* devices = nullptr;`, `std::vector<std::string> device_grants;`, `bool device_grant_all = false;`. The runtime sets `devices = deps_.devices` and `device_grants = manifest.devices` for scenes; the agent's `lua.run` VM sets `device_grant_all = true`. Holder: `device_grant_all ? "agent" : api.holder`.
- Lua (in `install_device`, longjmp rule: argument checks with raising calls first, the service call in a helper whose C++ objects are gone before Lua is touched):
  - `wisp.device.list()` → array of `{id, class, model, role, schema}` (`push_value` of `DeviceService::list()`); `{}` without the service.
  - `wisp.device.get(id, prop)` → value, or `nil, code`.
  - `wisp.device.call(id, cmd, args)` → result (`true` for a null result), or `nil, code`. `args`: a table or nil/none (anything else raises an argument error); converted with `lua_to_json` + `parse_json`.
  - Codes: `"unavailable"` without the service; `"permission_denied"` for an id not in `device_grants` (unless `device_grant_all`), checked before the service; otherwise `wisp::to_string(code)` of the service's error.
  - Calls are synchronous on `wisp.scene`; allowed in background and headless scenes (deviation 12).
- Scene API text (`schema.cpp`, appended to the `## wisp.device` list), exactly:

```
- `wisp.device.list()` returns the peripherals that describe themselves, as an array
  of `{id, class, model, role, schema}`. `schema.props` are read-only values;
  `schema.cmds` are commands with their `args` (`type`, inclusive `min`/`max`,
  `unit`, `choices`, `optional` with its `default`).
- `wisp.device.get(id, prop)` returns a property's value, or `nil, code`.
- `wisp.device.call(id, cmd, args)` runs a command (`args` a table, may be left out)
  and returns its result, `true` when it returns nothing, or `nil, code`.
- Codes: `not_found`, `invalid_argument` (the argument breaks the schema),
  `permission_denied` (the id is not in `permissions.device`), `rate_limited`
  (called again sooner than the command allows), `busy` (in use elsewhere, or off
  while the screen is off), `device_failure`, `unavailable`.
- `get` and `call` need the id in `permissions.device` of `scene.json`; `list` does
  not. A call returns within 50 ms: a longer action (a 2 s vibration) starts and
  ends by itself.
```

  and in `## scene.json`: `- \`permissions.device\`: the device ids \`wisp.device.get\` and \`wisp.device.call\` may use, at most 8.`

- [ ] **Step 1: Write the failing tests.**

```cpp
// scene_manifest_test.cpp
TEST(SceneManifest, ParsesDevicePermissions)       // ["haptic.main"] -> devices == {"haptic.main"}
TEST(SceneManifest, RejectsBadDevicePermissions)   // 9 ids; "Haptic"; 3 (not a string) -> InvalidArgument with the messages above

// device_binding_test.cpp (SceneRuntimeFixture; configure_deps sets d.devices to a DeviceService
// over a registry with FakeGeneric("haptic.main", &kFakeGenericSchema))
TEST_F(DeviceBinding, DeclaredSceneCallsTheDevice)
// scene.json permissions.device ["haptic.main"]; main.lua logs the result of
// wisp.device.call("haptic.main","pulse",{ms=150}) -> log "true"; invocations()[0] ms 150, strength 80.
TEST_F(DeviceBinding, UndeclaredSceneIsDenied)
// no permissions -> log "nil permission_denied"; invocations() empty.
TEST_F(DeviceBinding, ErrorsAreSnakeCaseCodes)
// declared; ms=5 -> "nil invalid_argument"; unknown cmd -> "nil not_found";
// second pulse at once -> "nil rate_limited".
TEST_F(DeviceBinding, GetNeedsThePermissionToo)
TEST_F(DeviceBinding, ListNeedsNoPermission)       // #wisp.device.list() == 1, [1].schema.cmds[1].name == "pulse"
TEST_F(DeviceBinding, HolderIsTheScene)            // log ring has "device haptic.main.pulse by scene.<id>"
TEST_F(DeviceBinding, NonTableArgsRaise)           // call(..., 5) -> script error naming argument #3
TEST_F(DeviceBinding, NoServiceIsUnavailable)      // d.devices null -> "nil unavailable"; list() == {}
TEST_F(DeviceBinding, AgentLuaRunIsGrantedAsAgent)
// rt->run_lua("return wisp.device.call('haptic.main','pulse',{ms=150})") -> result true;
// ring has "by agent".
```

- [ ] **Step 2: Run** `ctest --test-dir build/sim -R 'SceneManifest|DeviceBinding|Schema' --output-on-failure`. Expected: FAIL (`Schema.DocIsUpToDate` after the text change).
- [ ] **Step 3: Implement.** Copy the expected `scene-api.md` as the failure message says; run `python3 tools/gen_agent_docs.py`, then `--check`.
- [ ] **Step 4: Run** the same filter plus `-R 'SensorBinding|LuaTools|Docs|ExamplesTest'`. Expected: PASS.
- [ ] **Step 5: Commit** `feat(scene): scenes call the devices they declare`.

---

### Task 14: The worked example end to end, and the AGENTS.md generic-path rule

**Files:**
- Modify: `tests/CMakeLists.txt` (`app_agent_test` also links `wisp_port_sim_dev`)
- Test: `tests/app/app_agent_test.cpp`
- Modify: `AGENTS.md` section 3 "Device model" (separate commit)

**Interfaces:**
- Consumes: `SimHaptic` (Task 11), `App::agent_tools()` (Task 12), the scene wiring of `DeviceService` (Task 13). Proves spec 5: a generic device added by a board line reaches the agent and Lua with no change to hal enums, wiring or bindings beyond H1's.

- [ ] **Step 1: Write the test.**

```cpp
TEST(AppAgent, HapticIsReachableFromTheAgentAndFromLua)
// UiNetHarness with display and extra_devices = {SimHaptic("haptic.main", "main", clock)}:
// agent_tools()->call("device_list", "", ctx) lists "haptic.main";
// call("device_call", {"id":"haptic.main","op":"invoke","name":"pulse","args":{"ms":150}}) ok
//   and a following op get "active" returns true;
// after the clock passes the interval, call("lua_run",
//   {"code":"return wisp.device.call('haptic.main','pulse',{ms=150})"}) returns result true;
// the log ring holds "device haptic.main.pulse by agent" twice.
TEST(AppAgent, HapticIsOffWhileTheScreenIsOff)
// power manager enters standby -> device_call invoke -> error "busy"; after waking -> ok.
```

- [ ] **Step 2: Run** `ctest --test-dir build/sim -R AppAgent --output-on-failure`. Expected: PASS (all pieces exist); if it fails, the failure is a wiring gap to fix in this task.
- [ ] **Step 3: Commit** `test(app): a self-describing device reaches the agent and Lua`.
- [ ] **Step 4:** Add to AGENTS.md section 3 "Device model", after the identity bullets: "A device no core service knows is generic (interface `None`, class `other`) and describes itself with a `hal::DeviceSchema` (read-only properties, commands with ranges and `min_interval_ms`). The agent (`device.list`, `device.call`) and scenes (`wisp.device.*`) reach it only through `core::DeviceService`. Commands exist only on generic devices; a typed device may declare read-only properties." Commit `docs(agents): the generic device path`.

---

### Task 15: Documents, backlog and full check

**Files:**
- Modify: `docs/superpowers/specs/2026-11-14-device-model-design.md`: spec 1.1.2's table gains the topics row (`system_duties.cpp:205,207`); 2.4 item 3 notes that `MediaStatus::removable` was added; 2.3 notes per-kind uniqueness for generic devices; 3.6 states the check order of `call()`; 4.2 the agent's error codes; 4.3 `true` for a null result and `get` needing the permission; 13's H1 notes that ESP ids moved to configs in H2 (deviations 1-14, each as a one-line statement of the rule now in the code).
- Modify: `docs/architecture.md`: the device-model part (interfaces, roles, `by_role`, `DeviceService`, the generic path); "Machine-readable output": manifest caps `role` and `schema`, class `other`; "Event topics": `<power key id>.short/long`, `storage.sd.*` source is the card's id.
- Modify: `docs/porting-guide.md` "Step 3: Write `board.cpp`": roles (the vocabulary table, uniqueness, which roles a board must give: display/touch/power/clock/imu/wifi/audio `main`, storage `internal`/`removable`, button `power`/`talk`); how a board adds a generic device (a driver with a `DeviceSchema`, `standby: "off"`, one `registry.add()`); the actuator guideline of spec 3.5 (ranges, clamp in `invoke()`, `min_interval_ms`, bounded durations, safe state on construction and in `set_power(Standby)`, return within 50 ms).
- Regenerated `docs/scene-api.md` and `docs_data.cpp` are already committed (Task 13).
- Modify: `CHANGELOG.md` `Unreleased`: devices found by role; generic devices with `device.list`/`device.call` and `wisp.device.list/get/call`; `permissions.device`; manifest `role`/`schema`/`other`; the simulator's `haptic.main`.
- Modify: `docs/backlog.md`: the device checks below; H2's move of ESP ids into `BoardSpec` slots; `docs/hardware/*.md` manifest lines with `role` (after a device run); the install-time prompt for `permissions.device` (phase I).

- [ ] **Step 1:** Edit the documents above.
- [ ] **Step 2:** Run `bash tools/ci.sh`. Expected: exit 0, no warnings in test output.
- [ ] **Step 3:** Run the simulator against `tools/fake_openai.py` with a scripted turn that calls `device_list` then `device_call` pulse `{"ms":150}`: the log shows `sim: haptic pulse 150 ms 80%` and `device haptic.main.pulse by agent`, Home shows the note "设备 haptic.main：pulse". Run `--disable haptic.main` once: boot completes, the self-check shows it failed ("disabled by --disable"). Record what was observed in the commit body.
- [ ] **Step 4: Commit** `docs: record the device identity and generic path`.

**Device checks for the owner (not run by the agent):**
- Both boards build (`idf.py bmgr -c ../boards -b <board> && idf.py build`) with Task 3's ESP and board edits.
- `WISP_MANIFEST` on both boards: same devices, ids, models and caps as before, plus `role` (`main`, `internal`/`removable`, `power`, `talk`); no new boot-report failure.
- Self-check: every device green; the SD row on the 2.16 follows card insert/remove; display, battery and clock readouts present.
- Voice: hold the talk key (2.16 Vol-, 1.75C BOOT), speak, hear the answer.
- Standby and wake with the power key, short and long press; the power key still wakes from deep standby (role-based polling); on the 1.75C both the GPIO3 and the PMU fallback paths if switchable.
- Net: Wi-Fi connects, the RTC is set after SNTP, the admin device page lists caps `role` and shows nothing new missing.
- `mem` idle: unchanged against `ram_budget.hpp` (no internal-RAM change is intended).
