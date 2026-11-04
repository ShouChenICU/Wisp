// The self-check screen (spec 8): the single-column, scrollable LVGL UI
// shown right after boot on any board with a display. See self_check.hpp
// for how App::start()/App::stop() own this object's lifetime.

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "app_impl.hpp"
#include "factory_reset.hpp"
#include "firmware_confirm.hpp"
#include "loopback.hpp"
#include "lvgl.h"
#include "net_cards.hpp"
#include "net_section.hpp"
#include "self_check.hpp"
#include "wisp/app/app.hpp"
#include "wisp/app/format.hpp"
#include "wisp/app/power_key_controller.hpp"
#include "wisp/app/power_manager.hpp"
#include "wisp/base/json.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/core/log.hpp"
#include "wisp/core/settings.hpp"
#include "wisp/core/system_settings.hpp"
#include "wisp/hal/clock.hpp"
#include "wisp/hal/display.hpp"
#include "wisp/hal/display_geometry.hpp"
#include "wisp/hal/imu.hpp"
#include "wisp/hal/power.hpp"
#include "wisp/net/pairing_service.hpp"
#include "wisp/net/wifi_service.hpp"
#include "wisp/os/os.hpp"
#include "wisp/ui/strings.hpp"
#include "wisp/ui/ui_runtime.hpp"

