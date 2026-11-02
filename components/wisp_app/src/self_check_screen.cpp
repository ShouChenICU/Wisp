// The self-check screen (spec 8): the single-column, scrollable LVGL UI
// shown right after boot on any board with a display. See self_check.hpp
// for how App::start()/App::stop() own this object's lifetime.

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "app_impl.hpp"
#include "loopback.hpp"
#include "lvgl.h"
#include "self_check.hpp"
#include "wisp/app/app.hpp"
#include "wisp/app/format.hpp"
#include "wisp/app/power_key_controller.hpp"
#include "wisp/app/power_manager.hpp"
#include "wisp/base/json.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/core/log.hpp"
#include "wisp/core/settings.hpp"
#include "wisp/hal/clock.hpp"
#include "wisp/hal/display.hpp"
#include "wisp/hal/display_geometry.hpp"
#include "wisp/hal/imu.hpp"
#include "wisp/hal/power.hpp"
#include "wisp/os/os.hpp"
#include "wisp/ui/ui_runtime.hpp"

namespace wisp::app {

namespace {

constexpr uint32_t kSampleTaskPeriodMs = 20;      // ~50Hz IMU sampling.
constexpr uint64_t kSlowSampleIntervalMs = 1000;  // power/clock/storage.
constexpr uint32_t kUiRefreshMs = 100;
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

    // --- build() helpers; each appends one section to `col`. ---
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

    void start_sampling();
    void sample_loop();

    void start_perf_timer();
    void emit_perf_line();

    void subscribe_buttons();
    void on_button_event(const core::Event& e);
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
    lv_obj_t* touch_label_ = nullptr;
    lv_obj_t* button_label_ = nullptr;
    lv_obj_t* loop_status_label_ = nullptr;
    lv_obj_t* perf_label_ = nullptr;
    lv_obj_t* msgbox_ = nullptr;
    lv_obj_t* msgbox_yes_ = nullptr;
    lv_timer_t* refresh_timer_ = nullptr;
    // The single root container build() creates everything else under
    // (see build()). Deleted (lv_obj_delete(), recursively deleting every
    // child -- the touch test box, the "录音回放" button, the perf row,
    // etc.) in ~SelfCheck()'s UI-thread teardown step, specifically so
    // that none of those widgets' LV_EVENT_* callbacks (all registered
    // with `this` as user_data) can still fire after this SelfCheck is
    // freed -- fix round 1, issue 1: deleting only refresh_timer_/msgbox_
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

    // --- WISP_PERF emission. ---
    std::unique_ptr<os::Timer> perf_timer_;

