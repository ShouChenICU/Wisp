#pragma once

// Boot orchestration: wires every wisp_core service (and, when a display
// is present, wisp_ui) together behind one App object that both the PC
// simulator and the ESP firmware both construct from a
// Platform. See AGENTS.md's layering rule -- wisp_app sits above wisp_ui,
// so this header may include wisp_core/wisp_ui headers freely, but (like
// every portable layer) must never include platform headers.
//
// App::start() follows spec 6.1's boot sequence:
//   1. (Re)initializes logging (wisp::log::init()).
//   2. Calls Platform::board's init(), then fills in a
//      DeviceState::Ok report entry for every device that's now in the
//      registry but that init() didn't already record an entry for
//      (board-recorded entries, e.g. a Failed or Missing device, are left
//      untouched) -- see board.hpp.
//   3. Starts the EventBus, then StorageManager::init() (a failure here
//      sets the "storage" boot-report note to "readonly" and boot
//      continues read-only rather than failing), starts ButtonService and
//      the storage worker, and a periodic 2000ms timer that queues
//      StorageManager::refresh() on that worker.
//   4. If "display.main" is registered: applies the `display` settings
//      namespace's `brightness` key (default 200) to it, starts a
//      UiRuntime over it (plus "touch.main" if also registered).
//      Otherwise records the "mode": "headless" boot-report note.
//      Either way, constructs the PowerMonitor (spec 6.3/6.7)
//      and starts its own internal sample Timer, then constructs the
//      PowerManager (spec 6.7, handed a pointer to the
//      PowerMonitor just built) and starts its periodic idle-check
//      Timer, then -- if a UiRuntime was started -- builds the
//      self-check screen (see src/self_check.hpp's start_self_check(),
//      spec 8) on it, blocking until that has actually run. PowerManager
//      is constructed before the self-check screen specifically because
//      the screen's power-key handling calls into it (see
//      src/self_check_screen.cpp).
//   Scenes (scene-runtime-core spec; src/scene_wiring.hpp): right after
//      storage, the "wisp.fs" StorageWorker starts and the scene repository
//      recovers interrupted installs on it; with a screen, the scene
//      runtime (and, given Platform::http_client, the scenes' HTTP proxy)
//      is built before networking -- the admin routes reach it -- and
//      started once the self-check screen is up, the screen a scene
//      replaces and returns to.
//   4b. Networking (connectivity spec 4.1; src/net_wiring.hpp): builds
//      PairingService and -- when "net.wifi" is
//      registered -- WifiService, whose tick() runs on a "wisp.net" task
//      every 250 ms so boot never waits for Wi-Fi; applies
//      PowerManager::level() to WifiService::set_power_level() (deep
//      standby turns the radio off) and, with the scenes' HTTP proxy, runs
//      deep standby's network windows; and, given Platform::http_server,
//      builds the admin router and serves it on http_port while the
//      station has an IP or the setup hotspot is up (always, with
//      http_always_on); while the hotspot is up it also runs
//      Platform::captive_server on port 80. Given Platform::sntp, a
//      TimeSyncService sets the clock from the network while the station
//      has an IP (connectivity spec 5.6). Each successful admin server
//      start emits
//      "WISP_HTTP {"port":n}". Records the "net" boot-report note:
//      "missing" (no "net.wifi"), "off" (no saved network) or
//      "connecting".
//   5. Records the elapsed boot_ms (now that step 4 -- the self-check
//      screen included -- has actually finished, per spec 6.1; the
//      screen's header, which already rendered once with boot_ms still
//      at its default, is then re-rendered with this final value), emits
//      "WISP_BOOT_REPORT <json>" and "WISP_MANIFEST <json>" (in that
//      order) via Platform::emit_line, then publishes
//      "system.boot.completed".
//   6. Given Platform::read_line, starts the serial console task
//      (console.hpp, src/console_task.hpp).
//   7. Marks the running image valid (FirmwareUpdater::mark_valid()): a
//      freshly updated image that crashes or resets anywhere before this
//      point is rolled back by the bootloader (connectivity spec 6.5).

#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "wisp/app/board.hpp"
#include "wisp/base/result.hpp"
#include "wisp/base/value.hpp"
#include "wisp/core/boot_report.hpp"
#include "wisp/core/device_registry.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/core/firmware_update.hpp"
#include "wisp/core/settings.hpp"
#include "wisp/core/storage_manager.hpp"
#include "wisp/net/http.hpp"
#include "wisp/net/pairing_service.hpp"
#include "wisp/net/time_sync.hpp"

namespace wisp::ui {
class SystemLayer;
class UiRuntime;
}  // namespace wisp::ui

namespace wisp::core {
class ButtonService;
class ModelProfileService;
class SceneRepository;
class SystemSettings;
class NotificationService;
class WakeLockRegistry;
class OccupancyRegistry;
class SensorService;
class StorageWorker;
}  // namespace wisp::core

