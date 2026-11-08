#pragma once

// The self-check screen (spec 8) itself, the hardware diagnostics of mvp
// spec 6.4: a scrollable column of a title with summary chips and titled
// section cards. It is built on demand, on a screen of its own, and torn
// down when the user goes back (see self_check.hpp). The page has a
// language of its own (PageText in self_check_layout.hpp), English until
// its switcher changes it. What the system shows on every screen (the
// power key, the network cards, the confirm dialogs, WISP_PERF) is not
// here but in SystemDuties (system_duties.hpp).
//
// Each section's widgets are a class of their own (self_check_header.hpp,
// self_check_devices.hpp, self_check_sensors.hpp, self_check_readouts.hpp,
// self_check_tests.hpp, net_section.hpp, scene_section.hpp). Nothing outside
// wisp_app's own sources includes this header.
//
// While a scene is open (its own screen loaded over this one) the screen
// keeps running hidden. A language switch re-texts the widgets in place;
// nothing is ever rebuilt.

#include <atomic>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>

#include "loopback.hpp"
#include "lvgl.h"
#include "net_section.hpp"
#include "scene_section.hpp"
#include "scene_wiring.hpp"
#include "self_check_devices.hpp"
#include "self_check_header.hpp"
#include "self_check_model.hpp"
#include "self_check_readouts.hpp"
#include "self_check_sampler.hpp"
#include "self_check_sensors.hpp"
#include "self_check_tests.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/core/settings.hpp"
#include "wisp/hal/clock.hpp"
#include "wisp/hal/display.hpp"
#include "wisp/hal/display_geometry.hpp"
#include "wisp/hal/imu.hpp"
#include "wisp/hal/power.hpp"
#include "wisp/os/os.hpp"

namespace wisp::ui {
class SystemLayer;
class UiRuntime;
}  // namespace wisp::ui

namespace wisp::app {

class App;

class SelfCheck {
public:
    // `scenes` (null: no "Scenes" rows, only the empty card) lists the
    // installed scenes; it must outlive this screen.
    SelfCheck(App& app, ui::SystemLayer& system, SceneWiring* scenes);
    ~SelfCheck();

    // Must be called once, from the UI thread, right after construction
    // (see start_self_check()): builds the page on a new screen and loads
    // it.
    void build();

    // The screen the page is built on; nullptr before build().
    lv_obj_t* screen() const { return screen_; }

private:
    // BootReport's boot_ms.
    uint64_t boot_ms() const;

    // The safe area (spec 5.6) to lay the root column out in: that of
    // the system layer's screen shape, which App::start() derived from
    // display_'s caps (an invalid shape falls back to a circle).
    hal::Area compute_safe_area() const;

    // Creates root_ and every widget under it.
    void build_widgets();
    // UI thread: makes `lang` the page's language and re-texts every label
    // on the page in it. A no-op if it already is. The network cards and
    // the dialogs are system UI and keep the system language.
    void set_page_lang(ui::Lang lang);
    void build_devices(lv_obj_t* col);

    // Any thread: posts `fn` onto the UI thread, where it runs only while
    // this screen is still up (alive_).
    bool post(std::function<void()> fn);

    void subscribe_buttons();
    void on_button_event(const core::Event& e);
    void subscribe_power();
    void on_power_event(const core::Event& e);
    // net.wifi.state / net.ap.state (connectivity spec 5.5): posts
    // sync_net() onto the UI thread.
    void subscribe_net();
    void on_net_event(const core::Event& e);
    // UI thread: re-reads the Wi-Fi state into the network section.
    void sync_net();
    // UI thread: the time sync row follows the stored time zone. One
    // atomic load while nothing changed.
    void sync_settings();

    void start_loopback();
    void toggle_perf_overlay();
    // UI thread: records a passed interactive test and updates its mark
    // and the summary chip.
    void pass_test(InteractiveTest test);

