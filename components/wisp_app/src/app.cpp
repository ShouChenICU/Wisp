#include "wisp/app/app.hpp"

#include <algorithm>
#include <ctime>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "app_impl.hpp"
#include "console_task.hpp"
#include "factory_reset.hpp"
#include "firmware_confirm.hpp"
#include "loopback.hpp"
#include "net_cards.hpp"
#include "net_section.hpp"
#include "power_sampling.hpp"
#include "self_check.hpp"
#include "storage_io.hpp"
#include "wisp/agent/context.hpp"
#include "wisp/agent/conversation.hpp"
#include "wisp/base/error.hpp"
#include "wisp/base/json.hpp"
#include "wisp/core/button_service.hpp"
#include "wisp/core/log.hpp"
#include "wisp/core/reminders.hpp"
#include "wisp/core/topics.hpp"
#include "wisp/hal/audio.hpp"
#include "wisp/hal/button.hpp"
#include "wisp/hal/display.hpp"
#include "wisp/hal/display_geometry.hpp"
#include "wisp/hal/power.hpp"
#include "wisp/hal/roles.hpp"
#include "wisp/hal/touch.hpp"
#include "wisp/net/model_probe.hpp"
#include "wisp/net/wifi_service.hpp"
#include "wisp/os/os.hpp"
#include "wisp/scene/minute_ticker.hpp"
#include "wisp/shell/scene_list.hpp"
#include "wisp/ui/strings.hpp"
#include "wisp/ui/system_layer.hpp"
#include "wisp/ui/ui_runtime.hpp"