namespace wisp::scene {
class SceneRuntime;
}

namespace wisp::agent {
class AgentService;
}

namespace wisp::hal {
struct ScreenShape;
}

namespace wisp::net {
class WifiService;
class PairingService;
}  // namespace wisp::net

namespace wisp::app {

class App;
class PowerManager;
struct LoopbackStatus;

// Outcome of one Platform::read_line call.
enum class LineRead { Line, Timeout, Closed };

// One extra console command (Platform::console_commands).
struct ConsoleCommand {
    // The first word that selects it.
    std::string name;
    // Its line in `help`.
    std::string usage;
    // Gets every word of the line, args[0] == name; returns the text to
    // print (possibly several lines).
    std::function<std::string(App& app, const std::vector<std::string>& args)> run;
};

// Everything App needs that varies by port/board. Constructed by the
// simulator main or the ESP firmware main and handed
// to App by value (App takes ownership of `board`/`settings`).
struct Platform {
    std::unique_ptr<Board> board;
    std::unique_ptr<core::SettingsBackend> settings;

    // Writes one whole line of diagnostics output (e.g. to the serial
    // console or stdout) -- no trailing newline expected to be added by
    // the caller or present in `line`; the implementation adds whatever
    // line ending its destination needs.
    std::function<void(const std::string& line)> emit_line;

    // Fallback shutdown, for the self-check UI's power-off handling when no
    // "power.*" device capable of powering itself off is
    // registered. May be empty if the platform has no such fallback.
    std::function<void()> request_power_off;

    // Returns heap diagnostics in bytes: "internal_free" and
    // "internal_largest" (byte-addressable internal RAM and its largest
    // free block, checked against wisp/app/ram_budget.hpp),
    // "internal_total" (that RAM's heap size), and
    // "large_free"/"large_total" (PSRAM: free and total size; a platform
    // with no PSRAM reports "large_total" 0 or leaves it out), for the
    // self-check UI, WISP_PERF and the admin API. A key may be missing;
    // the whole function may be empty (no heap diagnostics available).
    std::function<Value()> heap_info;

    // Turns automatic light sleep on or off (scene-device spec 4.1):
    // wisp::app::PowerManager calls it with `true` in deep standby while
    // power/light_sleep is set and no cpu wake lock is held, and with
    // `false` when any of that stops. On real hardware dynamic frequency
    // scaling stays on and only tickless light sleep is toggled
    // (firmware/main/main.cpp); the simulator logs. Empty when the board
    // cannot light-sleep safely: deep standby then only turns Wi-Fi off.
    std::function<void(bool enable)> set_light_sleep;

    // Adds the platform's image decoders to LVGL (ui::UiConfig::
    // install_image_decoders): baseline JPEG for ui.image. Empty: images
    // are PNG only.
    std::function<void()> install_image_decoders;

    // The fonts partition image (ui::UiConfig::font_image): the mapped
    // "fonts" partition on the device, fonts.bin read into memory in the
    // simulator. The bytes must outlive the App. Empty: large text uses
    // bitmap fonts.
    std::span<const uint8_t> font_image;

    // The glyphs partition image (ui::UiConfig::glyph_image): the mapped
    // "glyphs" partition on the device, glyphs.bin read into memory in
    // the simulator. The bytes must outlive the App. Empty: CJK body
    // text uses the built-in font where allowed (`builtin_glyphs`, host
    // builds only), else the 16 px vector font.
    std::span<const uint8_t> glyph_image;
    bool builtin_glyphs = true;

    std::string firmware_version = "0.1.0";
    // What the firmware runs on, as the self-check header shows it next to
    // the version: the chip target on ESP, "simulator" on the PC. Empty:
    // not shown.
    std::string target;

    // Fills the buffer with cryptographically strong random bytes (pairing
    // tokens and codes, the hotspot password). Required for networking:
    // without it no net service is built, as if "net.wifi" were missing.
    std::function<void(uint8_t*, size_t)> random;