namespace wisp::app {

namespace {

constexpr uint32_t kSampleTaskPeriodMs = 20;      // ~50Hz IMU sampling.
constexpr uint64_t kSlowSampleIntervalMs = 1000;  // power/clock/storage.
// How often sample_loop() still takes one power/clock "slow sample"
// while standby_ is true -- spec 6.7 asks for "约 30s", not a full stop
// (see sample_loop()'s standby branch).
constexpr uint32_t kStandbySlowSampleMs = 30000;
constexpr uint32_t kUiRefreshMs = 100;
// How often the refresh timer re-reads the network services (and the
// pending factory-reset and firmware requests) even without an event: the
// bus drops its oldest events when full, and a lost net.ap.state,
// net.pairing.code, system.factory_reset or system.firmware must not leave
// a card up (or missing) for good.
// The reads are a few short lock-protected copies and an atomic.
constexpr uint64_t kNetResyncMs = 1000;
constexpr uint32_t kPerfReportMs = 5000;
constexpr int kBallTravelPx = 20;

lv_color_t color_for_state(core::DeviceState state) {
    switch (state) {
        case core::DeviceState::Ok:
            return lv_color_hex(0x4FC3F7);
        case core::DeviceState::Degraded:
            return lv_color_hex(0xFFB74D);
        case core::DeviceState::Missing:
            return lv_color_hex(0x9E9E9E);
        case core::DeviceState::Failed:
            return lv_color_hex(0xE57373);
    }
    return lv_color_hex(0xE57373);
}

// "<label>: <value>", the shape of every readout row in every language.
std::string labeled(const std::string& label, const std::string& value) {
    return label + ": " + value;
}

lv_obj_t* make_row_label(lv_obj_t* parent) {
    lv_obj_t* label = lv_label_create(parent);
    lv_obj_set_width(label, LV_PCT(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
    return label;
}

// A borderless, non-scrollable horizontal row (used to put the IMU tilt
// ball and the "录音回放" button's status text next to a label).
lv_obj_t* make_row_container(lv_obj_t* parent) {
    lv_obj_t* row = lv_obj_create(parent);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_style_border_width(row, 0, 0);
    lv_obj_set_style_pad_all(row, 0, 0);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    return row;
}

}  // namespace

// Every non-public member/method below is this screen's own business;
// nothing outside self_check_screen.cpp names this type (see
// self_check.hpp's forward declaration).
class SelfCheck {
public:
    explicit SelfCheck(App& app);
    ~SelfCheck();

    // Must be called once, from the UI thread, right after construction
    // (see start_self_check() below).
    void build();

    // Rebuilds every widget in the current ui::lang() (spec 8.3), keeping
    // the scroll position and an open confirm dialog. Must be called from
    // the UI thread.
    void rebuild();

    // Re-renders the header's "启动 .. ms" text from BootReport's current
    // boot_ms. Must be called from the UI thread; see
    // refresh_self_check_header() in self_check.hpp for why this exists --
    // build_header() runs before App::start() has called
    // BootReport::set_boot_ms() (spec 6.1's boot sequence only finishes,
    // and boot_ms is only final, once this screen is already built), so
    // the header initially shows a stale (zero) value that this corrects
    // once the real one is known.
    void refresh_header();

private:
    // A snapshot of the "wisp.sample" task's latest readings, protected
    // by sample_mutex_ -- see start_sampling()/refresh_ui().
    struct Snapshot {
        hal::ImuSample imu{};
        bool imu_ok = true;
        hal::PowerStatus power{};
        bool power_ok = true;
        int64_t rtc_unix = 0;
        bool rtc_ok = true;
    };

    // Returns the string build_header()/refresh_header() should show.
    std::string header_text() const;

    // The safe area (spec 5.6) to lay the root column out in: derived
    // from display_'s caps via wisp::hal::screen_shape_from_caps()/
    // safe_rect(), with the same fallback rule as
    // DeviceRegistry::manifest() (see device_registry.cpp) -- an invalid
    // "shape"/"corner_radius" falls back to treating the panel as
    // circular (the most conservative shape, logged via WLOG_W); missing/
    // invalid width/height falls back to the full screen.
    hal::Area compute_safe_area() const;

    // Creates root_ and every widget under it (build()/rebuild()).
    void build_widgets();

    // --- build_widgets() helpers; each appends one section to `col`. ---
    void build_header(lv_obj_t* col);
    void build_device_list(lv_obj_t* col);
    void build_imu_section(lv_obj_t* col);
    void build_power_section(lv_obj_t* col);
    void build_rtc_section(lv_obj_t* col);
    void build_storage_section(lv_obj_t* col);
    void build_touch_section(lv_obj_t* col);
    void build_button_hint_section(lv_obj_t* col);
    void build_loopback_section(lv_obj_t* col);
    void build_perf_section(lv_obj_t* col);
    void build_net_section(lv_obj_t* col);

    void start_sampling();
    void sample_loop();
    // The power/clock "slow sample" -- factored out of
    // sample_loop() so the standby branch can call it on its own ~30s
    // cadence instead of the active-loop cadence (kSlowSampleIntervalMs).
    void sample_slow();

    void start_perf_timer();
    void emit_perf_line();

    void subscribe_buttons();
    void on_button_event(const core::Event& e);
    void subscribe_power();
    void on_power_event(const core::Event& e);
    void subscribe_settings();
    void on_settings_event(const core::Event& e);
    // net.wifi.state / net.ap.state / net.pairing.code (connectivity spec
    // 5.5) and system.factory_reset {state:"requested"}: each posts the
    // matching sync onto the UI thread, which reads the services' current
    // state rather than trusting a possibly stale payload.
    void subscribe_net();
    void on_net_event(const core::Event& e);
    void on_reset_event(const core::Event& e);
    // system.firmware: any state may open or close the update dialog.
    void on_firmware_event(const core::Event& e);
    // UI thread: re-reads the Wi-Fi state into the network section and
    // opens, refreshes or closes the provisioning card to match.
    void sync_wifi();
    // UI thread: shows the service's active pairing code (if any) with a
    // countdown to its expiry, or closes the card.
    void sync_pairing();
    // Keeps the stacking order on the top layer: provisioning card, then
    // pairing card, then the dialogs.
    void raise_overlays();
    // Turns a sleeping screen on for something the user is waiting to see
    // (a pairing code, the factory-reset question). UI thread.
    void wake_screen();
    // UI thread: takes a pending factory-reset request (see
    // take_factory_reset_request()) and shows the dialog for it.
    void sync_reset();
    void show_reset_dialog();
    void close_reset_dialog();
    // UI thread: takes a pending firmware request and shows the dialog for
    // it, or closes the dialog once nothing awaits an answer (declined
    // elsewhere, timed out).
    void sync_firmware();
    void show_firmware_dialog();
    void close_firmware_dialog();
    // Brings ui::lang() in line with the stored language, rebuilding if it
    // changed. UI thread only.
    void sync_language();
    void handle_power_key_action(PowerKeyController::Action action);
    void show_confirm_dialog();
    void close_confirm_dialog();
    void do_power_off();

    void start_loopback();
    void toggle_perf_overlay();

    void refresh_ui();
    void update_storage_labels();

    // Runs `fn` on the UI thread and blocks until it has finished running
    // there, regardless of which thread calls this -- see ~SelfCheck().
    void run_on_ui_thread_blocking(std::function<void()> fn);

    static void refresh_timer_cb(lv_timer_t* timer);
    static void touch_event_cb(lv_event_t* e);
    static void loopback_btn_cb(lv_event_t* e);
    static void perf_long_press_cb(lv_event_t* e);
    static void msgbox_btn_cb(lv_event_t* e);
    static void reset_btn_cb(lv_event_t* e);
    static void firmware_btn_cb(lv_event_t* e);

    // A confirm box in show_confirm_dialog()'s style (the msgbox's default
    // width clamped to the safe area) with Yes/No footer buttons, both
    // wired to `cb` with this screen as user data. Returns the box; *yes
    // receives its Yes button.
    lv_obj_t* make_confirm_box(ui::StrId title, ui::StrId text, lv_event_cb_t cb, lv_obj_t** yes);
    lv_obj_t* make_confirm_box(ui::StrId title, const std::string& text, lv_event_cb_t cb,
                               lv_obj_t** yes);

    App& app_;
    ui::UiRuntime* ui_;
    std::shared_ptr<hal::Display> display_;
    std::shared_ptr<hal::Imu> imu_;
    std::shared_ptr<hal::Power> power_;
    std::shared_ptr<hal::Clock> clock_;
    core::Settings ui_settings_;

    PowerKeyController power_key_;
    bool overlay_enabled_ = false;

    // --- widgets (valid only while this App's UiRuntime is alive). ---
    lv_obj_t* header_label_ = nullptr;
    lv_obj_t* imu_label_ = nullptr;
    lv_obj_t* imu_ball_ = nullptr;
    lv_obj_t* power_label_ = nullptr;
    lv_obj_t* rtc_label_ = nullptr;
    lv_obj_t* storage_int_label_ = nullptr;
    lv_obj_t* storage_sd_label_ = nullptr;
    // The device list's own "storage.sd" row (see build_device_list()):
    // kept live by update_storage_labels(), rather than left at
    // whatever DeviceState build_device_list() saw once at startup, so
    // it can't disagree with the dedicated storage section below it
    // about whether the card is actually online right now. id/model are
    // cached from build_device_list() since the row's text is otherwise
    // only ever derived once, from BootReport, not from StorageManager.
    lv_obj_t* device_list_sd_row_ = nullptr;
    std::string device_list_sd_id_;
    std::string device_list_sd_model_;
    lv_obj_t* touch_label_ = nullptr;
    lv_obj_t* button_label_ = nullptr;
    lv_obj_t* loop_status_label_ = nullptr;
    // The last loopback status shown, re-rendered by rebuild(). UI thread
    // only.
    LoopbackStatus loop_status_;
    // SystemSettings::revision() as of the last sync_language(); empty
    // until the first one, which always reads the stored language. UI
    // thread only.
    std::optional<uint32_t> language_revision_;
    lv_obj_t* perf_label_ = nullptr;
    lv_obj_t* msgbox_ = nullptr;
    lv_obj_t* msgbox_yes_ = nullptr;
    // The factory-reset confirmation, kept apart from the power-off dialog
    // above so neither one's buttons (or the power key) can answer the
    // other. At most one of each is open.
    lv_obj_t* reset_box_ = nullptr;
    lv_obj_t* reset_yes_ = nullptr;
    // Set once the user confirmed and the reset task started: later
    // requests are ignored while the reset runs towards its restart.
    bool reset_confirmed_ = false;
    // The firmware-update confirmation; the third kind of dialog, likewise
    // never stacked on the other two.
    lv_obj_t* fw_box_ = nullptr;
    lv_obj_t* fw_yes_ = nullptr;
    // os::now_ms() of the last periodic network resync (see kNetResyncMs).
    uint64_t last_net_resync_ms_ = 0;

    // --- Network block and cards (connectivity spec 8.1). UI thread only.
    NetSection net_section_;
    ProvisioningCard setup_card_;
    PairingCard pair_card_;
    lv_timer_t* refresh_timer_ = nullptr;
    // The single root container build() creates everything else under
    // (see build()). Deleted (lv_obj_delete(), recursively deleting every
    // child -- the touch test box, the "录音回放" button, the perf row,
    // etc.) in ~SelfCheck()'s UI-thread teardown step, specifically so
    // that none of those widgets' LV_EVENT_* callbacks (all registered
    // with `this` as user_data) can still fire after this SelfCheck is
    // freed -- deleting only refresh_timer_/msgbox_
    // left every other widget (and their callbacks) alive until the next
    // lv_deinit(), so a touch/click between stop_self_check() returning
    // and UiRuntime::stop() actually running could call back into
    // already-freed memory.
    lv_obj_t* root_ = nullptr;

    // --- "wisp.sample" task + its mutex-protected snapshot. ---
    os::Mutex sample_mutex_;
    Snapshot snapshot_;
    std::atomic<bool> sample_running_{false};
    std::unique_ptr<os::Task> sample_task_;

    // Mirrors PowerManager::standby(), kept up to date
    // by subscribe_power()'s "power.state" handler rather than read live
    // from PowerManager itself -- sample_loop() runs on its own thread
    // with no UI-task affinity to synchronize against, so this is its
    // own atomic snapshot (same pattern as PowerManager's own standby_).
    // While true, sample_loop() stops reading the IMU at
    // kSampleTaskPeriodMs entirely -- spec 6.7's "待机时...没有高频轮询"
    // applies to this task too, not just UiRuntime's -- but still takes
    // one power/clock "slow sample" about every kStandbySlowSampleMs;
    // see sample_loop()'s standby branch.
    std::atomic<bool> standby_{false};
    // Given (woken) by the "power.state":"active" handler so sample_loop
    // () resumes promptly on wake, rather than waiting out its own
    // kStandbySlowSampleMs poll -- see sample_loop()'s doc comment. Also
    // given by ~SelfCheck() so a sample_running_ that just went false is
    // noticed immediately even if this task is currently blocked here.
    os::Semaphore standby_wake_{1, 0};

    // --- WISP_PERF emission. ---
    std::unique_ptr<os::Timer> perf_timer_;

    // --- "button."/"power.state"/"system.settings" bus subscriptions. ---
    core::SubscriptionId button_sub_ = 0;
    core::SubscriptionId power_sub_ = 0;
    core::SubscriptionId settings_sub_ = 0;
    core::SubscriptionId net_sub_ = 0;
    core::SubscriptionId reset_sub_ = 0;
    core::SubscriptionId fw_sub_ = 0;
};

SelfCheck::SelfCheck(App& app)
    : app_(app),
      ui_(app.ui()),
      display_(app.registry().get<hal::Display>("display.main")),
      imu_(app.registry().get<hal::Imu>("imu.main")),
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
      }) {}

SelfCheck::~SelfCheck() {
    // Unsubscribe from "power.state" before anything else, so standby_
    // can't change (and nothing else calls standby_wake_.give()) from
    // this point on -- EventBus::unsubscribe()'s own contract guarantees
    // the handler never runs again once this returns.
    if (power_sub_ != 0) {
        app_.bus().unsubscribe(power_sub_);
        power_sub_ = 0;
    }
    // A language change already posted by on_settings_event() still runs
    // before the UI-thread teardown at the end of this destructor (the post
    // queue is FIFO), while every widget it rebuilds is still alive.
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

    // Stop the sampling task -- thread-agnostic, and nothing it touches
    // (snapshot_, the hal:: device shared_ptrs) requires the UI thread.
    sample_running_.store(false, std::memory_order_release);
    // sample_loop() may currently be blocked on standby_wake_ (if
    // standby_ was true when this destructor started) -- wake it
    // unconditionally so it notices sample_running_ went false promptly
    // rather than waiting out its own bounded poll (see sample_loop()'s
    // doc comment).
    standby_wake_.give();
    if (sample_task_) {
        sample_task_->join();
        sample_task_.reset();
    }

    if (perf_timer_) {
        perf_timer_->stop();
        perf_timer_.reset();
    }

    if (button_sub_ != 0) {
        app_.bus().unsubscribe(button_sub_);
        button_sub_ = 0;
    }

    // Give PowerManager's generic "any button wakes
    // standby" handler back ownership of "button.power" now that this
    // screen's own PowerKeyController (the thing set_power_key_owned(true)
    // in subscribe_buttons() handed it off to) is going away -- see
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
    // run_on_ui_thread_blocking(). If `ui_` is already gone (shouldn't
    // happen given app.cpp's teardown order, but safe either way),
    // there's nothing left to tear down: UiRuntime::stop()'s lv_deinit()
    // already released every lv_obj_t/lv_timer_t this screen created.
    if (ui_ == nullptr) {
        return;
    }
    run_on_ui_thread_blocking([this] {
        if (msgbox_ != nullptr) {
            lv_msgbox_close(msgbox_);
            msgbox_ = nullptr;
            msgbox_yes_ = nullptr;
        }
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
            // Recursively deletes every widget build() created under it
            // (and, with them, every LV_EVENT_* callback registered with
            // `this` as user_data) -- see root_'s doc comment above.
            lv_obj_delete(root_);
            root_ = nullptr;
        }
    });
}