namespace wisp::app {

namespace {

constexpr uint32_t kStorageRefreshMs = 2000;
// How often PowerManager::tick() is driven. Settings
// "power"/"screen_off_s" is specified (and set, default 60) in whole
// seconds, so sub-second resolution here would be pointless; 1s keeps
// the idle-timeout check's own overhead (and the "wisp.power" Timer
// callback's wake rate) negligible.
constexpr uint32_t kPowerTickMs = 1000;
// At most 50 short notices to read at boot, or to write at shutdown.
constexpr uint32_t kNotifyLoadMs = 2000;
constexpr uint32_t kNotifyFlushMs = 1000;
// How long stop() waits for changed settings to be written (spec 3.8).
constexpr uint32_t kSettingsFlushMs = 2000;
// Before a restart, power-off or deep standby (spec 3.8): changed settings,
// then whatever the storage worker still has queued.
constexpr uint32_t kShutdownSettingsFlushMs = 2000;
constexpr uint32_t kShutdownDrainMs = 5000;
// How often run_io() retries a full storage queue.
constexpr uint32_t kIoBusyPollMs = 10;

// The screen shape the system layer and the self-check screen lay out in
// (spec 5.6). Invalid shape caps fall back to a circle, the most
// conservative shape, as DeviceRegistry::manifest() does; UiRuntime::start()
// has already refused a display without a usable width and height.
hal::ScreenShape display_shape(const hal::Display& display) {
    const Properties& caps = display.info().caps;
    auto shape = hal::screen_shape_from_caps(caps);
    if (shape) {
        return shape.value();
    }
    WLOG_W("app", "%s: invalid shape caps (%s); treating as circle", display.info().id.c_str(),
           shape.error().message.c_str());
    hal::ScreenShape fallback;
    fallback.kind = hal::ScreenShapeKind::Circle;
    auto width = caps.find("width");
    auto height = caps.find("height");
    fallback.width = width != caps.end() ? static_cast<int32_t>(width->second.as_int()) : 0;
    fallback.height = height != caps.end() ? static_cast<int32_t>(height->second.as_int()) : 0;
    return fallback;
}

// The settings cell is the readers' source: a short read under its lock,
// with no flash, so any thread may call these (the scene task, the UI,
// wisp.agent, wisp.fs).
int utc_offset_of(const core::SystemSettings& settings) {
    return settings.cell().read(
        [](const core::SystemSettingsValues& v) { return v.utc_offset_min; });
}

ui::Lang lang_of(const core::SystemSettings& settings) {
    return settings.cell().read(
        [](const core::SystemSettingsValues& v) { return ui::to_lang(v.language); });
}

uint8_t notify_vol_of(const core::SystemSettings& settings) {
    return settings.cell().read([](const core::SystemSettingsValues& v) {
        return static_cast<uint8_t>(std::clamp(v.notify_vol, 0, 100));
    });
}

bool time_24h_of(const core::SystemSettings& settings) {
    return settings.cell().read([](const core::SystemSettingsValues& v) { return v.time_24h; });
}

}  // namespace

App::App(Platform platform) : impl_(std::make_unique<Impl>(std::move(platform))) {}

App::~App() { stop(); }

Result<void> App::start() {
    if (impl_->started) {
        return Result<void>::err(ErrorCode::Busy, "App already started");
    }

    // A previous start()/stop() cycle on this same App may have left
    // loopback permanently refusing new "wisp.loop" tasks (see
    // join_loopback() in loopback.cpp, called from stop()); clear that
    // now so "Mic test" works again after a restart.
    reset_loopback(*this);

    uint64_t boot_t0 = os::now_ms();

    // Step 1: logging.
    log::init();

    // Step 2: board bring-up, then fill in the implicit Ok entries for
    // whatever the board didn't already report on (see board.hpp).
    impl_->platform.board->init(impl_->registry, impl_->report);
    for (const auto& device : impl_->registry.all()) {
        if (!impl_->report.find(device->info().id)) {
            impl_->report.record(
                {device->info().id, device->info().cls, core::DeviceState::Ok, ""});
        }
    }
    // The wiring below finds these devices by role main; one registered
    // without it would be treated as missing with nothing saying why.
    for (const core::RoleGap& gap : core::main_role_gaps(impl_->registry)) {
        std::string ids;
        for (const std::string& id : gap.ids) {
            if (!ids.empty()) ids += ", ";
            ids += id;
        }
        WLOG_W("app", "no device with role main among %s: %s", hal::to_string(gap.iface),
               ids.c_str());
    }
    if (auto key = core::power_key_also_talks(impl_->registry)) {
        WLOG_W("app",
               "%s holds roles power and talk: holding it to talk also triggers the power "
               "key's long press",
               key->c_str());
    }

    // Step 3: core services.
    impl_->bus.start();
    if (!impl_->storage.init()) {
        impl_->report.set_note("storage", "readonly");
    }
    auto buttons_started = impl_->buttons.start();
    if (!buttons_started) {
        WLOG_E("app", "button input unavailable: %s", buttons_started.error().message.c_str());
        impl_->report.set_note("buttons", "unavailable");
        for (const auto& button : impl_->registry.list<hal::Button>()) {
            impl_->report.record({button->info().id, hal::DeviceClass::Button,
                                  core::DeviceState::Failed, "button service unavailable"});
        }
    }
    // Scene lifecycle edges, before the scenes that report them. The
    // handler reads the notifications, the scene order and the agent at
    // drain time: all three are set below before the runtime starts or a
    // delete can be asked for, and outlive the inbox (stop()). On wisp.bus,
    // which must not touch flash: the scene order saves through the worker
    // and the tool index refreshes through it.
    {
        App::Impl* impl = impl_.get();
        impl_->lifecycle = os::make_large<core::EdgeInbox<scene::SceneTransition, 32>>(
            "scene_lifecycle", impl_->bus, [impl](const scene::SceneTransition& t) {
                if (t.state == scene::SceneTransition::State::Running) {
                    if (impl->recents) impl->recents->opened(t.id.str());
                } else if (t.state == scene::SceneTransition::State::Deleted) {
                    const std::string id = t.id.str();
                    if (impl->recents) impl->recents->forget(id);
                    if (impl->notifications) {
                        impl->notifications->dismiss_source(id, core::DismissReason::Dismiss);
                    }
                }
                if (impl->agent) impl->agent->on_scene_transition(t);
            });
        if (!impl_->lifecycle) {
            WLOG_E("app",
                   "no memory for the scene lifecycle inbox; the scene order, notices "
                   "and scene tools miss scene changes");
        }
        std::lock_guard<os::Mutex> lock(impl_->lifecycle_gate.mu);
        impl_->lifecycle_gate.inbox = impl_->lifecycle.get();
    }
    // Scenes: the storage worker, and the repository recovered on it,
    // before anything can list or open a scene.
    {
        SceneWiringDeps deps{impl_->platform, impl_->bus, impl_->storage};
        deps.on_lifecycle = [gate = &impl_->lifecycle_gate](const scene::SceneTransition& t) {
            // Under the gate's lock: App::stop() closes it before the inbox
            // goes, so a push never reaches a destroyed inbox.
            std::lock_guard<os::Mutex> lock(gate->mu);
            if (gate->inbox != nullptr) gate->inbox->push(t);
        };
        impl_->scenes = os::make_large<SceneWiring>(std::move(deps));
    }
    SceneWiring* scenes = impl_->scenes.get();
    if (scenes != nullptr) {
        scenes->start_storage();
        // Until the settings cache is attached below (or for good, if it
        // cannot be), settings calls reach flash, so they run on the worker.
        impl_->settings_front.set_worker(&scenes->worker());
    } else {
        WLOG_E("app", "no memory for the scenes; scenes are off");
    }
    // Notifications, loaded before anything can post: on the storage
    // worker when there is one (load() reads flash), else right here.
    {
        App::Impl* impl = impl_.get();
        core::StorageWorker* worker = scenes != nullptr ? &scenes->worker() : nullptr;
        // Without the worker nothing is saved (the service then never
        // starts its timer).
        std::function<bool(std::function<void()>)> submit_io;
        if (worker != nullptr) {
            submit_io = [worker](std::function<void()> job) {
                return worker->submit(std::move(job));
            };
        } else {
            WLOG_W("app", "no storage worker; notifications are not saved");
        }
        impl->notifications = std::make_unique<core::NotificationService>(core::NotificationDeps{
            .bus = impl->bus,
            .storage = impl->storage,
            .submit_io = std::move(submit_io),
            .make_timer = {},
            .now_ms = os::now_ms,
            .wall_s =
                [impl] {
                    const int64_t t = impl->wall_now();
                    return t >= scene::MinuteTicker::kValidSince ? t : int64_t{0};
                },
        });
        core::NotificationService* notes = impl->notifications.get();
        auto load = [notes] {
            if (auto r = notes->load(); !r) {
                WLOG_W("app", "notifications not loaded: %s", r.error().message.c_str());
            }
        };
        auto loaded = std::make_shared<os::Semaphore>(1, 0);
        if (worker != nullptr && worker->submit([load, loaded] {
                load();
                loaded->give();
            })) {
            if (!loaded->take(kNotifyLoadMs)) WLOG_W("app", "notifications still loading");
        } else {
            load();
        }
        // The todos (mvp spec 5.2) post to the notifications just made, so
        // they come right after; loaded on the worker like them.
        if (auto system = impl->storage.write_root(core::Area::System); !system) {
            WLOG_W("app", "no System storage; todos and reminders are off");
        } else if (worker == nullptr) {
            WLOG_W("app", "no storage worker; todos and reminders are off");
        } else {
            impl->reminders = std::make_unique<core::ReminderScheduler>(
                core::ReminderDeps{
                    .storage = impl->storage,
                    .bus = impl->bus,
                    .notes = notes,
                    // The worker or nothing: a timer callback must never
                    // end up running a pass (which writes flash) itself.
                    // Interactive: a pass rewrites one small file and a
                    // reminder should ring on time. stop() waits for a
                    // queued pass by queuing behind it, and the router's
                    // todo actions must run before that too, so all three
                    // use this one lane (a full lane: the retry timer for a
                    // pass, drain_io for stop()).
                    .submit_io =
                        [worker](std::function<void()> job) {
                            return worker->submit(std::move(job), core::IoLane::Interactive);
                        },
                    .drain_io = [worker](uint32_t ms) { return worker->drain(ms); },
                    .wall_s = [impl] { return impl->wall_now(); },
                    .utc_offset_min = [impl] { return utc_offset_of(impl->system_settings); },
                    .missed_title =
                        [impl](size_t count) {
                            return ui::tr_fill(ui::StrId::ReminderMissed,
                                               lang_of(impl->system_settings),
                                               std::to_string(count));
                        },
                    .make_timer = {},
                },
                system.value() + "/todo/todos.json");
            core::ReminderScheduler* reminders = impl->reminders.get();
            Result<void> r = Result<void>::err(ErrorCode::Busy, "storage worker refused");
            if (auto ran =
                    worker->run_sync([reminders, &r] { r = reminders->load(); }, os::kForever);
                !ran) {
                r = ran;
            }
            if (!r) WLOG_W("app", "todos not loaded: %s", r.error().message.c_str());
        }
        // The scene list's order (mvp spec 6.4), loaded on the worker too.
        if (auto system = impl->storage.write_root(core::Area::System); system && worker) {
            impl->recents = std::make_unique<shell::SceneRecents>(
                impl->storage, system.value() + "/shell/recents.json",
                [worker](std::function<void()> job) { return worker->submit(std::move(job)); });
            shell::SceneRecents* recents = impl->recents.get();
            Result<void> r = Result<void>::err(ErrorCode::Busy, "storage worker refused");
            if (auto ran = worker->run_sync([recents, &r] { r = recents->load(); }, os::kForever);
                !ran) {
                r = ran;
            }
            if (!r) WLOG_W("app", "scene order not loaded: %s", r.error().message.c_str());
        }
    }
    // Settings from memory from here on, saved on the worker. Loaded
    // before anything below reads a setting; with the worker running the
    // load happens on it, else right here (StorageWorker::run_sync()).
    if (scenes != nullptr) {
        App::Impl* impl = impl_.get();
        impl->settings_cache = os::make_large<core::CachedSettingsBackend>(
            impl->settings_front.platform(), scenes->worker(),
            [impl] { impl->report_persist_failed(); });
        if (!impl->settings_cache) {
            WLOG_E("app", "no memory for the settings cache; settings are written directly");
        } else if (auto r = impl->settings_front.load_and_attach(*impl->settings_cache); !r) {
            WLOG_W("app", "settings not loaded into memory (%s); reading them directly",
                   r.error().message.c_str());
            impl->settings_cache.reset();
        }
    }
    // The settings cell starts from what is stored, read from the cache
    // just attached, before anything below reads the language or follows it.
    impl_->system_settings.seed_cell();
    // The SD card hot-plug probe, on the card's worker (wisp.sd.io, or
    // wisp.fs without one).
    impl_->refresh_pending.store(false);
    impl_->storage_timer =
        std::make_unique<os::Timer>("wisp.storage", kStorageRefreshMs, /*periodic=*/true,
                                    [this] { (void)queue_storage_refresh(*this); });
    impl_->storage_timer->start();
    {
        App::Impl* impl = impl_.get();
        impl_->models.set_on_persist_failed([impl] { impl->report_persist_failed(); });
        Result<void> loaded = Result<void>::ok();
        if (auto r = impl_->run_io([&] { loaded = impl_->models.init(); }, os::kForever); !r) {
            loaded = r;
        }
        if (!loaded) {
            WLOG_W("app", "model profiles unavailable: %s", loaded.error().message.c_str());
        }
    }
    if (impl_->platform.http_client) {
        impl_->models.set_prober(
            net::make_model_prober(*impl_->platform.http_client, [] { return os::now_ms(); }));
    }

    // Step 4: UI (or headless). The device UI language comes first so the
    // first frame is already in it; the system duties follow the settings
    // cell for later changes (their first delivery finds this language and
    // changes nothing).
    ui::set_lang(ui::to_lang(impl_->system_settings.get().language));
    auto display = impl_->registry.by_role<hal::Display>(hal::role::kMain);
    std::shared_ptr<hal::Touch> touch;
    hal::ScreenShape shape;
    int64_t boot_brightness = 200;
    if (display) {
        core::Settings display_settings(*impl_->platform.settings, "display");
        boot_brightness = std::clamp<int64_t>(display_settings.get_int("brightness", 200), 0, 255);
        display->set_brightness(static_cast<uint8_t>(boot_brightness));

        touch = impl_->registry.by_role<hal::Touch>(hal::role::kMain);
        ui::UiConfig ui_config;
        ui_config.install_image_decoders = impl_->platform.install_image_decoders;
        ui_config.font_image = impl_->platform.font_image;
        ui_config.glyph_image = impl_->platform.glyph_image;
        ui_config.builtin_glyphs = impl_->platform.builtin_glyphs;
        impl_->ui = std::make_unique<ui::UiRuntime>(display, touch, std::move(ui_config));
        if (!impl_->ui->start()) {
            WLOG_E("app", "UiRuntime failed to start; continuing headless");
            impl_->ui.reset();
            impl_->report.set_note("mode", "headless");
        } else {
            // Before anything that shows on the top layer (the self-check
            // screen and its dialogs, later the scenes).
            shape = display_shape(*display);
            impl_->ui->run_blocking(
                [&] { impl_->system_layer = std::make_unique<ui::SystemLayer>(shape); });
        }
    } else {
        impl_->report.set_note("mode", "headless");
    }

    // PowerManager (spec 6.7) is constructed here -- once `ui`
    // is settled either way -- rather than inside the `if (display)`
    // branch above, so it also exists (minus the display/touch/UI
    // pieces, which all tolerate being null) in headless mode: the
    // idle-timeout/platform-power-hook half of its job is useful
    // regardless of whether there's a screen. It must exist before the
    // self-check screen is built below, since that screen's power-key
    // handling calls into it.

    // PowerMonitor is constructed here too -- before PowerManager, which
    // holds a non-owning pointer into it -- and, like PowerManager itself,
    // regardless of headless mode: polling the main power device and
    // publishing power.charging.changed/power.battery.low has nothing to do
    // with whether there's a screen. It's completely inert if no Power
    // device has role main (see power_monitor.hpp).
    auto power_dev = impl_->registry.by_role<hal::Power>(hal::role::kMain);
    impl_->power_monitor =
        std::make_unique<core::PowerMonitor>(impl_->bus, power_dev, &impl_->system_settings.cell());
    impl_->power_monitor->start();
    if (impl_->notifications) {
        // One low-battery notice, with the level it fell to; charging clears
        // it. Edges of the supply cell: each is acted on against the value
        // last seen, so a coalesced call acts once. `low` starts clear so a
        // supply that is already low when this subscribes still posts. The
        // charging edge goes first: a first sample that is both charging and
        // low posts the notice and keeps it, as the events' order did.
        const core::StateCell<core::PowerSupply>& supply = impl_->power_monitor->supply();
        impl_->supply_sub = supply.subscribe([notes = impl_->notifications.get(), low = false,
                                              charging = supply.get().charging](
                                                 const core::PowerSupply& s) mutable {
            if (s.charging && !charging) {
                notes->dismiss_code(core::notice::kBatteryLow, core::DismissReason::Dismiss);
            }
            if (s.low && !low) {
                notes->dismiss_code(core::notice::kBatteryLow, core::DismissReason::Dismiss);
                notes->post_system(core::notice::kBatteryLow, std::to_string(s.battery_percent));
            }
            charging = s.charging;
            low = s.low;
        });
    }

    // The notification sounds, before PowerManager (its activity hook
    // silences them). Without a speaker they are skipped.
    impl_->tones = std::make_unique<core::TonePlayer>(
        impl_->registry.by_role<hal::AudioOut>(hal::role::kMain), &impl_->wakelocks);
    if (auto r = impl_->tones->start(); !r) {
        WLOG_W("app", "notification sounds off: %s", r.error().message.c_str());
    }

    // The IMU's subscribers, before the scene runtime and the self-check
    // screen that read it. Standby and deep standby pause every
    // subscription (spec 6.1): App follows PowerManager's level cell for
    // it once PowerManager exists, as the service keeps no reference to it.
    impl_->sensors = std::make_unique<core::SensorService>(
        impl_->registry.by_role<hal::Imu>(hal::role::kMain), impl_->occupancy);
    if (auto r = impl_->sensors->start(); !r) {
        WLOG_W("app", "sensors off: %s", r.error().message.c_str());
    }
    // Generic devices (device model spec 3.6), for the agent tools and the
    // scenes; it follows PowerManager's level cell like the sensors, once
    // PowerManager exists (devices_power_sub below).
    impl_->devices = std::make_unique<core::DeviceService>(
        impl_->registry, &impl_->occupancy,
        impl_->platform.device_clock_ms ? impl_->platform.device_clock_ms
                                        : std::function<uint64_t()>(os::now_ms));

    auto power_key = impl_->registry.by_role<hal::Button>(hal::role::kPower);
    impl_->power_manager = std::make_unique<PowerManager>(PowerManagerDeps{
        .bus = impl_->bus,
        .registry = impl_->registry,
        .locks = impl_->wakelocks,
        .display = display,
        .storage = &impl_->storage,
        .power_monitor = impl_->power_monitor.get(),
        .ui = impl_->ui.get(),
        .buttons = &impl_->buttons,
        .power_key = power_key ? power_key->info().id : std::string(),
        .set_light_sleep = impl_->platform.set_light_sleep,
        .before_deep_standby = [this] { flush_storage(); },
        .on_activity =
            [tones = impl_->tones.get()] {
                // The user is there: an alarm stops (spec 5.1).
                tones->silence();
            },
        .system_settings = &impl_->system_settings.cell(),
    });
    if (display) {
        // The panel already shows this level (applied above, before the UI
        // started); PowerManager dims relative to it.
        impl_->power_manager->set_brightness(static_cast<uint8_t>(boot_brightness));
    }
    // Standby and deep standby pause every IMU subscription (spec 6.1).
    impl_->sensors_power_sub = impl_->sensors->follow_power(impl_->power_manager->level_cell());
    // At Standby and below "standby: off" devices are unreachable through
    // the service (device model spec 3.6). On wisp.bus, like the sensors.
    impl_->devices_power_sub = impl_->power_manager->level_cell().subscribe(
        [devices = impl_->devices.get()](const hal::PowerLevel& level) {
            devices->set_power_level(level);
        });
    impl_->power_timer =
        std::make_unique<os::Timer>("wisp.power", kPowerTickMs, /*periodic=*/true, [this] {
            const uint64_t now = os::now_ms();
            impl_->power_manager->tick(now);
            post_wakelock_reminders(impl_->wakelocks, *impl_->notifications, now);
        });
    impl_->power_timer->start();

    // The firmware update service, before networking routes uploads to
    // it. Only a device with a screen can confirm an update: the system
    // duties follow its progress cell there.
    // Every OTA call that touches flash runs on the storage worker (or
    // right here without one, as on a board without scenes).
    core::FirmwareUpdateService::Deps firmware_deps;
    firmware_deps.updater = impl_->platform.firmware.get();
    firmware_deps.running_version = impl_->platform.firmware_version;
    firmware_deps.confirmable = impl_->ui != nullptr;
    firmware_deps.restart = [this] { restart_for_firmware(*this); };
    impl_->firmware = std::make_unique<core::FirmwareUpdateService>(std::move(firmware_deps),
                                                                    impl_->bus, storage_worker());

    // The scene runtime, built (not started) before networking so the
    // admin routes can be handed it.
    // wisp.device.info() and the panel's header. Runs on the scene task
    // and the UI thread, neither of which may touch flash: PowerMonitor's
    // last sample (a copy under its lock), NetWiring::wifi_state() (the
    // Wi-Fi cell, memory only) and the service's cached enabled flag,
    // never a read of the power chip, Settings or a file. Both
    // pointers are set before the runtime and the status shell start and
    // reset only after they have stopped.
    std::function<Value()> device_status;
    if (impl_->ui) {
        App::Impl* impl = impl_.get();
        device_status = [impl] {
            Value battery;
            if (impl->power_monitor) {
                if (auto st = impl->power_monitor->last_status()) {
                    battery = Value(Map{{"percent", Value(st->battery_percent)},
                                        {"charging", Value(st->charging)}});
                }
            }
            // No "wifi" without a WifiService: wisp.device.info() then says
            // "off", and the panel leaves the item out.
            Map status{{"battery", std::move(battery)}};
            if (std::optional<net::WifiState> wifi =
                    impl->net ? impl->net->wifi_state() : std::nullopt) {
                status.emplace("wifi", Value(net::to_string(*wifi)));
                status.emplace("wifi_enabled", Value(impl->net->wifi()->enabled()));
            }
            return Value(std::move(status));
        };
    }
    if (impl_->ui && scenes != nullptr) {
        App::Impl* impl = impl_.get();
        scenes->build_runtime(
            *impl_->ui, *impl_->system_layer, shape, device_status,
            [impl] {
                if (impl->power_manager && impl->power_manager->standby()) {
                    impl->power_manager->exit_standby();
                }
            },
            &impl_->wakelocks, impl_->notifications.get(), impl_->sensors.get(),
            impl_->power_monitor ? &impl_->power_monitor->supply() : nullptr,
            &impl_->system_settings.cell(), impl_->devices.get());
    }
    scene::SceneRuntime* scene_runtime = scenes != nullptr ? scenes->runtime() : nullptr;
    {
        App::Impl* impl = impl_.get();
        impl_->router = std::make_unique<NotificationRouter>(NotificationRouterDeps{
            .notes = *impl_->notifications,
            .power = impl_->power_manager.get(),
            .tones = impl_->tones.get(),
            .scenes = scene_runtime,
            .locks = &impl_->wakelocks,
            .notify_volume = [impl] { return notify_vol_of(impl->system_settings); },
            .ui = impl_->ui.get(),
            .reminders = impl_->reminders.get(),
            // The reminders' lane (see their submit_io above), so their
            // stop() waits for a todo action queued before it.
            .submit_io = impl_->reminders && scenes != nullptr
                             ? std::function<bool(std::function<void()>)>(
                                   [worker = &scenes->worker()](std::function<void()> job) {
                                       return worker->submit(std::move(job),
                                                             core::IoLane::Interactive);
                                   })
                             : nullptr,
            // The shell is built later, and goes before the router is
            // stopped; a post queued meanwhile finds it or nothing.
            .open_home =
                [impl] {
                    if (!impl->ui) return;
                    impl->ui->post([impl] {
                        if (impl->shell) impl->shell->show(shell::Page::Home);
                    });
                },
        });
        impl_->router->start();
    }

    // The agent, before networking so the admin routes can be handed it;
    // only with a screen (it asks there before irreversible steps) and a
    // scene runtime (most of its tools work on scenes).
    if (impl_->ui && impl_->system_layer && scene_runtime != nullptr &&
        scenes->repository() != nullptr) {
        build_agent(shape);
    }
    // The voice, with the agent it talks to.
    if (impl_->agent) {
        App::Impl* impl = impl_.get();
        impl_->voice = os::make_large<VoiceWiring>(VoiceWiringDeps{
            .bus = impl->bus,
            .registry = impl->registry,
            .models = impl->models,
            .settings = impl->system_settings,
            .agent = *impl->agent->service(),
            .locks = &impl->wakelocks,
            .tones = impl->tones.get(),
            .http = impl->platform.http_client.get(),
            // As the agent sees it: without a Wi-Fi service the host's
            // network is assumed.
            .network_up =
                [impl] {
                    std::optional<net::WifiState> st =
                        impl->net ? impl->net->wifi_state() : std::nullopt;
                    return !st || *st == net::WifiState::Connected;
                },
            // An atomic the "wisp.net" task writes each tick (network_up
            // reads the Wi-Fi cell): wisp.voice never calls into the
            // WifiService.
            .network_expected = [impl] { return impl->net && impl->net->network_expected(); },
            .screen_on = [impl] { return impl->power_manager && !impl->power_manager->standby(); },
            // App::stop() stops the voice before the buttons.
            .buttons = &impl->buttons,
        });
        if (!impl_->voice) WLOG_E("app", "no memory for the voice; voice is off");
    }

    // Step 4b: networking (connectivity spec 4.1), before the self-check
    // screen so that screen can show the network state. Never waits for
    // Wi-Fi -- see net_wiring.hpp.
    impl_->net = std::make_unique<NetWiring>(NetWiringDeps{
        .platform = impl_->platform,
        .registry = impl_->registry,
        .report = impl_->report,
        .bus = impl_->bus,
        .storage = impl_->storage,
        .models = impl_->models,
        .system_settings = impl_->system_settings,
        .power_monitor = impl_->power_monitor.get(),
        .power_manager = impl_->power_manager.get(),
        .wakelocks = &impl_->wakelocks,
        .scene_http = scenes != nullptr ? scenes->http() : nullptr,
        // Only a device with a screen can confirm a factory reset.
        .request_factory_reset =
            impl_->ui ? std::function<void()>([this] { request_factory_reset(*this); }) : nullptr,
        .firmware = impl_->firmware.get(),
        .scenes = scenes != nullptr ? scenes->repository() : nullptr,
        .scene_host = scene_runtime,
        // Deleting needs the device's yes, so a screen.
        .request_scene_delete = scene_runtime != nullptr
                                    ? std::function<Result<void>(const std::string&, bool)>(
                                          [this](const std::string& id, bool with_data) {
                                              return impl_->scenes->request_delete(id, with_data);
                                          })
                                    : nullptr,
        .occupancy = &impl_->occupancy,
        .files = scenes != nullptr ? scenes->files() : nullptr,
        .worker = storage_worker(),
        .agent = impl_->agent ? impl_->agent->api() : nullptr,
        .memory = impl_->agent ? impl_->agent->memory() : nullptr,
        .todos = impl_->reminders.get(),
        .now_ms = {},  // os::now_ms
        .submit_io = impl_->io_submitter(),
        .on_persist_failed = [impl = impl_.get()] { impl->report_persist_failed(); },
        .run_io =
            [impl = impl_.get()](std::function<void()> job) {
                return impl->run_io(std::move(job), os::kForever);
            },
        .reboot = impl_->platform.reboot ? std::function<void()>([this] {
            flush_storage();
            impl_->platform.reboot();
        })
                                         : nullptr,
    });
    impl_->net->start();
    if (impl_->agent) {
        impl_->agent->attach_ws(impl_->net->ws_hub());
        // Before agent->start(); agent->stop() releases it before
        // net.reset(). Without a WifiService, AgentDeps::network_up.
        net::WifiService* wifi = impl_->net->wifi();
        impl_->agent->service()->set_wifi(wifi != nullptr ? &wifi->wifi_cell() : nullptr);
    }

    // The capsule, the panel and the banners, once everything their
    // device_status reads (NetWiring::wifi_state()) is set.
    if (impl_->ui && impl_->system_layer) {
        App::Impl* impl = impl_.get();
        impl_->status_shell = os::make_large<StatusShell>(StatusShellDeps{
            .ui = *impl_->ui,
            .layer = *impl_->system_layer,
            .notes = *impl_->notifications,
            .router = *impl_->router,
            .scenes = scene_runtime,
            .device_status = std::move(device_status),
            .utc_offset_min = [impl] { return utc_offset_of(impl->system_settings); },
            .time_24h = [impl] { return time_24h_of(impl->system_settings); },
            .wall_s = [impl] { return impl->wall_now(); },
            .repo = scenes != nullptr ? scenes->repository() : nullptr,
            .submit_io = scenes != nullptr ? std::function<bool(std::function<void()>)>(
                                                 [scenes](std::function<void()> job) {
                                                     return scenes->worker().submit(std::move(job));
                                                 })
                                           : nullptr,
            // Both owners go only after status_shell->stop(), the first
            // step of App::stop().
            .power = impl_->power_manager ? &impl_->power_manager->level_cell() : nullptr,
            .wifi = impl_->net->wifi() != nullptr ? &impl_->net->wifi()->wifi_cell() : nullptr,
        });
        if (impl_->status_shell) {
            impl_->status_shell->start();
        } else {
            WLOG_E("app", "no memory for the status shell; no capsule or panel");
        }
    }
    // Once the router and the banners are up: what came due while the
    // device was off is reported now. reminders->stop() releases the
    // time-sync subscription before net.reset().
    if (impl_->reminders) {
        net::TimeSyncService* time = impl_->net->time_sync();
        impl_->reminders->start(time != nullptr ? &time->result_cell() : nullptr);
    }

    if (impl_->ui) {
        // The system duties, then the home screen (mvp spec 6.6), on the
        // UI thread. run_blocking() returns only once both have run, so a
        // stop() right after start() always finds them to tear down.
        App::Impl* impl = impl_.get();
        impl_->duties = std::make_unique<SystemDuties>(*this);
        impl_->shell = std::make_unique<shell::Shell>(shell::ShellDeps{
            .ui = *impl_->ui,
            .layer = *impl_->system_layer,
            .shape = shape,
            .bus = impl_->bus,
            .conversation = nullptr,  // set once the voice has started, below
            .open_diagnostics =
                [this, impl] {
                    start_self_check(*this, *impl->system_layer, impl->scenes.get(),
                                     impl->self_check);
                },
            .close_diagnostics = [impl] { stop_self_check(impl->self_check); },
            .diagnostics_end_room =
                [impl](int32_t room) { set_self_check_end_room(impl->self_check, room); },
            .leave_scene = [impl]() -> Result<bool> {
                scene::SceneRuntime* rt = impl->scenes ? impl->scenes->runtime() : nullptr;
                if (rt == nullptr) return Result<bool>::ok(false);
                std::optional<std::string> id = rt->foreground();
                if (!id) return Result<bool>::ok(false);
                // On the UI thread close() only queues the back.
                if (auto r = rt->close(*id); !r)
                    return Result<bool>::err(r.error().code, r.error().message);
                return Result<bool>::ok(true);
            },
            .admin_url = [this]() -> std::string {
                net::WifiService* w = wifi();
                if (w == nullptr) return {};
                const net::WifiStatus st = w->status();
                if (st.state != net::WifiState::Connected || st.ip.empty()) return {};
                return admin_url(st.ip, impl_->platform.http_port);
            },
            .now_ms = {},
            .agent = impl_->agent ? impl_->agent->service() : nullptr,
            // Built above; power_manager.reset() comes after shell->stop().
            .power = impl_->power_manager ? &impl_->power_manager->level_cell() : nullptr,
            // The runtime lives until scenes.reset(), after shell->stop().
            .scenes = scene_runtime != nullptr ? &scene_runtime->overview() : nullptr,
            .list_scenes = scenes != nullptr
                               ? std::function<shell::ShellListing(const std::function<bool()>&)>(
                                     [scenes](const std::function<bool()>& cancelled) {
                                         return scenes->shell_scenes(cancelled);
                                     })
                               : nullptr,
            .read_scene_icon =
                scenes != nullptr
                    ? std::function<std::shared_ptr<const os::LargeBuffer>(const std::string&)>(
                          [scenes](const std::string& id) { return scenes->shell_icon(id); })
                    : nullptr,
            .listing_current = scenes != nullptr ? std::function<bool()>([scenes] {
                return scenes->shell_listing_current();
            })
                                                 : nullptr,
            .scene_has_data = scenes != nullptr ? std::function<bool(const std::string&)>(
                                                      [scenes](const std::string& id) {
                                                          return scenes->scene_has_data(id);
                                                      })
                                                : nullptr,
            .open_scene =
                [impl](const std::string& id) {
                    scene::SceneRuntime* rt = impl->scenes ? impl->scenes->runtime() : nullptr;
                    if (rt == nullptr) return;
                    // On the UI thread launch() only queues.
                    if (auto r = rt->launch(id, scene::LaunchReason::User); !r)
                        WLOG_W("app", "scene %s not opened: %s", id.c_str(),
                               r.error().message.c_str());
                },
            .delete_scene = [impl](const std::string& id, bool with_data) -> Result<void> {
                if (!impl->scenes) return Result<void>::err(ErrorCode::Unsupported, "no scenes");
                return impl->scenes->ask_delete(id, with_data);
            },
            .recents = impl_->recents.get(),
            .settings = [impl] { return impl->system_settings.cell().get(); },
            .set_settings =
                [impl](std::map<std::string, Value> changes) {
                    if (auto r =
                            impl->system_settings.set(changes, core::SettingSurface::DevicePage);
                        !r)
                        WLOG_W("app", "setting not stored: %s", r.error().message.c_str());
                },
            .wifi_status = [this]() -> std::string {
                net::WifiService* w = wifi();
                if (w == nullptr) return ui::tr(ui::StrId::NetNoHardware);
                if (!w->enabled()) return ui::tr(ui::StrId::WifiOff);
                const std::vector<net::SavedNetwork> saved = w->saved_networks();
                const bool all_rejected =
                    !saved.empty() && std::all_of(saved.begin(), saved.end(),
                                                  [](const auto& n) { return n.auth_failed; });
                return wifi_status_text(w->status(), !saved.empty(), all_rejected, os::now_ms(),
                                        ui::lang());
            },
            .firmware_version = [impl] { return impl->platform.firmware_version; },
            .start_wifi_setup =
                [this] {
                    net::WifiService* w = wifi();
                    if (w == nullptr) return;
                    if (auto r = w->start_provisioning(os::now_ms()); !r)
                        WLOG_W("app", "provisioning not started: %s", r.error().message.c_str());
                },
            .submit_io = scenes != nullptr ? std::function<bool(std::function<void()>)>(
                                                 [scenes](std::function<void()> job) {
                                                     return scenes->worker().submit(std::move(job));
                                                 })
                                           : nullptr,
            // As the admin page's restart: what waits for flash first.
            .restart = impl_->platform.reboot ? std::function<void()>([this] {
                flush_storage();
                impl_->platform.reboot();
            })
                                              : nullptr,
            .power_off = impl_->duties->can_power_off()
                             ? std::function<void()>([impl] { impl->duties->power_off(); })
                             : nullptr,
            .wifi = wifi(),
            .models = &impl->models,
            .submit_network =
                [impl](std::function<void()> job) {
                    return impl->net && impl->net->submit_settings(std::move(job));
                },
        });
        impl_->ui->run_blocking([impl] {
            impl->duties->start();
            impl->shell->start();
        });
        // Only now: a scene's view remembers the screen it replaces and
        // loads it again when it goes, so that must be the home screen.
        if (scenes != nullptr) {
            // NetWiring, the time-sync cell's owner, is built after the
            // runtime and destroyed before it: stop_runtime() lets go first.
            if (scene::SceneRuntime* rt = scenes->runtime()) {
                net::TimeSyncService* sync = impl->net ? impl->net->time_sync() : nullptr;
                rt->attach_time_sync(sync != nullptr ? &sync->result_cell() : nullptr);
            }
            scenes->start_runtime(&impl->power_manager->level_cell());
        }
        // Once the runtime its tools drive runs.
        if (impl_->agent) {
            if (auto r = impl_->agent->start(); !r) {
                WLOG_E("app", "agent not started: %s", r.error().message.c_str());
                // Every transcript would be refused: the voice stays off,
                // and says so, rather than recording for nothing.
                if (impl_->voice) {
                    WLOG_W("app", "voice off: the agent is not running");
                    impl_->voice.reset();
                }
            }
        }
        // Once the agent takes text; without its tasks the voice is off.
        if (impl_->voice && !impl_->voice->start()) impl_->voice.reset();
        if (impl_->voice) {
            shell::ConversationController* conv = &impl_->voice->conversation();
            impl_->ui->run_blocking([impl, conv] { impl->shell->set_conversation(conv); });
        }
    }

    // boot_ms is recorded here -- right after Step 4, once the home
    // screen (when there is one) is up -- per spec 6.1's boot sequence,
    // which only *ends* at step ⑤ (the first screen going up).
    impl_->report.set_boot_ms(os::now_ms() - boot_t0);

    // Step 5: finalize and report.
    const std::string board_name = impl_->platform.board->name();
    // spec 9.2 ("--no-sd 时 Library 只有内置根"): surfaces the actual
    // root list, rather than leaving it something only a unit test can
    // see, so a smoke test can assert on it directly without depending
    // on whether a card happened to be registered.
    {
        wisp::List library_roots;
        for (const std::string& root : impl_->storage.roots(core::Area::Library)) {
            library_roots.emplace_back(root);
        }
        impl_->report.set_note("library_roots", Value(std::move(library_roots)));
    }
    impl_->platform.emit_line(
        "WISP_BOOT_REPORT " +
        to_json(impl_->report.to_value(board_name, impl_->platform.firmware_version)));
    impl_->platform.emit_line("WISP_MANIFEST " + to_json(impl_->registry.manifest(board_name)));

    impl_->bus.publish({core::topics::kSystemBootCompleted, "app", 0, {}});
    // Spec 3.6: the scenes that were in the background come back once boot
    // has completed (in the background, read and started off this task).
    if (impl_->scenes) impl_->scenes->restore_background();

    // Last, so a command never sees a half-built App.
    start_console(*this);

    // Step 7: this image booted all the way; a fresh update stays.
    impl_->firmware->mark_running_valid();

    impl_->started = true;
    return Result<void>::ok();
}

void App::stop() {
    // Reverse of start()'s step 3/4 order. Every call below is documented
    // as idempotent/safe even if the corresponding start() step never ran
    // (e.g. stop() called before start(), or called again after an
    // earlier stop()), so no `started` guard is needed here -- that flag
    // only gates start() itself.
    //
    // The shell (with the diagnostics, if open) and the system duties
    // are torn down before ButtonService/UiRuntime/EventBus -- see
    // app_impl.hpp's Lifetime note: they hold bus subscriptions and
    // references to `ui`/`registry`, so they must release those before
    // the things they reference go away. An in-flight "Mic test"
    // loopback task is joined before them; its status posts check that
    // the diagnostics are still up before touching them.
    // The console first: its commands reach the net services.
    stop_console(*this);
    // The capsule, the panel and the banners let go of the router and the
    // system layer.
    if (impl_->status_shell) {
        impl_->status_shell->stop();
        impl_->status_shell.reset();
    }
    // Nothing reacts to notifications any more (no wake, sound or scene).
    if (impl_->router) {
        impl_->router->stop();
        impl_->router.reset();
    }
    join_loopback(*this);
    // wisp.agent first: its tools drive the scene runtime, its dialogs sit
    // on the system layer, and it reaches flash through the storage worker
    // (a run_sync() after the worker stops would run inline on its stack,
    // which may be in PSRAM). The object stays until networking, whose
    // routes hold its api(), is gone.
    // The voice before the agent it submits to and listens to.
    if (impl_->voice) {
        if (impl_->ui && impl_->shell) {
            App::Impl* impl = impl_.get();
            impl_->ui->run_blocking([impl] { impl->shell->set_conversation(nullptr); });
        }
        impl_->voice->stop();
        impl_->voice.reset();
    }
    if (impl_->agent) impl_->agent->stop();
    // No more timer or pass on the worker; the router's queued actions,
    // already on the worker's queue, run before stop() returns.
    if (impl_->reminders) impl_->reminders->stop();
    // Before the shell: closing the scene in front loads the screen it
    // replaced again, and its final store flush goes to the worker.
    if (impl_->scenes) impl_->scenes->stop_runtime();
    if (impl_->ui && (impl_->shell || impl_->duties)) {
        App::Impl* impl = impl_.get();
        impl_->ui->run_blocking([impl] {
            if (impl->shell) impl->shell->stop();
            if (impl->duties) impl->duties->stop();
        });
    }
    impl_->shell.reset();
    impl_->duties.reset();
    // Closed with the shell; this catches one opened without it.
    stop_self_check(impl_->self_check);
    // After the duties, which close their dialogs through it.
    if (impl_->ui && impl_->system_layer) {
        impl_->ui->run_blocking([&] { impl_->system_layer.reset(); });
    }
    // A confirmed factory reset still running uses the bus, storage and
    // the net services; let it finish (it ends in Platform::reboot).
    join_factory_reset(*this);
    // Likewise a confirmed firmware update on its way to the restart.
    join_firmware_install(*this);

    // Networking next: the HTTP server's handlers and the "wisp.net" task
    // call into PowerManager (brightness), the net services and storage,
    // so both stop before any of those go away.
    if (impl_->net) {
        impl_->net->stop();
        impl_->net.reset();
    }
    // The reset status outlives this run (it lives on Impl), and the next
    // start's duties would show a request still in it: with the duties
    // gone, the reset task joined and no route left to ask, it goes back
    // to Idle (a new seq; the mirror publishes nothing for Idle).
    impl_->reset_status.update([](ResetStatus& s) {
        s.phase = ResetStatus::Phase::Idle;
        ++s.seq;
    });
    // Scene lifecycle edges: the gate closes (a push in progress finishes
    // first), then the inbox goes (a drain in progress finishes first), so
    // the handler never reaches the agent reset next. Before the
    // notifications close and the scene order's last flush, too. The
    // scene task has stopped; a delete still queued on wisp.fs (until
    // scenes->stop()) finds the gate closed and its edge is dropped.
    {
        std::lock_guard<os::Mutex> lock(impl_->lifecycle_gate.mu);
        impl_->lifecycle_gate.inbox = nullptr;
    }
    impl_->lifecycle.reset();
    // Stopped above; nothing holds its api() any more.
    impl_->agent.reset();
    // The agent's tools and the router (both gone) were its last users.
    impl_->reminders.reset();
    // Nothing streams into it any more.
    impl_->firmware.reset();
    // Notifications: nothing new from the supply cell or the lifecycle
    // inbox (gone above), no more saves on the timer,
    // then whatever is still unsaved goes to the worker before it stops.
    if (impl_->notifications) {
        impl_->supply_sub.reset();
        impl_->notifications->close();
        if (!impl_->notifications->flush(kNotifyFlushMs)) {
            WLOG_W("app", "notifications not saved before shutdown");
        }
    }
    // The scene order too; nothing feeds it any more.
    if (impl_->recents && impl_->scenes) {
        // Logged on the worker: past the timeout the job may still run.
        shell::SceneRecents* recents = impl_->recents.get();
        auto ran = impl_->scenes->worker().run_sync(
            [recents] {
                if (auto r = recents->flush(); !r)
                    WLOG_W("app", "scene order not saved: %s", r.error().message.c_str());
            },
            kSettingsFlushMs);
        if (!ran) WLOG_W("app", "scene order not saved: %s", ran.error().message.c_str());
    }
    // Settings changed so far go to the worker while it still runs.
    if (auto r = impl_->settings_front.flush(kSettingsFlushMs); !r) {
        WLOG_W("app", "settings not saved before shutdown: %s", r.error().message.c_str());
    }
    // The storage-refresh timer queues onto the worker stopped below.
    if (impl_->storage_timer) {
        impl_->storage_timer->stop();
        impl_->storage_timer.reset();
    }
    // Nothing calls into the scenes any more: the proxy, then the worker
    // (drained, so the last scene's flush is on disk).
    if (impl_->scenes) {
        impl_->scenes->stop();
        // The cache points at the worker: from here on Settings go to the
        // platform backend, once whatever was changed meanwhile is written
        // (inline now that the worker has stopped).
        if (auto r = impl_->settings_front.detach_and_flush(); !r) {
            WLOG_W("app", "settings not saved: %s", r.error().message.c_str());
        }
        impl_->settings_front.set_worker(nullptr);
        impl_->settings_cache.reset();
        impl_->scenes.reset();
    }
    // Its last reader, the shell's listing task, is gone.
    impl_->recents.reset();

    // Nothing subscribes any more: the scene runtime and the self-check
    // screen are gone.
    impl_->sensors_power_sub.reset();
    if (impl_->sensors) {
        impl_->sensors->stop();
        impl_->sensors.reset();
    }
    // Its last callers, the agent tools and the scenes, are gone.
    impl_->devices_power_sub.reset();
    impl_->devices.reset();

    // PowerManager is torn down next -- after the self-check screen
    // (which calls into it), before buttons/ui/bus (which it in turn
    // holds pointers/a reference into) are themselves stopped/reset.
    if (impl_->power_timer) {
        impl_->power_timer->stop();
        impl_->power_timer.reset();
    }
    // If stop() happens while in standby, leave standby first: buttons/
    // storage/display are members of Impl (buttons, storage) or held by
    // shared_ptr past this App's lifetime (display), so without this, a
    // later start() on the very same App instance (App supports
    // start()/stop()/start() -- see reset_loopback()'s own doc comment)
    // would resume with the screen still asleep, SD probing still
    // paused, and button input / power monitoring still in standby mode.
    //
    // The standby() check below must run on the UI task, not here: the
    // power_timer tick that was in flight (or already queued onto the UI
    // task, via ui_->post([this] { enter_standby_if_still_idle(...); })
    // in power_manager.cpp) when power_timer->stop() above returned may
    // not have run yet, so standby() read from this (stop()'s own)
    // thread could still say false even though that queued tick is
    // about to flip it true. Checking it here and skipping the post
    // below when it reads false would race: that queued tick would then
    // run anyway (UiRuntime's post queue is FIFO, so strictly before
    // ~PowerManager()'s own post() a few lines down), leaving the device
    // in standby with nothing left to undo it, since power_manager.reset()
    // destroys the only thing that could. Posting unconditionally and
    // checking standby() from *inside* the posted job instead sidesteps
    // the race entirely: FIFO guarantees that job runs after any
    // already-queued enter_standby_if_still_idle(), so it always sees
    // the final state.
    if (impl_->power_manager) {
        PowerManager* pm = impl_->power_manager.get();
        if (impl_->ui) {
            impl_->ui->run_blocking([pm] {
                if (pm->standby()) {
                    pm->exit_standby();
                }
            });
        } else if (pm->standby()) {
            pm->exit_standby();
        }
    }
    impl_->power_manager.reset();
    // After power_manager, whose activity hook calls it.
    impl_->tones.reset();
    // power_timer, which posts reminders, has stopped.
    impl_->notifications.reset();

    // power_monitor is torn down right after power_manager
    // (which points into it, like storage/storage_timer below) -- its
    // own stop() tears down its internal Timer; see power_monitor.hpp.
    impl_->power_monitor.reset();

    // State-cell, inbox and button-listener owners rely on this order:
    // voice (its talk key) -> shell -> duties -> power manager -> buttons
    // -> ui -> bus, so button listeners are removed before ButtonService
    // stops, UI-dispatched subscriptions and inboxes are released before
    // the UI runtime goes and cells and inboxes before the bus
    // (state_cell.hpp).
    impl_->buttons.stop();
    if (impl_->ui) {
        impl_->ui->stop();
        impl_->ui.reset();
    }
    impl_->bus.stop();

    impl_->started = false;
}

void App::build_agent(const hal::ScreenShape& shape) {
    App::Impl* impl = impl_.get();
    SceneWiring& scenes = *impl->scenes;
    auto system = impl->storage.write_root(core::Area::System);
    if (!system) {
        WLOG_W("app", "no System storage for conversations; the agent is off");
        return;
    }
    // Everything below runs on wisp.agent (or an HTTP thread) and reads only
    // what other tasks keep up to date in memory: never Settings, a file or
    // a device.
    auto wall_s = [impl] {
        const int64_t t = impl->wall_now();
        return t >= scene::MinuteTicker::kValidSince ? t : int64_t{0};
    };
    auto wifi = [impl]() -> std::string {
        std::optional<net::WifiState> st = impl->net ? impl->net->wifi_state() : std::nullopt;
        return st ? std::string(net::to_string(*st)) : std::string();
    };
    scene::SceneRuntime* runtime = scenes.runtime();
    agent::ContextSource context = [impl, shape, wall_s, wifi, runtime](agent::Source source) {
        agent::DeviceSnapshot d;
        d.lang = ui::lang_code(lang_of(impl->system_settings));
        d.shape = shape;
        d.foreground = runtime->foreground().value_or(std::string());
        for (const scene::BackgroundScene& b : runtime->background_scenes()) {
            d.background.push_back(b.id);
        }
        if (impl->power_monitor) {
            if (auto st = impl->power_monitor->last_status()) {
                d.battery = st->battery_percent;
                d.charging = st->charging;
            }
        }
        d.wifi = wifi();
        d.wall_s = wall_s();
        d.utc_offset_min = utc_offset_of(impl->system_settings);
        d.weekday = d.wall_s != 0 ? agent::weekday_of(d.wall_s, d.utc_offset_min) : 0;
        // Memory notes come with the memory service; none until then.
        d.source = source;
        return d;
    };
    auto device_status = [impl, shape, wifi]() {
        Map out;
        Value battery, charging;
        if (impl->power_monitor) {
            if (auto st = impl->power_monitor->last_status()) {
                battery = Value(st->battery_percent);
                charging = Value(st->charging);
            }
        }
        out["battery"] = std::move(battery);
        out["charging"] = std::move(charging);
        const std::string w = wifi();
        out["wifi"] = w.empty() ? Value() : Value(w);
        out["power"] =
            impl->power_manager ? Value(hal::to_string(impl->power_manager->level())) : Value();
        List occupancy;
        for (const core::OccupancyEntry& e : impl->occupancy.list()) {
            occupancy.push_back(Value(Map{{"device", Value(e.device)},
                                          {"holder", Value(e.holder)},
                                          {"mode", Value(core::to_string(e.mode))}}));
        }
        out["occupancy"] = Value(std::move(occupancy));
        const char* kind = shape.kind == hal::ScreenShapeKind::Circle        ? "circle"
                           : shape.kind == hal::ScreenShapeKind::RoundedRect ? "rounded_rect"
                                                                             : "rect";
        out["shape"] = Value(kind);
        const hal::Area safe = hal::safe_rect(shape);
        out["safe_area"] = Value(Map{{"x1", Value(static_cast<int64_t>(safe.x1))},
                                     {"y1", Value(static_cast<int64_t>(safe.y1))},
                                     {"x2", Value(static_cast<int64_t>(safe.x2))},
                                     {"y2", Value(static_cast<int64_t>(safe.y2))}});
        return Value(std::move(out));
    };
    impl->agent = os::make_large<AgentWiring>(AgentWiringDeps{
        .bus = impl->bus,
        .storage = impl->storage,
        .worker = scenes.worker(),
        .models = impl->models,
        .settings = impl->system_settings,
        .notes = *impl->notifications,
        .reminders = impl->reminders.get(),
        .locks = &impl->wakelocks,
        .ui = *impl->ui,
        .layer = *impl->system_layer,
        .shape = shape,
        .runtime = *runtime,
        .repo = *scenes.repository(),
        .files = scenes.files(),
        .scene_http = scenes.http(),
        .conversation_dir = system.value() + "/agent",
        .memory_path = system.value() + "/memory/notes.json",
        .drafts_root = scenes.data_root() + "/.drafts",
        .http = impl->platform.http_client.get(),
        .run_io =
            [impl](std::function<void()> job) {
                return impl->run_io(std::move(job), os::kForever);
            },
        .run_io_interactive =
            [impl](std::function<void()> job) {
                return impl->run_io(std::move(job), os::kForever, App::Impl::kIoBusyBudgetMs,
                                    core::IoLane::Interactive);
            },
        .delete_scene =
            [impl](const std::string& id, bool with_data, std::function<void(Result<void>)> done) {
                impl->scenes->delete_scene(id, with_data, std::move(done));
            },
        .wake =
            [impl] {
                if (impl->power_manager && impl->power_manager->standby()) {
                    impl->power_manager->exit_standby();
                }
            },
        .screen_off = [impl] { return impl->power_manager && impl->power_manager->standby(); },
        // Asked only while the agent has no Wi-Fi cell (see set_wifi() in
        // App::start()): without a Wi-Fi service (the simulator without
        // one) the host's network is assumed.
        .network_up =
            [impl] {
                std::optional<net::WifiState> st =
                    impl->net ? impl->net->wifi_state() : std::nullopt;
                return !st || *st == net::WifiState::Connected;
            },
        .lang = [impl] { return std::string(ui::lang_code(lang_of(impl->system_settings))); },
        .wall_s = wall_s,
        .context = std::move(context),
        .devices = impl->devices.get(),
        .device_status = std::move(device_status),
    });
    if (!impl->agent) WLOG_E("app", "no memory for the agent; the agent is off");
}

bool App::Impl::submit_io(std::function<void()> job) {
    core::StorageWorker* worker = scenes ? &scenes->worker() : nullptr;
    if (worker == nullptr || worker->on_worker()) {
        job();
        return true;
    }
    if (worker->submit(job)) return true;
    // Not running (boot or shutdown): run_sync() runs it right here. Timeout
    // means it got queued after all and runs later; Busy that it never will.
    Result<void> r = worker->run_sync(std::move(job), 0);
    return r || r.error().code == ErrorCode::Timeout;
}

Result<void> App::Impl::run_io(std::function<void()> job, uint32_t timeout_ms,
                               uint32_t busy_budget_ms, core::IoLane lane) {
    core::StorageWorker* worker = scenes ? &scenes->worker() : nullptr;
    if (worker == nullptr) {
        job();
        return Result<void>::ok();
    }
    // A full lane drains within a few jobs; a worker that is stopping
    // stays Busy until it has stopped, and then runs it inline.
    Result<void> r = worker->run_sync(job, timeout_ms, lane);
    for (uint32_t waited = 0; !r && r.error().code == ErrorCode::Busy && waited < busy_budget_ms;
         waited += kIoBusyPollMs) {
        os::sleep_ms(kIoBusyPollMs);
        r = worker->run_sync(job, timeout_ms, lane);
    }
    return r;
}

void App::Impl::report_persist_failed() {
    // Built before anything saves; reset only once the worker has stopped.
    if (notifications) notifications->post_system(core::notice::kSettingsNotSaved, "");
}

void App::flush_storage() {
    if (auto r = impl_->settings_front.flush(kShutdownSettingsFlushMs); !r) {
        WLOG_W("app", "settings not saved before going down: %s", r.error().message.c_str());
    }
    // Both workers: card work queued on wisp.sd.io is written too.
    core::StorageRouter* router = storage_router();
    if (router != nullptr && !router->drain(kShutdownDrainMs)) {
        WLOG_W("app", "storage work still queued (%u jobs); going down anyway",
               static_cast<unsigned>(router->pending()));
    }
}

bool queue_storage_refresh(App& app) {
    App::Impl* impl = app.impl_.get();
    if (impl->refresh_pending.exchange(true)) return false;
    std::function<void()> job = [impl] {
        impl->storage.refresh();
        impl->refresh_pending.store(false);
    };
    // Card work (it takes only the card's I/O lock, and may deinit a
    // pulled card): on wisp.sd.io when it runs, else on wisp.fs.
    core::StorageRouter* router = app.storage_router();
    if (router == nullptr) {
        job();
        return true;
    }
    if (router->submit(core::Medium::Sd, job)) return true;
    // Not running (boot or shutdown): run_sync() runs it right here. Timeout
    // means it got queued after all and runs later; Busy that it never will.
    Result<void> r = router->run_sync(core::Medium::Sd, std::move(job), 0);
    if (r || r.error().code == ErrorCode::Timeout) return true;
    impl->refresh_pending.store(false);
    return false;
}

void sample_power_now(App& app) {
    if (app.impl_->power_monitor) app.impl_->power_monitor->sample();
}

core::DeviceRegistry& App::registry() { return impl_->registry; }
core::BootReport& App::report() { return impl_->report; }
core::EventBus& App::bus() { return impl_->bus; }
core::StorageManager& App::storage() { return impl_->storage; }
core::ButtonService& App::buttons() { return impl_->buttons; }
ui::UiRuntime* App::ui() { return impl_->ui.get(); }
ui::SystemLayer* App::system_layer() { return impl_->system_layer.get(); }
scene::SceneRuntime* App::scenes() { return impl_->scenes ? impl_->scenes->runtime() : nullptr; }
core::SceneRepository* App::scene_repository() {
    return impl_->scenes ? impl_->scenes->repository() : nullptr;
}
PowerManager* App::power_manager() { return impl_->power_manager.get(); }
core::WakeLockRegistry& App::wakelocks() { return impl_->wakelocks; }
core::NotificationService* App::notifications() { return impl_->notifications.get(); }
core::ReminderScheduler* App::reminders() { return impl_->reminders.get(); }
core::OccupancyRegistry& App::occupancy() { return impl_->occupancy; }
core::SensorService* App::sensors() { return impl_->sensors.get(); }
core::DeviceService* App::devices() { return impl_->devices.get(); }
agent::AgentService* App::agent() { return impl_->agent ? impl_->agent->service() : nullptr; }
const agent::ToolRegistry* App::agent_tools() const {
    return impl_->agent ? impl_->agent->tools() : nullptr;
}
core::MemoryStore* App::memory() { return impl_->agent ? impl_->agent->memory() : nullptr; }
shell::ConversationController* App::conversation() {
    return impl_->voice ? &impl_->voice->conversation() : nullptr;
}
shell::Shell* App::shell() { return impl_->shell.get(); }
core::StorageWorker* App::storage_worker() {
    return impl_->scenes ? &impl_->scenes->worker() : nullptr;
}
core::StorageRouter* App::storage_router() {
    return impl_->scenes ? &impl_->scenes->router() : nullptr;
}
net::WifiService* App::wifi() { return impl_->net ? impl_->net->wifi() : nullptr; }
net::PairingService* App::pairing() { return impl_->net ? impl_->net->pairing() : nullptr; }
net::TimeSyncService* App::time_sync() { return impl_->net ? impl_->net->time_sync() : nullptr; }
core::ModelProfileService& App::models() { return impl_->models; }
core::SystemSettings& App::system_settings() { return impl_->system_settings; }
const Platform& App::platform() const { return impl_->platform; }

std::string App::dispatch_report() const {
    std::string report = "bus dropped=" + std::to_string(impl_->bus.dropped()) +
                         " cell_subs=" + std::to_string(impl_->bus.cell_subscriptions());
    // One line per inbox. The scene lifecycle inbox, the duties and the
    // voice are built before the console task starts and destroyed after
    // it stops (start(), stop()), so the console's `mem` never races them.
    if (impl_->lifecycle) {
        report +=
            "\ninbox scene_lifecycle overflows=" + std::to_string(impl_->lifecycle->overflows());
    }
    if (impl_->duties) {
        report +=
            "\ninbox power_key overflows=" + std::to_string(impl_->duties->power_key_overflows());
    }
    if (impl_->voice) {
        if (std::optional<uint32_t> n = impl_->voice->talk_key_overflows()) {
            report += "\ninbox talk_key overflows=" + std::to_string(*n);
        }
    }
    return report;
}

}  // namespace wisp::app
