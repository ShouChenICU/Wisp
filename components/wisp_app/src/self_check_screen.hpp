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
#include "wisp/core/button_service.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/core/settings_schema.hpp"
#include "wisp/core/state_cell.hpp"
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

    // The talk overlay's room at the bottom of the screen
    // (shell::TalkOverlay::end_room(), 0 with no card): the page ends its
    // scroll range above it while it lasts, then goes back to its own end.
    void set_end_room(int32_t room);

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

    // Adds the button test's ButtonService listener.
    void subscribe_buttons();
    // wisp.button, under ButtonService's lock: posts "<id>.<edge>" to the
    // button test.
    void on_button_edge(const core::ButtonEdge& e);
    // UI thread: follows the power level, Wi-Fi, hotspot, scenes overview
    // and settings cells (see the members below).
    void subscribe_cells();
    // UI thread: re-reads the Wi-Fi state into the network section, for
    // the cells' callbacks and the live values (see kNetLiveReadMs).
    void sync_net();

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
    // UI thread: the scene list is needed afresh, at build and on each
    // move of the overview's catalog; asks for it (ask_scene_list()) unless
    // an ask is in flight, and owes it until an answer to an ask made
    // since is shown.
    void request_scene_list();
    // UI thread: asks the storage worker for the scene list; the "Scenes"
    // card takes it on the UI thread if no newer need came meanwhile.
    void ask_scene_list();
    // UI thread, on the refresh tick: asks again for a scene list that is
    // owed (see scene_list_owed_): kSceneListRetryMs after a refused ask,
    // kSceneListLostMs after one the worker took.
    void retry_scene_list(uint64_t now);
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
    // The card's id: the medium with role removable, unless it also holds
    // role internal (StorageManager then treats it as internal only).
    // Empty: no card, the boot report's rows stand as they are.
    const std::string sd_id_;

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

    // Whether the device list's card row (sd_id_) follows StorageManager::
    // status() live (see build_devices()).
    bool sd_row_live_ = false;
    // This boot's interactive test results. UI thread only.
    InteractiveTests tests_;
    // The last loopback status shown, re-rendered by set_page_lang(). UI
    // thread only.
    LoopbackStatus loop_status_;
    // The settings cell's utc_offset_min and time_24h as last delivered (or
    // read at build), for the time sync row. UI thread only.
    int utc_offset_min_ = core::SettingsSnapshot{}.utc_offset_min;
    bool time_24h_ = core::SettingsSnapshot{}.time_24h;
    // os::now_ms() of the last read of the live network values (see
    // kNetLiveReadMs).
    uint64_t last_net_read_ms_ = 0;
    // os::now_ms() of the last storage request (see kStorageResyncMs), and
    // whether its answer is still on the way.
    uint64_t last_storage_ms_ = 0;
    bool storage_pending_ = false;
    // The last answer, shown again in a new page language.
    std::optional<core::StorageStatus> last_storage_;
    // Whether the scene list owes a listing: from request_scene_list()
    // until an answer to an ask made since is shown. A refused job, a
    // failed listing and an answer dropped by a full UI queue all leave it
    // set, and nothing else would list again until the catalog moves.
    // scene_list_gen_ counts the needs: an answer to an ask made before
    // the latest need is not shown, and asks again. scene_list_ask_
    // numbers the asks; scene_list_in_flight_ says whether the worker took
    // the latest and its answer has not come, which holds back a newer
    // need's ask and the 1 s retry (see kSceneListLostMs).
    // last_scene_list_ms_ is os::now_ms() at the latest ask. UI thread
    // only.
    bool scene_list_owed_ = false;
    bool scene_list_in_flight_ = false;
    uint32_t scene_list_gen_ = 0;
    uint32_t scene_list_ask_ = 0;
    uint64_t last_scene_list_ms_ = 0;

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

    // --- Subscriptions, all released at the start of ~SelfCheck() (the
    // cells first thing, the button listener after the sampler stops),
    // before App::stop() resets their owners. ---
    // App's ButtonService listener; 0 once removed.
    uint32_t button_listener_ = 0;
    // Button-test posts dropped by a full post queue; the listener logs the
    // first and every kDroppedPostLogInterval-th.
    std::atomic<uint32_t> dropped_button_posts_{0};
    // PowerManager::level_cell(), on wisp.bus: the sampler's standby flag
    // (memory only), off the UI thread.
    core::CellSubscription power_sub_;
    // WifiService::wifi_cell() and ap_cell(), on the UI thread: sync_net().
    // None without Wi-Fi.
    core::CellSubscription wifi_sub_;
    core::CellSubscription ap_sub_;
    // SceneRuntime::overview(), on the UI thread: the scene list on a
    // catalog move, follow_foreground() on a foreground change. None
    // without a scene runtime.
    core::CellSubscription scene_sub_;
    // SystemSettings::cell(), on the UI thread: the time sync row.
    core::CellSubscription settings_sub_;
};

}  // namespace wisp::app
