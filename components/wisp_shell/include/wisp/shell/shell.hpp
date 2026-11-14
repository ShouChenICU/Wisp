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
// Choice pages (wisp/shell/choice_page.hpp, mvp spec 6.4). A settings
// row that opens one has open_choice() build it on a screen of its own
// and load it, over the settings page, which stays as it is. The page
// is then still Settings (page()), with choice_page() set. A drag to the
// right on it brings the settings page back the same way a page drag
// does, following the finger, and deletes the choice page; so does a
// pick, at once. show(), open_diagnostics() and stop() close it first.
//
// Language. The shell follows ui::lang() (which the system duties set
// from the system.settings event): when it changes, the home screen
// re-texts and an open sub-page is built again in the new language, at
// the scroll position it had, on the talk overlay's tick (at most
// a quarter of a second later; while a drag runs, once it is over).
//
// The scene list is listed on a one-shot task of the shell's,
// "wisp.shell.list" (kListStackBytes, its stack in PSRAM: it never touches
// flash, every read is a storage worker job ShellDeps::list_scenes and
// read_scene_icon wait for), sorted by ShellDeps::recents, and handed to
// the page on the UI thread; the task then decodes the icons not decoded
// before, one at a time, posting each. So neither the UI thread nor the
// worker holds a whole listing or a PNG decode. While the page is open
// every scene.state event lists it again. At most one such task runs: a
// request while it lists makes it list once more when done, however many
// requests came (Idle -> Running -> Again). Opening the page shows the
// last listing at once; it lists again only when something may have
// changed (a scene.state since that listing, a lost post, or
// ShellDeps::listing_current saying so). Results carry the lister's
// generation and an older one than shown is dropped. The decoded icons are
// shared: the icon cache (on the task), the last listing and the cards (on
// the UI thread) each hold a reference, and whichever lets go last frees
// the pixels (Mem::Large, no LVGL state). A listing's cards and icons go
// to whichever scene list page is open when they arrive, if they are not
// older than what it shows. stop() joins the task, after telling it to
// stop: the lister gives up after the read in progress (one worker job,
// while the workers still run -- App::stop() stops the shell first), and
// the task then reads no icon, decodes nothing and posts nothing.
//
// The hardware diagnostics (the self-check screen, "System -> Hardware
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
// The talk overlay (wisp/shell/talk_overlay.hpp, mvp spec 6.5): while any
// screen but Home is in front (a scene, a sub-page, a choice or
// connections page, the diagnostics -- !home_in_front()), every change of
// the conversation or the voice state is forwarded to it, and so is the
// moment Home leaves the front; the moment Home comes in front it hides,
// since Home shows the same itself. Its own tap is show(Home): a scene is
// sent back as leave_scene() does, a page or the diagnostics close as
// their own way back closes them. Any "scene.state" event has it look
// again, since the scene that was in front may no longer be; it does not
// hide it, which would drop a say or note the overlay has already taken.
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
#include "wisp/shell/choice_page.hpp"
#include "wisp/shell/connections_page.hpp"
#include "wisp/shell/scene_list.hpp"
#include "wisp/ui/strings.hpp"
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
    // While the diagnostics are open: the talk overlay's room at the
    // bottom of the screen (TalkOverlay::end_room(), 0 with no card), for
    // their scrolling page to end above. UI thread; empty: none.
    std::function<void(int32_t)> diagnostics_end_room;
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
    // The installed scenes, on the shell's listing task "wisp.shell.list",
    // one call at a time: it may wait for the storage worker and must not
    // touch flash itself. Icon bytes at most kIconMaxBytes (or only their
    // icon_hash), available false while the scene's files are on an SD
    // card that is out. Asked when the scene list opens and something may
    // have changed, or a scene changes while it is open, so it should keep
    // what it read rather than read it all again. `cancelled` turns true
    // once the shell stops: the lister then gives up between two of its
    // reads and answers `cancelled`, so stop() waits for one read at most.
    std::function<ShellListing(const std::function<bool()>& cancelled)> list_scenes;
    // Scene `id`'s icon file (at most kIconMaxBytes), or null, on the
    // listing task like list_scenes: for an icon list_scenes named by its
    // hash alone that the shell has not decoded. Empty: such an icon is not
    // shown.
    std::function<std::shared_ptr<const os::LargeBuffer>(const std::string&)> read_scene_icon;
    // Decodes an icon file on the listing task. Empty: decode_scene_icon
    // (a test seam).
    std::function<std::shared_ptr<const SceneIcon>(const os::LargeBuffer&, int32_t side)>
        decode_icon;
    // Whether list_scenes would find nothing new since its last call (no
    // install, update or remove, no card change). UI thread, no file work.
    // Empty: the scene list is read each time it opens.
    std::function<bool()> listing_current;
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
    // The scene list's order; read on the listing task (any thread).
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
    // The settings page's restart and power off, after its confirm dialog;
    // posted to the UI thread. Empty: the row is not there.
    std::function<void()> restart, power_off;
    net::WifiService* wifi = nullptr;
    core::ModelProfileService* models = nullptr;
    std::function<bool(std::function<void()>)> submit_network;
};