    void refresh_ui();
    // UI thread: the time sync row from TimeSyncService::status().
    void refresh_time_sync();
    // UI thread: the performance card's internal-RAM and PSRAM rows, from
    // Platform::heap_info().
    void refresh_perf_ram();
    // UI thread: asks the storage worker for StorageManager::status() (it
    // reaches the flash, see SceneWiring::storage_status_async()), at most
    // one request at a time; show_storage() takes the answer.
    void request_storage();
    // UI thread: the storage card and the live SD row from `status`.
    void show_storage(const core::StorageStatus& status);
    // Any thread: asks the storage worker for the scene list; the "Scenes"
    // card takes it on the UI thread.
    void request_scene_list();
    // Any thread: lets the sampler drop the IMU while a scene is in front
    // (this screen is covered) and take it again once none is.
    void follow_foreground();
    // UI thread: a row was tapped.
    void open_scene(const std::string& id);

    static void refresh_timer_cb(lv_timer_t* timer);

    App& app_;
    ui::UiRuntime* ui_;
    // Its shape is the one the page is laid out in; outlives this screen
    // (see app_impl.hpp).
    ui::SystemLayer& system_;
    std::shared_ptr<hal::Display> display_;
    std::shared_ptr<hal::Power> power_;
    std::shared_ptr<hal::Clock> clock_;
    core::Settings ui_settings_;

    bool overlay_enabled_ = false;

    // --- Sections (their widgets are valid only while this App's
    // UiRuntime is alive). UI thread only. ---
    // The page's language and its fixed labels; outlives every section's
    // widgets.
    PageText text_;
    SelfCheckHeader header_;
    BoardSection board_;
    DeviceSection devices_;
    // connectivity spec 8.1.
    NetSection net_section_;
    // scene-runtime-core spec 8.
    SceneSection scenes_section_;
    SensorSection sensors_;
    PowerClockSection power_clock_;
    StorageSection storage_;
    TestSection tests_section_;
    PerfSection perf_;

    // Whether the device list's "storage.sd" row follows StorageManager::
    // status() live (see build_devices()).
    bool sd_row_live_ = false;
    // This boot's interactive test results. UI thread only.
    InteractiveTests tests_;
    // The last loopback status shown, re-rendered by set_page_lang(). UI
    // thread only.
    LoopbackStatus loop_status_;
    // SystemSettings::revision() as of the last sync_settings(); empty
    // until the first one. UI thread only.
    std::optional<uint32_t> settings_revision_;
    // The utc_offset_min setting as of that sync, for the time sync row.
    // UI thread only.
    int utc_offset_min_ = 480;
    bool time_24h_ = true;
    // os::now_ms() of the last periodic network resync (see kNetResyncMs).
    uint64_t last_net_resync_ms_ = 0;
    // os::now_ms() of the last periodic scene list (see kSceneResyncMs).
    uint64_t last_scene_list_ms_ = 0;
    // os::now_ms() of the last storage request (see kStorageResyncMs), and
    // whether its answer is still on the way.
    uint64_t last_storage_ms_ = 0;
    bool storage_pending_ = false;
    // The last answer, shown again in a new page language.
    std::optional<core::StorageStatus> last_storage_;

    SceneWiring* scenes_;
    // Cleared by the UI-thread teardown: a post, a scene list or a
    // loopback status still on its way to the UI thread then finds this
    // screen gone. UI thread only.
    std::shared_ptr<bool> alive_ = std::make_shared<bool>(true);
    // The screen everything is built on, created by build() and deleted
    // with the page (see the file header).
    lv_obj_t* screen_ = nullptr;

    lv_timer_t* refresh_timer_ = nullptr;
    // The single root container build() creates everything else under, on
    // screen_. Deleted (with screen_, recursively, with every child and so
    // every LV_EVENT_* callback registered with a section as user_data) in
    // ~SelfCheck()'s UI-thread teardown, so that no widget callback can
    // still fire after this SelfCheck is freed.
    lv_obj_t* root_ = nullptr;

    SelfCheckSampler sampler_;

    // --- Bus subscriptions. ---
    core::SubscriptionId button_sub_ = 0;
    core::SubscriptionId power_sub_ = 0;
    core::SubscriptionId net_sub_ = 0;
    core::SubscriptionId scene_sub_ = 0;
};

}  // namespace wisp::app