    // --- "button." bus subscription. ---
    core::SubscriptionId button_sub_ = 0;
};

SelfCheck::SelfCheck(App& app)
    : app_(app),
      ui_(app.ui()),
      display_(app.registry().get<hal::Display>("display.main")),
      imu_(app.registry().get<hal::Imu>("imu.main")),
      power_(app.registry().get<hal::Power>("power.main")),
      clock_(app.registry().get<hal::Clock>("clock.main")),
      ui_settings_(*app.platform().settings, "ui") {}

SelfCheck::~SelfCheck() {
    // Stop the sampling task first -- thread-agnostic, and nothing it
    // touches (snapshot_, the hal:: device shared_ptrs) requires the UI
    // thread.
    sample_running_.store(false, std::memory_order_release);
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
    // forever in practice (M1, fix round 1).
    while (!ui_->post([fn, done] {
        fn();
        done->give();
    })) {
        os::sleep_ms(1);
    }
    done->take(os::kForever);
}

void SelfCheck::build() {
    lv_obj_t* screen = lv_screen_active();
    lv_obj_t* col = lv_obj_create(screen);
    root_ = col;
    // The root column's position and size are exactly the display's safe
    // area (spec 5.6), not the full screen -- LVGL clips children to
    // their parent's box by default, so nothing any build_*_section()
    // below adds can ever land outside it. An 8px inner padding on all
    // four sides keeps text off the safe area's own edge; no separate
    // corner margin is needed since the safe area already accounts for
    // the screen's shape.
    hal::Area safe = compute_safe_area();
    lv_obj_set_pos(col, safe.x1, safe.y1);
    lv_obj_set_size(col, safe.x2 - safe.x1 + 1, safe.y2 - safe.y1 + 1);
    lv_obj_set_style_border_width(col, 0, 0);
    lv_obj_set_style_pad_all(col, 8, 0);
    lv_obj_set_style_pad_row(col, 10, 0);
    lv_obj_set_flex_flow(col, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_scroll_dir(col, LV_DIR_VER);
    // No scrollbar: on this rounded-corner screen a bar hugging the right
    // edge would sit under/near the top-right and bottom-right corners
    // (M4, fix round 1).
    lv_obj_set_scrollbar_mode(col, LV_SCROLLBAR_MODE_OFF);

    build_header(col);
    build_device_list(col);
    build_imu_section(col);
    build_power_section(col);
    build_rtc_section(col);
    build_storage_section(col);
    build_touch_section(col);
    build_button_hint_section(col);
    build_loopback_section(col);
    build_perf_section(col);

    start_sampling();
    start_perf_timer();
    subscribe_buttons();

    refresh_timer_ = lv_timer_create(&SelfCheck::refresh_timer_cb, kUiRefreshMs, this);

    overlay_enabled_ = ui_settings_.get_bool("perf_overlay", false);
    if (overlay_enabled_) {
        ui_->set_perf_overlay(true);
    }
}

std::string SelfCheck::header_text() const {
    uint64_t boot_ms = 0;
    Value rep = app_.report().to_value(app_.platform().board->name());
    if (const Value* v = rep.find("boot_ms")) {
        boot_ms = static_cast<uint64_t>(v->as_int());
    }

    char buf[160];
    std::snprintf(buf, sizeof(buf), "%s  v%s  启动 %llu ms", app_.platform().board->name().c_str(),
                  app_.platform().firmware_version.c_str(),
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
    lv_label_set_text(make_row_label(col), "设备列表");

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
        std::snprintf(buf, sizeof(buf), "%s · %s · %s", id.c_str(), model.c_str(),
                      state_label(state));
        lv_obj_t* row = make_row_label(col);
        lv_label_set_text(row, buf);
        lv_obj_set_style_text_color(row, color_for_state(state), 0);
    }
}

void SelfCheck::build_imu_section(lv_obj_t* col) {
    lv_obj_t* row = make_row_container(col);

    imu_label_ = make_row_label(row);
    lv_obj_set_flex_grow(imu_label_, 1);
    lv_label_set_text(imu_label_, imu_ ? "IMU: --" : "IMU: 缺失");

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
    lv_label_set_text(power_label_, power_ ? "电源: --" : "电源: 缺失");
}

void SelfCheck::build_rtc_section(lv_obj_t* col) {
    rtc_label_ = make_row_label(col);
    lv_label_set_text(rtc_label_, clock_ ? "时间: --" : "时间: 缺失");
}

void SelfCheck::build_storage_section(lv_obj_t* col) {
    storage_int_label_ = make_row_label(col);
    storage_sd_label_ = make_row_label(col);
    lv_label_set_text(storage_int_label_, "内置 Flash: --");
    lv_label_set_text(storage_sd_label_, "SD 卡: --");
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
    lv_label_set_text(touch_label_, "触摸测试区");
}

void SelfCheck::build_button_hint_section(lv_obj_t* col) {
    button_label_ = make_row_label(col);
    lv_label_set_text(button_label_, "按键: --");
}

void SelfCheck::build_loopback_section(lv_obj_t* col) {
    lv_obj_t* row = make_row_container(col);

    lv_obj_t* btn = lv_button_create(row);
    lv_obj_add_event_cb(btn, &SelfCheck::loopback_btn_cb, LV_EVENT_CLICKED, this);
    lv_obj_t* btn_label = lv_label_create(btn);
    lv_label_set_text(btn_label, "录音回放");

    loop_status_label_ = make_row_label(row);
    lv_obj_set_flex_grow(loop_status_label_, 1);
    lv_label_set_text(loop_status_label_, "就绪");
}

void SelfCheck::build_perf_section(lv_obj_t* col) {
    perf_label_ = make_row_label(col);
    lv_label_set_text(perf_label_, "FPS 0 · CPU 0% · flush 0.0ms");
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
        if (imu_) {
            auto r = imu_->read();
            std::lock_guard<os::Mutex> lock(sample_mutex_);
            snapshot_.imu_ok = static_cast<bool>(r);
            if (r) {
                snapshot_.imu = r.value();
            }
        }

        uint64_t now = os::now_ms();
        if (now - last_slow >= kSlowSampleIntervalMs) {
            last_slow = now;
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

        os::sleep_ms(kSampleTaskPeriodMs);
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

    // Task 18b review round 1 (C1): from here on, this screen's own
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
    // PowerKeyController call happen on one single thread (fix round 1,
    // issue 2).
    std::string topic = e.topic;
    std::string source = e.source;
    ui_->post([this, topic, source] {
        char buf[96];
        std::snprintf(buf, sizeof(buf), "按键: %s", topic.c_str());
        lv_label_set_text(button_label_, buf);

        PowerKeyController::Action action = PowerKeyController::Action::None;
        if (source == "button.power") {
            // Task 18b review round 1 (C1): PowerManager::standby() --
            // not any state PowerKeyController tracks itself -- is the
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
    });
}

void SelfCheck::handle_power_key_action(PowerKeyController::Action action) {
    // Task 18b: screen on/off now goes through PowerManager (panel/touch
    // sleep, UI pause, button poll widening, the platform DFS/light
    // -sleep hook -- spec 6.7), not a direct hal::Display::set_power()
    // call.
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
        return;  // Already up -- see global-constraints.md Review Focus #4.
    }
    msgbox_ = lv_msgbox_create(nullptr);
    lv_msgbox_add_title(msgbox_, "关机确认");
    lv_msgbox_add_text(msgbox_, "是否关机？");
    msgbox_yes_ = lv_msgbox_add_footer_button(msgbox_, "是");
    lv_obj_t* no_btn = lv_msgbox_add_footer_button(msgbox_, "否");
    lv_obj_add_event_cb(msgbox_yes_, &SelfCheck::msgbox_btn_cb, LV_EVENT_CLICKED, this);
    lv_obj_add_event_cb(no_btn, &SelfCheck::msgbox_btn_cb, LV_EVENT_CLICKED, this);
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
        // Fall through to the platform fallback below (M3, fix round 1)
        // rather than silently doing nothing -- the user already
        // confirmed shutdown, so a failed Power::power_off() shouldn't
        // leave the device stuck on.
    }
    if (app_.platform().request_power_off) {
        app_.platform().request_power_off();
    }
}

void SelfCheck::start_loopback() {
    run_loopback(app_, [this](const char* status) {
        std::string text = status;
        ui_->post([this, text] { lv_label_set_text(loop_status_label_, text.c_str()); });
    });
}

void SelfCheck::toggle_perf_overlay() {
    overlay_enabled_ = !overlay_enabled_;
    ui_->set_perf_overlay(overlay_enabled_);
    ui_settings_.set("perf_overlay", overlay_enabled_);
}

void SelfCheck::refresh_timer_cb(lv_timer_t* timer) {
    auto* self = static_cast<SelfCheck*>(lv_timer_get_user_data(timer));
    self->refresh_ui();
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
            std::snprintf(buf, sizeof(buf), "IMU: 读取失败");
        }
        lv_label_set_text(imu_label_, buf);

        float ox = std::clamp(snap.imu.ax, -1.0f, 1.0f) * static_cast<float>(kBallTravelPx);
        float oy = std::clamp(-snap.imu.ay, -1.0f, 1.0f) * static_cast<float>(kBallTravelPx);
        lv_obj_align(imu_ball_, LV_ALIGN_CENTER, static_cast<int32_t>(ox),
                     static_cast<int32_t>(oy));
    }

