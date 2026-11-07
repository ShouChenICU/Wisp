#pragma once

// Shell (MVP spec 6.2, 6.3, 6.6): the device's own pages, native C++
// screens rather than scenes. The device boots into the home screen
// (wisp/shell/home.hpp): the wisp centred in the safe area on the wisp's
// background (black; white on e-paper), the current turn's text and the
// hints.
//
// Pages. A swipe on the home screen opens a sub-page and the opposite
// swipe on it comes back (navigate(), wisp/shell/gestures.hpp). A
// sub-page is a screen of its own, built when it opens and deleted when
// the home screen comes back: the history (wisp/shell/history.hpp), the
// scene list (wisp/shell/scene_list.hpp) and the settings
// (wisp/shell/settings_page.hpp). Swipes are tracked on the page's
// screen, which hears its children's presses (page_frame.hpp); a press that
// scrolled the list, or started on a slider, is no swipe, and the
// history's up swipe goes home only from its bottom end.
//
// The scene list is listed on the storage worker (ShellDeps::list_scenes
// in a submit_io job), sorted by ShellDeps::recents, and handed to the
// page on the UI thread; while it is open every scene.state event lists it
// again.
//
// The hardware diagnostics (the self-check screen, "About -> Hardware
// diagnostics" in spec 6.4) is not built at boot: open_diagnostics() has
// the App build it on a screen of its own and load it, and a back swipe
// (or show(Home)) brings the home screen back and has the App tear it
// down. While the diagnostics screen is in front the back gesture is the
// shell's; a scene opened over it takes the gesture and clears it again
// when it goes, so the shell sets it again each time that screen is
// loaded.
//
// A scene in front. show() and open_diagnostics() first send the scene
// in front back (ShellDeps::leave_scene: it goes to the background or
// closes, as the back gesture would) and then load their own screen at
// once, so the scene's view is never left running off-screen without its
// back gesture; the view, no longer the active screen, then goes without
// loading the screen it replaced. When the scene cannot be sent back the
// navigation is refused and the scene stays in front.
//
// Threading: every member is UI thread only, like every lv_* call. A
// swipe's or back swipe's work is posted (it arrives from inside an LVGL
// event or the pointer's read callback, where the screen under the finger
// must not be deleted); a post still queued after stop() does nothing.

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <string>
#include <vector>

#include "lvgl.h"
#include "wisp/base/result.hpp"
#include "wisp/base/value.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/core/system_settings.hpp"
#include "wisp/hal/display_geometry.hpp"
#include "wisp/shell/scene_list.hpp"

namespace wisp::ui {
class SystemLayer;
class UiRuntime;
class WispView;
}  // namespace wisp::ui

namespace wisp::agent {
class AgentService;
}  // namespace wisp::agent

namespace wisp::shell {

class ConversationController;
class Home;
class HistoryPage;
class SceneRecents;
class SettingsPage;

enum class Page : uint8_t { Home, Settings, Scenes, History };
// "home" "settings" "scenes" "history"
const char* to_string(Page p);

struct ShellDeps {
    ui::UiRuntime& ui;
    ui::SystemLayer& layer;
    hal::ScreenShape shape;
    core::EventBus& bus;
    ConversationController* conversation = nullptr;  // null without voice
    // The App builds the self-check screen on a new screen and loads it /
    // tears it down. Called on the UI thread; either may be empty (no
    // diagnostics).
    std::function<void()> open_diagnostics, close_diagnostics;
    // Sends the scene in front back (SceneRuntime::close(), which on the
    // UI thread only queues it): true if there was one, false if none; an
    // error refuses the navigation. Empty: no scenes.
    std::function<Result<bool>()> leave_scene;
    // "http://<ip>:<port>" of the admin page, or "" while there is none;
    // the home screen's hints show it. UI thread. Empty: never shown.
    std::function<std::string()> admin_url;
    // The clock the home screen times its fade, hold and hints by. Empty:
    // os::now_ms.
    std::function<uint64_t()> now_ms;

