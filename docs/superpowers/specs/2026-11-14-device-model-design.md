# Device model, driver adapters and board kit: design

- Date: 2026-11-14
- Status: approved by the owner on 2026-11-14 (section 20), with the scope corrections recorded there
- Scope: phase H of the architecture roadmap (`docs/superpowers/plans/2026-11-14-architecture-optimization-roadmap.md`, section H). Device identity (interfaces, classes, roles), a generic access path for devices no core service knows, the agent tool and Lua binding built on it, chip adapters moved out of the ESP port, a shared board kit, target readiness for the next SoC ("ESP32-S31"), input and display generality, and channel roles for audio capture.
- Prerequisites: foundation spec (`2026-11-01-foundation-design.md`, section 5 "HAL and capability list"), scene-device spec (`2026-11-05-scene-device-design.md`, standby policy and occupancy), agent-core spec (`2026-11-05-agent-core-design.md`, confirmation), flash-thread spec (`2026-11-06-flash-thread-design.md`, task and memory rules).
- Basis: a read of `main` at `85a99aa` (2026-11-14). Every statement about current behaviour below names the file and line it comes from.
- Owner rulings already in force: the agent gets as much control as possible and is asked as rarely as possible (roadmap "Scope"); `device.call` is allowed for every self-describing device without confirmation (roadmap "Open decisions", item 2, proposal accepted).

---

## 1. Background and goals

### 1.1 Current state

#### 1.1.1 The device class doubles as the type tag

- `hal::DeviceClass` is a closed enum of ten classes (`components/wisp_hal/include/wisp/hal/device.hpp:28-39`). Each interface declares `static constexpr DeviceClass kClass` (for example `display.hpp:48`, `wifi.hpp:55`), and `DeviceRegistry::get<T>()` compares `device->info().cls == T::kClass` and then `static_pointer_cast`s (`components/wisp_core/include/wisp/core/device_registry.hpp:47-54`; `list<T>()` the same at `:58-67`).
- `info().cls` is plain data that each concrete class writes itself (`info_.cls = kClass`, e.g. `components/wisp_port_esp/dev/esp_power.cpp:29`). Nothing ties it to the C++ type, so a class can be mislabelled, and the cast is then undefined behaviour.
- The class is not one-to-one with an interface: `hal::Wifi` uses `DeviceClass::Network` (`wifi.hpp:55`). A second network interface (Ethernet, a cellular modem) would share the class, and `get<hal::Wifi>()` would cast it to `Wifi`.
- A peripheral that matches none of the ten classes (a vibration motor, an RGB LED, a light sensor) cannot be registered without adding an enum value, which also needs `to_string`/`class_from_string` entries (`components/wisp_hal/src/device.cpp:5-43`).

#### 1.1.2 Devices are found by fixed ids

Portable code asks for devices by literal id. The complete list (comments excluded):

| Where | Id | What the caller needs | Replacement |
|---|---|---|---|
| `components/wisp_app/src/app.cpp:351` | `display.main` | the screen the UI runs on | role lookup `Display` `main` |
| `app.cpp:360` | `touch.main` | the pointer over that screen | role lookup `Touch` `main` |
| `app.cpp:398` | `power.main` | battery for `PowerMonitor` | role lookup `Power` `main` |
| `app.cpp:406` | `audio_out.main` | speaker for `TonePlayer` | role lookup `AudioOut` `main` |
| `app.cpp:451` | `imu.main` | motion for `SensorService` | role lookup `Imu` `main` |
| `components/wisp_app/src/net_wiring.cpp:31` | `net.wifi` | the station radio | role lookup `Wifi` `main` |
| `net_wiring.cpp:40` | `clock.main` | RTC to set after SNTP | role lookup `Clock` `main` |
| `net_wiring.cpp:176` | `power.main` | battery for the net card | role lookup `Power` `main` |
| `components/wisp_app/src/voice_wiring.cpp:56,59` | `audio_out.main`, `audio_in.main` | speaker and microphone for voice | role lookups `main` |
| `components/wisp_app/src/loopback.cpp:36-37` | `audio_in.main`, `audio_out.main` | microphone test | role lookups `main` |
| `components/wisp_app/src/system_duties.cpp:47-48` | `display.main`, `power.main` | screen and power-off | role lookups `main` |
| `system_duties.cpp:182` | `button.power` (event source) | which key is the power key | id resolved once from role `power` |
| `components/wisp_app/src/power_manager.cpp:73` | `button.power` (event source) | the same | id resolved once from role `power` |
| `system_duties.cpp:205,207` | `button.power.short`, `button.power.long` (topics) | the power key's gestures | `<power key id>` + `.short`/`.long` |
| `components/wisp_app/src/self_check_screen.cpp:62-64` | `display.main`, `power.main`, `clock.main` | readouts | role lookups `main` |
| `self_check_screen.cpp:246,499,501` | `storage.sd` | the removable card row | role lookup `Storage` `removable` |
| `components/wisp_app/src/self_check_readouts.cpp:146,148` | `storage.int`, `storage.sd` | row labels | nothing: use the medium's own `removable` flag |
| `components/wisp_core/src/button_service.cpp:305` | `button.power` | the one key polled in standby | id resolved once from role `power` (injected) |
| `components/wisp_core/include/wisp/core/sensor_service.hpp:87` (used at `sensor_service.cpp:155,175`) | `imu.main` | the occupancy key | injected: the id of the `Imu` it was given |
| `components/wisp_core/src/storage_manager.cpp:23-24` | `storage.int`, `storage.sd` | internal and removable media | role lookups `Storage` `internal` / `removable` |
| `simulator/main.cpp:215,418,801` | `clock.main`, `display.main` | host-side tools | role lookups `main` |

That is 11 files under `components/` plus `simulator/main.cpp` (the roadmap counted 13 including the header/source pair of `SensorService`). The ESP port hard-codes the same ids inside the devices: `esp_display.cpp:77`, `esp_touch.cpp:15`, `esp_power.cpp:28`, `esp_clock.cpp:28`, `esp_imu.cpp:31`, `esp_audio.cpp:135,252`, `esp_storage.cpp:124,154`, `esp_wifi.hpp:64`. The boards repeat them again for the boot report (`boards/waveshare_esp32_s3_touch_amoled_2_16/board.cpp:197,242,298,321,337,580-591`), so a board cannot choose an id without the port and the board disagreeing.

Roles already exist for one case: a button declares caps `role: "talk"` (`components/wisp_hal/include/wisp/hal/button.hpp:22-24`) and `shell::find_talk_button()` scans for it (`components/wisp_shell/src/conversation.cpp:547-556`). This is why the 1.75C board could call its BOOT key `button.boot` without breaking voice (`docs/hardware/waveshare_esp32_s3_touch_amoled_1_75c.md:48`). The design generalises this.

#### 1.1.3 No generic way to reach a device

- `hal::Device` has `info()` and `set_power()` only (`device.hpp:92-102`). Everything else is a typed interface that some core service drives.
- The agent's device tools are `device.status`, `settings.get/set`, `notify.list/dismiss` (`components/wisp_agent/src/tools_device.cpp:64-187`); none of them reaches a device. Scenes get screen geometry and status (`components/wisp_scene/src/bind_device.cpp:68-73`) and IMU samples through `SensorService` (`bind_sensor.cpp:48-60`). The admin console lists devices read-only (`components/wisp_admin/src/routes_device.cpp:27-56`).
- So a new simple peripheral needs a new HAL interface, a core service, wiring in `wisp_app`, an agent tool and a Lua binding before the agent can use it.

#### 1.1.4 Chip adapters live in the port

- `EspPower` (AXP2101), `EspClock` (PCF85063) and `EspImu` (QMI8658) are in `components/wisp_port_esp/dev/` and include the chip drivers' headers (`include/wisp/port/esp/esp_power.hpp:14`, `esp_clock.hpp:15`, `esp_imu.hpp:17`). `EspPmuPowerKey` wraps `EspPower` and includes `axp2101_conv.h` (`esp_button.hpp:6,53`).
- Therefore `wisp_port_esp` requires `axp2101 pcf85063 qmi8658` (`components/wisp_port_esp/CMakeLists.txt:46`): the platform layer depends on board-level chips, against AGENTS.md section 3 ("Board independence"). A board with a different PMU has to edit the port.
- `EspAudioIn` hard-codes the two physical channels of this board's ES7210 (`esp_audio.cpp:272-273`; rationale in `include/wisp/port/esp/audio_downmix.hpp:3-10`), another board fact in the port.

#### 1.1.5 The two boards are mostly a copy

- `boards/waveshare_esp32_s3_touch_amoled_1_75c/board.cpp` (520 lines) and `boards/waveshare_esp32_s3_touch_amoled_2_16/board.cpp` (628 lines) share 371 identical lines (323 excluding blank and comment lines; measured with `diff --unchanged-line-format`). Identical or near-identical functions: `err_name`, `uses_qspi`, `record_failed`, `add_device`, `init_display` (differs only in `shape`/`corner_radius`), `init_touch`, `get_i2c_bus`, `init_power`, `hold_outputs_during_light_sleep`, `init_audio_device`, `init_audio`, `init_internal_storage`, `init_wifi`, `sync_system_clock_from_rtc`, and the bmgr-failure block that records every id by hand (`2_16/board.cpp:578-594`, `1_75c/board.cpp:480-493`). The real differences: the RTC probe (`1_75c/board.cpp:266-273`), the IMU address probe and axis map (`:294-305`, `:88`), the power-key fallback (`:313-344`), the SD card (2.16 only), and the panel shape.
- Pins are written twice. The 2.16's `board.cpp:149-158` holds display reset 39, touch reset 40, PA enable 46 and display CS 12, which are also in `board_devices.yaml:96,103,128` and `board_peripherals.yaml:71`; the comment there says to "keep these four in sync". `setup_device.c` overrides the touch reset/interrupt pins that bmgr has already copied from the YAML (`2_16/setup_device.c:78-81`, `1_75c/setup_device.c:103-106`). Button pins exist only in C++ (`2_16/board.cpp:132-134`). The SD mount point is in both `board.cpp:95` and `board_devices.yaml:69`.

#### 1.1.6 Target-bound configuration

- `firmware/sdkconfig.defaults` is "shared by every board" (`:1-6`) but sets `CONFIG_ESP32S3_INSTRUCTION_CACHE_32KB` (`:16`) and `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240` (`:9`), which are S3 options. It also assumes PSRAM: `CONFIG_SPIRAM_TRY_ALLOCATE_WIFI_LWIP` (`:119`), `CONFIG_MBEDTLS_EXTERNAL_MEM_ALLOC` (`:164`), `CONFIG_MDNS_MEMORY_ALLOC_SPIRAM` (`:135`).
- `firmware/main/main.cpp:76` fixes the DFS minimum at 80 MHz.
- The boards write `config.model = "ESP32-S3"` and `bands = {"2.4GHz"}` for Wi-Fi (`2_16/board.cpp:529-530`): facts of the SoC, not of the board.
- What is already chosen by target: the LVGL blend routines (`firmware/CMakeLists.txt:75-81`, `components/wisp_port_esp/CMakeLists.txt:67-72`) and the stack placement policy (`include/wisp/port/esp/stack_placement.hpp:19-34`).