    if (power_) {
        char buf[96];
        if (snap.power_ok) {
            std::snprintf(buf, sizeof(buf), "电源: %d%% %s", snap.power.battery_percent,
                          snap.power.charging ? "充电中" : "未充电");
        } else {
            std::snprintf(buf, sizeof(buf), "电源: 读取失败");
        }
        lv_label_set_text(power_label_, buf);
    }

    if (clock_) {
        std::string text =
            "时间: " + (snap.rtc_ok ? format_time(snap.rtc_unix) : std::string("读取失败"));
        lv_label_set_text(rtc_label_, text.c_str());
    }

    update_storage_labels();

    ui::UiPerf perf = ui_->perf();
    char perf_buf[96];
    std::snprintf(perf_buf, sizeof(perf_buf), "FPS %.0f · CPU %u%% · flush %.1fms", perf.fps,
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
            std::snprintf(buf, sizeof(buf), "%s: 缺失", label);
        } else {
            uint64_t used =
                media->total_bytes > media->free_bytes ? media->total_bytes - media->free_bytes : 0;
            std::snprintf(buf, sizeof(buf), "%s: %s  %s / %s", label,
                          media->online ? "在线" : "离线", format_bytes(used).c_str(),
                          format_bytes(media->total_bytes).c_str());
        }
        return std::string(buf);
    };

    lv_label_set_text(storage_int_label_, format_row("内置 Flash", int_status).c_str());
    lv_label_set_text(storage_sd_label_, format_row("SD 卡", sd_status).c_str());
}

void SelfCheck::touch_event_cb(lv_event_t* e) {
    auto* self = static_cast<SelfCheck*>(lv_event_get_user_data(e));
    if (lv_event_get_code(e) == LV_EVENT_PRESSING) {
        lv_indev_t* indev = lv_indev_active();
        lv_point_t p{};
        if (indev != nullptr) {
            lv_indev_get_point(indev, &p);
        }
        char buf[48];
        std::snprintf(buf, sizeof(buf), "触点: (%d, %d)", static_cast<int>(p.x),
                      static_cast<int>(p.y));
        lv_label_set_text(self->touch_label_, buf);
    } else {
        lv_label_set_text(self->touch_label_, "触摸测试区");
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