    // The sub-pages' services. Every one may be empty (or null): the page
    // then leaves that part out or shows nothing. Unless said otherwise
    // they are called on the UI thread and must not touch flash; anything
    // that does runs inside a submit_io job.
    // The history's conversation and "new conversation".
    agent::AgentService* agent = nullptr;
    // The installed scenes, on the storage worker (a submit_io job): icon
    // bytes at most kIconMaxBytes, available false while the scene's files
    // are on an SD card that is out.
    std::function<std::vector<ShellScene>()> list_scenes;
    // Opens a scene in front (SceneRuntime::launch(), which on the UI thread
    // only queues).
    std::function<void(const std::string&)> open_scene;
    // Asks the user and deletes (SceneWiring::ask_delete(), which never
    // waits); an error if it could not even ask.
    std::function<Result<void>(const std::string&, bool with_data)> delete_scene;
    // The scene list's order; read on the storage worker.
    SceneRecents* recents = nullptr;
    // The system settings as last stored (a cache, no flash).
    std::function<core::SystemSettingsValues()> settings;
    // SystemSettings::set(), called inside a submit_io job.
    std::function<void(std::map<std::string, Value>)> set_settings;
    // The Wi-Fi status line, the admin address and the firmware version.
    std::function<std::string()> wifi_status, firmware_version;
    // WifiService::start_provisioning(); the card is the system duties'.
    std::function<void()> start_wifi_setup;
    // Queues a job on the storage worker; false if it was not taken.
    std::function<bool(std::function<void()>)> submit_io;
};

class Shell {
public:
    explicit Shell(ShellDeps deps);
    // Makes no LVGL call: stop() first when started.
    ~Shell();

    Shell(const Shell&) = delete;
    Shell& operator=(const Shell&) = delete;

    // Builds the home screen and loads it. Idempotent.
    void start();
    // Closes the diagnostics if open, loads the screen that was active
    // before start() and deletes the home screen. Idempotent.
    void stop();

    // Logs "shell: page <name>" and loads the page's screen, after sending
    // a scene in front back (refused, nothing changes). Any page closes
    // the diagnostics if they are open; Home deletes a sub-page's screen.
    void show(Page p);
    Page page() const { return page_; }
    // nullptr before start() and after stop().
    lv_obj_t* home_screen() const;
    // The open sub-page's screen, or nullptr on the home screen.
    lv_obj_t* page_screen() const { return sub_; }
    // The wisp on the home screen; nullptr without one (no memory).
    ui::WispView* wisp() const;
    // The home screen's content; nullptr before start() and after stop().
    Home* home() const { return home_.get(); }
    // The open sub-page, or nullptr when another page (or none) is open.
    HistoryPage* history() const { return history_.get(); }
    SceneList* scene_list() const { return scenes_.get(); }
    SettingsPage* settings_page() const { return settings_.get(); }

    // Opens the hardware diagnostics over the home screen; a no-op while
    // they are open. The back handler -> close_diagnostics(), show(Home).
    void open_diagnostics();
    bool diagnostics_open() const { return diag_ != nullptr; }

    // The conversation once the voice has started (it starts after the
    // home screen must be up); nullptr without voice.
    void set_conversation(ConversationController* conversation);

private:
    // False when a scene in front could not be sent back.
    bool leave_scene();
    void post_show(Page p);
    lv_obj_t* build_page(Page p);
    void delete_page();
    // Destroys the sub-page objects (their screen stays).
    void drop_pages();
    // Lists the scenes on the worker for the open scene list.
    void list_scenes();
    static void page_event_cb(lv_event_t* e);
    void close_diagnostics();
    void set_diag_back_handler();
    static void diag_loaded_cb(lv_event_t* e);

    ShellDeps deps_;
    Page page_ = Page::Home;
    // What was active before start(), loaded again by stop().
    lv_obj_t* prev_ = nullptr;
    std::unique_ptr<Home> home_;
    lv_obj_t* home_screen_ = nullptr;
    // The open sub-page's screen, and the press being tracked on it.
    lv_obj_t* sub_ = nullptr;
    bool page_tracking_ = false;
    bool page_top_zone_ = false;
    bool page_at_bottom_ = true;
    lv_obj_t* page_list_ = nullptr;
    int32_t page_scroll_y_ = 0;
    lv_point_t page_start_{};
    std::unique_ptr<HistoryPage> history_;
    std::unique_ptr<SceneList> scenes_;
    std::unique_ptr<SettingsPage> settings_;
    // Replaced with each sub-page: a scene listing still on its way finds
    // its page gone.
    std::shared_ptr<bool> page_alive_;
    // scene.state while started; a listing already queued is not queued
    // again.
    core::SubscriptionId scene_sub_ = 0;
    std::shared_ptr<std::atomic<bool>> listing_;
    // The diagnostics' screen while it is open (owned by the App's
    // self-check object).
    lv_obj_t* diag_ = nullptr;
    // Cleared by stop(): a back swipe's post queued before it does
    // nothing.
    std::shared_ptr<bool> alive_;
};

}  // namespace wisp::shell
