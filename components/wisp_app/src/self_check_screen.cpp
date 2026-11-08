// The self-check screen (spec 8), the hardware diagnostics: page framing,
// lifecycle, bus subscriptions and timers. See self_check.hpp for who
// builds and tears it down.

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
#include "wisp/app/ram_budget.hpp"
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
#include "wisp/ui/wisp_model.hpp"

namespace wisp::app {

namespace {

constexpr uint32_t kUiRefreshMs = 100;
// How often the refresh timer re-reads the network state into the network
// section even without an event: the bus drops its oldest events when
// full. The reads are a few short lock-protected copies.
constexpr uint64_t kNetResyncMs = 1000;
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
    // Whatever the handlers below already posted finds alive_ cleared by
    // the teardown at the end of this destructor.
    if (net_sub_ != 0) {
        app_.bus().unsubscribe(net_sub_);
        net_sub_ = 0;
    }
    if (scene_sub_ != 0) {
        app_.bus().unsubscribe(scene_sub_);
        scene_sub_ = 0;
    }

    // Thread-agnostic: nothing the sampler touches requires the UI thread.
    sampler_.stop();

    if (button_sub_ != 0) {
        app_.bus().unsubscribe(button_sub_);
        button_sub_ = 0;
    }

    // LVGL-specific teardown must run on the UI thread -- see
    // UiRuntime::run_blocking(), which runs it inline when this is already
    // the UI thread (the back gesture's teardown). If `ui_` is already gone
    // there's nothing left to tear down: UiRuntime::stop()'s lv_deinit()
    // already released every lv_obj_t/lv_timer_t this screen created.
    if (ui_ == nullptr) {
        return;
    }
    ui_->run_blocking([this] {
        *alive_ = false;
        if (refresh_timer_ != nullptr) {
            lv_timer_delete(refresh_timer_);
            refresh_timer_ = nullptr;
        }
        // See root_'s doc comment. The shell has loaded another screen
        // first: an active screen must not be deleted.
        if (screen_ != nullptr) {
            lv_obj_delete(screen_);
            screen_ = nullptr;
            root_ = nullptr;
        }
    });
}

bool SelfCheck::post(std::function<void()> fn) {
    return ui_->post([alive = alive_, fn = std::move(fn)] {
        if (*alive) fn();
    });
}

void SelfCheck::build() {
    build_widgets();

    sampler_.start();
    subscribe_buttons();
    subscribe_power();
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
    // The network state as it is now (networking started long before).
    sync_net();
    sync_settings();
    last_net_resync_ms_ = os::now_ms();

    refresh_timer_ = lv_timer_create(&SelfCheck::refresh_timer_cb, kUiRefreshMs, this);

    // SystemDuties applied the stored overlay at boot; the toggle starts
    // from it.
    overlay_enabled_ = ui_settings_.get_bool("perf_overlay", false);

    lv_screen_load(screen_);
}

void SelfCheck::build_widgets() {
    screen_ = lv_obj_create(nullptr);
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
    perf_.build(col, text_, [this] { toggle_perf_overlay(); });
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

hal::Area SelfCheck::compute_safe_area() const { return hal::safe_rect(system_.shape()); }

void SelfCheck::subscribe_buttons() {
    // The button test only; what the power key does is SystemDuties'.
    button_sub_ =
        app_.bus().subscribe("button.", [this](const core::Event& e) { on_button_event(e); });
}

void SelfCheck::on_button_event(const core::Event& e) {
    std::string topic = e.topic;
    if (!post([this, topic] {
            tests_section_.set_button_event(topic);
            pass_test(InteractiveTest::Button);
        })) {
        WLOG_W("self_check", "post(button test) dropped (post queue full)");
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

void SelfCheck::subscribe_net() {
    net_sub_ = app_.bus().subscribe("net.", [this](const core::Event& e) { on_net_event(e); });
}

void SelfCheck::on_net_event(const core::Event& e) {
    if (e.topic != "net.wifi.state" && e.topic != "net.ap.state") return;
    if (!post([this] { sync_net(); })) {
        WLOG_W("self_check", "post(%s) dropped (post queue full)", e.topic.c_str());
    }
}

void SelfCheck::sync_net() { net_section_.refresh(os::now_ms()); }

void SelfCheck::sync_settings() {
    // The revision is read before the value: a change stored in between
    // moves it again, so the next sync reads the value once more.
    uint32_t revision = app_.system_settings().revision();
    if (settings_revision_ == revision) return;
    settings_revision_ = revision;
    const core::SystemSettingsValues now = app_.system_settings().get();
    utc_offset_min_ = now.utc_offset_min;
    time_24h_ = now.time_24h;
    refresh_time_sync();
}

void SelfCheck::set_page_lang(ui::Lang lang) {
    if (!text_.set_lang(lang)) {
        return;
    }
    // Everything not bound in text_ is re-rendered from what the sections
    // (or this screen) last showed; each compares before it sets. The
    // network cards and the dialogs are system UI and keep the system
    // language (SystemDuties).
    board_.show_lang(lang);
    header_.show_devices(devices_.summary());
    header_.show_tests(tests_.passed_count());
    devices_.retext();
    net_section_.retext(os::now_ms());
    tests_section_.show(tests_);
    tests_section_.set_loopback(loop_status_);
    tests_section_.retext();
    scenes_section_.retext();
    if (last_storage_) show_storage(*last_storage_);
    refresh_ui();
}

void SelfCheck::start_loopback() {
    // The task may outlive this screen (the user goes back mid-test): it
    // only reaches the screen through post(), which drops the status once
    // the screen is gone.
    run_loopback(app_, [ui = ui_, alive = alive_, this](const LoopbackStatus& status) {
        ui->post([alive, this, status] {
            if (!*alive) return;
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
    self->sync_settings();
    self->refresh_ui();
    const uint64_t now = os::now_ms();
    if (now - self->last_net_resync_ms_ >= kNetResyncMs) {
        self->last_net_resync_ms_ = now;
        self->sync_net();
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
}

void SelfCheck::refresh_ui() {
    // Every section compares before it sets, so a tick with nothing new
    // invalidates nothing.
    SampleSnapshot snap = sampler_.snapshot();
    sensors_.update(snap);
    power_clock_.update(snap);
    refresh_time_sync();
    perf_.update(ui_->perf());
    refresh_perf_ram();
    scenes_section_.update_states();
}

void SelfCheck::refresh_perf_ram() {
    // Same Platform::heap_info() map SystemDuties reads for WISP_PERF
    // (internal_free/internal_largest/large_free), plus large_total (the
    // PSRAM's size) and internal_total, for the used percentages.
    Value heap = app_.platform().heap_info ? app_.platform().heap_info() : Value();
    InternalRam ram = internal_ram(heap);
    int64_t internal_total = 0;
    if (const Value* v = heap.find("internal_total")) {
        internal_total = v->as_int();
    }
    int64_t psram_free = 0;
    int64_t psram_total = 0;
    if (const Value* v = heap.find("large_free")) {
        psram_free = v->as_int();
    }
    if (const Value* v = heap.find("large_total")) {
        psram_total = v->as_int();
    }
    perf_.update_ram(ram.free, ram.largest, internal_total, psram_free, psram_total);
}

void SelfCheck::refresh_time_sync() {
    std::optional<int64_t> local;
    if (net::TimeSyncService* sync = app_.time_sync()) {
        const net::TimeSyncStatus st = sync->status();
        if (st.synced) local = st.last_sync + int64_t{utc_offset_min_} * 60;
    }
    power_clock_.show_sync(local, time_24h_);
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

App::Impl::Impl(Platform p)
    : platform(with_settings_front(std::move(p))), storage(registry, bus), buttons(registry, bus) {}
App::Impl::~Impl() = default;

}  // namespace wisp::app