void SelfCheck::run_on_ui_thread_blocking(std::function<void()> fn) {
    if (ui_->on_ui_thread()) {
        fn();
        return;
    }
    auto done = std::make_shared<os::Semaphore>(1, 0);
    // Retry (rather than give up) if the post queue is momentarily full:
    // this teardown step must actually run -- see root_'s doc comment --
    // not merely be attempted once. UiRuntime's own UI task continuously
    // drains that queue, so a slot frees up promptly; this never spins
    // forever in practice.
    while (!ui_->post([fn, done] {
        fn();
        done->give();
    })) {
        os::sleep_ms(1);
    }
    done->take(os::kForever);
}

void SelfCheck::build() {
    build_widgets();

    start_sampling();
    start_perf_timer();
    subscribe_buttons();
    subscribe_power();
    subscribe_settings();
    subscribe_net();
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

void SelfCheck::rebuild() {
    int32_t scroll_y = root_ != nullptr ? lv_obj_get_scroll_y(root_) : 0;
    bool dialog_open = msgbox_ != nullptr;
    bool reset_open = reset_box_ != nullptr;
    bool fw_open = fw_box_ != nullptr;
    bool setup_open = setup_card_.is_open();
    bool pair_open = pair_card_.is_open();
    std::string pair_code = pair_card_.code();
    uint64_t pair_expires = pair_card_.expires_ms();
    close_confirm_dialog();
    close_reset_dialog();
    close_firmware_dialog();
    setup_card_.close();
    pair_card_.close();
    if (root_ != nullptr) {
        lv_obj_delete(root_);
        root_ = nullptr;
    }
    device_list_sd_row_ = nullptr;

    build_widgets();
    // Fill the live readouts now rather than leaving "--" until the next
    // refresh tick.
    refresh_ui();
    lv_obj_update_layout(root_);
    lv_obj_scroll_to_y(root_, scroll_y, LV_ANIM_OFF);
    // Reopen in stacking order (see raise_overlays()).
    if (setup_open) {
        sync_wifi();
    }
    if (pair_open) {
        pair_card_.open(std::move(pair_code), pair_expires, os::now_ms(), compute_safe_area());
    }
    if (dialog_open) {
        show_confirm_dialog();
    }
    if (reset_open) {
        show_reset_dialog();
    }
    if (fw_open) {
        show_firmware_dialog();
    }
}

void SelfCheck::build_widgets() {
    lv_obj_t* screen = lv_screen_active();
    lv_obj_t* col = lv_obj_create(screen);
    root_ = col;
    // The root column fills the whole screen with no background or radius
    // of its own, so the screen reads as one uniform surface edge to
    // edge; content is kept off the invisible parts purely by padding
    // (spec 5.6). Each side pads by that side's safe inset plus 8px, so:
    // horizontally nothing ever leaves the safe columns, and vertically
    // rows may scroll through the top/bottom bands (corners) but at either
    // scroll extreme the first/last row rests fully inside the safe area.
    hal::Area safe = compute_safe_area();
    int32_t screen_w = lv_display_get_horizontal_resolution(nullptr);
    int32_t screen_h = lv_display_get_vertical_resolution(nullptr);
    constexpr int32_t kInnerPad = 8;
    lv_obj_set_pos(col, 0, 0);
    lv_obj_set_size(col, screen_w, screen_h);
    lv_obj_set_style_border_width(col, 0, 0);
    lv_obj_set_style_radius(col, 0, 0);
    lv_obj_set_style_bg_opa(col, LV_OPA_TRANSP, 0);
    lv_obj_set_style_pad_left(col, safe.x1 + kInnerPad, 0);
    lv_obj_set_style_pad_right(col, screen_w - 1 - safe.x2 + kInnerPad, 0);
    lv_obj_set_style_pad_top(col, safe.y1 + kInnerPad, 0);
    lv_obj_set_style_pad_bottom(col, screen_h - 1 - safe.y2 + kInnerPad, 0);
    lv_obj_set_style_pad_row(col, 10, 0);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(col, LV_DIR_VER);
    // No scrollbar: on this rounded-corner screen a bar hugging the right
    // edge would sit under/near the top-right and bottom-right corners.
    lv_obj_set_scrollbar_mode(col, LV_SCROLLBAR_MODE_OFF);

    build_header(col);
    build_device_list(col);
    build_net_section(col);
    build_imu_section(col);
    build_power_section(col);
    build_rtc_section(col);
    build_storage_section(col);
    build_touch_section(col);
    build_button_hint_section(col);
    build_loopback_section(col);
    build_perf_section(col);
}

std::string SelfCheck::header_text() const {
    uint64_t boot_ms = 0;
    Value rep = app_.report().to_value(app_.platform().board->name());
    if (const Value* v = rep.find("boot_ms")) {
        boot_ms = static_cast<uint64_t>(v->as_int());
    }

    char buf[160];
    std::snprintf(buf, sizeof(buf), "%s  v%s  %s %llu ms", app_.platform().board->name().c_str(),
                  app_.platform().firmware_version.c_str(), ui::tr(ui::StrId::BootTime),
                  static_cast<unsigned long long>(boot_ms));
    return buf;
}

void SelfCheck::build_header(lv_obj_t* col) {
    header_label_ = make_row_label(col);
    lv_label_set_text(header_label_, header_text().c_str());
}

void SelfCheck::refresh_header() { lv_label_set_text(header_label_, header_text().c_str()); }

hal::Area SelfCheck::compute_safe_area() const {
    int32_t screen_w = lv_display_get_horizontal_resolution(nullptr);
    int32_t screen_h = lv_display_get_vertical_resolution(nullptr);
    hal::Area full_screen{0, 0, screen_w - 1, screen_h - 1};
    if (!display_) {
        return full_screen;
    }

    const Properties& caps = display_->info().caps;
    auto shape = hal::screen_shape_from_caps(caps);
    if (shape) {
        return hal::safe_rect(shape.value());
    }

    // Parse failed -- same fallback rule as DeviceRegistry::manifest()
    // (see device_registry.cpp): treat the panel as circular (the most
    // conservative shape) if width/height are at least usable, otherwise
    // give up on a safe area and use the full screen.
    auto width_it = caps.find("width");
    auto height_it = caps.find("height");
    if (width_it == caps.end() || width_it->second.as_int() <= 0 || height_it == caps.end() ||
        height_it->second.as_int() <= 0) {
        WLOG_W("self_check",
               "display.main: invalid shape caps (%s) and no usable width/height; "
               "using the full screen",
               shape.error().message.c_str());
        return full_screen;
    }

    WLOG_W("self_check", "display.main: invalid shape caps (%s); treating as circle",
           shape.error().message.c_str());
    hal::ScreenShape fallback;
    fallback.kind = hal::ScreenShapeKind::Circle;
    fallback.width = static_cast<int32_t>(width_it->second.as_int());
    fallback.height = static_cast<int32_t>(height_it->second.as_int());
    return hal::safe_rect(fallback);
}

void SelfCheck::build_device_list(lv_obj_t* col) {
    lv_label_set_text(make_row_label(col), ui::tr(ui::StrId::DeviceList));

    std::vector<std::string> ids;
    for (const auto& entry : app_.report().entries()) {
        if (std::find(ids.begin(), ids.end(), entry.id) == ids.end()) {
            ids.push_back(entry.id);
        }
    }

    for (const auto& id : ids) {
        auto found = app_.report().find(id);
        core::DeviceState state = found ? found->state : core::DeviceState::Missing;
        auto device = app_.registry().find(id);
        std::string model = device ? device->info().model : std::string("-");

        char buf[160];
        std::snprintf(buf, sizeof(buf), "%s | %s | %s", id.c_str(), model.c_str(),
                      state_label(state));
        lv_obj_t* row = make_row_label(col);
        lv_label_set_text(row, buf);
        lv_obj_set_style_text_color(row, color_for_state(state), 0);

        if (id == "storage.sd" && device != nullptr) {
            // BootReport only ever reflects whether the card was present
            // at boot (spec 6.1: devices aren't re-registered after
            // init()), so this row would otherwise keep saying "缺失"
            // forever once a card is inserted later -- update_storage_
            // labels() (below, driven by the live StorageManager::
            // status()) re-renders it every kUiRefreshMs instead. Only
            // when the device actually exists in the registry: a
            // "storage.sd" boot-report entry with no backing device
            // (e.g. `--disable storage.sd`) is a real, permanent
            // Failed/Missing that StorageManager::status() can't ever
            // contradict (it simply won't have an entry for it either),
            // so that original coloring must stick.
            device_list_sd_row_ = row;
            device_list_sd_id_ = id;
            device_list_sd_model_ = model;
        }
    }
}

void SelfCheck::build_net_section(lv_obj_t* col) { net_section_.build(col, os::now_ms()); }

void SelfCheck::build_imu_section(lv_obj_t* col) {
    lv_obj_t* row = make_row_container(col);

    imu_label_ = make_row_label(row);
    lv_obj_set_flex_grow(imu_label_, 1);
    lv_label_set_text(imu_label_,
                      labeled("IMU", imu_ ? "--" : ui::tr(ui::StrId::StateMissing)).c_str());

    lv_obj_t* frame = lv_obj_create(row);
    lv_obj_set_size(frame, 2 * kBallTravelPx + 20, 2 * kBallTravelPx + 20);
    lv_obj_set_style_radius(frame, LV_RADIUS_CIRCLE, 0);
    lv_obj_remove_flag(frame, LV_OBJ_FLAG_SCROLLABLE);

    imu_ball_ = lv_obj_create(frame);
    lv_obj_set_size(imu_ball_, 14, 14);
    lv_obj_set_style_radius(imu_ball_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(imu_ball_, lv_color_hex(0x4FC3F7), 0);
    lv_obj_remove_flag(imu_ball_, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_align(imu_ball_, LV_ALIGN_CENTER, 0, 0);
}

void SelfCheck::build_power_section(lv_obj_t* col) {
    power_label_ = make_row_label(col);
    lv_label_set_text(power_label_, labeled(ui::tr(ui::StrId::Battery),
                                            power_ ? "--" : ui::tr(ui::StrId::StateMissing))
                                        .c_str());
}

void SelfCheck::build_rtc_section(lv_obj_t* col) {
    rtc_label_ = make_row_label(col);
    lv_label_set_text(
        rtc_label_,
        labeled(ui::tr(ui::StrId::Time), clock_ ? "--" : ui::tr(ui::StrId::StateMissing)).c_str());
}

void SelfCheck::build_storage_section(lv_obj_t* col) {
    storage_int_label_ = make_row_label(col);
    storage_sd_label_ = make_row_label(col);
    lv_label_set_text(storage_int_label_, labeled(ui::tr(ui::StrId::InternalFlash), "--").c_str());
    lv_label_set_text(storage_sd_label_, labeled(ui::tr(ui::StrId::SdCard), "--").c_str());
}

void SelfCheck::build_touch_section(lv_obj_t* col) {
    lv_obj_t* box = lv_obj_create(col);
    lv_obj_set_width(box, LV_PCT(100));
    lv_obj_set_height(box, 60);
    lv_obj_add_flag(box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(box, &SelfCheck::touch_event_cb, LV_EVENT_PRESSING, this);
    lv_obj_add_event_cb(box, &SelfCheck::touch_event_cb, LV_EVENT_RELEASED, this);

    touch_label_ = lv_label_create(box);
    lv_obj_center(touch_label_);
    lv_label_set_text(touch_label_, ui::tr(ui::StrId::TouchTestArea));
}

void SelfCheck::build_button_hint_section(lv_obj_t* col) {
    button_label_ = make_row_label(col);
    lv_label_set_text(button_label_, labeled(ui::tr(ui::StrId::Button), "--").c_str());
}

void SelfCheck::build_loopback_section(lv_obj_t* col) {
    lv_obj_t* row = make_row_container(col);

    lv_obj_t* btn = lv_button_create(row);
    lv_obj_add_event_cb(btn, &SelfCheck::loopback_btn_cb, LV_EVENT_CLICKED, this);
    lv_obj_t* btn_label = lv_label_create(btn);
    lv_label_set_text(btn_label, ui::tr(ui::StrId::LoopbackButton));

    loop_status_label_ = make_row_label(row);
    lv_obj_set_flex_grow(loop_status_label_, 1);
    lv_label_set_text(loop_status_label_, loopback_status_text(loop_status_).c_str());
}

void SelfCheck::build_perf_section(lv_obj_t* col) {
    perf_label_ = make_row_label(col);
    lv_label_set_text(perf_label_, "FPS 0 | CPU 0% | flush 0.0ms");
    lv_obj_add_flag(perf_label_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(perf_label_, &SelfCheck::perf_long_press_cb, LV_EVENT_LONG_PRESSED, this);
}

void SelfCheck::start_sampling() {
    sample_running_.store(true, std::memory_order_release);
    os::TaskConfig config;
    config.name = "wisp.sample";
    SelfCheck* self = this;
    sample_task_ = os::Task::spawn(config, [self] { self->sample_loop(); });
}

void SelfCheck::sample_loop() {
    uint64_t last_slow = 0;
    while (sample_running_.load(std::memory_order_acquire)) {
        // Standby means "no periodic high-frequency polling" (spec 6.7)
        // -- IMU sampling at kSampleTaskPeriodMs (~50Hz) stops entirely
        // while standby_ is true.
        //
        // spec 6.7 also asks for power/charging status (and the clock)
        // to keep being read "约 30s" during standby rather than stopped
        // outright, unlike IMU sampling above -- see sample_slow().
        // take() returning false means kStandbySlowSampleMs elapsed with
        // no wake signal -- exactly the "about 30s, take one slow
        // sample" case; true means something actually woke this task
        // (sample_running_ went false, or standby ended) and there is
        // nothing to sample right now.
        if (standby_.load(std::memory_order_acquire)) {
            if (!standby_wake_.take(kStandbySlowSampleMs)) {
                sample_slow();
            }
            continue;
        }
        if (imu_) {
            auto r = imu_->read();
            // enter_standby() can power the IMU off
            // while this read() (which runs with no lock held) is still
            // in flight. Publishing a resulting DeviceFailure into
            // snapshot_ after the fact would leave one bogus "IMU 异常"
            // frame visible right after waking -- refresh_ui() only ever
            // shows whatever is already in snapshot_, it has no way to
            // tell a stale failure from a fresh one. Drop this result
            // once standby_ has flipped true in the meantime instead; the
            // next iteration takes the standby branch above rather than
            // trying to read again.
            if (!standby_.load(std::memory_order_acquire)) {
                std::lock_guard<os::Mutex> lock(sample_mutex_);
                snapshot_.imu_ok = static_cast<bool>(r);
                if (r) {
                    snapshot_.imu = r.value();
                }
            }
        }

        uint64_t now = os::now_ms();
        if (now - last_slow >= kSlowSampleIntervalMs) {
            last_slow = now;
            sample_slow();
        }

        os::sleep_ms(kSampleTaskPeriodMs);
    }
}

void SelfCheck::sample_slow() {
    if (power_) {
        auto r = power_->status();
        std::lock_guard<os::Mutex> lock(sample_mutex_);
        snapshot_.power_ok = static_cast<bool>(r);
        if (r) {
            snapshot_.power = r.value();
        }
    }
    if (clock_) {
        auto r = clock_->get_unix();
        std::lock_guard<os::Mutex> lock(sample_mutex_);
        snapshot_.rtc_ok = static_cast<bool>(r);
        if (r) {
            snapshot_.rtc_unix = r.value();
        }
    }
}

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

    // From here on, this screen's own
    // PowerKeyController is the *only* thing that decides what a
    // "button.power" event does to the screen -- PowerManager's own
    // generic "any button wakes standby" handler (power_manager.cpp)
    // must leave button.power alone, or the two could each independently
    // decide to flip the screen for the very same physical press (one
    // reacting to `down`, the other -- with its own, now-stale idea of
    // "is the screen on" -- reacting to the `short`/`long` that follows
    // it), producing a wake-then-immediately-resleep flash. See
    // PowerManager::set_power_key_owned()'s doc comment.
    if (PowerManager* pm = app_.power_manager()) {
        pm->set_power_key_owned(true);
    }
}

void SelfCheck::on_button_event(const core::Event& e) {
    // on_button_event() itself runs on the EventBus dispatcher thread
    // (see subscribe_buttons()), but PowerKeyController is not
    // thread-safe against concurrent callers -- msgbox_btn_cb() (below)
    // also drives it, from the UI thread, via on_confirm(). Posting the
    // *whole* decision (on_short()/on_long() included, not just the
    // resulting Action) onto the UI thread, rather than calling those
    // here and only posting the result, is what makes every
    // PowerKeyController call happen on one single thread.
    std::string topic = e.topic;
    std::string source = e.source;
    // A dropped post() here silently loses whatever power-key action
    // (screen off/on) this event was about to decide -- log it so that
    // isn't invisible (see event_bus.hpp for why this must not block
    // waiting for a slot instead).
    if (!ui_->post([this, topic, source] {
            lv_label_set_text(button_label_,
                              labeled(ui::tr(ui::StrId::Button), topic.c_str()).c_str());

            PowerKeyController::Action action = PowerKeyController::Action::None;
            if (source == "button.power") {
                // PowerManager::standby() -- not any state
                // PowerKeyController tracks itself -- is the
                // single source of truth for "is the screen on"; read it
                // fresh right before deciding, on this same UI thread (the
                // only thread that ever calls into PowerManager's
                // enter_standby()/exit_standby() on this screen's behalf, so
                // there is no race between this read and this screen's own
                // handle_power_key_action() below acting on the result).
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
    // Mirrors subscribe_buttons()'s pattern, but for
    // PowerManager's own "power.state" broadcast rather than raw button
    // events -- lets sample_loop() (running on its own thread, not the
    // UI task) stop polling the IMU while in standby without reaching
    // into PowerManager itself.
    power_sub_ =
        app_.bus().subscribe("power.state", [this](const core::Event& e) { on_power_event(e); });
}

void SelfCheck::on_power_event(const core::Event& e) {
    auto it = e.payload.find("state");
    bool standby = it != e.payload.end() && it->second.as_string() == "standby";
    bool was_standby = standby_.exchange(standby, std::memory_order_acq_rel);
    if (was_standby && !standby) {
        // Wake sample_loop() promptly instead of leaving it to notice on
        // its own, up to 1s later (see sample_loop()'s doc comment).
        standby_wake_.give();
    }
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
    // post is dropped, the refresh timer's sync_language() still catches
    // the change.
    if (!ui_->post([this] { sync_language(); })) {
        WLOG_W("self_check", "post(language change) dropped (post queue full)");
    }
}

void SelfCheck::sync_language() {
    // The revision is read before the value: a change stored in between
    // moves it again, so the next sync reads the value once more.
    uint32_t revision = app_.system_settings().revision();
    if (language_revision_ == revision) {
        return;
    }
    language_revision_ = revision;
    std::optional<ui::Lang> lang = ui::parse_lang(app_.system_settings().get().language);
    if (!lang || *lang == ui::lang()) {
        return;
    }
    // set_lang() and the rebuild run together on the UI thread, so no
    // label is ever built half in the old language and half in the new.
    ui::set_lang(*lang);
    rebuild();
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

void SelfCheck::sync_wifi() {
    const uint64_t now = os::now_ms();
    net_section_.refresh(now);
    net::WifiService* wifi = app_.wifi();
    std::optional<net::ApSession> ap = wifi != nullptr ? wifi->ap_session() : std::nullopt;
    if (!ap) {
        setup_card_.close();
        return;
    }
    const net::ApSession& shown = setup_card_.session();
    if (setup_card_.is_open() && shown.ssid == ap->ssid && shown.password == ap->password &&
        shown.ip == ap->ip) {
        return;
    }
    setup_card_.open(*ap, compute_safe_area());
    raise_overlays();
}

void SelfCheck::sync_pairing() {
    const uint64_t now = os::now_ms();
    net::PairingService* pairing = app_.pairing();
    uint64_t expires_ms = 0;
    std::optional<std::string> code =
        pairing != nullptr ? pairing->active_code(now, &expires_ms) : std::nullopt;
    if (!code) {
        pair_card_.close();
        return;
    }
    if (pair_card_.is_open() && pair_card_.code() == *code &&
        pair_card_.expires_ms() == expires_ms) {
        return;
    }
    pair_card_.open(std::move(*code), expires_ms, now, compute_safe_area());
    raise_overlays();
    wake_screen();
}

void SelfCheck::raise_overlays() {
    pair_card_.raise();
    // A msgbox lives under its own full-screen backdrop on the top layer.
    for (lv_obj_t* box : {msgbox_, reset_box_, fw_box_}) {
        if (box != nullptr) lv_obj_move_foreground(lv_obj_get_parent(box));
    }
}

void SelfCheck::wake_screen() {
    PowerManager* pm = app_.power_manager();
    if (pm != nullptr && pm->standby()) {
        pm->exit_standby();
    }
}

void SelfCheck::sync_reset() {
    if (take_factory_reset_request(app_)) {
        show_reset_dialog();
    }
}

void SelfCheck::show_reset_dialog() {
    if (reset_box_ != nullptr || reset_confirmed_) {
        return;  // one dialog per request at a time; nothing once confirmed
    }
    if (msgbox_ != nullptr || fw_box_ != nullptr) {
        // One confirm dialog at a time (spec 8). The person at the device
        // is already answering another question, so the browser gets a
        // definite "no" now rather than a dialog that may never show.
        WLOG_I("self_check", "factory reset request refused: another dialog is open");
        cancel_factory_reset(app_);
        return;
    }
    reset_box_ = make_confirm_box(ui::StrId::ResetTitle, ui::StrId::ResetPrompt,
                                  &SelfCheck::reset_btn_cb, &reset_yes_);
    wake_screen();
    WLOG_I("self_check", "factory reset: waiting for confirmation on the screen");
}

void SelfCheck::close_reset_dialog() {
    if (reset_box_ == nullptr) {
        return;
    }
    lv_msgbox_close(reset_box_);
    reset_box_ = nullptr;
    reset_yes_ = nullptr;
}

void SelfCheck::sync_firmware() {
    if (take_firmware_request(app_)) {
        show_firmware_dialog();
    }
    if (fw_box_ != nullptr && !pending_firmware(app_)) {
        close_firmware_dialog();
    }
}

void SelfCheck::show_firmware_dialog() {
    if (fw_box_ != nullptr) {
        return;
    }
    std::optional<core::FirmwarePending> pending = pending_firmware(app_);
    if (!pending) {
        return;
    }
    if (msgbox_ != nullptr || reset_box_ != nullptr) {
        // As for the factory reset: one confirm dialog at a time, and the
        // browser hears "rejected" now.
        WLOG_I("self_check", "firmware update refused: another dialog is open");
        decline_firmware(app_);
        return;
    }
    // One version per line: a git-described version is about as wide as
    // the box, and LVGL would otherwise break it mid-number.
    std::string text = ui::tr(ui::StrId::FirmwarePrompt);
    text += "\n" + pending->current + "\n→ " + pending->next;
    if (pending->downgrade) {
        text += "\n";
        text += ui::tr(ui::StrId::FirmwareDowngrade);
    }
    fw_box_ =
        make_confirm_box(ui::StrId::FirmwareTitle, text, &SelfCheck::firmware_btn_cb, &fw_yes_);
    raise_overlays();
    wake_screen();
    WLOG_I("self_check", "firmware %s: waiting for confirmation on the screen",
           pending->next.c_str());
}

void SelfCheck::close_firmware_dialog() {
    if (fw_box_ == nullptr) {
        return;
    }
    lv_msgbox_close(fw_box_);
    fw_box_ = nullptr;
    fw_yes_ = nullptr;
}

void SelfCheck::handle_power_key_action(PowerKeyController::Action action) {
    // Screen on/off now goes through PowerManager (panel/touch sleep, UI
    // pause, button poll widening, the platform DFS/light-sleep hook --
    // spec 6.7), not a direct hal::Display::set_power() call.
    PowerManager* pm = app_.power_manager();
    switch (action) {
        case PowerKeyController::Action::None:
            break;
        case PowerKeyController::Action::ScreenOff:
            if (pm != nullptr) {
                pm->enter_standby();
            }
            break;
        case PowerKeyController::Action::ScreenOn:
            if (pm != nullptr) {
                pm->exit_standby();
            }
            break;
        case PowerKeyController::Action::ShowConfirm:
            show_confirm_dialog();
            break;
        case PowerKeyController::Action::ScreenOnAndConfirm:
            if (pm != nullptr) {
                pm->exit_standby();
            }
            show_confirm_dialog();
            break;
        case PowerKeyController::Action::PowerOff:
            do_power_off();
            break;
        case PowerKeyController::Action::CancelConfirm:
            close_confirm_dialog();
            break;
    }
}

void SelfCheck::show_confirm_dialog() {
    if (msgbox_ != nullptr) {
        return;  // Already up -- spec 8: no stacked confirm dialogs.
    }
    if (reset_box_ != nullptr || fw_box_ != nullptr) {
        // Nor on top of the factory-reset or firmware question: the long
        // press is ignored, and the key controller is told no dialog came
        // up, so the next short press turns the screen off as usual.
        WLOG_I("self_check", "power-off dialog not shown: another dialog is open");
        (void)power_key_.on_confirm(false);
        return;
    }
    msgbox_ = make_confirm_box(ui::StrId::PowerOffTitle, ui::StrId::PowerOffPrompt,
                               &SelfCheck::msgbox_btn_cb, &msgbox_yes_);
}

lv_obj_t* SelfCheck::make_confirm_box(ui::StrId title, ui::StrId text, lv_event_cb_t cb,
                                      lv_obj_t** yes) {
    return make_confirm_box(title, std::string(ui::tr(text)), cb, yes);
}

lv_obj_t* SelfCheck::make_confirm_box(ui::StrId title, const std::string& text, lv_event_cb_t cb,
                                      lv_obj_t** yes) {
    lv_obj_t* box = lv_msgbox_create(nullptr);
    // lv_msgbox_class's own default width is a fixed LV_DPI_DEF * 2 (see
    // lv_msgbox.c), unrelated to this screen's actual safe area -- fine
    // on this board's 480px circular screen (where it happens to fit
    // comfortably inside the inscribed safe square), but spec 8/AGENTS.
    // md require every UI element to respect safe_rect(), and a smaller
    // round screen would clip it. Clamp to whichever is smaller.
    hal::Area safe = compute_safe_area();
    int32_t safe_width = safe.x2 - safe.x1 + 1;
    lv_obj_set_width(box, std::min<int32_t>(LV_DPI_DEF * 2, safe_width));
    lv_msgbox_add_title(box, ui::tr(title));
    lv_msgbox_add_text(box, text.c_str());
    *yes = lv_msgbox_add_footer_button(box, ui::tr(ui::StrId::Yes));
    lv_obj_t* no_btn = lv_msgbox_add_footer_button(box, ui::tr(ui::StrId::No));
    lv_obj_add_event_cb(*yes, cb, LV_EVENT_CLICKED, this);
    lv_obj_add_event_cb(no_btn, cb, LV_EVENT_CLICKED, this);
    // Lay it out now rather than at the next refresh: a touch read in
    // between would otherwise hit-test the box at zero size and land on
    // whatever is under it.
    lv_obj_update_layout(box);
    return box;
}

void SelfCheck::close_confirm_dialog() {
    if (msgbox_ == nullptr) {
        return;
    }
    lv_msgbox_close(msgbox_);
    msgbox_ = nullptr;
    msgbox_yes_ = nullptr;
}

void SelfCheck::do_power_off() {
    if (power_) {
        auto result = power_->power_off();
        if (result) {
            return;
        }
        // Logged, rather than silently falling through, so a device
        // that ends up stuck running the deep-sleep fallback below (the
        // PMIC can't be told to cut power) leaves a trace of *why* --
        // otherwise the only symptom is the screen going dark and
        // nothing else happening.
        WLOG_E("self_check", "Power::power_off() failed: %s", result.error().message.c_str());
        // Fall through to the platform fallback below
        // rather than silently doing nothing -- the user already
        // confirmed shutdown, so a failed Power::power_off() shouldn't
        // leave the device stuck on.
    }
    if (app_.platform().request_power_off) {
        // The ESP fallback (esp_deep_sleep_start(), no wakeup source)
        // does not power down the AMOLED panel or its PMIC rail (see
        // docs/hardware/waveshare_esp32_s3_touch_amoled_2_16.md) -- deep sleep only
        // stops the SoC. Without turning the panel off first, this
        // confirm dialog (its own GRAM contents) would stay lit,
        // indistinguishable from a hang, and keep drawing power for as
        // long as the battery lasts. set_power(false) runs here, on
        // this (UI) thread, as spec 5.3 requires for every Display
        // call.
        if (display_) {
            display_->set_power(false);
        }
        app_.platform().request_power_off();
    }
}

void SelfCheck::start_loopback() {
    run_loopback(app_, [this](const LoopbackStatus& status) {
        ui_->post([this, status] {
            loop_status_ = status;
            lv_label_set_text(loop_status_label_, loopback_status_text(status).c_str());
        });
    });
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
    self->sync_language();
    self->refresh_ui();
    const uint64_t now = os::now_ms();
    if (now - self->last_net_resync_ms_ >= kNetResyncMs) {
        self->last_net_resync_ms_ = now;
        self->sync_wifi();
        self->sync_pairing();
        self->sync_reset();
        self->sync_firmware();
    }
    self->net_section_.tick(now);
    self->pair_card_.tick(now);
}

void SelfCheck::refresh_ui() {
    Snapshot snap;
    {
        std::lock_guard<os::Mutex> lock(sample_mutex_);
        snap = snapshot_;
    }

    if (imu_) {
        char buf[160];
        if (snap.imu_ok) {
            std::snprintf(buf, sizeof(buf), "IMU: ax=%.2f ay=%.2f az=%.2f gx=%.1f gy=%.1f gz=%.1f",
                          snap.imu.ax, snap.imu.ay, snap.imu.az, snap.imu.gx, snap.imu.gy,
                          snap.imu.gz);
        } else {
            std::snprintf(buf, sizeof(buf), "IMU: %s", ui::tr(ui::StrId::ReadFailed));
        }
        lv_label_set_text(imu_label_, buf);

        float ox = std::clamp(snap.imu.ax, -1.0f, 1.0f) * static_cast<float>(kBallTravelPx);
        float oy = std::clamp(-snap.imu.ay, -1.0f, 1.0f) * static_cast<float>(kBallTravelPx);
        lv_obj_align(imu_ball_, LV_ALIGN_CENTER, static_cast<int32_t>(ox),
                     static_cast<int32_t>(oy));
    }

    if (power_) {
        std::string value;
        if (snap.power_ok) {
            char level[16];
            std::snprintf(level, sizeof(level), "%d%%", snap.power.battery_percent);
            value = snap.power.battery_percent < 0 ? ui::tr(ui::StrId::LevelUnknown) : level;
            value += "  ";
            value += ui::tr(snap.power.charging ? ui::StrId::Charging : ui::StrId::NotCharging);
        } else {
            value = ui::tr(ui::StrId::ReadFailed);
        }
        lv_label_set_text(power_label_, labeled(ui::tr(ui::StrId::Battery), value).c_str());
    }

    if (clock_) {
        std::string value =
            snap.rtc_ok ? format_time(snap.rtc_unix) : std::string(ui::tr(ui::StrId::ReadFailed));
        lv_label_set_text(rtc_label_, labeled(ui::tr(ui::StrId::Time), value).c_str());
    }

    update_storage_labels();

    ui::UiPerf perf = ui_->perf();
    char perf_buf[96];
    std::snprintf(perf_buf, sizeof(perf_buf), "FPS %.0f | CPU %u%% | flush %.1fms", perf.fps,
                  perf.cpu_percent, perf.avg_flush_ms);
    lv_label_set_text(perf_label_, perf_buf);
}

void SelfCheck::update_storage_labels() {
    core::StorageStatus status = app_.storage().status();
    const core::MediaStatus* int_status = nullptr;
    const core::MediaStatus* sd_status = nullptr;
    for (const auto& media : status.media) {
        if (media.id == "storage.int") {
            int_status = &media;
        } else if (media.id == "storage.sd") {
            sd_status = &media;
        }
    }

    auto format_row = [](const char* label, const core::MediaStatus* media) {
        char buf[160];
        if (media == nullptr) {
            std::snprintf(buf, sizeof(buf), "%s: %s", label, ui::tr(ui::StrId::StateMissing));
        } else {
            uint64_t used =
                media->total_bytes > media->free_bytes ? media->total_bytes - media->free_bytes : 0;
            std::snprintf(buf, sizeof(buf), "%s: %s  %s / %s", label,
                          ui::tr(media->online ? ui::StrId::Online : ui::StrId::Offline),
                          format_bytes(used).c_str(), format_bytes(media->total_bytes).c_str());
        }
        return std::string(buf);
    };

    lv_label_set_text(storage_int_label_,
                      format_row(ui::tr(ui::StrId::InternalFlash), int_status).c_str());
    lv_label_set_text(storage_sd_label_, format_row(ui::tr(ui::StrId::SdCard), sd_status).c_str());

    if (device_list_sd_row_ != nullptr) {
        // sd_status == nullptr means "storage.sd" isn't in the registry
        // at all (never happens once BootReport already has a row for
        // it, since that implies the device was registered), so this
        // only ever distinguishes online (Ok, "正常") from offline/
        // not-yet-mounted (Missing, "缺失" -- the same label this row
        // already had at boot, when every registered-but-cardless SD
        // device starts out). Never Degraded/Failed: this row keeps its
        // original boot-time coloring for those, since a device that
        // actually failed, or was never registered in the first place,
        // can't suddenly come online later.
        core::DeviceState state = (sd_status != nullptr && sd_status->online)
                                      ? core::DeviceState::Ok
                                      : core::DeviceState::Missing;
        char buf[160];
        std::snprintf(buf, sizeof(buf), "%s | %s | %s", device_list_sd_id_.c_str(),
                      device_list_sd_model_.c_str(), state_label(state));
        lv_label_set_text(device_list_sd_row_, buf);
        lv_obj_set_style_text_color(device_list_sd_row_, color_for_state(state), 0);
    }
}

void SelfCheck::touch_event_cb(lv_event_t* e) {
    auto* self = static_cast<SelfCheck*>(lv_event_get_user_data(e));
    if (lv_event_get_code(e) == LV_EVENT_PRESSING) {
        lv_indev_t* indev = lv_indev_active();
        lv_point_t p{};
        if (indev != nullptr) {
            lv_indev_get_point(indev, &p);
        }
        char buf[64];
        std::snprintf(buf, sizeof(buf), "%s: (%d, %d)", ui::tr(ui::StrId::Touch),
                      static_cast<int>(p.x), static_cast<int>(p.y));
        lv_label_set_text(self->touch_label_, buf);
    } else {
        lv_label_set_text(self->touch_label_, ui::tr(ui::StrId::TouchTestArea));
    }
}

void SelfCheck::loopback_btn_cb(lv_event_t* e) {
    auto* self = static_cast<SelfCheck*>(lv_event_get_user_data(e));
    self->start_loopback();
}

void SelfCheck::perf_long_press_cb(lv_event_t* e) {
    auto* self = static_cast<SelfCheck*>(lv_event_get_user_data(e));
    self->toggle_perf_overlay();
}

void SelfCheck::msgbox_btn_cb(lv_event_t* e) {
    auto* self = static_cast<SelfCheck*>(lv_event_get_user_data(e));
    auto* target = static_cast<lv_obj_t*>(lv_event_get_target(e));
    bool yes = (target == self->msgbox_yes_);
    PowerKeyController::Action action = self->power_key_.on_confirm(yes);
    self->close_confirm_dialog();
    self->handle_power_key_action(action);
}

void SelfCheck::reset_btn_cb(lv_event_t* e) {
    auto* self = static_cast<SelfCheck*>(lv_event_get_user_data(e));
    auto* target = static_cast<lv_obj_t*>(lv_event_get_target(e));
    bool yes = (target == self->reset_yes_);
    self->close_reset_dialog();
    if (yes) {
        // Off the UI thread: the reset waits for the network and writes
        // flash before it restarts the device. If the task could not
        // start, the reset was answered "cancelled" and a later request
        // may ask again.
        self->reset_confirmed_ = start_factory_reset(self->app_);
    } else {
        cancel_factory_reset(self->app_);
    }
}

void SelfCheck::firmware_btn_cb(lv_event_t* e) {
    auto* self = static_cast<SelfCheck*>(lv_event_get_user_data(e));
    auto* target = static_cast<lv_obj_t*>(lv_event_get_target(e));
    bool yes = (target == self->fw_yes_);
    self->close_firmware_dialog();
    if (yes) {
        // Off the UI thread: switching the boot slot re-verifies the image.
        (void)start_firmware_install(self->app_);
    } else {
        decline_firmware(self->app_);
    }
}

void start_self_check(App& app, std::unique_ptr<SelfCheck>& slot) {
    auto self_check = std::make_unique<SelfCheck>(app);
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