#### 1.1.7 Assumptions about the hardware shape

- `Mem::Large` maps to `MALLOC_CAP_SPIRAM` with no fallback (`components/wisp_port_esp/os/os_esp.cpp:97-98`), so without PSRAM every large allocation returns null. The LVGL heap falls back to internal RAM itself (`components/wisp_ui/src/lv_mem_wisp.cpp:98-101`), the Lua allocator does not (`components/wisp_scene/src/vm.cpp:666`). `docs/porting-guide.md` ("Scene runtime") claims both fall back, which is wrong for Lua. (The owner's ruling makes 8 MB of PSRAM a requirement instead of designing a fallback, section 8.3.)
- The UI accepts only RGB565 (`components/wisp_ui/src/ui_runtime.cpp:11`, `components/wisp_ui/config/lv_conf.h:53`) and registers only a pointer input device (`ui_runtime.cpp:619-626`); a board with keys but no touch screen cannot move focus.
- The agent is built only when a UI, a system layer and a scene runtime exist (`app.cpp:583-589`); voice only with the agent (`app.cpp:591`). Confirmation already degrades: with no confirmation service the agent refuses (`components/wisp_agent/src/agent_service.cpp:284-286`). But `AgentWiring` also needs the `UiRuntime` for the UI tool executor, the `UiInspector` and the `ConfirmService` (`components/wisp_app/src/agent_wiring.cpp:431-448`), and the scene runtime it needs is built only over a `UiRuntime` (`components/wisp_app/src/scene_wiring.cpp:136-150`). A `ConfirmPort` alone would not make a headless agent possible. (Out of scope by the owner's ruling: a device without a display is not a supported configuration, section 1.4.)
- `AudioIn` is mono-only (`components/wisp_hal/include/wisp/hal/audio.hpp:30-38`); the port downmixes (`audio_downmix.hpp:28-34`).

### 1.2 Goals

1. **The next peripheral is cheap.** A simple peripheral (haptics, LED, a sensor read) needs a driver and one board line; the agent and scenes can use it with no change in `wisp_hal` enums, the port, core services, `wisp_app` wiring, the agent's tool list or the Lua bindings.
2. **The next board is cheap.** A third S3 board with known chips is a declarative `BoardSpec` plus the bmgr YAML, with pins written once.
3. **The next SoC is additive.** Everything target-specific is in places chosen by `IDF_TARGET`; adding a target adds files and changes no portable code.
4. **Type safety without RTTI.** A typed lookup can only return an object of that interface.
5. **No regression.** Both boards behave as today at every migration step; `WISP_MANIFEST` stays single-line JSON with only additive changes.

### 1.3 Decisions taken before this spec

From the roadmap, section H (binding unless the code shows otherwise; section 18 lists where this spec changes them):

- interface tags separate from `DeviceClass`, which stays a display category;
- roles instead of fixed ids (`registry.by_role<T>("main")`);
- `describe()` / `get(prop)` / `invoke(cmd, Value)` on `hal::Device`, with an agent tool `device.call` and a Lua binding `wisp.device.call`;
- chip adapters move next to their drivers; the port no longer requires chip drivers;
- a board kit driven by a declarative `BoardSpec`, pins from the bmgr YAML only;
- target-specific sdkconfig, a support matrix (amended at approval: PSRAM of 8 MB or more is required, no fallback is designed);
- LVGL keypad/encoder groups when the board has such devices; pixel format in display caps;
- ~~a `ConfirmPort` so the agent and voice start without a display~~ (removed at approval, section 1.4);
- channel roles for `AudioIn` in caps.

Owner: `device.call` needs no confirmation for any self-describing device.

### 1.4 Non-goals

- New scene capability modules, the `BindingModule` registry and the install-time permission prompt (phase I). This phase adds one Lua binding (section 4.3) in the existing style.
- The composition-root refactor (`ServiceStack`, `Platform` split, moving `PowerManager`; phase L).
- Power optimisations (phase F). Devices keep the existing `standby` caps contract.
- Writing the S31 port, bumping ESP-IDF, or new rendering acceleration. This phase only prepares the places those changes will go (section 8) and lists the facts they need (section 17.1).
- E-paper or monochrome panels, an encoder interface, multi-channel capture or echo cancellation (listed as follow-ups in sections 9 and 11).
- Hot-plugging devices at run time. `DeviceRegistry::add/remove` keep working, but no service re-resolves roles after boot.
- A plugin system that discovers drivers at run time. Boards name their drivers in code and CMake (section 6.3).
- A device without a display. It is not a supported configuration for now: the agent and voice keep requiring the UI (`app.cpp:583-591`). The existing behaviour stays: without a main display the boot report records headless mode and the device only degrades. No `ConfirmPort`, no optional UI dependencies in `AgentWiring`, no UI-free tool groups (owner's ruling at approval, section 20).
- Running without PSRAM, or with less than 8 MB. PSRAM of 8 MB or more is required; no fallback is designed (section 8.3).

---

## 2. Device identity: interfaces, classes and roles

### 2.1 Interface identity comes from the type

```cpp
namespace wisp::hal {

// One value per typed interface a core service drives. A device that only
// speaks the generic path (section 3) reports None.
enum class Interface : uint8_t {
    None, Display, Touch, Imu, Power, Clock, Button, AudioOut, AudioIn, Storage, Wifi,
};
const char* to_string(Interface i);

class Device {
public:
    virtual ~Device() = default;
    virtual const DeviceInfo& info() const = 0;
    virtual Interface interface() const { return Interface::None; }
    virtual Result<void> set_power(PowerLevel level);
    // Generic path, section 3.
    virtual const DeviceSchema* describe() const { return nullptr; }
    virtual Result<Value> get(std::string_view prop);
    virtual Result<Value> invoke(std::string_view cmd, const Map& args);
};

class Display : public Device {
public:
    static constexpr Interface kInterface = Interface::Display;
    static constexpr DeviceClass kClass = DeviceClass::Display;
    Interface interface() const final { return kInterface; }
    ...
};
}
```

- `DeviceRegistry::get<T>(id)`, `list<T>()` and `by_role<T>(role)` check `device->interface() == T::kInterface` before the `static_pointer_cast`.
- `interface()` is `final` in each interface class, so a concrete device cannot report another interface than the one it inherits. That makes the cast sound without RTTI, which a data field (as `info().cls` is today) cannot guarantee.
- `DeviceRegistry::add()` also rejects (`InvalidArgument`, recorded by the caller as a failed device) a device whose `info().cls` does not match its interface's class (`Wifi` ↔ `Network`, the others by name), or whose interface is `None` and whose class is not `Other`. This catches mislabelling at boot instead of at a cast.

> In the code: `interface()` is not virtual; `hal::Device` stores the tag, set through a constructor only the interface classes reach, so no subclass can claim an interface it does not implement (`wisp/hal/device.hpp`). `add()` also rejects a typed device that describes commands, a `Display` with any schema, and a schema that fails `hal::check_schema()`.

**One typed interface per device object.** A chip with two functions registers two device objects that share the driver handle, as `EspPmuPowerKey` already does for the AXP2101's power key next to `EspPower` (`esp_button.hpp:53,62`), and as audio already does with separate `EspAudioOut`/`EspAudioIn` objects over one codec bus. Alternative considered: an `as(Interface)` method returning a pointer per interface, so one object can implement several. Rejected: implementing two interfaces that both derive from `Device` needs virtual inheritance or a hand-written subobject table, and no current or planned chip needs it; the generic path (section 3) already lets any typed device expose extra read-only properties.

### 2.2 What `DeviceClass` is for afterwards

`DeviceClass` stays the display category shown in `WISP_MANIFEST` (`"class"`), `WISP_BOOT_REPORT` and the admin device list (`routes_device.cpp:36`), the prefix of ids, the simulator's `--disable <class>` (`components/wisp_port_sim/sim_board.cpp:28-38`), and `PowerManager`'s audio ordering (`power_manager.cpp:446-450`). It gains one value, `Other` (`"other"`), for every generic-only device. The id prefix carries the kind of an `Other` device (`haptic.main`, `led.status`), so adding a vibration motor changes no enum.

Alternative considered: turn `DeviceInfo::cls` into a free string so the manifest could say `"class": "haptic"`. Rejected: it touches the boot report, the admin page, the self-check grouping and the simulator for no behaviour gain; the id already says what the device is.

### 2.3 Roles

A **role** says what a device is for, so portable code can ask for "the main display" without knowing the board's id.

- **Where it is declared:** caps `role`, a string (or a list of strings, for a device that serves two purposes, such as a single key that is both the power key and the talk key on a minimal board). The board sets it (through `BoardSpec`, section 7); the device stores it in its caps, as buttons do today (`components/wisp_port_sim/dev/sim_button.cpp:13`).
- **How code asks:** `registry.by_role<hal::Display>("main")` returns the device whose interface is `Display` and whose role includes `main`, or null. `hal::has_role(const DeviceInfo&, std::string_view)` parses the caps.
- **Uniqueness:** `DeviceRegistry::add()` rejects a second device with the same (interface, role) pair (`AlreadyExists`); the board kit records it as a failed device with reason `role main already taken by display.main`. With uniqueness enforced at registration, `by_role()` never has to choose among several candidates; with zero candidates it returns null, exactly as `get<T>("display.main")` does today, and every caller already handles null (for example `app.cpp:353`, `power_monitor.hpp:70`).
- **Generic devices** may have roles too (`led.status` with role `status`), scoped to interface `None`.

> In the code: for interface `None` the uniqueness key is (kind, role), the kind being the id's prefix before the dot, so `haptic.main` and `led.main` can both hold `main`; a generic device's id must match `^[a-z][a-z0-9_]*\.[a-z0-9_]+$` and be at most 32 bytes. At boot `App` logs a warning for each of Display, Touch, Power, Clock, Imu, AudioOut, AudioIn and Wifi that has devices but no holder of role `main`, and for a key that holds both `power` and `talk`: such a key has no gesture arbitration yet, so holding it to talk also triggers the power key's long press.

> In the code: a Button's id must be `button.<name>` (same character rule and 32-byte limit as a generic id); `add()` refuses any other (`InvalidArgument`, "<id>: a button id is button.<name>"), because `ButtonService` publishes `<id>.<edge>` and the system's key handlers subscribe to `button.`, so a key named otherwise would be heard by no one.

Alternative considered: keep fixed ids and make every board use them. Rejected: ids are also event topics (`button_service.cpp:448-452`, `button.power.short`) and the user-visible names in the manifest; a board should name a key after its label (`button.boot`), and the caller only cares about the purpose.

**Role vocabulary** (closed list in `wisp/hal/roles.hpp` as constants; a board may use other names for devices only it or the generic path cares about):

| Interface | Role | Used by |
|---|---|---|
| Display | `main` | `UiRuntime`, self-check, system duties, simulator tools |
| Touch | `main` | `UiRuntime` (the pointer over the main display) |
| AudioOut | `main` | `TonePlayer`, `PcmPlayer`, loopback |
| AudioIn | `main` | `VoiceService`, loopback |
| Power | `main` | `PowerMonitor`, net card, self-check, power-off |
| Clock | `main` | RTC sync at boot and after SNTP |
| Imu | `main` | `SensorService` |
| Wifi | `main` | `WifiService` |
| Storage | `internal`, `removable` | `StorageManager`, self-check |
| Button | `power` | `ButtonService` standby polling, `PowerManager`, system duties |
| Button | `talk` | `TalkKey` (existing) |
| Button | `nav_prev`, `nav_next`, `nav_ok`, `nav_back` | LVGL keypad (section 9.1) |

The volume keys get no role: no portable code reads them (no use outside the port and the simulator).

### 2.4 Migration of every hard-coded id

Each row of the table in 1.1.2 becomes the replacement listed there. Three kinds:

1. **Role lookup** at the place that builds the service (`app.cpp`, `net_wiring.cpp`, `voice_wiring.cpp`, `loopback.cpp`, `system_duties.cpp`, `self_check_screen.cpp`, `simulator/main.cpp`, `StorageManager::init`).
2. **Injected id** where a service compares event sources or keys occupancy: `ButtonService` takes the power key's id (resolved once from role `power`; buttons are registered by the board before `ButtonService` starts, `app.cpp:149-161`); `PowerManager` and `SystemDuties` get the same id; `SensorService` uses `imu->info().id` of the device it is given instead of `kDevice`.

   > In the code: `ButtonService` marks the key whose caps hold role `power` when it refreshes its buttons (no constructor argument); `PowerManager` gets the id through `PowerManagerDeps::power_key`; `SystemDuties` resolves it at construction and compares the topics `<id>.short`/`<id>.long`.

3. **Nothing**: `self_check_readouts.cpp:146-148` switches on the medium's `removable` flag that `StorageManager` already reports.

   > In the code: `StorageManager::MediaStatus` has `bool removable` (true for the medium found by role `removable`). The topics `storage.sd.inserted`/`storage.sd.removed` stay fixed system topic names; their `source` is `StorageManager`'s component name (`kStorageManagerSource`, `"storage_manager"`), since the SD cell's mirror publishes them and boards choose device ids.

The port's devices stop choosing ids: every `Esp*` constructor takes `id` and `role` from its config struct (`EspDisplayConfig`, `EspTouchConfig`, ... gain two fields), filled by the board kit with today's ids as defaults. `shell::find_talk_button()` becomes `by_role<hal::Button>("talk")`.

> In the code (H1): the ESP devices keep today's ids and add caps `role`; `EspPmuPowerKey` takes a `role`, and the boards give their power keys role `power`. Ids and roles move into the board configs with H2's board kit.

A CI grep guard (in `tools/ci.sh`) fails on a literal `"<class>.main"`, `"net.wifi"`, `"button.power"`, `"storage.int"` or `"storage.sd"` in `components/wisp_*/` other than the port and the simulator board, so the literals do not come back.

> In the code: the guard is in `tools/check_layering.py` (run by `tools/ci.sh`, comments stripped, with a self-test). It rejects `"<kind>.main"`, `"net.wifi"`, `"button.<name>"`, `"storage.int"` and `"storage.sd"` in the portable components and in `simulator/*.cpp`; ports, boards, drivers and tests may name ids.

> In the code (H2): the ESP devices take `id` and `role` from their configs (`EspDisplayConfig` ... `EspSdStorageConfig`; `EspInternalStorage::create(id, role)` and `EspGpioButton` as arguments), filled from the board kit's `Slot`s, so the port holds no id and the guard now covers `wisp_port_esp` too (plan H2 deviation 9); only the simulator port, boards, drivers and tests may name ids.

---

## 3. The generic access path

### 3.1 Purpose and boundary

The typed interfaces stay the way core services use devices: `UiRuntime` flushes a `Display`, `VoiceService` reads an `AudioIn`. The generic path is for devices that no service knows: it lets a driver describe properties and commands, and lets the agent and scenes use them through one service. It is not a second way to drive a display, a microphone or the radio.

### 3.2 Schema

A driver declares its schema as a constant table; the table is the only source. `describe()` returns it; `DeviceRegistry::manifest()` and `device.list` render it under caps key `schema`, so drivers never write that key by hand (the registry rejects a device whose caps already contain `schema`).

```cpp
namespace wisp::hal {

enum class ParamType : uint8_t { Bool, Int, Number, String, Enum };

struct ParamSpec {
    const char* name;                 // ^[a-z][a-z0-9_]{0,31}$
    ParamType type;
    const char* desc = "";            // English, one line, for the agent
    double min = 0, max = 0;          // Int/Number, inclusive; both 0: no range
    const char* unit = "";            // "ms", "%", "°C", "lx", ...
    std::span<const char* const> choices = {};   // Enum
    uint16_t max_len = 64;            // String, bytes of UTF-8
    bool optional = false;            // args only
    Value def = {};                   // default for an optional arg
};

struct CmdSpec {
    const char* name;                 // ^[a-z][a-z0-9_]{0,31}$
    const char* desc;
    std::span<const ParamSpec> args;  // at most 8
    bool changes_state = true;        // false: a pure query that needs arguments
    uint32_t min_interval_ms = 0;     // calls closer together are refused
};

struct DeviceSchema {
    std::span<const ParamSpec> props; // read-only, at most 16
    std::span<const CmdSpec> cmds;    // at most 16
};

Value render_schema(const DeviceSchema&);                       // caps "schema"
Result<Map> validate_args(const CmdSpec&, const Value& args);   // fills defaults
}
```

Rendered form (lists, so the declared order survives; `Map` is ordered by key):

```json
"schema": {
  "props": [{"name": "active", "type": "bool", "desc": "Motor running"}],
  "cmds": [{"name": "pulse", "desc": "Vibrate once", "changes_state": true, "min_interval_ms": 100,
            "args": [{"name": "ms", "type": "int", "min": 10, "max": 2000, "unit": "ms"},
                     {"name": "strength", "type": "int", "min": 0, "max": 100, "unit": "%",
                      "optional": true, "default": 80}]}]
}
```

Decisions:

- **Properties are read-only; every change is a command.** Alternative considered: writable properties with a `set(prop, value)`. Rejected: it gives two ways to make the same change and splits the "changes state" and rate-limit flags across two places. A writable setting of a device is a command (`set_color`).
- **The table, not caps, is the source.** Alternative considered: drivers write the schema straight into caps as a `Value`, as the roadmap phrased it ("in caps"). Rejected: validation would have to parse a `Value` tree on every call, and a typo in a hand-built map would only show at run time. The table is checked by a unit test per driver (names, ranges, counts) and is rendered into caps for everything that reads the manifest, so the outside view is unchanged.
- **Five types are enough** for the peripherals in view (haptics, LEDs, buzzers, relays, light/temperature sensors). Lists and nested objects are not supported: a command that needs a list is a sign it should be a typed interface.

> In the code: every schema type is a literal type and drivers declare their tables `constexpr` (they live in flash); a default is a `hal::ParamDefault` scalar (`.def = 80`), not a `Value`, and its kind must match the argument's type (Int: Int; Number: Int or Number; Bool: Bool; String and Enum: String). `validate_args()` fills a missing optional argument with a `Value` of the default's own kind. `hal::check_schema()` also refuses names that are Lua reserved words (scenes pass arguments as a table constructor) and a String parameter whose `max_len` exceeds `hal::kMaxStringParamBytes` (256).

### 3.3 Calls, threading and errors

- `get(prop)` returns the property's current value; `invoke(cmd, args)` runs a command with arguments already validated and defaulted, and returns a small `Value` (a `Map`, or null).
- **Threads:** callable from any task except the UI thread and ISRs. Devices are internally thread-safe (foundation spec 5.3); the service does not serialise calls. A `Display` never declares a schema, since every `Display` call belongs to the UI thread (`display.hpp:35-45`). Drivers must not touch flash (AGENTS.md section 5); they get no `Settings` access.
- **Blocking:** a call returns within 50 ms (`hal::kDeviceCallBudgetMs`); a bus transaction of a few milliseconds is fine. An action that lasts longer (a 2 s vibration, a fade) is started by the call and finished by the driver itself (an `os::Timer`), so the caller is never held. `DeviceService` measures each call and logs `WLOG_W` once per (device, command) when the budget is exceeded.
- **Errors** (`Result<Value>`, existing `ErrorCode`s): `NotFound` (no such device, property or command), `InvalidArgument` (validation; the message names the argument and the rule), `PermissionDenied` (section 3.4), `RateLimited` (section 3.5), `Busy` (another holder has the device exclusively, or the device is powered down for standby), `DeviceFailure` (bus error), `Unsupported` (default implementations).
- **Power levels:** the generic path never wakes a device. A device that declares caps `standby: "off"` is switched by `PowerManager` like any other (`power_manager.cpp:438-466`), and a call while it is off answers `Busy`.

> In the code: `DeviceService` follows `PowerManager`'s level cell (`set_power_level()`, subscribed by `App`) and refuses reads and calls on a `standby: "off"` device at `Standby` and `DeepStandby`; the cell's delivery arrives after `PowerManager` has switched the devices, so this check is advisory and the driver itself refuses (`Busy`) while off.

### 3.4 What is deliberately not reachable

The service reaches only what a schema declares, and adds one rule: **commands are allowed only on devices whose interface is `None`.** A typed device may declare read-only properties (an IMU's die temperature, a PMU's input current), never commands. With that rule:

| Not reachable | Why |
|---|---|
| Display flush, brightness, panel power | UI thread only (`display.hpp:35-45`); brightness is a setting with its own range and UI (`settings.set`) |
| Touch points | Owned by the LVGL input device on the UI thread |
| Raw audio streams, gain, volume | Privacy, and arbitration with voice and tones through occupancy; scene audio is phase I's `wisp.audio`; volume is a setting |
| Wi-Fi connect, scan, credentials, AP | Secrets (AGENTS.md section 6) and `WifiService`'s state machine; the agent already sees the network in `device.status` |
| Storage | `StorageManager` and the flash-thread rule |
| `power_off`, `set_power` | `PowerManager` owns power levels and the user-facing power flows |
| Button edge sinks | `ButtonService` owns the ISR path |

Alternative considered: a per-device allow-list in the service. Rejected: the interface rule says the same thing in one line and cannot drift.

### 3.5 Protection from physical harm without confirmation

The owner ruled that `device.call` asks no confirmation. What still protects the hardware:

1. **Ranges in the schema** are enforced before the driver is called (`validate_args`). A driver must also clamp in `invoke()` itself, since board code and tests can call it directly.
2. **`min_interval_ms`** per command, enforced by `DeviceService` across all callers: a call closer than that to the previous accepted one returns `RateLimited` with `retry_after_ms` in the error message. A driver sets it for commands that must not be repeated quickly (a solenoid, a buzzer, a relay).
3. **Bounded actions**: a driver guideline (in `docs/porting-guide.md`) that an actuator which can overheat or drain the battery has no "on forever" command; it takes a duration with a maximum, and the driver stops it.
4. **Standby**: such devices declare `standby: "off"`, so `PowerManager` switches them off with the screen.
5. **Safe state on construction and in `set_power(Standby)`**: required of every actuator driver.

State-changing calls are logged (`WLOG_I "device <id>.<cmd> by <holder>"`), so the serial log and the admin log stream show what the agent did.

### 3.6 `core::DeviceService`

The service layer rule (AGENTS.md section 3) says the agent tool and the Lua binding are thin adapters over one service API. New in `wisp_core`:

```cpp
namespace wisp::core {
class DeviceService {
public:
    DeviceService(DeviceRegistry& registry, OccupancyRegistry* occupancy,
                  std::function<uint64_t()> now_ms = os::now_ms);
    // Devices that describe themselves: id, class, model, role, schema.
    List list() const;
    Result<Value> get(const std::string& id, const std::string& prop) const;
    // holder: "agent" or "scene.<id>", for occupancy and the log line.
    Result<Value> call(const std::string& id, const std::string& cmd, const Value& args,
                       const std::string& holder);
};
}
```

`call()` does, in order: find the device; find the command in `describe()`; refuse commands on typed devices; `validate_args`; check `min_interval_ms` (one small table guarded by a mutex); check occupancy (an exclusive holder other than `holder` → `Busy`; nobody takes generic devices exclusively yet, phase I may); `invoke`; measure; log. `App` constructs it next to `SensorService` with no UI dependency.

> In the code: `call()` checks, in order, device (and id length), command, typed device (`PermissionDenied`), `validate_args`, occupancy (`Busy`), standby (`Busy`), then `min_interval_ms`, checked and stamped in one critical section right before `invoke()`, so a call refused for another reason does not start the interval and two racing calls cannot both pass; a call that reaches `invoke()` is stamped whatever it returns. The rate table has 32 fixed slots; when it is full of running intervals a new rate-limited command is refused (`RateLimited`, `retry_after_ms` = the soonest any ends) instead of evicting one. A result whose JSON exceeds 1 KiB becomes `DeviceFailure` ("result too large") here, for both adapters. `for_each()` hands out `list()`'s entries one at a time.

---

## 4. The agent tools and the Lua binding

### 4.1 `device.list`

- Read-only, `ToolRisk::Normal`, `ToolThread::Agent`.
- Returns `{"devices": [{"id", "class", "model", "role", "schema"}]}` for every device with a schema, at most 32. Typed devices without a schema are left out; the agent already sees their state in `device.status` (`tools_device.cpp:64-80`).

> In the code: the list is rendered one device at a time and stops at 32 devices or 64 KiB of JSON, whole entries only, adding `"truncated": true` when it was cut.

> In the code: `device.list` and `device.call` are registered only when `DeviceService` lists at least one device when the agent is built (`AgentWiring`); the registry does not change after boot (section 1.4), so a board's tool list is fixed, and on a board without such a device the request carries neither tool. `wisp.device.*` is the same on every board.

Alternative considered: one tool `device.call` with an `op: "list"`. Rejected: the argument schema would have to allow `id`/`name` to be missing for one op and required for another, which the validator's JSON Schema subset (`components/wisp_agent/src/args_schema.cpp`) cannot express; two small tools are clearer to the model. The roadmap's "one agent tool" therefore becomes two (section 18).

### 4.2 `device.call`

```json
{"type": "object",
 "properties": {
   "id":   {"type": "string", "maxLength": 32},
   "op":   {"type": "string", "enum": ["get", "invoke"]},
   "name": {"type": "string", "maxLength": 32},
   "args": {"type": "object"}},
 "required": ["id", "op", "name"]}
```

- `ToolRisk::Note`, `ToolThread::Agent` (devices do not touch flash), default 5 s timeout.
- `get` returns `{"value": ...}`; `invoke` returns `{"result": ...}`. A result whose JSON exceeds 1 KiB becomes the error `result_too_large` (drivers return small values by contract).
- Errors map to `tool_error(<snake_case ErrorCode>, message)`, the existing convention (`tools_device.cpp:183`).
- When the command `changes_state`, the result carries a localised note (new `StrId::AgentNoteDevice`, "Device {id}: {cmd}", zh/ja/en), as `settings.set` does (`tools_device.cpp:130-134`). No confirmation, per the owner's ruling.
- Holder `"agent"`.

> In the code: errors go through the agent's existing `detail::tool_code()` mapping like every other tool: `not_found`, `invalid_value`, `rate_limited`, `busy`, `device_failure`, and outside the tool description `denied` (a typed device's command; unreachable in practice, since `add()` refuses a typed device that describes commands) and whatever a driver returns outside its contract (`unsupported`, `timeout`, `quota_exceeded`, `unavailable`, else `failed`). An oversized result is `device_failure` (section 3.6), not `result_too_large`. The note names only the device id and the command.

### 4.3 `wisp.device.list/get/call` in Lua

Added to the existing `wisp.device` table (`components/wisp_scene/src/bind_device.cpp`), following the binding style of `bind_sensor.cpp:42-60`:

- `wisp.device.list()` → array of `{id, class, model, role, schema}` tables;
- `wisp.device.get(id, prop)` → value, or `nil, code`;
- `wisp.device.call(id, cmd, args)` → result, or `nil, code` (`code` is the snake_case `ErrorCode`, as `wisp.sensor` returns today).

Calls are synchronous on `wisp.scene` (the 50 ms budget keeps the scene task responsive). Holder `"scene.<id>"`, or `"agent"` in the agent's `lua.run`.

**Who may call:** the agent's own `lua.run` context always (owner's autonomy ruling, as the roadmap's phase I also assumes). A scene only when its `scene.json` declares the device: `"permissions": {"device": ["haptic.main"]}` (at most 8 ids, parsed by `core::SceneManifest` next to `http_hosts`, `components/wisp_core/include/wisp/core/scene_manifest.hpp:70`). An undeclared id answers `permission_denied`. The install-time prompt for this permission belongs to phase I's general mechanism (open question 17.2-3).

> In the code: `wisp.device.get` needs the same `permissions.device` entry as `call`, and `permission_denied` is decided before the service is reached; `list()` needs none and returns at most 32 devices, with nothing signalling a cut. A command whose result is null returns `true`. `rate_limited` reaches a scene as the code alone (no `retry_after_ms`). A call's arguments are capped at 16 KiB of JSON and nesting depth 2, which every argument set a schema allows fits. Background and headless scenes may call declared devices.

### 4.4 Admin console

No new REST endpoint in this phase. `GET /api/v1/devices` already returns caps (`routes_device.cpp:31-40`), which now include `role` and `schema`, so the web page can show them. A `POST` to call a command can be added later as another thin adapter over `DeviceService`.

> In the code: `GET /api/v1/devices` renders caps with `hal::caps_of()`, as the manifest does, so `schema` is included. The web page shows class `other` with a translated name; caps (role, schema) render generically.

---

## 5. Worked example: a haptic motor on a new board

Say a board adds a TI DRV2605L haptic driver on the shared I2C bus. Files written:

1. **`drivers/drv2605/`**, a new component:
   - `drv2605.c`, `include/drv2605.h`: the plain-C chip driver (init, play effect, real-time value), as the existing drivers are (`drivers/axp2101/CMakeLists.txt:10-14`).
   - `wisp/drv2605_haptic.cpp`, `include/wisp/drivers/drv2605_haptic.hpp`: the adapter, `class Drv2605Haptic : public hal::Device` with interface `None`, class `Other`, caps `standby: "off"`, and a schema with `pulse(ms 10..2000, strength 0..100 = 80)` (`min_interval_ms` 100), `effect(id 1..123)`, and property `active`. `pulse` starts the motor and arms an `os::Timer` to stop it; `set_power(Standby)` stops it.
   - `CMakeLists.txt`: `REQUIRES esp_driver_i2c wisp_hal wisp_os`.
   - A host unit test of the schema table (names, ranges) under `tests/drivers/`.
2. **`boards/<board>/board.cmake`**: append `drv2605` to `WISP_BOARD_REQUIRES` (section 6.3).
3. **`boards/<board>/board.cpp`**: one line in the board's chip bring-up:
   `kit.add({"haptic.main", hal::DeviceClass::Other, "main"}, wisp::drivers::Drv2605Haptic::create(kit.i2c("i2c_master")));`

No change in: `hal::DeviceClass`/`Interface` (the device is `Other`/`None`), `wisp_port_esp`, any core service, `wisp_app` wiring, the agent's tool list or the Lua bindings. What the user gets: the device in `WISP_MANIFEST` and the admin device list with its schema; `device.list` shows it to the agent; `device.call {"id": "haptic.main", "op": "invoke", "name": "pulse", "args": {"ms": 150}}` vibrates; a scene that declares the device calls `wisp.device.call("haptic.main", "pulse", {ms = 150})`; the motor stops when the screen goes off.

The simulator gets a `SimHaptic` (`haptic.main`, model `sim-haptic`, logging `sim: haptic pulse 150 ms 80%`) so this path is tested end to end on the host (section 14).

An RGB LED would be the same shape: a driver over the port's generic LEDC or RMT helper, a schema `set_color(r, g, b)`, `off()`, properties `r, g, b`.

---

## 6. Driver adapters out of the port

### 6.1 Where adapters go

`EspPower`, `EspPmuPowerKey`, `EspClock` and `EspImu` move into their driver components as `drivers/axp2101/wisp/axp2101_power.cpp` (with the power key), `drivers/pcf85063/wisp/pcf85063_clock.cpp`, `drivers/qmi8658/wisp/qmi8658_imu.cpp`, with headers `include/wisp/drivers/<chip>_<role>.hpp` and namespace `wisp::drivers`. Each driver component then `REQUIRES wisp_hal wisp_os` in addition to its IDF dependencies; the dependency points down the layers, so no layering rule changes. The C driver stays usable on its own.

> In the code: each driver also requires `wisp_core` (for `WLOG_*`), and `axp2101` requires `wisp_port_esp` for `kButtonStandbyPollMs` (`wisp/port/esp/button_input.hpp`), the standby read interval its power key shares with `EspGpioButton`; the dependency points from the driver to the port, and `tools/check_layering.py` rejects the reverse (plan H2 deviation 3). The adapter configs default `model` to the chip's name, so boards stop repeating it (deviation 5).

Alternatives considered:

- **A separate component per adapter** (`drivers/axp2101_wisp/`). Rejected: twice the directories for no isolation gain; ESP-IDF's component discovery only scans direct subdirectories of `EXTRA_COMPONENT_DIRS` (`firmware/CMakeLists.txt:26-30`), so a nested adapter component would need extra CMake.
- **A factory registry keyed by chip name** (`make_device("AXP2101", bus, config)`). Rejected: per-chip configs differ (address probing, axis maps, `kImuMount` at `1_75c/board.cpp:88`), so a string-keyed factory needs a `Value` config and loses type checking; self-registration through static constructors interacts badly with `--gc-sections` (the port already needs `WHOLE_ARCHIVE` for less, `components/wisp_port_esp/CMakeLists.txt:52-55`). Board code calling `Qmi8658Imu::create(bus, config)` is shorter and checked by the compiler.

### 6.2 What `wisp_port_esp` keeps

Everything that is SoC- or IDF-generic: `os`, logging, NVS settings, console, Wi-Fi, HTTP, SNTP, OTA, `mem`/`sleep` reports, the JPEG decoder and font partition; the bus-level device wrappers that work for any chip behind an IDF driver (`EspDisplay` over `esp_lcd`, `EspTouch` over `esp_lcd_touch`, `EspAudioOut`/`EspAudioIn` over `esp_codec_dev`, `EspGpioButton`, `EspInternalStorage`/`EspSdStorage`); and small generic helpers the kit and drivers share (`gpio_hold_in_light_sleep(pins)`, `configure_input(pin, active_low)`). `IDF_REQUIRES` loses `axp2101 pcf85063 qmi8658`. `tools/check_layering.py` gains a rule: `wisp_port_esp`'s requirements must not name a component under `drivers/`.

The `EspAudioIn` channel count becomes configuration (section 11). The Wi-Fi model and bands become target facts in the port (`EspWifi` defaults from `CONFIG_IDF_TARGET` and `SOC_WIFI_SUPPORT_5G`), so boards stop writing `"ESP32-S3"`.

> In the code: no GPIO helpers were added to the port (plan H2 deviation 4): bmgr configures the button inputs from their `gpio` peripherals, and the kit is the only code that holds outputs through light sleep, so that loop stays in `boards/common/board_kit.cpp`. The target facts are `kTargetFacts` in `wisp/port/esp/target_facts.hpp` (DFS minimum, Wi-Fi model); `target.hpp` fails the build for a target without a row, and the 5 GHz band follows `SOC_WIFI_SUPPORT_5G`.

### 6.3 How a board selects drivers

`boards/<board>/board.cmake` sets `WISP_BOARD_REQUIRES` (for the two current boards: `axp2101 pcf85063 qmi8658`). `firmware/main/CMakeLists.txt` includes it and appends the list to its `REQUIRES` (it compiles `board.cpp` today, `firmware/main/CMakeLists.txt:7-9`). A `.cmake` file in the board directory is not compiled by bmgr's `SRC_DIRS` scan (AGENTS.md section 8 concerns `.c`/`.cpp`).

> In the code: ESP-IDF expands component requirements in a separate CMake run that sees build properties but no cache variables, so `firmware/main` cannot include `boards/${WISP_BOARD}/board.cmake` (`WISP_BOARD` is empty there). `firmware/CMakeLists.txt` includes it before `project()` and publishes the list with `idf_build_set_property(WISP_BOARD_REQUIRES ...)`; `firmware/main/CMakeLists.txt` reads it with `idf_build_get_property()` (plan H2 deviation 1).

---

## 7. The board kit

### 7.1 Location

The shared bring-up code goes in `boards/common/` (`board_kit.hpp`, `board_kit.cpp`) and is compiled into `firmware/main` with the board's `board.cpp`. It is ESP- and bmgr-specific, so it is not part of the port (the port must not depend on bmgr device types or chip drivers) and not a portable layer.

To verify first in the plan: that `idf.py bmgr -c ../boards` ignores a subdirectory without `board_info.yaml`. If it does not, the kit goes to `firmware/main/board_kit/` instead; nothing else in this design changes.

### 7.2 `BoardSpec`

```cpp
namespace wisp::board_kit {

struct Slot { const char* id; hal::DeviceClass cls; const char* role; };

struct DisplaySpec {           // bmgr device "display_lcd"
    Slot slot{"display.main", hal::DeviceClass::Display, "main"};
    const char* bmgr = "display_lcd";
    const char* model;         // caps facts bmgr does not carry
    const char* shape;         // "rect" | "rounded_rect" | "circle"
    int corner_radius = 0;
    const char* panel;
    int area_align = 1;
    port::esp::EspDisplayConfig::PanelSleep sleep;
    uint32_t sleep_out_settle_ms = 0;
    // draw_buf_lines/draw_buf_mem/render_mode as in EspDisplayConfig
};
struct TouchSpec    { Slot slot{"touch.main", ...}; const char* bmgr = "lcd_touch"; const char* model; };
struct AudioOutSpec { Slot slot{"audio_out.main", ...}; const char* bmgr = "audio_dac"; const char* model; };
struct AudioInSpec  { Slot slot{"audio_in.main", ...}; const char* bmgr = "audio_adc"; const char* model;
                      std::span<const char* const> channels; };          // section 11
struct SdSpec       { Slot slot{"storage.sd", ..., "removable"}; const char* bmgr = "fs_sdcard"; const char* model; };
struct ButtonSpec   { Slot slot; const char* bmgr_gpio; bool active_low; const char* model;
                      hal::ButtonInputMode mode = hal::ButtonInputMode::EdgeEvent; };

struct BoardSpec {
    const char* name;
    std::optional<DisplaySpec> display;
    std::optional<TouchSpec> touch;
    std::optional<AudioOutSpec> audio_out;
    std::optional<AudioInSpec> audio_in;
    std::optional<SdSpec> sd;
    std::span<const ButtonSpec> buttons;
    std::span<const Slot> chips;   // devices the board brings up itself, for the failure records
    // Called after display and touch, before buttons and audio (today's order).
    void (*bring_up_chips)(Kit&) = nullptr;
};

class Kit {   // what bring_up_chips() and board.cpp use
public:
    i2c_master_bus_handle_t i2c(const char* bmgr_periph);
    void add(const Slot&, Result<std::shared_ptr<hal::Device>> device);  // registers, sets role, or records failure
    void missing(const Slot&, std::string reason);
    void failed(const Slot&, std::string reason);
    core::DeviceRegistry& registry();
};

void init(const BoardSpec&, core::DeviceRegistry&, core::BootReport&);
}
```

`init()` runs today's sequence, which both boards share: internal storage before `esp_board_manager_init()`; on a bmgr failure, every declared slot is recorded as failed with the bmgr error and Wi-Fi still starts (`2_16/board.cpp:568-594`); display, touch, `bring_up_chips`, RTC sync, buttons, audio, SD card, holding outputs through light sleep, Wi-Fi last (`:596-616`, rationale in the header comment `:35-40`). Failure reporting is the kit's `add()`/`failed()`/`missing()`; a failed device is recorded and not registered, a missing one recorded `Missing`, and nothing aborts (AGENTS.md section 3 "Device model").

Afterwards the 2.16's `board.cpp` is a `BoardSpec` constant plus a `bring_up_chips()` of three calls (PMU, RTC, IMU), about 60 lines; the 1.75C's adds its RTC probe, IMU address probe and power-key fallback, about 120 lines.

> In the code: `boards/common/board_kit.hpp` also offers `make_board(spec)`, the `app::Board` a board's `create_board()` returns. `DisplaySpec` has no draw-buffer fields, since neither board sets them; the first board that needs them adds the fields, which map one to one onto `EspDisplayConfig` (plan H2 deviation 6). `ButtonSpec` carries `model` (default `"gpio-button"`) and an optional `fallback`, a function the board supplies that the kit registers once when the GPIO path is unavailable (the peripheral failed, or the edge service cannot be installed); it replaces the 1.75C's hand-written PMU power-key fallback, and the 1.75C's boot-time GPIO3 warning is dropped, its electrical checks being tracked in its hardware document (deviation 7, owner decision).

### 7.3 Pins: the YAML is the only source

- **Display reset and CS, touch reset**: read from the bmgr device configs the kit already fetches (`dev_display_lcd_config_t`, `dev_lcd_touch_config_t`; the boards already read fields of both, `2_16/board.cpp:213-224,266-268`). The kit computes the "hold during light sleep" list from them; `kPinDisplayReset/TouchReset/DisplayCs` disappear.
- **PA enable**: read from the audio DAC's PA peripheral (`gpio_pa_control`, `board_peripherals.yaml:67-73`).
- **Buttons**: declared as bmgr `gpio` peripherals (input, with pull) named `btn_power`, `btn_boot`, ...; `ButtonSpec::bmgr_gpio` names one, and the kit reads its pin from bmgr. bmgr configures the pin, replacing the boards' `gpio_config` calls (`2_16/board.cpp:368-377`).
- **`setup_device.c`** stops overriding `rst_gpio_num`/`int_gpio_num`, which bmgr has already copied from the YAML (`2_16/setup_device.c:78-81`, `1_75c/setup_device.c:103-106`).
- **SD mount point**: read from the `fs_sdcard` config instead of `kSdMountPoint` (`2_16/board.cpp:95`).

To verify first in the plan: the bmgr call that returns a peripheral's config (the boards use only `esp_board_periph_get_handle` and `esp_board_manager_get_device_config` today, `2_16/board.cpp:201-207,288,456`). If bmgr exposes no peripheral config for `gpio`, the fallback is: buttons and the PA pin stay as constants, but only in the `BoardSpec` (one place), and the duplicated hold-list constants still go away because the display and touch pins come from device configs.

Panel facts that bmgr does not carry (model string, shape, measured corner radius, area alignment, DCS sleep) stay in `BoardSpec`: they are Wisp caps, not wiring.

> In the code: the bmgr facts were confirmed on a real run before the move (`esp_board_manager_get_periph_config()` returns a `gpio` peripheral's `periph_gpio_config_t`), so no fallback was needed. Touch `levels` and `flags` (`swap_xy`, `mirror_x`, `mirror_y`) moved into the YAML as well, since the same `setup_device.c` factories overrode them; both touch factories are now plain pass-throughs and `setup_device.c` holds only the panel init tables (plan H2 deviation 8, owner decision; device check: touch follows the finger in all four corners).

### 7.4 What a board directory contains afterwards

`board_info.yaml`, `board_peripherals.yaml`, `board_devices.yaml` (bmgr, with buttons as `gpio` peripherals); `setup_device.c` only for custom bmgr factories (panel init tables); `board.cpp` (`BoardSpec` and chip bring-up); `board.cmake` (driver list); `sdkconfig.defaults.board` (flash, PSRAM, console, partition table choice); optionally `partitions.csv` when no shared table fits (section 8.2); `README.md`.

---

## 8. Target readiness

### 8.1 sdkconfig by target

ESP-IDF loads `sdkconfig.defaults.<IDF_TARGET>` after `sdkconfig.defaults`. Move the S3-only options (`CONFIG_ESP32S3_INSTRUCTION_CACHE_32KB`, `CONFIG_ESP_DEFAULT_CPU_FREQ_MHZ_240`) to `firmware/sdkconfig.defaults.esp32s3`. A new target adds `firmware/sdkconfig.defaults.<target>` and nothing else in the shared file. The DFS minimum in `firmware/main/main.cpp:76` moves next to the maximum as a per-target constant in the port (chosen by `CONFIG_IDF_TARGET_*`).

To verify first in the plan: that the target-specific file is also picked up when `SDKCONFIG_DEFAULTS` is passed explicitly, as the multi-board build does (`docs/porting-guide.md`, "Building several boards"). If not, that command lists `sdkconfig.defaults.<target>` explicitly.

PSRAM-dependent options stay in the shared file: every supported board has PSRAM of 8 MB or more (section 8.4).

### 8.2 Partition tables by flash size

Shared tables in `boards/common/partitions/`: `16mb.csv` (the 2.16's table, byte for byte) and `32mb.csv` (the 1.75C's). Both boards point `CONFIG_PARTITION_TABLE_CUSTOM_FILENAME` at them; their devices see no change. A shared table is frozen once a board ships with it (the rule in `docs/porting-guide.md`, "Rules for changing the partition table"): a change becomes a new file, never an edit. A board with special needs keeps its own `partitions.csv`.

Alternative considered: keep one copy per board. Rejected only mildly: it works, but a third 16 MB board would copy the 2.16's table and the conventions (names, the 16 MB cache limit) would drift one copy at a time.

### 8.3 PSRAM: 8 MB or more, required

Owner's ruling at approval: PSRAM of at least 8 MB is a requirement of every supported board. No fallback is designed: `Mem::Large` keeps mapping to `MALLOC_CAP_SPIRAM` (`os_esp.cpp:97-98`) and the Lua allocator keeps using `Mem::Large` only (`vm.cpp:666,879,889`).

At boot the ESP platform reads the PSRAM size it found (`heap_caps_get_total_size(MALLOC_CAP_SPIRAM)`). With less than 8 MB, or none, `App` records the boot-report note `"psram": "<N>MB"` (`"none"` without PSRAM) and logs one `WLOG_E`; with 8 MB or more there is no note. The device then keeps booting, so the serial log and the boot report stay readable; nothing else is promised to work, and the build does not try to work around it.

> In the code: the size is `esp_psram_get_size()`, the detected chip size (0 when PSRAM did not initialise), passed to `App` as `Platform::psram_bytes` (-1, unmeasured, in the simulator). `heap_caps_get_total_size(MALLOC_CAP_SPIRAM)` is the PSRAM heap, about 5 MB on an 8 MB board, so it would have flagged both boards (plan H2 deviation 2). N is the size in whole MiB, truncated; the threshold is `kMinPsramBytes` in `wisp/app/ram_budget.hpp`.

`docs/porting-guide.md` ("Scene runtime") states the requirement instead of its wrong claim that the Lua heap falls back.

### 8.4 Support matrix (for `docs/porting-guide.md`)

| Feature | Requirement | Evidence |
|---|---|---|
| Boot, self-check, serial console | an ESP-IDF target within the pinned range (`firmware/main/idf_component.yml`) | — |
| Admin console, OTA | Wi-Fi; flash with two OTA slots of at least 1.25× the image (image 3.6 MB on 2026-11-12, `docs/hardware/waveshare_esp32_s3_touch_amoled_2_16.md:26`) | 80 % slot budget |
| Large text, CJK body text | `fonts` (2.4 MB) and `glyphs` (1.1 MB) partitions; PSRAM for the glyph cache | porting guide, partition table |
| Recommended flash | 16 MB | both boards |
| PSRAM | 8 MB or more, required (section 8.3) | both boards have 8 MB (idle free 6.76 MB, `docs/hardware/waveshare_esp32_s3_touch_amoled_2_16.md:72`) |
| UI | a `Display` with `pixel_format: rgb565` and a `Touch` or keypad roles (section 9) | — |
| Voice | `AudioIn` 16 kHz mono, `AudioOut` mono at the TTS rate (porting guide, "Voice and the talk key") | — |
| Internal RAM | the budget in `wisp/app/ram_budget.hpp` (worst case 40 KB free / 16 KB block) | — |

### 8.5 Everything target-specific, in one list

After this phase a new target touches only: `firmware/sdkconfig.defaults.<target>`; `components/wisp_port_esp/lv_blend/<target>/` (optional; LVGL's C path otherwise, `firmware/CMakeLists.txt:69-81`); `stack_placement.hpp` (only if the target's flash/PSRAM rules differ, `stack_placement.hpp:7-18`); the per-target DFS minimum and Wi-Fi bands in the port; the IDF version range (a separate, project-wide change); `docs/porting-guide.md` "Chip differences". No portable file names a chip (AGENTS.md section 3, unchanged).

---

## 9. Input and display generality

### 9.1 Keypad navigation for system UI

When the registry has buttons with roles `nav_prev`, `nav_next`, `nav_ok` (and optionally `nav_back`), `UiRuntime` registers an LVGL keypad input device and a default focus group, beside the pointer device (`ui_runtime.cpp:619-626`). `ButtonService` already turns presses into events (`button_service.cpp:448-452`); a small adapter in `wisp_app` maps the `down`/`up` events of the nav-role buttons to `UiRuntime::push_key(key, pressed)`, a thread-safe ring of 8 entries drained by the keypad read callback on the UI thread. LVGL calls stay on the UI thread (AGENTS.md section 3).

Scope in this phase, system UI only:

- `ui::SystemLayer` dialogs add their buttons to the group and focus the safe answer (Deny, Cancel), so confirmation, factory reset and firmware confirmation work without touch;
- `nav_back` performs the shell's back action (the same as the back gesture);
- `nav_next`/`nav_prev` move focus; LVGL scrolls focused objects into view, which keeps the first and last rows of the settings list reachable.

Not in scope: focus order on every shell page (phase M's `PageStack` owns pages), scene key input (phase I's `wisp.input.on("key")`), the on-screen keyboard.

> In the code (how H3 differs from the wording above):
>
> 1. The focus group is not LVGL's default group. With `lv_group_set_default()` LVGL adds every button, switch and textarea of every screen to it when created, and `lv_group_focus_next()` does not skip objects on screens that are not loaded. `UiRuntime` creates one group for the keypad only, and `SystemLayer` fills it with what is in front (the focus scope), rebuilding it when that changes.
> 2. Any nav role creates the keypad. `nav_prev` and `nav_back` are optional; `nav_next` and `nav_ok` are needed to move focus and to answer (the group wraps, so `nav_next` alone reaches every item). `nav_back` alone works, since it needs no focus. `App` logs a warning at boot when a nav key exists without `nav_next` or `nav_ok`. The keypad indev is created before the pointer, so the pointer stays first in `lv_indev_get_next(nullptr)` (LVGL inserts at the list head).
> 3. The ring of 8 takes one key at a time: LVGL's keypad handles one key, and a second key's press would count as the first one still pressing. A press is accepted only while no nav key is down and two slots are free (the press and its release), so a release always fits; the release of a key that is not down is ignored.
> 4. Keys are refused while the UI is paused. Any button already wakes the screen; without this the waking press would also act on whatever is focused after the resume. After a pause, the keypad's LVGL state (including `keypad.last_state`) is reset on the next keypad read, not in the resume branch, so a key held across standby neither clicks nor repeats.
> 5. On a board with touch the first key only shows the focus. LVGL sets `LV_STATE_FOCUS_KEY` only when the focusing input device is a keypad, and a focus set in code is attributed to the pointer when there is one. Whenever the focused object lacks `LV_STATE_FOCUS_KEY`, a Next/Prev/OK press only adds it (no move, no click). On a keys-only board a focus set in code is already shown. Code sets a focus only on a dialog's safe answer and the error card's Back when they open, and gives back a screen focus a key had shown when the piece over it closes; anything else (the screen, the panel) is focused only by a key, because focusing scrolls a list to the object and tells it (a textarea brings up the keyboard, a scene hears it). So a screen reached by touch stays unfocused, and keeps its scroll position, when a dialog, the error card or the panel over it closes; the next key focuses the first enabled object and only shows it.
> 6. `nav_back` acts on the press and does, first that applies: answers an open confirm dialog "no"; the error card's Back; hides the keyboard; closes the panel; calls the back handler (scene or diagnostics); otherwise `Shell::back()` (Wi-Fi password form or detail, choice page, sub-page to Home). On the home screen it does nothing. The back gesture alone would not cover shell pages, which go back by page drags.
> 7. Settings rows are plain clickable objects, not LVGL widget classes, so `shell::detail::make_list_row()` (`page_frame.cpp`) marks them `ui::kKeyFocusable` (`ui::make_key_focusable()`), and `collect_focusable()` descends into such rows so a control inside one (a network row's info button) stays reachable. Key focus brings the focused object's whole row into `hal::row_span()` (`ui::show_focus`, `kFocusRow` on list rows and row boxes); focusing the group's first or last object scrolls the list to its top or bottom, so the ends stay fully visible. A key held while focus moves (a dialog opens or closes by code) is dropped with `lv_indev_wait_release`: it acts on neither object. Other shell pages stay with phase M's `PageStack`.
> 8. The focus scope is the active screen, whatever it is, so the widgets of a scene in front are reachable like any other; scenes get no key events (phase I).
>
> The simulator has the keys with `--nav-keys` (Page Up = prev, Page Down or Tab = next, Return or keypad Enter = OK, Escape or Backspace = back).

**Encoder**: deferred. No current board has one. When one does, it gets a typed interface `hal::Encoder` (`read_delta()`, `pressed()`) and an LVGL encoder input device on the same group; the design above does not change.

### 9.2 Pixel format

`Display` caps gain `pixel_format` (`"rgb565"`; absent means `rgb565`, so current boards need no change). `rgb565_be` keeps its meaning. `UiRuntime::start()` fails with `Unsupported` for any other format; `App` then takes the existing headless path (`app.cpp:367-371`) and records the note `"display": "unsupported_pixel_format"`. `lv_conf.h` keeps `LV_COLOR_DEPTH 16`.

> In the code: a `pixel_format` that is present but is not the string `"rgb565"` (another string, a number) is unsupported; absent is `rgb565`. The boot report keeps `"mode": "headless"` beside `"display": "unsupported_pixel_format"`.

What an e-paper or monochrome panel would still need (out of scope): an LVGL colour depth or a conversion in `flush()` to 1/2/4-bit; a full-refresh render mode (the reserved `render_mode: "full"`, `docs/porting-guide.md` capability table); refresh pacing and ghosting control (partial versus full updates); a theme without animations or grey levels; standby semantics for a panel that keeps its image unpowered; separate `WISP_PERF` baselines.

---

## 10. ~~Headless agent and `ConfirmPort`~~ (removed at approval)

Removed by the owner's ruling (section 20, answer 4): a device without a display is not a supported configuration for now, so this phase adds no `ConfirmPort`, keeps `AgentWiring`'s UI dependencies required and registers no UI-free tool groups. The agent and voice keep requiring the UI; without a main display the boot report records headless mode and the device only degrades, as today. Section number kept so later references stay valid.

---

## 11. Audio capture channel roles

`AudioIn` caps gain `channels`: a list with one role per physical channel, `"mic"` or `"ref"` (the speaker signal looped back for echo cancellation). Both boards declare `["mic", "mic"]` (two microphones, `docs/hardware/waveshare_esp32_s3_touch_amoled_1_75c.md:41`). `EspAudioIn` takes the list from `AudioInSpec` instead of hard-coding two channels (`esp_audio.cpp:272-273`) and averages the `mic` channels, skipping `ref` ones; `downmix_stereo_to_mono` becomes a general `downmix(frames, roles)` with the same rounding, tested on the host. The `AudioIn::read()` contract stays mono 16-bit; echo cancellation and multi-channel reads are a follow-up for phase K's voice protocols.

---

## 12. Interface summary

New or changed, by component:

- `wisp_hal`: `Interface`, `Device::interface()/describe()/get()/invoke()`, `kInterface` and `final interface()` on every interface, `DeviceClass::Other`, `ParamSpec/CmdSpec/DeviceSchema`, `render_schema`, `validate_args`, `has_role`, `wisp/hal/roles.hpp`, caps keys `role`, `pixel_format` (Display), `channels` (AudioIn).
- `wisp_core`: `DeviceRegistry::by_role<T>()`, interface-checked `get/list`, the `add()` checks (class/interface match, role uniqueness, no hand-written `schema`), `manifest()` renders `schema`; `DeviceService`; `ButtonService`, `SensorService`, `StorageManager` take ids or roles instead of literals; `SceneManifest` parses `permissions.device`.
- `wisp_agent`: `device.list`, `device.call`.
- `wisp_scene`: `wisp.device.list/get/call`.
- `wisp_ui`: keypad input device, `push_key()`, default group, `pixel_format` check; `SystemLayer` dialogs join the group.
- `wisp_app`: role lookups, `DeviceService` construction, nav-key adapter, `psram` (below 8 MB) and `display` notes.
- `wisp_port_esp`: ids/roles from configs, `AudioIn` channel config, Wi-Fi model/bands by target, the PSRAM size check, DFS minimum by target, generic GPIO helpers; chip adapters removed.
- `wisp_port_sim`: roles on `SimBoard`'s devices, `SimHaptic`.
- `drivers/*`: adapters; `boards/common/`: the kit and partition tables; `boards/*`: `BoardSpec`, `board.cmake`, YAML button peripherals.

`WISP_MANIFEST` changes, all additive: caps `role` on every device that has one (today only the talk key), caps `schema` on self-describing devices, `"class": "other"` for generic devices, `pixel_format`/`channels` caps where the boards declare them. `WISP_BOOT_REPORT` gains the notes `psram` and `display`. Top-level structure and field names are unchanged.

---

## 13. Migration and plan split

The phase is too large for one plan. Three plans, each leaving both boards working and each useful on its own:

**Plan H1: device identity and the generic path** (host-testable; no device behaviour change except the new tools).
1. `Interface`, `interface()`, interface-checked `get/list`; `DeviceClass::Other`; `add()` checks. Existing devices already satisfy them.
2. Roles: `has_role`, `by_role`, uniqueness; the simulator board and the ESP port (ids/roles from configs, with today's ids as defaults; boards pass the same values) declare roles.
3. Migrate every literal in the table of 1.1.2, file by file; the CI grep guard last.
4. Schema types, `validate_args`, `render_schema`; `DeviceService`; `SimHaptic`.
5. `device.list`, `device.call`; `wisp.device.list/get/call` with `permissions.device`.
Delivers: no fixed ids in portable code; generic devices usable by the agent and scenes (in the simulator at once, on hardware once a board adds one).

> In the code: H1 did not move the ESP ids into configs (step 2): the ESP devices keep today's ids and gain caps `role`; that move is H2's, with the board kit's slots.

**Plan H2: board kit, driver adapters, target readiness** (ESP; device checks on both boards). Depends on H1 (ids and roles come from configs).
1. Adapters into `drivers/*`, `board.cmake`, the port drops chip requirements, the layering rule.
2. `boards/common/` kit and `BoardSpec`; the 2.16 first, then the 1.75C; bmgr verifications of 7.1 and 7.3 come first.
3. Pins from YAML; `setup_device.c` overrides removed.
4. `sdkconfig.defaults.esp32s3`, DFS minimum and Wi-Fi facts by target, shared partition tables.
5. The PSRAM size check and the `psram` note (section 8.3); the porting guide states the 8 MB requirement.
6. `AudioIn` channel roles.
7. Porting guide: support matrix, new board steps, actuator guideline.
Delivers: a third S3 board is a `BoardSpec`; the port is board-independent; the S31 port has defined places to go.

> In the code: H2 is done (plan `docs/superpowers/plans/2026-11-18-device-model-2-board-kit.md`, which records its deviations): drivers carry their adapters, both boards are `BoardSpec`s on the board kit with pins only in the YAML, S3 options are in `firmware/sdkconfig.defaults.esp32s3`, the target facts in `target_facts.hpp`, the partition tables shared by flash size, the boot report records a PSRAM below 8 MB, and `AudioIn` declares `channels`. The device checks of section 14.4 are the owner's to run (`docs/backlog.md`). Step 7's actuator guideline already matched section 3.5 from H1.

**Plan H3: keypad input for system UI and the pixel-format cap** (host-testable, plus device checks). Depends on H1 only; can run in parallel with H2.
1. `pixel_format` caps and check.
2. Keypad input device, `push_key()`, default group, dialogs, back; simulator keys for the nav roles.
Delivers: boards with keys and no touch are supported for system UI. (The headless agent was removed at approval, section 10.)

The simulator board is updated in H1 (roles, `SimHaptic`) and H3 (nav keys on the keyboard).

---

## 14. Testing

Following AGENTS.md section 5 (GoogleTest, TDD, clean output) and `docs/testing.md`.

### 14.1 Unit tests (host)

- Registry: `get<T>`/`list<T>`/`by_role<T>` return only matching interfaces (a fake `Other` device with id `display.x` is never returned as a `Display`); `add()` rejects class/interface mismatch, a duplicate (interface, role), a hand-written `schema` cap; role as string and as list.
- Schema: `validate_args` for each type, range edges (inclusive), missing required, unknown argument, defaults filled, integral doubles accepted as `Int`, `max_len` in bytes; `render_schema` output shape.
- `DeviceService`: commands refused on typed devices; `min_interval_ms` with a fake clock; exclusive occupancy → `Busy`; result size; log on budget overrun.
- Agent tools: `device.list` filtering and limit; `device.call` argument schema, error mapping, the note only for `changes_state`.
- Lua: `wisp.device.call` success, `nil, code` on each error, `permission_denied` for an undeclared id, holder names.
- Migration: `StorageManager`, `ButtonService`, `SensorService`, `PowerManager` tests use boards whose ids are not the old literals (for example `button.side` with role `power`), proving no literal remains.
- Keypad: `push_key` ordering and overflow; a dialog's default focus.
- Port headers that are pure (downmix by roles, target fact tables) under `tests/port_esp/`, as today.
- Driver schema tables under `tests/drivers/` (names, counts, ranges).

> In the code: `AgentWiring::tools()` and `App::agent_tools()` (const pointers to the agent's `ToolRegistry`, null without an agent) let tests check the tool order and the end-to-end path through the real wiring; `Platform::device_clock_ms` lets them move `DeviceService`'s clock.

### 14.2 Simulator and smoke tests

- `tests/smoke/sim_smoke.py`: the expected device list gains `haptic.main`; the manifest is still one line of JSON; caps `role` present on the `main` devices; `schema` present on `haptic.main`.

### 14.3 Static checks

- `tools/check_layering.py`: the port must not require `drivers/` components.
- `tools/ci.sh`: the id-literal grep guard (section 2.4).

### 14.4 Device checks (the owner runs these; never reported as verified)

On both boards, after H2:
1. `WISP_BOOT_REPORT`/`WISP_MANIFEST`: same devices, ids, models and caps as before, plus `role`; no new failure.
2. Hardware diagnostics: every device green; touch, button and microphone tests pass.
3. Voice: hold the talk key, speak, hear the answer (the talk key is still found by role).
4. Standby and wake: screen off and on with the power key; the power key still wakes from deep standby (role-based polling in `ButtonService`).
5. Light sleep: the held outputs (now derived from YAML) still hold: no panel re-init, no speaker pop after 10 minutes of standby (2.16 checklist item 29).
6. `mem` idle and worst case (AGENTS.md section 5): unchanged against `ram_budget.hpp`.
7. `WISP_PERF` `bench scroll 10`: frame rate at the baseline (no rendering change is intended).
8. OTA upload accepted and confirmed (the partition table file moved but is byte-identical).
9. 1.75C: RTC still reported `missing`, IMU orientation unchanged, power key on GPIO3.

After H3, on a board with keys (or the 2.16 with nav roles assigned to its volume keys for the test): dialogs answerable by keys; back works.

> In the code: no board change is committed for this. The nav-role assignment on the 2.16's volume keys is a local, uncommitted patch to `boards/waveshare_esp32_s3_touch_amoled_2_16/board.cpp` for the check, so H2 (which moves ids and roles into `BoardSpec`) has nothing to merge. A nav key pressed in deep standby is expected to wake the device; that depends on `ButtonService`'s standby polling reporting nav keys.

---

## 15. Documentation

In the same commits as the code (roadmap "Global Constraints"):

- AGENTS.md section 3 "Device model": ids are names, roles are how code finds devices; one typed interface per device object; the generic path and the commands-only-on-generic-devices rule; the port must not require chip drivers. Section 5: drivers carry their Wisp adapters.
- `docs/architecture.md`: device identity, `DeviceService`, the manifest fields.
- `docs/porting-guide.md`: the kit and `BoardSpec`; buttons as bmgr `gpio` peripherals; `board.cmake`; shared partition tables; the support matrix; the actuator guideline (3.5); the 8 MB PSRAM requirement in place of the wrong Lua fallback statement; the target list of 8.5.
- `docs/hardware/*.md`: the manifest lines with `role`.
- Foundation spec section 5: a note pointing to this spec for identity and roles.

---

## 16. What the next boards cost

Honest estimates after this phase, excluding the hardware documentation and acceptance, which dominate and do not shrink:

- **A third S3 board with the same chip families** (CO5300-class panel over QSPI, CST-class touch, ES8311/ES7210, AXP2101, QMI8658): three YAML files (adapted from the vendor's or ESP-Claw's), `setup_device.c` only if the panel needs an init table, `sdkconfig.defaults.board` (about 15 lines), `board.cmake` (one line), `board.cpp` (60 to 120 lines: a `BoardSpec` and any probing). Today the same board needs about 550 lines of `board.cpp`, of which some 370 are copied.
- **A third S3 board with one new chip** (another PMU, say): the above plus `drivers/<chip>/` (the C driver and a 100 to 200 line adapter, with host tests of its conversions). No port change.
- **A first board on the new SoC**: the above, plus the target work of 8.5, which needs the facts in 17.1 first. The largest known item is outside this phase: moving the whole project to the ESP-IDF version the SoC needs (the porting guide's unverified note says ≥ 6.1, beyond the pinned `>=6.0.3,<6.1`), with its own plan and regression of both S3 boards. Then: `sdkconfig.defaults.<target>`; frame-rate baseline on LVGL's C path (the S3 SIMD routines do not apply); internal-RAM budget and stack placement measured; light-sleep and button-wake checks (the GPIO wake path uses RTC IO, `docs/hardware/waveshare_esp32_s3_touch_amoled_1_75c.md:135`); bmgr support for the target and board. A plan for it can be sized only after 17.1 is answered.

---

## 17. Risks and open questions

### 17.1 Facts needed from the owner about "ESP32-S31"

The project's notes on this chip are marked unverified (`docs/porting-guide.md`, "Planned target"); this spec assumes nothing beyond "a new ESP32-family SoC". Needed before its port plan:

1. The ESP-IDF target name (the `idf.py set-target` argument) and the first ESP-IDF release that supports it; whether that release is stable.
2. CPU: architecture, number of cores, maximum and minimum DFS frequencies (for the per-target DFS constants, `main.cpp:75-76`).
3. Internal SRAM size, and how much ESP-IDF and Wi-Fi leave free (for `ram_budget.hpp`).
4. PSRAM on the target board: size, type and speed; whether DMA can read it (`SOC_PSRAM_DMA_CAPABLE`, for `draw_buf_mem: "large"`); whether task stacks in PSRAM are allowed during flash operations (for `stack_placement.hpp`).
5. Flash on the target board: size, vendor/ID, and whether ESP-IDF supports suspend for it (for XIP and auto-suspend, `docs/porting-guide.md` "Font partitions").
6. Instruction/data cache size options for the target (the S3 sets 32 KB, `firmware/sdkconfig.defaults:16`).
7. Radio: on-chip Wi-Fi or a co-processor (esp-hosted would change `EspWifi`); bands (2.4 or 2.4 + 5 GHz); SoftAP support (the setup hotspot).
8. Sleep: light-sleep support with PSRAM, wake sources, which GPIOs can wake (RTC IO or equivalent).
9. Console: USB-Serial-JTAG or UART on the target board.
10. The first target board: name (bmgr/`esp_boards` naming), its display interface (QSPI, MIPI-DSI, RGB), touch, audio codec, PMU, buttons, SD.
11. Versions of esp_board_manager, esp_lcd drivers, LVGL, littlefs and mdns that support the target.

### 17.2 Other open questions, with recommendations

1. **Two agent tools instead of one** (`device.list` + `device.call`, section 4.1). Recommendation: accept.
2. **Commands only on generic devices** (section 3.4). This forbids, for example, a PMU "charge current" command. Recommendation: accept; a typed device that needs a control gets it through its service and a setting, with a UI.
3. **Scene access to `wisp.device.call`**: a scene must declare the device in `permissions.device` (enforced at call time from H1); the install-time prompt comes with phase I's general permission mechanism (same proposal as roadmap open decision 3). Between H1 and phase I a declared device is granted without a prompt. Recommendation: accept this window, since scenes are mostly written by the agent and the protections of section 3.5 apply to every caller. If the owner does not accept it, H1 exposes the binding only in the agent's `lua.run` and scenes get it with phase I.
4. ~~**Headless confirmation denies** (section 10.1).~~ Answered at approval: headless devices are not supported for now; section 10 removed.
5. ~~**`Mem::Large` falls back instead of refusing to boot** (section 8.3).~~ Answered at approval: PSRAM of 8 MB or more is required; no fallback (section 8.3 rewritten).
6. **Shared partition tables** (section 8.2). Recommendation: accept; low value but low cost.
7. **Encoder deferred** until a board has one (section 9.1). Recommendation: accept.

### 17.3 Risks

| Risk | Effect | Mitigation |
|---|---|---|
| bmgr cannot read peripheral configs or rejects `boards/common/` | YAML-only pins or the kit location fail | verified first in H2; fallbacks in 7.1 and 7.3 keep the rest of the design |
| A migrated lookup misses a role on one board | a service silently does nothing (null device) | the boot report already shows devices; tests boot boards with non-default ids; the grep guard; device check 1-4 |
| Light-sleep hold list derived from YAML differs from today's constants | panel re-init or speaker pop in standby | unit-compare the derived list with today's constants for both boards before deleting them; device check 5 |
| A driver blocks longer than the budget | the scene task or the agent stalls | budget warning log; actions run asynchronously in the driver; review checklist in the porting guide |
| The agent overuses an actuator | wear, noise, battery | ranges, `min_interval_ms`, bounded durations, standby off (section 3.5) |
| Manifest consumers assume a closed class list | an `"other"` class breaks a client | the only consumers are the smoke test and the admin page, both updated in H1 |
| `sdkconfig.defaults.<target>` not applied with explicit `SDKCONFIG_DEFAULTS` | S3 builds lose the 32 KB cache (fps drop) | verified in H2; `WISP_PERF` device check 7 |

---

## 18. Changes to the roadmap's decisions

| Roadmap (section H) | This spec | Why |
|---|---|---|
| Each interface has its own `kInterface` id | Also a `final` virtual `interface()`; one typed interface per object; `DeviceClass::Other` added | A data tag can be mislabelled; `Other` lets a new peripheral register without an enum change (1.1.1, 2.1, 2.2) |
| `by_role<T>("main")`; `*.main` literals in 13 files go | Adopted, plus role uniqueness at `add()`, roles for storage and the power key, injected ids, a grep guard; 11 files + the simulator | The literal list also contains `button.power` and the storage ids (1.1.2) |
| `describe()` in caps, `get`, `invoke`; one agent tool | Schema is a C++ table rendered into caps; properties read-only; commands only on generic devices; two tools (`device.list`, `device.call`) | Typed validation; one way to change state; a hard boundary for what the agent can reach; a clearer tool schema (3.2, 3.4, 4.1) |
| Adapters move to `drivers/<chip>/wisp_adapter.*` | `drivers/<chip>/wisp/` inside the same component, `EspPmuPowerKey` included; boards select drivers in `board.cmake`; a layering rule | The power key also depends on the AXP2101; `firmware/main` must require the board's drivers (6.1, 6.3) |
| `boards/common/` shared init; pins from the bmgr YAML only | Adopted, with two plan-time bmgr checks and fallbacks; buttons become bmgr `gpio` peripherals; `setup_device.c` overrides removed | The pins are duplicated in three places today (1.1.5) |
| Chip sdkconfig to `sdkconfig.defaults.<target>`; `Mem::Large` fallback with a boot note; support matrix | Adopted at the firmware level, plus DFS minimum and Wi-Fi facts by target and shared partition tables; the fallback replaced at approval by a required 8 MB of PSRAM with a boot-report note when less is found; porting-guide correction | Owner's ruling (section 20); the porting guide wrongly claims a Lua heap fallback (1.1.7) |
| Keypad/encoder group | Keypad via button nav roles for system UI; encoder deferred | No board has an encoder (9.1) |
| `ConfirmPort` lets the agent and voice start without a display | Removed at approval: a device without a display is not supported for now | Owner's ruling (section 20) |
| `AudioIn` channel roles in caps | Adopted; the port's hard-coded channel count becomes board configuration | A board fact sits in the port today (1.1.4) |

---

## 19. Acceptance

- No portable file (outside the port and the simulator board) contains a device-id literal; the grep guard passes.
- `get<T>`/`list<T>`/`by_role<T>` return only objects of interface `T`; the registry rejects mislabelled devices and duplicate roles.
- In the simulator, the agent can list `haptic.main` and pulse it through `device.call`; a scene that declares it can call it from Lua; an undeclared scene gets `permission_denied`; a typed device's commands are refused.
- `wisp_port_esp` requires no chip driver; `tools/check_layering.py` enforces it.
- Both boards build from `BoardSpec`, with pins only in YAML (or, under the 7.3 fallback, only in `BoardSpec`), and pass device checks 1-9 of section 14.4.
- `firmware/sdkconfig.defaults` names no chip; `sdkconfig.defaults.esp32s3` exists.
- A board with less than 8 MB of PSRAM (or none) keeps booting and records the `psram` note with an error log (device check optional; no fallback is designed).
- With nav roles on two simulator keys, a system dialog is answered without the pointer.
- `WISP_MANIFEST` and `WISP_BOOT_REPORT` remain single-line JSON; the smoke test and documentation describe the added fields.
- `bash tools/ci.sh` is green after each plan.

---

## 20. Approval

Approved by the owner on 2026-11-14. Answers to the open questions of section 17.2:

1. Two agent tools (`device.list`, `device.call`) instead of one: accepted.
2. Commands only for generic devices (interface `None`); typed devices such as the PMU are not reachable through the generic path: accepted.
3. A scene that declares `permissions.device` may call the device from H1 on, checked at call time; the install-time prompt comes with phase I; until then a declared permission is honoured: accepted.
4. Headless devices are not supported for now; the headless agent (`ConfirmPort`, optional UI dependencies, UI-free tool groups) is removed from this phase (sections 1.4 and 10).
5. PSRAM minimum 8 MB, required; no fallback (section 8.3).
6. Partition tables shared by flash size: accepted.
7. Encoder input deferred: accepted.

The facts about the new SoC (section 17.1) are still to be supplied by the owner; they are needed for H2 only.
