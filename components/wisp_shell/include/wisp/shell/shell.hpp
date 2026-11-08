#pragma once

// Shell (MVP spec 6.2, 6.3, 6.6): the device's own pages, native C++
// screens rather than scenes. The device boots into the home screen
// (wisp/shell/home.hpp): the wisp centred in the safe area on the wisp's
// background (black; white on e-paper), the current turn's text and the
// hints.
//
// Pages. A drag on the home screen opens a sub-page and the opposite drag
// on it comes back (navigate(), wisp/shell/gestures.hpp). A sub-page is a
// screen of its own, built when it opens and deleted when the home screen
// comes back: the history (wisp/shell/history.hpp), the scene list
// (wisp/shell/scene_list.hpp) and the settings
// (wisp/shell/settings_page.hpp).
//
// Page drags (spec 6.3). Every page's content sits in one root container
// as large as the screen (Home::root(), page_frame.hpp). Presses are
// tracked on the page's screen, which hears its children's (DragTrack).
// Once the finger passes the slop along an axis with a target, the
// target page is built (a sub-page) and its root moved onto the screen in
// front, next to the current one; from then on both roots follow the
// finger along that axis, and nothing else is built or laid out until the
// release. No drag starts on an object with kNoSwipe (a slider), outside
// the page's root (a card over it), where a scrollable object under the
// finger could still scroll that way (the history's list away from its
// bottom end, the home screen's text column), or while Home keeps the
// press (Home::may_drag()). While the drag runs, the scrollables under
// the finger are not scrollable, so LVGL does not scroll them too, and
// LVGL's long press is held off. The release completes the navigation or
// snaps back (DragTrack::release()); either way the roots slide the rest
// of the way, and at the end the target's root goes back to its own
// screen, which is loaded (completed) or, for a sub-page, deleted
// (snapped back). A press lost to the system layer snaps back. The screen
// in front changes only once the slide is over, in one
// lv_screen_load(), so whoever checks which screen is in front (the scene
// runtime) never sees a screen half loaded; when another screen came in
// front meanwhile (a scene), the drag snaps back instead. show(),
// open_diagnostics() and stop() end a drag or slide at once, snapped back.
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
// The talk overlay (wisp/shell/talk_overlay.hpp, mvp spec 6.5): while a
// scene is in front (the active screen is none of the shell's own --
// scene_in_front()), every change of the conversation or the voice state
// is forwarded to it; its own tap opens the home screen the same way
// leave_scene() does. Any "scene.state" event hides it at once, since the
// scene that was in front may no longer be.
//
// Threading: every member is UI thread only, like every lv_* call. A back
// swipe's work is posted (it arrives from inside the pointer's read
// callback, where the screen under the finger must not be deleted), and a
// drag's end runs on its slide's animation, never inside the release's
// event; a post still queued after stop() does nothing.

#include <atomic>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <vector>

#include "lvgl.h"
#include "wisp/base/result.hpp"
#include "wisp/base/value.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/core/system_settings.hpp"
#include "wisp/hal/display_geometry.hpp"
#include "wisp/shell/scene_list.hpp"
#include "wisp/voice/voice_service.hpp"

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
class DragTrack;
class Home;
class HistoryPage;
class SceneRecents;
class SettingsPage;
class TalkOverlay;