class Shell {
public:
    // The scene listing task "wisp.shell.list": a manifest parse or
    // LodePNG's decode of one icon, with a margin.
    static constexpr size_t kListStackBytes = 12288;
    static constexpr int kListPriority = 2;

    explicit Shell(ShellDeps deps);
    // Makes no LVGL call: stop() first when started. Joins a listing task
    // still running (none once stop() has run).
    ~Shell();

    Shell(const Shell&) = delete;
    Shell& operator=(const Shell&) = delete;

    // Builds the home screen and loads it. Idempotent.
    void start();
    // Closes the diagnostics if open, loads the screen that was active
    // before start() and deletes the home screen. Idempotent. Joins the
    // scene listing task first.
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
    // Whether the scene listing task is running (tests).
    bool listing() const;
    // The open sub-page, or nullptr when another page (or none) is open.
    HistoryPage* history() const { return history_.get(); }
    SceneList* scene_list() const { return scenes_.get(); }
    SettingsPage* settings_page() const { return settings_.get(); }
    // The talk overlay; never null once constructed (hidden, not absent,
    // while there is nothing to show).
    TalkOverlay* talk() const { return talk_.get(); }

    // Shows a choice page over the settings page; a no-op on any other
    // page. Its pick, then the page closes (posted).
    void open_choice(ChoiceSpec spec);
    void open_connections(ConnectionKind kind);
    ConnectionsPage* connections_page() const { return connections_.get(); }
    // The open choice page and its screen, or nullptr.
    ChoicePage* choice_page() const { return choice_.get(); }
    lv_obj_t* choice_screen() const { return choice_screen_; }

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
    // Deletes the choice page, loading the settings page first when the
    // choice page is in front.
    void close_choice();
    // The open sub-page built again (a language change).
    void rebuild_page();
    // Re-texts the pages when ui::lang() changed.
    void follow_lang();
    // Destroys the sub-page objects (their screen stays).
    void drop_pages();
    // Lists the scenes on the listing task for the open scene list; when
    // `opening`, not if the last listing is known to be current.
    void list_scenes(bool opening = false);
    // Whether the last listing is still what list_scenes would say.
    bool scene_cache_current() const;
    // Marks the listing task stopping and joins it.
    void join_listing();
    // The listing task's state, and what it runs (shell.cpp).
    struct ListRun;
    struct ListJob;

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
        // From a choice page back to the settings page.
        bool from_choice = false;
        bool from_detail = false;
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

    // The talk overlay. True while the home screen is the active screen.
    bool home_in_front() const;
    // The home screen loaded (the overlay hides) or replaced (it shows
    // what there is).
    static void home_loaded_cb(lv_event_t* e);
    // Coalesces a burst of conversation/voice changes into one UI-thread
    // update_talk() already queued.
    void request_talk();
    void update_talk();
    // Hands the overlay card's room (TalkOverlay::end_room()) to the
    // scrolling pages under it when it changed.
    void sync_end_room();
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
    // The choice page over the settings page, its screen and root.
    std::unique_ptr<ChoicePage> choice_;
    std::unique_ptr<ConnectionsPage> connections_;
    lv_obj_t* choice_screen_ = nullptr;
    lv_obj_t* choice_root_ = nullptr;
    // The language the pages were built in.
    ui::Lang lang_ = ui::Lang::Zh;
    // Replaced with each sub-page: a scene listing still on its way finds
    // its page gone.
    std::shared_ptr<bool> page_alive_;
    // scene.state while started.
    core::SubscriptionId scene_sub_ = 0;
    // The listing task and its requests (see the file header); shared
    // with the task, made by start().
    std::shared_ptr<ListRun> list_run_;
    // Decoded icons by scene id, kept across listings and pages so an
    // unchanged icon is decoded once. Touched on the listing task only.
    struct IconCache;
    std::shared_ptr<IconCache> icons_;
    // The last listing, icons as decoded so far: what the scene list shows
    // at once when it opens again. Empty before the first. Kept up to date
    // while the page is closed too, so a warm open can trust it.
    std::optional<std::vector<ShellScene>> scene_cache_;
    // Its lister's generation, and scene_events_ as it was when that
    // listing started.
    uint64_t cache_gen_ = 0;
    uint64_t cache_events_ = 0;
    // scene.state events seen (bumped on the bus thread), and lost posts
    // of a listing: either makes the last listing stale.
    std::atomic<uint64_t> scene_events_{0};
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
    int32_t end_room_ = 0;  // the room last handed to the pages

    // Cleared by stop(): a back swipe's post queued before it does
    // nothing.
    std::shared_ptr<bool> alive_;
};

}  // namespace wisp::shell
