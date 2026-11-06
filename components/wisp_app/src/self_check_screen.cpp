// The self-check screen (spec 8): page framing, lifecycle, bus
// subscriptions and timers. See self_check_screen.hpp for how the class is
// split over files, and self_check.hpp for how App::start()/App::stop()
// own this object's lifetime.

#include "self_check_screen.hpp"

#include <algorithm>
#include <memory>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "app_impl.hpp"
#include "self_check.hpp"
#include "self_check_layout.hpp"
#include "wisp/app/app.hpp"
#include "wisp/app/power_manager.hpp"
#include "wisp/base/json.hpp"
#include "wisp/core/log.hpp"
#include "wisp/core/system_settings.hpp"
#include "wisp/net/pairing_service.hpp"
#include "wisp/net/time_sync.hpp"
#include "wisp/net/wifi_service.hpp"
#include "wisp/scene/scene_runtime.hpp"
#include "wisp/ui/strings.hpp"
#include "wisp/ui/system_layer.hpp"
#include "wisp/ui/ui_runtime.hpp"

namespace wisp::app {

namespace {

constexpr uint32_t kUiRefreshMs = 100;
// How often the refresh timer re-reads the network services (and the
// pending factory-reset and firmware requests) even without an event: the
// bus drops its oldest events when full, and a lost net.ap.state,
// net.pairing.code, system.factory_reset or system.firmware must not leave
// a card up (or missing) for good.
// The reads are a few short lock-protected copies and an atomic.
constexpr uint64_t kNetResyncMs = 1000;
constexpr uint32_t kPerfReportMs = 5000;
// How often the scene list is read again without a scene.state event, for
// one the bus dropped. Each is a walk of the scene packages on the storage
// worker, so far apart; a state change shows within one refresh anyway
// (SceneSection::update_states()).
constexpr uint64_t kSceneResyncMs = 30000;
// How often storage is measured, on the storage worker. A request whose
// answer never came (the UI queue was full) is given up after
// kStorageRetryMs.
constexpr uint64_t kStorageResyncMs = 1000;
constexpr uint64_t kStorageRetryMs = 10000;
// Between the root column's safe-area padding and its content, and
// between its children.
constexpr int32_t kInnerPad = 8;
constexpr int32_t kSectionGap = 10;

}  // namespace

SelfCheck::SelfCheck(App& app, ui::SystemLayer& system, SceneWiring* scenes)
    : app_(app),
      ui_(app.ui()),
      system_(system),
      display_(app.registry().get<hal::Display>("display.main")),
      power_(app.registry().get<hal::Power>("power.main")),
      clock_(app.registry().get<hal::Clock>("clock.main")),
      ui_settings_(*app.platform().settings, "ui"),
      net_section_(app.wifi()),
      setup_card_([this] {
          if (net::WifiService* wifi = app_.wifi()) {
              if (auto r = wifi->stop_provisioning(); !r) {
                  WLOG_W("self_check", "stopping the setup hotspot failed: %s",
                         r.error().message.c_str());
              }
          }
      }),
      pair_card_([this] {
          if (net::PairingService* pairing = app_.pairing()) pairing->cancel_code();
      }),
      scenes_(scenes),
      sampler_(app.sensors(), power_, clock_) {}

SelfCheck::~SelfCheck() {
    // Unsubscribe from "power.state" before anything else, so the
    // sampler's standby flag can't change from this point on --
    // EventBus::unsubscribe()'s own contract guarantees the handler never
    // runs again once this returns.
    if (power_sub_ != 0) {
        app_.bus().unsubscribe(power_sub_);
        power_sub_ = 0;
    }
    // A language change already posted by on_settings_event() still runs
    // before the UI-thread teardown at the end of this destructor (the post
    // queue is FIFO), while every dialog it reopens is still alive.
    if (settings_sub_ != 0) {
        app_.bus().unsubscribe(settings_sub_);
        settings_sub_ = 0;
    }
    // Same for the network and factory-reset syncs: whatever they already
    // posted runs before the teardown below.
    if (net_sub_ != 0) {
        app_.bus().unsubscribe(net_sub_);
        net_sub_ = 0;
    }
    if (reset_sub_ != 0) {
        app_.bus().unsubscribe(reset_sub_);
        reset_sub_ = 0;
    }
    if (fw_sub_ != 0) {
        app_.bus().unsubscribe(fw_sub_);
        fw_sub_ = 0;
    }
    if (scene_sub_ != 0) {
        app_.bus().unsubscribe(scene_sub_);
        scene_sub_ = 0;
    }

    // Thread-agnostic: nothing the sampler touches requires the UI thread.
    sampler_.stop();

    if (perf_timer_) {
        perf_timer_->stop();
        perf_timer_.reset();
    }

    if (button_sub_ != 0) {
        app_.bus().unsubscribe(button_sub_);
        button_sub_ = 0;
    }

    // Give PowerManager's generic "any button wakes standby" handler back
    // ownership of "button.power" now that this screen's own
    // PowerKeyController (the thing set_power_key_owned(true) in
    // subscribe_buttons() handed it off to) is going away -- see
    // PowerManager::set_power_key_owned()'s doc comment. A no-op today
    // (App::stop() destroys PowerManager right after this object), but a
    // future scene that can replace the self-check screen without also
    // tearing down PowerManager would otherwise be left with "button.power"
    // permanently un-owned: PowerManager skips it forever, and nothing else
    // picks it back up, so the power key would stop waking the device from
    // standby.
    if (PowerManager* pm = app_.power_manager()) {
        pm->set_power_key_owned(false);
    }

    // LVGL-specific teardown must run on the UI thread -- see
    // UiRuntime::run_blocking(). If `ui_` is already gone (shouldn't
    // happen given app.cpp's teardown order, but safe either way),
    // there's nothing left to tear down: UiRuntime::stop()'s lv_deinit()
    // already released every lv_obj_t/lv_timer_t this screen created.
    if (ui_ == nullptr) {
        return;
    }
    ui_->run_blocking([this] {
        *alive_ = false;
        close_confirm_dialog();
        close_reset_dialog();
        close_firmware_dialog();
        // The cards live on the top layer, outside root_.
        setup_card_.close();
        pair_card_.close();
        if (refresh_timer_ != nullptr) {
            lv_timer_delete(refresh_timer_);
            refresh_timer_ = nullptr;
        }
        if (root_ != nullptr) {
            // See root_'s doc comment.
            lv_obj_delete(root_);
            root_ = nullptr;
        }
    });
}

void SelfCheck::build() {
    build_widgets();

    sampler_.start();
    start_perf_timer();
    subscribe_buttons();
    subscribe_power();
    subscribe_settings();
    subscribe_net();
    // Every scene.state (installed, opened, closed, failed, deleted) lists
    // the scenes again, and says whether a scene now covers this screen.
    scene_sub_ = app_.bus().subscribe("scene.state", [this](const core::Event&) {
        request_scene_list();
        follow_foreground();
    });
    follow_foreground();
    request_scene_list();
    last_scene_list_ms_ = os::now_ms();
    request_storage();
    // Anything the net services announced before the subscription above
    // (the build runs after networking has started).
    sync_wifi();
    sync_pairing();
    sync_reset();
    sync_firmware();

    refresh_timer_ = lv_timer_create(&SelfCheck::refresh_timer_cb, kUiRefreshMs, this);

    overlay_enabled_ = ui_settings_.get_bool("perf_overlay", false);
    if (overlay_enabled_) {
        ui_->set_perf_overlay(true);
    }
}

void SelfCheck::build_widgets() {
    if (screen_ == nullptr) screen_ = lv_screen_active();
    lv_obj_t* col = lv_obj_create(screen_);
    root_ = col;
    // No theme styles at all: LVGL sets LV_STATE_SCROLLED on the column
    // when a scroll begins and clears it when it ends, and with the theme's
    // state-dependent styles on it that re-styles (and so redraws) every
    // label on the page, twice per scroll, including the parts under an
    // opaque card that wisp/ui/overlay.hpp would otherwise skip.
    lv_obj_remove_style_all(col);
    // The root column fills the whole screen with no background or radius
    // of its own, so the screen reads as one uniform surface edge to
    // edge; content is kept off the invisible parts purely by padding
    // (spec 5.6). Each side pads by that side's safe inset plus kInnerPad,
    // so: horizontally nothing ever leaves the safe columns, and vertically
    // rows may scroll through the top/bottom bands (corners) but at either
    // scroll extreme the first/last row rests fully inside the safe area.
    hal::Area safe = compute_safe_area();
    int32_t screen_w = lv_display_get_horizontal_resolution(nullptr);
    int32_t screen_h = lv_display_get_vertical_resolution(nullptr);
    lv_obj_set_pos(col, 0, 0);
    lv_obj_set_size(col, screen_w, screen_h);
    lv_obj_set_style_pad_left(col, safe.x1 + kInnerPad, 0);
    lv_obj_set_style_pad_right(col, screen_w - 1 - safe.x2 + kInnerPad, 0);
    lv_obj_set_style_pad_top(col, safe.y1 + kInnerPad, 0);
    lv_obj_set_style_pad_bottom(col, screen_h - 1 - safe.y2 + kInnerPad, 0);
    lv_obj_set_style_pad_row(col, kSectionGap, 0);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(col, LV_DIR_VER);
    // No scrollbar: on a rounded or round screen a bar hugging the right
    // edge would sit under/near the corners.
    lv_obj_set_scrollbar_mode(col, LV_SCROLLBAR_MODE_OFF);

    header_.build(col, text_, [this] { (void)devices_.scroll_to_first_problem(); });
    const Platform& platform = app_.platform();
    board_.build(
        col, text_,
        BoardSection::Info{platform.board->name(), platform.target, platform.firmware_version},
        [this](ui::Lang lang) { set_page_lang(lang); });
    board_.set_boot_ms(boot_ms());
    build_devices(col);
    lv_obj_t* net_card = make_card(col, ui::StrId::SectionNetwork, text_);
    net_section_.build(net_card, text_, os::now_ms());
    sensors_.build(col, text_, sampler_.has_imu());
    power_clock_.build(col, text_, power_ != nullptr, clock_ != nullptr);
    storage_.build(col, text_);
    tests_section_.build(col, text_,
                         TestSection::Callbacks{
                             [this] { pass_test(InteractiveTest::Touch); },
                             [this] { start_loopback(); },
                         },
                         loop_status_);
    scenes_section_.build(
        col, text_,
        [this](const std::string& id) {
            scene::SceneRuntime* rt = app_.scenes();
            return rt != nullptr ? rt->status(id) : core::SceneStatus{};
        },
        [this](const std::string& id) { open_scene(id); });
    perf_.build(col, text_, [this] { toggle_perf_overlay(); });

    header_.show_devices(devices_.summary());
    header_.show_tests(tests_.passed_count());
    tests_section_.show(tests_);
}

void SelfCheck::build_devices(lv_obj_t* col) {
    std::vector<std::string> ids;
    for (const auto& entry : app_.report().entries()) {
        if (std::find(ids.begin(), ids.end(), entry.id) == ids.end()) {
            ids.push_back(entry.id);
        }
    }

    std::vector<DeviceRowInfo> rows;
    rows.reserve(ids.size());
    sd_row_live_ = false;
    for (const auto& id : ids) {
        auto found = app_.report().find(id);
        auto device = app_.registry().find(id);
        DeviceRowInfo row;
        row.id = id;
        row.model = device ? device->info().model : std::string("-");
        row.state = found ? found->state : core::DeviceState::Missing;
        row.reason = found ? found->reason : std::string();
        rows.push_back(std::move(row));

        if (id == "storage.sd" && device != nullptr) {
            // BootReport only ever reflects whether the card was present
            // at boot (spec 6.1: devices aren't re-registered after
            // init()), so this row would otherwise keep showing a missing
            // card forever once one is inserted later -- show_storage()
            // (driven by the live StorageManager::status()) follows it
            // instead. Only when the device actually exists in the
            // registry: a "storage.sd" boot-report entry with no backing
            // device (e.g. `--disable storage.sd`) is a real, permanent
            // Failed/Missing that StorageManager::status() can't ever
            // contradict (it simply won't have an entry for it either),
            // so that original state must stick.
            sd_row_live_ = true;
        }
    }
    devices_.build(col, text_, std::move(rows));
}

uint64_t SelfCheck::boot_ms() const {
    Value rep = app_.report().to_value(app_.platform().board->name());
    const Value* v = rep.find("boot_ms");
    return v != nullptr ? static_cast<uint64_t>(v->as_int()) : 0;
}

void SelfCheck::refresh_header() { board_.set_boot_ms(boot_ms()); }

hal::Area SelfCheck::compute_safe_area() const { return hal::safe_rect(system_.shape()); }

void SelfCheck::start_perf_timer() {
    perf_timer_ = std::make_unique<os::Timer>("wisp.perf", kPerfReportMs, /*periodic=*/true,
                                              [this] { emit_perf_line(); });
    perf_timer_->start();
}

void SelfCheck::emit_perf_line() {
    ui::UiPerf perf = ui_->perf();

    Value heap = app_.platform().heap_info ? app_.platform().heap_info() : Value();
    int64_t heap_internal_free = 0;
    int64_t heap_large_free = 0;
    if (const Value* v = heap.find("internal_free")) {
        heap_internal_free = v->as_int();
    }
    if (const Value* v = heap.find("large_free")) {
        heap_large_free = v->as_int();
    }

    Map payload{
        {"fps", static_cast<double>(perf.fps)},
        {"cpu", static_cast<int64_t>(perf.cpu_percent)},
        {"flush_ms", static_cast<double>(perf.avg_flush_ms)},
        {"frames", static_cast<int64_t>(perf.frames)},
        {"heap_internal_free", heap_internal_free},
        {"heap_large_free", heap_large_free},
    };
    app_.platform().emit_line("WISP_PERF " + to_json(Value(payload)));
}

void SelfCheck::subscribe_buttons() {
    button_sub_ =
        app_.bus().subscribe("button.", [this](const core::Event& e) { on_button_event(e); });

    // From here on, this screen's own PowerKeyController is the *only*
    // thing that decides what a "button.power" event does to the screen --
    // PowerManager's own generic "any button wakes standby" handler
    // (power_manager.cpp) must leave button.power alone, or the two could
    // each independently decide to flip the screen for the very same
    // physical press (one reacting to `down`, the other -- with its own,
    // now-stale idea of "is the screen on" -- reacting to the
    // `short`/`long` that follows it), producing a wake-then-immediately-
    // resleep flash. See PowerManager::set_power_key_owned()'s doc comment.
    if (PowerManager* pm = app_.power_manager()) {
        pm->set_power_key_owned(true);
    }
}

void SelfCheck::on_button_event(const core::Event& e) {
    // on_button_event() itself runs on the EventBus dispatcher thread, but
    // PowerKeyController is not thread-safe against concurrent callers --
    // on_power_off_answer() also drives it, from the UI thread, via
    // on_confirm(). Posting the *whole* decision (on_short()/on_long()
    // included, not just the resulting Action) onto the UI thread is what
    // makes every PowerKeyController call happen on one single thread.
    std::string topic = e.topic;
    std::string source = e.source;
    // A dropped post() here silently loses whatever power-key action
    // (screen off/on) this event was about to decide -- log it so that
    // isn't invisible (see event_bus.hpp for why this must not block
    // waiting for a slot instead).
    if (!ui_->post([this, topic, source] {
            tests_section_.set_button_event(topic);
            pass_test(InteractiveTest::Button);

            PowerKeyController::Action action = PowerKeyController::Action::None;
            if (source == "button.power") {
                // PowerManager::standby() -- not any state
                // PowerKeyController tracks itself -- is the single source
                // of truth for "is the screen on"; read it fresh right
                // before deciding, on this same UI thread (the only thread
                // that ever calls into PowerManager's enter_standby()/
                // exit_standby() on this screen's behalf, so there is no
                // race between this read and handle_power_key_action()
                // below acting on the result).
                PowerManager* pm = app_.power_manager();
                bool screen_on = pm == nullptr || !pm->standby();
                if (topic == "button.power.short") {
                    action = power_key_.on_short(screen_on);
                } else if (topic == "button.power.long") {
                    action = power_key_.on_long(screen_on);
                }
            }
            if (action != PowerKeyController::Action::None) {
                handle_power_key_action(action);
            }
        })) {
        WLOG_W("self_check", "post(power key action) dropped (post queue full)");
    }
}

void SelfCheck::subscribe_power() {
    // Lets the sampler (running on its own thread, not the UI task) stop
    // polling the IMU while in standby without reaching into PowerManager
    // itself.
    power_sub_ =
        app_.bus().subscribe("power.state", [this](const core::Event& e) { on_power_event(e); });
}

void SelfCheck::on_power_event(const core::Event& e) {
    auto it = e.payload.find("state");
    // Both standby levels have the screen off; dim keeps it on.
    const bool off = it != e.payload.end() && (it->second.as_string() == "standby" ||
                                               it->second.as_string() == "deep_standby");
    sampler_.set_standby(off);
}

void SelfCheck::subscribe_settings() {
    settings_sub_ = app_.bus().subscribe("system.settings",
                                         [this](const core::Event& e) { on_settings_event(e); });
}

void SelfCheck::on_settings_event(const core::Event& e) {
    if (e.payload.find("language") == e.payload.end()) {
        return;
    }
    // The language itself is read back from SystemSettings on the UI
    // thread rather than taken from the payload, so a stale event queued
    // behind a newer change can never switch the screen back. If this
    // post is dropped, the refresh timer's sync_system_language() still
    // catches the change.
    if (!ui_->post([this] { sync_system_language(); })) {
        WLOG_W("self_check", "post(language change) dropped (post queue full)");
    }
}

void SelfCheck::subscribe_net() {
    net_sub_ = app_.bus().subscribe("net.", [this](const core::Event& e) { on_net_event(e); });
    reset_sub_ = app_.bus().subscribe("system.factory_reset",
                                      [this](const core::Event& e) { on_reset_event(e); });
    fw_sub_ = app_.bus().subscribe("system.firmware",
                                   [this](const core::Event& e) { on_firmware_event(e); });
}

void SelfCheck::on_net_event(const core::Event& e) {
    bool posted = true;
    if (e.topic == "net.wifi.state" || e.topic == "net.ap.state") {
        posted = ui_->post([this] { sync_wifi(); });
    } else if (e.topic == "net.pairing.code") {
        posted = ui_->post([this] { sync_pairing(); });
    }
    if (!posted) {
        WLOG_W("self_check", "post(%s) dropped (post queue full)", e.topic.c_str());
    }
}

void SelfCheck::on_reset_event(const core::Event& e) {
    auto it = e.payload.find("state");
    if (it == e.payload.end() || it->second.type() != Value::Type::String ||
        it->second.as_string() != "requested") {
        return;
    }
    // The pending flag is taken on the UI thread, not here: if this post
    // is dropped, the flag is still set for the periodic resync.
    if (!ui_->post([this] { sync_reset(); })) {
        WLOG_W("self_check", "post(factory reset dialog) dropped (post queue full)");
    }
}

void SelfCheck::on_firmware_event(const core::Event&) {
    // The state is re-read on the UI thread; a dropped post is made up for
    // by the periodic resync.
    if (!ui_->post([this] { sync_firmware(); })) {
        WLOG_W("self_check", "post(firmware dialog) dropped (post queue full)");
    }
}

void SelfCheck::start_loopback() {
    run_loopback(app_, [this](const LoopbackStatus& status) {
        ui_->post([this, status] {
            loop_status_ = status;
            tests_section_.set_loopback(status);
            if (tests_.on_loopback(status)) {
                tests_section_.show(tests_);
                header_.show_tests(tests_.passed_count());
            }
        });
    });
}

void SelfCheck::pass_test(InteractiveTest test) {
    if (tests_.pass(test)) {
        tests_section_.show(tests_);
        header_.show_tests(tests_.passed_count());
    }
}

void SelfCheck::toggle_perf_overlay() {
    overlay_enabled_ = !overlay_enabled_;
    ui_->set_perf_overlay(overlay_enabled_);
    ui_settings_.set("perf_overlay", overlay_enabled_);
}

void SelfCheck::refresh_timer_cb(lv_timer_t* timer) {
    auto* self = static_cast<SelfCheck*>(lv_timer_get_user_data(timer));
    // The fallback for a lost system.settings event: one atomic load per
    // tick while nothing changes.
    self->sync_system_language();
    self->refresh_ui();
    const uint64_t now = os::now_ms();
    if (now - self->last_net_resync_ms_ >= kNetResyncMs) {
        self->last_net_resync_ms_ = now;
        self->sync_wifi();
        self->sync_pairing();
        self->sync_reset();
        self->sync_firmware();
    }
    if (now - self->last_scene_list_ms_ >= kSceneResyncMs) {
        self->last_scene_list_ms_ = now;
        self->request_scene_list();
        // Closing a faulted foreground scene's error panel runs no
        // on_close and publishes no scene.state (close_slot() only
        // publishes "stopped" for a scene that was actually running), so
        // the subscription above can miss it; this catches up with
        // foreground() either way.
        self->follow_foreground();
    }
    if (now - self->last_storage_ms_ >= kStorageResyncMs) self->request_storage();
    self->net_section_.tick(now);
    self->pair_card_.tick(now);
}

void SelfCheck::refresh_ui() {
    // Every section compares before it sets, so a tick with nothing new
    // invalidates nothing.
    SampleSnapshot snap = sampler_.snapshot();
    sensors_.update(snap);
    power_clock_.update(snap);
    refresh_time_sync();
    perf_.update(ui_->perf());
    scenes_section_.update_states();
}

void SelfCheck::refresh_time_sync() {
    std::optional<int64_t> local;
    if (net::TimeSyncService* sync = app_.time_sync()) {
        const net::TimeSyncStatus st = sync->status();
        if (st.synced) local = st.last_sync + int64_t{utc_offset_min_} * 60;
    }
    power_clock_.show_sync(local);
}

void SelfCheck::follow_foreground() {
    // Read live rather than from the event: the last event of a burst
    // leaves the right answer even if an earlier one was dropped.
    scene::SceneRuntime* rt = app_.scenes();
    sampler_.set_covered(rt != nullptr && rt->foreground().has_value());
}

void SelfCheck::request_scene_list() {
    if (scenes_ == nullptr) return;
    // The callback runs on the UI thread, where alive_ is also cleared.
    scenes_->list_async([this, alive = alive_](std::vector<SceneEntry> entries) {
        if (!*alive) return;
        scenes_section_.set_entries(std::move(entries));
    });
}

void SelfCheck::open_scene(const std::string& id) {
    scene::SceneRuntime* rt = app_.scenes();
    if (rt == nullptr) return;
    // On the UI thread this only queues the open; the scene's screen comes
    // up when it has loaded.
    if (auto r = rt->open(id); !r) {
        WLOG_W("self_check", "%s: not opened: %s", id.c_str(), r.error().message.c_str());
    }
}

void SelfCheck::request_storage() {
    const uint64_t now = os::now_ms();
    if (storage_pending_ && now - last_storage_ms_ < kStorageRetryMs) return;
    last_storage_ms_ = now;
    if (scenes_ == nullptr) {
        // No worker (the scenes could not be set up): measured here, as
        // rarely as the worker would.
        show_storage(app_.storage().status());
        return;
    }
    storage_pending_ =
        scenes_->storage_status_async(*ui_, [this, alive = alive_](core::StorageStatus status) {
            if (!*alive) return;
            storage_pending_ = false;
            show_storage(status);
        });
}

void SelfCheck::show_storage(const core::StorageStatus& status) {
    if (!last_storage_ || &status != &*last_storage_) last_storage_ = status;
    storage_.update(status);
    if (!sd_row_live_) {
        return;
    }
    // The row follows the card: online is Ok, offline or not yet mounted
    // is Missing, as every registered but cardless SD device starts out at
    // boot.
    bool online = std::any_of(status.media.begin(), status.media.end(), [](const auto& media) {
        return media.id == "storage.sd" && media.online;
    });
    if (devices_.set_state("storage.sd",
                           online ? core::DeviceState::Ok : core::DeviceState::Missing)) {
        header_.show_devices(devices_.summary());
    }
}

void start_self_check(App& app, ui::SystemLayer& system, SceneWiring* scenes,
                      std::unique_ptr<SelfCheck>& slot) {
    auto self_check = std::make_unique<SelfCheck>(app, system, scenes);
    self_check->build();
    slot = std::move(self_check);
}

void stop_self_check(std::unique_ptr<SelfCheck>& self_check) { self_check.reset(); }

void refresh_self_check_header(ui::UiRuntime& ui, std::unique_ptr<SelfCheck>& self_check) {
    if (!self_check) {
        return;
    }
    SelfCheck* self = self_check.get();
    ui.post([self] { self->refresh_header(); });
}

App::Impl::Impl(Platform p)
    : platform(std::move(p)), storage(registry, bus), buttons(registry, bus) {}
App::Impl::~Impl() = default;

}  // namespace wisp::app