enum class Page : uint8_t { Home, Settings, Scenes, History };
enum class Swipe : uint8_t;
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
    // bytes at most kIconMaxBytes (or only their icon_hash), available
    // false while the scene's files are on an SD card that is out. Asked
    // each time the scene list opens or a scene changes, so it should keep
    // what it read rather than read it all again.
    std::function<ShellListing()> list_scenes;
    // Scene `id`'s icon file (at most kIconMaxBytes), or null, on the
    // storage worker: for an icon list_scenes named by its hash alone that
    // the shell has not decoded. Empty: such an icon is not shown.
    std::function<std::shared_ptr<const os::LargeBuffer>(const std::string&)> read_scene_icon;
    // Whether deleting scene `id` would find data to delete with it, on the
    // storage worker, asked when its long-press card opens. Empty: the card
    // never offers to delete data.
    std::function<bool(const std::string&)> scene_has_data;
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
    const hal::ScreenShape& shape() const { return deps_.shape; }
    // From a page drag's start until its slide is over.
    bool sliding() const { return drag_.phase != DragPhase::Idle; }
    // nullptr before start() and after stop().
    lv_obj_t* home_screen() const;
    // The open sub-page's screen, or nullptr on the home screen. During a
    // drag from the home screen, the page being dragged in.
    lv_obj_t* page_screen() const { return sub_; }
    // That page's content (page_frame.hpp's root).
    lv_obj_t* page_root() const { return sub_root_; }
    // The wisp on the home screen; nullptr without one (no memory).
    ui::WispView* wisp() const;
    // The home screen's content; nullptr before start() and after stop().
    Home* home() const { return home_.get(); }
    // The open sub-page, or nullptr when another page (or none) is open.
    HistoryPage* history() const { return history_.get(); }
    SceneList* scene_list() const { return scenes_.get(); }
    SettingsPage* settings_page() const { return settings_.get(); }
    // The talk overlay; never null once constructed (hidden, not absent,
    // while there is nothing to show).
    TalkOverlay* talk() const { return talk_.get(); }

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

    // Page drags.
    enum class DragPhase : uint8_t { Idle, Following, Sliding };
    struct Drag {
        DragPhase phase = DragPhase::Idle;
        Page to = Page::Home;
        // The screen in front, which both roots are on meanwhile.
        lv_obj_t* host = nullptr;
        lv_obj_t* from_root = nullptr;
        lv_obj_t* to_root = nullptr;
        // The finger's direction, and how far it can go.
        int32_t ux = 0, uy = 0, extent = 1;
        int32_t offset = 0;
        bool complete = false;
        // Made unscrollable while the drag runs.
        std::vector<lv_obj_t*> frozen;
        // What the press started on (watched for its deletion; nullptr
        // when that is the host itself), and the press's input device.
        lv_obj_t* target = nullptr;
        lv_indev_t* indev = nullptr;
    };
    static void press_cb(lv_event_t* e);
    void on_press(lv_event_t* e);
    // Whether a drag that way may take the press that started on `target`
    // on the screen `scr`.
    bool may_drag(lv_obj_t* scr, lv_obj_t* target, Swipe dir) const;
    void begin_drag(Page to, Swipe dir, lv_obj_t* target);
    void place_drag(int32_t offset);
    void settle_drag(bool complete);
    void finish_drag();
    // Ends a drag or its slide at once, snapped back.
    void abort_drag();
    // A drag whose press can no longer end it on the host snaps back:
    // the object it started on was deleted (LVGL sends no PRESS_LOST
    // then, and the press may go on to something outside the host), or
    // the device reads released without the host seeing it.
    static void target_deleted_cb(lv_event_t* e);
    void check_drag();
    static void slide_cb(void* var, int32_t v);
    static void slide_done_cb(lv_anim_t* a);
    void close_diagnostics();
    void set_diag_back_handler();
    static void diag_loaded_cb(lv_event_t* e);

    // The talk overlay. True while the active screen is a scene's own
    // (none of the shell's).
    bool scene_in_front() const;
    // Coalesces a burst of conversation/voice changes into one UI-thread
    // update_talk() already queued.
    void request_talk();
    void update_talk();
    void on_voice_bus(const core::Event& e);
    static void talk_timer_cb(lv_timer_t* t);

    ShellDeps deps_;
    Page page_ = Page::Home;
    // What was active before start(), loaded again by stop().
    lv_obj_t* prev_ = nullptr;
    std::unique_ptr<Home> home_;
    lv_obj_t* home_screen_ = nullptr;
    // The open sub-page's screen and its content.
    lv_obj_t* sub_ = nullptr;
    lv_obj_t* sub_root_ = nullptr;
    // The press being tracked on the page in front, until it becomes a
    // drag or is ruled out.
    std::unique_ptr<DragTrack> track_;
    bool pressing_ = false;
    bool press_top_zone_ = false;
    lv_obj_t* press_target_ = nullptr;
    Drag drag_;
    // SceneListDeps::has_data over ShellDeps::scene_has_data, asked on the
    // storage worker; empty without either.
    std::function<void(const std::string&, std::function<void(bool)>)> scene_has_data_query();

    std::unique_ptr<HistoryPage> history_;
    std::unique_ptr<SceneList> scenes_;
    std::unique_ptr<SettingsPage> settings_;
    // Replaced with each sub-page: a scene listing still on its way finds
    // its page gone.
    std::shared_ptr<bool> page_alive_;
    // scene.state while started.
    core::SubscriptionId scene_sub_ = 0;
    // Set while a listing for the current page waits on the worker, so a
    // burst of events queues one. Replaced with each sub-page, like
    // page_alive_: a new page never waits on an old page's listing, whose
    // answer it would not get.
    std::shared_ptr<std::atomic<bool>> listing_;
    // Decoded icons by scene id, kept across listings and pages so an
    // unchanged icon is decoded once. Touched on the worker only.
    struct IconCache;
    std::shared_ptr<IconCache> icons_;
    // The last listing shown, icons as decoded so far: what the scene list
    // shows at once when it opens again. Empty before the first.
    std::optional<std::vector<ShellScene>> scene_cache_;
    // The open scene list's timing, until its first listing takes it.
    struct ListTiming;
    std::shared_ptr<ListTiming> open_timing_;
    // The diagnostics' screen while it is open (owned by the App's
    // self-check object).
    lv_obj_t* diag_ = nullptr;

    std::unique_ptr<TalkOverlay> talk_;
    uint32_t talk_listener_ = 0;  // on deps_.conversation
    core::SubscriptionId talk_voice_sub_ = 0;
    lv_timer_t* talk_timer_ = nullptr;
    std::atomic<voice::VoiceState> voice_state_{voice::VoiceState::Idle};
    std::atomic<uint8_t> voice_level_{0};
    std::atomic<bool> talk_posted_{false};

    // Cleared by stop(): a back swipe's post queued before it does
    // nothing.
    std::shared_ptr<bool> alive_;
};

}  // namespace wisp::shell