    // The admin page's HTTP server. Empty: no admin backend at all.
    std::unique_ptr<net::HttpServer> http_server;
    // Outgoing HTTP (the model profiles' connectivity test). Empty: that
    // test answers 503 unavailable.
    std::unique_ptr<net::HttpClient> http_client;
    // A WispPack with the web UI (spec 2.3). The bytes must outlive the
    // App. Empty, or not a valid pack: the API is served without a page.
    std::span<const uint8_t> assets;
    // The admin backend's port (spec 6.1: http://wisp.local:8900).
    uint16_t http_port = 8900;
    // Device only (null in the simulator): a second, minimal server that
    // App runs on port 80 while the setup hotspot is up, answering every
    // request with a redirect to the admin port's setup page so phones'
    // captive-portal probes open it (AdminServer::captive_handler()).
    std::unique_ptr<net::HttpServer> captive_server;
    // Serve from boot regardless of network state (the simulator, whose
    // server only listens on loopback); otherwise the server runs only
    // while the station has an IP or the setup hotspot is up.
    bool http_always_on = false;
    // Network time (connectivity spec 5.6): one SNTP request at a time.
    // Empty: no TimeSyncService, the clock is never synced from the
    // network.
    std::unique_ptr<net::Sntp> sntp;
    // Sets the system wall clock to UTC seconds after a sync; an error
    // fails that sync. Empty: left alone (the simulator's host clock is
    // already right).
    std::function<Result<void>(int64_t unix_s)> set_wall_clock;
    // Code pairing's limits; the defaults are spec 6.1's (a code lives two
    // minutes). Tests shorten code_ttl_ms to watch a code expire.
    net::PairingConfig pairing;

    // Writes an uploaded firmware image to the OTA slot not running
    // (connectivity spec 6.5). Empty: the board has no OTA slots, and
    // GET /system/firmware reports supported false.
    std::unique_ptr<core::FirmwareUpdater> firmware;

    // Restarts the device (admin "reboot", factory reset, a confirmed
    // firmware update). May be empty.
    std::function<void()> reboot;
    // Serial console input (spec 8.2; see console.hpp). Waits up to
    // timeout_ms for one line (stored without its line ending): Line when
    // one arrived, Timeout when none did, Closed once the input has ended
    // for good. The bounded wait is what lets App::stop() end the console
    // task without a way to interrupt a blocked read. Must not echo what
    // it reads. Empty: no console.
    std::function<LineRead(std::string& line, uint32_t timeout_ms)> read_line;
    // Platform-specific console commands (e.g. simulator-only debugging),
    // listed by `help` after the built-in ones. An entry named like a
    // built-in command is ignored.
    std::vector<ConsoleCommand> console_commands;

    // Scenes (scene-runtime-core spec 6.2, 6.4). Empty: installed scenes
    // live in <int>/scenes, code and data together. Set (the simulator's
    // --scenes-dir): dev mode -- the code comes straight from this working
    // directory, read-only (installs and deletes are refused), and the data
    // goes to scenes_data_dir.
    std::string scenes_dir;
    // The data root in dev mode (the simulator passes <data-dir>/scenes);
    // empty: <int>/scenes. Ignored without scenes_dir.
    std::string scenes_data_dir;
};

class App {
public:
    explicit App(Platform platform);

    // Calls stop() if start() succeeded and stop() wasn't already called.
    ~App();

    App(const App&) = delete;
    App& operator=(const App&) = delete;

    // Runs the boot sequence described in the file header above. Fails
    // with ErrorCode::Busy, doing nothing else, if this App is already
    // started (i.e. a previous start() succeeded and stop() hasn't been
    // called since) -- start() is not re-entrant/restart-on-call; call
    // stop() first to run the sequence again.
    Result<void> start();

    // Stops the serial console task, then the scene runtime (closing the
    // scene in front, whose last store flush goes to the worker), then the
    // self-check screen, then networking -- the "wisp.net" task,
    // then the HTTP server (whose handlers call into PowerManager, the
    // net services and the scenes), then the net services themselves --
    // then the storage-refresh timer, the scenes' HTTP proxy and the
    // storage worker (drained), and then (in reverse order) PowerManager,
    // ButtonService,
    // UiRuntime (if one was started) and EventBus. Idempotent, and safe
    // to call even if start() was never called or didn't succeed -- only
    // what was actually started is torn down. Stopping the UiRuntime
    // here (rather than leaving it to the destructor of some other
    // object) matters because LVGL 9.5 under this build only supports one
    // started UiRuntime per process at a time (see ui_runtime.hpp): tests
    // that construct Apps one after another rely on each one's stop()
    // (direct or via ~App()) fully releasing LVGL before the next App
    // starts its own UiRuntime.
    void stop();

    core::DeviceRegistry& registry();
    core::BootReport& report();
    core::EventBus& bus();
    core::StorageManager& storage();
    core::ButtonService& buttons();

    // nullptr in headless mode (no "display.main" device registered).
    ui::UiRuntime* ui();
    // The system layer (back gesture, capsule, panel, banners) on ui():
    // nullptr when ui() is. Its methods are called on the UI thread only.
    ui::SystemLayer* system_layer();

    // The scene runtime (scene-runtime-core spec 3): nullptr in headless
    // mode, and before start() or after stop().
    scene::SceneRuntime* scenes();
    // The installed scene packages: nullptr without a usable internal
    // storage root, and before start() or after stop(). Present in headless
    // mode too (the admin API lists and deletes scenes without a screen).
    core::SceneRepository* scene_repository();

    // Networking services (step 4b). wifi() is nullptr without a "net.wifi"
    // device; both are nullptr without Platform::random, and before start()
    // or after stop().
    net::WifiService* wifi();
    net::PairingService* pairing();
    // Network time: nullptr without Platform::sntp or a WifiService.
    net::TimeSyncService* time_sync();
    // Always available; loaded from storage by start() (step 3).
    core::ModelProfileService& models();
    // Applies brightness through PowerManager::set_brightness() while
    // started (otherwise the stored value takes effect at the next boot).
    core::SystemSettings& system_settings();

    // Non-null for the whole lifetime of a started App (see
    // start()'s Step 4) -- constructed regardless of headless mode,
    // since the idle-timeout/platform-power-hook half of PowerManager's
    // job (see power_manager.hpp) is useful even without a display.
    // Used by the self-check screen's power-key handling (see
    // src/self_check_screen.cpp) to route screen on/off through it
    // instead of calling hal::Display::set_power() directly. nullptr
    // before start() (or after stop()).
    PowerManager* power_manager();

    // Wake locks (scene-device spec 4.3); valid for the App's lifetime.
    core::WakeLockRegistry& wakelocks();
    // Notifications (scene-device spec 5): loaded by start(), saved and
    // reset by stop(); nullptr before start() or after stop().
    core::NotificationService* notifications();
    // Who uses which device (scene-device spec 6.3); valid for the App's
    // lifetime.
    core::OccupancyRegistry& occupancy();
    // The IMU's subscribers (scene-device spec 6.1): built by start(),
    // stopped and reset by stop(); nullptr before start() or after stop().
    core::SensorService* sensors();
    // The "wisp.fs" storage worker the scenes and notifications share:
    // nullptr without scenes, and before start() or after stop().
    core::StorageWorker* storage_worker();
    // The agent (agent-core spec 2), built and started with a screen and a
    // scene runtime; nullptr otherwise, and before start() or after stop().
    agent::AgentService* agent();

    // Writes what still waits for flash before the device restarts, powers
    // off or enters deep standby (spec 3.8 of
    // docs/superpowers/specs/2026-11-06-flash-thread-design.md): changed
    // Settings first (up to 2 s), then every job queued on the storage
    // worker (up to 5 s). A timeout is logged and ignored. Blocks; any
    // thread but the worker's.
    void flush_storage();

    // The Platform this App was constructed from (needed by the
    // self-check screen for request_power_off, heap_info,
    // firmware_version, board->name() and the settings backend -- see
    // src/self_check_screen.cpp). Valid for the whole lifetime of this
    // App, independent of start()/stop().
    const Platform& platform() const;

private:
    // Grants loopback.cpp's run_loopback()/join_loopback() (spec 8's
    // "录音回放" button) access to a per-App task handle stored in Impl,
    // so each App's "wisp.loop" task is tracked on that App itself
    // (torn down by App::stop() before EventBus/UiRuntime stop -- see
    // app.hpp's Lifetime note) rather than in a process-wide static.
    friend void run_loopback(App&, std::function<void(const LoopbackStatus&)>);
    friend void join_loopback(App&);
    // Clears the "stopping" flag join_loopback() sets, so loopback works
    // again after a start()/stop()/start() cycle on the same App -- see
    // app.cpp's start() and loopback.cpp.
    friend void reset_loopback(App&);
    // The factory reset (src/factory_reset.hpp) flushes the admin
    // WebSocket through NetWiring and keeps its "wisp.reset" task here.
    friend void request_factory_reset(App&);
    friend void cancel_factory_reset(App&);
    friend void factory_reset(App&);
    friend bool start_factory_reset(App&);
    friend void join_factory_reset(App&);
    friend bool take_factory_reset_request(App&);
    friend void set_reset_erase_budget(App&, uint32_t);
    // The firmware update's confirmation (src/firmware_confirm.hpp) keeps
    // its state and "wisp.fwup" task here.
    friend void request_firmware_confirm(App&);
    friend bool take_firmware_request(App&);
    friend std::optional<core::FirmwarePending> pending_firmware(App&);
    friend bool start_firmware_install(App&);
    friend void decline_firmware(App&);
    friend void restart_for_firmware(App&);
    friend void join_firmware_install(App&);
    // src/storage_io.hpp.
    friend bool queue_storage_refresh(App&);
    // The console task (src/console_task.hpp) lives on Impl.
    friend void start_console(App&);
    friend void stop_console(App&);
    friend void set_reset_spawner(
        App&,
        std::function<std::unique_ptr<os::Task>(const os::TaskConfig&, std::function<void()>)>);

    // start(): builds impl_->agent (src/agent_wiring.hpp) on the screen of
    // that shape.
    void build_agent(const hal::ScreenShape& shape);

    struct Impl;
    std::unique_ptr<Impl> impl_;
};

}  // namespace wisp::app
