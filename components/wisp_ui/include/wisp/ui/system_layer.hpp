#pragma once

// The system layer (scene runtime spec 3.5/3.6/8, scene-device spec
// 5.3/5.5/5.6): what the system shows above whatever screen is up -- the
// back gesture, the status capsule, the pull-down panel and its top-edge
// swipe, notification banners, the scene error panel, the on-screen
// keyboard and the modal confirm dialogs.
//
// There is no persistent container. Each piece is a direct child of
// lv_layer_top() that exists (or is shown) only while it is in use: a
// shown, unmarked top-layer object stops wisp/ui/overlay.hpp from cutting
// the areas it touches, so one full-screen container would make every
// scroll under the provisioning card redraw the card as well.
//   - The back gesture has no object to hit. While a back handler is set
//     the layer watches the pointer's reads (it wraps the pointer input
//     device's read callback) without taking them, so every press reaches
//     the widget under it as usual. Only once a press from the left edge
//     has moved far enough to the right does it take the press away from
//     that widget. The back arrow that follows the finger is unmarked and
//     shown only during such a swipe.
//   - The error panel is a card sized to what it holds, over an opaque
//     backdrop that hides the whole screen and takes every touch the
//     card does not. Both are marked as opaque overlays, as is the
//     keyboard.
//   - The top-edge swipe that opens the pull-down panel is watched the
//     same way, by the same observer: a press from the top zone that has
//     moved far enough down is taken from the widget under it. Whichever
//     of the two gestures crosses its trigger first owns the press.
//   - The status capsule is a small opaque pill at status_area(), shown
//     only while there is something to count, and marked.
//   - A banner is an opaque card near the top, shown for kBannerMs, one
//     at a time, and marked.
//   - The pull-down panel is a full-screen opaque backdrop without
//     children, marked like the error panel's, and over it a content
//     box exactly the safe area, marked as well. Content that scrolls
//     under the panel is cut by both.
//   - A confirm dialog is an lv_msgbox under LVGL's full-screen backdrop.
//     The dialogs guard dangerous actions (factory reset, firmware
//     install, deleting a scene), so the backdrop keeps every touch
//     outside the box from reaching the UI below. Neither the box nor the
//     backdrop is marked (the backdrop is translucent), so while a dialog
//     is open nothing under it skips a redraw; dialogs are rare and short.
//
// Every member is UI thread only, like every lv_* call.

#include <cstdint>
#include <deque>
#include <functional>
#include <string>
#include <vector>

#include "lvgl.h"
#include "wisp/hal/display_geometry.hpp"
#include "wisp/ui/strings.hpp"
#include "wisp/ui/theme.hpp"

namespace wisp::ui {

// What the error panel says about a scene that failed (spec 3.6).
struct ErrorPanelInfo {
    std::string scene_name;
    std::string file;
    int line = 0;
    std::string message;
};

// What the status capsule counts (spec 5.5).
struct StatusCounts {
    int background = 0;
    int unread = 0;
};

// A background scene in the pull-down panel.
struct PanelScene {
    std::string id;
    std::string name;
    uint64_t since_ms = 0;  // when it went to the background
};

// A notification in the pull-down panel, its text already rendered (a
// system notice through system_notice_text()).
struct PanelNote {
    std::string source, id;
    std::string badge;  // the first character of the scene's name; "" for system
    std::string title, body;
    std::string action_label;  // "" or the action button's text
};

// The panel's first row. `battery` < 0: unknown; `wifi` is shown as given
// ("" shows no Wi-Fi item).
struct PanelHeader {
    std::string time;
    int battery = -1;
    bool charging = false;
    std::string wifi;
};

// Everything the panel shows. Notes are newest first; `now_ms` is the time
// the scenes' since_ms are measured against.
struct PanelModel {
    PanelHeader header;
    std::vector<PanelScene> scenes;
    std::vector<PanelNote> notes;
    uint64_t now_ms = 0;
};

// What the panel's rows do. All empty: there is no panel.
struct PanelHandlers {
    // The panel is about to open; the owner refreshes the model.
    std::function<void()> on_open;
    std::function<void(const std::string& id)> open_scene, exit_scene;
    std::function<void(const std::string& source, const std::string& id)> tap_note, dismiss_note,
        note_action;
    std::function<void()> clear_all;
};

// One notification banner (spec 5.3).
struct BannerInfo {
    std::string source, id, badge, name, title, body;
};

class SystemLayer {
public:
    // Width of the back gesture zone along the left edge of each row of
    // the safe area (spec 3.5).
    static constexpr int32_t kBackZoneWidth = 16;
    // How far right a press from the zone moves before it is a back swipe.
    static constexpr int32_t kBackTriggerDx = 24;
    // How far right a press from the zone moves before the arrow shows.
    static constexpr int32_t kBackArrowSlop = 8;
    // Diameter of the back arrow shown during a swipe.
    static constexpr int32_t kBackArrowSize = 40;
    // The keyboard's language key, naming the current input: pinyin or
    // plain Latin letters. Fixed text in every UI language, so the key
    // maps can be constant tables.
    static constexpr const char* kPinyinKey = "中";
    static constexpr const char* kLatinKey = "EN";
    // The status capsule's size and its gap below the safe area's top
    // (refinement of spec 5.5; see status_area()).
    static constexpr int32_t kCapsuleW = 56, kCapsuleH = 24, kCapsuleTopGap = 4;
    // The top-edge swipe (spec 5.6): the zone's depth below each column's
    // first visible pixel, and how far down a press from it moves before
    // it opens the panel.
    static constexpr int32_t kTopZoneHeight = 16, kTopTriggerDy = 24;
    // The least height of a panel row, and a banner's height.
    static constexpr int32_t kPanelRowMinH = 48, kBannerH = 72;
    // How long a banner stays up.
    static constexpr uint32_t kBannerMs = 4000;

    // Creates nothing yet; each piece is built when first used.
    explicit SystemLayer(const hal::ScreenShape& shape);
    // Deletes every piece still up, without calling any callback.
    ~SystemLayer();

    SystemLayer(const SystemLayer&) = delete;
    SystemLayer& operator=(const SystemLayer&) = delete;

    const hal::ScreenShape& shape() const { return shape_; }

    // Sets what a back swipe does; an empty function turns the gesture off.
    // A press that starts in the zone (in_back_zone()) reaches the screen
    // below like any other. Once it has moved far enough to the right
    // (is_back_swipe()) the press is taken from the widget under it -- it
    // gets LV_EVENT_PRESS_LOST and no click, and a scroll it started stops
    // -- and an arrow follows the finger. Releasing still past the trigger
    // calls `on_back`; releasing after moving back left does not. A press
    // never starts a swipe while a confirm dialog is open or on the
    // keyboard.
    void set_back_handler(std::function<void()> on_back);
    // The back arrow while a swipe shows it, else nullptr.
    lv_obj_t* back_arrow() const;

    // Shows the counts in the status capsule, at status_area(): the
    // background scenes after LV_SYMBOL_PLAY and the unread notifications
    // after LV_SYMBOL_BELL, each only when it is not 0. Both 0 hides it.
    void set_status(StatusCounts c);
    // The capsule while it is shown, else nullptr.
    lv_obj_t* capsule() const;

    // Sets what the pull-down panel's rows do. With any handler set, a tap
    // on the capsule or a top-edge swipe opens the panel; with none, there
    // is no panel (an open one closes), no swipe, and the capsule takes no
    // touches. The swipe is watched like the back gesture: a press from
    // the top zone (in_top_zone()) reaches the screen below, and once it
    // has moved kTopTriggerDy down (is_top_swipe()) it is taken from the
    // widget under it, which gets LV_EVENT_PRESS_LOST; the release opens
    // the panel. A press never starts it while the keyboard or a confirm
    // dialog is up, or the panel is already open.
    void set_panel_handlers(PanelHandlers h);
    // Calls on_open, then shows the model over the whole screen.
    // Rows inside the safe area, each at least kPanelRowMinH tall: the
    // header (time, battery, Wi-Fi), the background scenes (a tap calls
    // open_scene and closes the panel, the close button exit_scene) and
    // the notifications (a tap calls tap_note and closes the panel; a
    // swipe to the left or the close button dismiss_note; the action
    // button note_action), then a clear-all button (clear_all). A tap on
    // no row, an upward swipe where the content does not scroll, or a
    // back swipe closes it; while it is open the back swipe calls no back
    // handler.
    void open_panel();
    void close_panel();
    bool panel_open() const { return panel_ != nullptr; }
    // Keeps `m` for the next open_panel(), and shows it now if open.
    void update_panel(PanelModel m);
    // The panel's content box (the safe area) while open, else nullptr.
    lv_obj_t* panel() const { return panel_; }

    // Queues a banner. One shows at a time, near the top of the safe area
    // and as wide as the rows it covers allow; it goes after kBannerMs or
    // an upward swipe, and a tap hides it and calls `on_tap`. Then the
    // next one shows.
    void show_banner(BannerInfo b, std::function<void()> on_tap);
    // The banner shown now, else nullptr.
    lv_obj_t* banner() const { return banner_; }

    // Shows the error panel, replacing one already up, and hides the
    // keyboard. It is a card as wide as the safe area, as tall as what it
    // holds (at most the safe area's height; a long message scrolls
    // inside it) and centred in it, over a backdrop that takes every
    // touch. Its buttons hide it and then call `on_reload` or `on_back`.
    void show_error(ErrorPanelInfo info, std::function<void()> on_reload,
                    std::function<void()> on_back);
    void hide_error();
    bool error_visible() const { return error_panel_ != nullptr; }

    // Shows the keyboard, typing into `textarea`. Its background runs from
    // about the middle of the safe area to the left, right and bottom
    // edges; each key row spans the visible width of its own rows, less a
    // few pixels. Where those rows are too narrow (a round screen) it
    // rises, at most to 55% of the height and never over `textarea`. For
    // another textarea than the one it types into, a fresh keyboard
    // replaces it. It hides itself on LV_EVENT_READY /
    // LV_EVENT_CANCEL and when `textarea` is deleted; its language key
    // switches between pinyin and plain Latin input (pinyin first when
    // the UI language is Chinese, then whichever was used last).
    void show_keyboard(lv_obj_t* textarea);
    void hide_keyboard();
    // The lv_keyboard while it is up, else nullptr.
    lv_obj_t* keyboard() const { return kb_; }

    // Opens a modal Yes/No dialog and returns its box. Yes is a `yes_role`
    // button (Danger for an action that destroys data), No a secondary
    // one. Answering closes it and then calls `on_answer`. Only one dialog
    // is open at a time: while one is, this returns nullptr and never
    // calls `on_answer`.
    lv_obj_t* open_confirm(StrId title, const std::string& text,
                           std::function<void(bool yes)> on_answer,
                           ButtonRole yes_role = ButtonRole::Primary);
    bool confirm_open() const { return confirm_box_ != nullptr; }
    // Closes `box` (if it is the open dialog) without calling its
    // callback.
    void close_confirm(lv_obj_t* box);

    // Moves every piece that is up to the front of the top layer, from
    // the bottom: the capsule, the banner, the panel, the error panel, the
    // keyboard, the back arrow and the dialog (with its backdrop). Whoever
    // adds other top-layer objects calls this afterwards.
    void raise();

    // Whether (x, y) is in the back gesture zone: a row of the safe area,
    // within kBackZoneWidth pixels of that row's visible left edge.
    static bool in_back_zone(const hal::ScreenShape& s, int32_t x, int32_t y);
    // Whether a press from `start` now at `end` is a back swipe: it
    // started in the zone and moved right by at least kBackTriggerDx, at
    // any angle.
    static bool is_back_swipe(const hal::ScreenShape& s, lv_point_t start, lv_point_t end);

    // The capsule's box: kCapsuleW x kCapsuleH, centred horizontally,
    // kCapsuleTopGap below the safe area's top. The same whether or not
    // the capsule shows, so a scene can keep clear of it.
    static hal::Area status_area(const hal::ScreenShape& s);
    // Whether (x, y) is in the top-edge zone: visible, and less than
    // kTopZoneHeight below the first visible pixel of column x (on a
    // circle the middle columns start at y = 0).
    static bool in_top_zone(const hal::ScreenShape& s, int32_t x, int32_t y);
    // Whether a press from `start` now at `end` is a top-edge swipe: it
    // started in the zone and moved down by at least kTopTriggerDy, at any
    // angle.
    static bool is_top_swipe(const hal::ScreenShape& s, lv_point_t start, lv_point_t end);

private:
    // Wraps the pointer input device's read callback while a back handler
    // is set, and puts it back. Only one layer at a time: while another
    // watches the pointer, this one logs an error and has no gesture.
    void hook_pointer();
    void unhook_pointer();
    // Follows one read of the pointer, before LVGL processes it.
    void observe(lv_indev_t* indev, const lv_indev_data_t& data);
    // Takes the current press from whatever widget has it.
    static void cancel_press(lv_indev_t* indev);
    // Ends a scroll the cancelled press had started, back within range.
    static void settle_scroll(lv_obj_t* obj);
    void show_arrow(lv_point_t at, int32_t dx, bool armed);
    void end_swipe();
    // Whether the pointer must be watched: a back handler or a panel.
    bool wants_pointer() const;
    void update_pointer_hook();
    bool has_panel() const;
    void update_capsule_touch();

    void build_panel();
    void fill_panel();
    // Removes the panel now or, from inside one of its events, on the
    // next timer pass.
    void discard_panel();
    void show_next_banner();
    void hide_banner();

    static void capsule_cb(lv_event_t* e);
    static void panel_cb(lv_event_t* e);
    static void banner_cb(lv_event_t* e);
    static void banner_timer_cb(lv_timer_t* t);
    static void panel_refill_cb(void* self);
    static void pointer_read_cb(lv_indev_t* indev, lv_indev_data_t* data);

    // Where the keyboard's band starts, and its bottom padding below the
    // key rows.
    struct KeyboardPlace {
        int32_t top;
        int32_t pad_bottom;
    };
    KeyboardPlace place_keyboard(lv_obj_t* textarea) const;
    void build_keyboard(lv_obj_t* textarea);
    // Hides the keyboard at once and deletes it now or, from inside one
    // of its own events, on the next timer pass.
    void discard_keyboard(bool now);

    static void error_btn_cb(lv_event_t* e);
    static void key_cb(lv_event_t* e);
    static void kb_done_cb(lv_event_t* e);
    static void ta_delete_cb(lv_event_t* e);
    static void confirm_btn_cb(lv_event_t* e);

    hal::ScreenShape shape_;
    hal::Area safe_;

    std::function<void()> on_back_;
    lv_indev_t* indev_ = nullptr;  // while hooked
    lv_indev_read_cb_t prev_read_cb_ = nullptr;
    bool pressed_ = false;        // the last read was a press
    bool candidate_ = false;      // the press started in the back zone
    bool taken_ = false;          // it crossed the trigger and was taken
    bool top_candidate_ = false;  // the press started in the top zone
    bool top_taken_ = false;      // it crossed the top trigger and was taken
    lv_point_t start_{};
    lv_obj_t* arrow_ = nullptr;  // hidden while no swipe shows it

    lv_obj_t* error_panel_ = nullptr;  // the card
    lv_obj_t* error_backdrop_ = nullptr;
    lv_obj_t* error_reload_ = nullptr;
    std::function<void()> on_reload_;
    std::function<void()> on_error_back_;

    lv_obj_t* kb_ = nullptr;
    lv_obj_t* ta_ = nullptr;   // what kb_ types into
    lv_obj_t* ime_ = nullptr;  // only while typing pinyin
    lv_obj_t* candidates_ = nullptr;
    bool pinyin_;
    // > 0 while LVGL's keyboard handler runs from key_cb(): the keyboard
    // must then outlive the event.
    int kb_event_depth_ = 0;

    lv_obj_t* confirm_box_ = nullptr;
    lv_obj_t* confirm_yes_ = nullptr;
    std::function<void(bool)> on_answer_;

    lv_obj_t* capsule_ = nullptr;  // hidden while both counts are 0
    lv_obj_t* capsule_label_ = nullptr;

    PanelHandlers panel_handlers_;
    PanelModel model_;
    lv_obj_t* panel_ = nullptr;  // the content box
    lv_obj_t* panel_backdrop_ = nullptr;
    // > 0 while one of the panel's event callbacks runs: the panel must
    // then outlive the event.
    int panel_event_depth_ = 0;
    bool refill_queued_ = false;

    struct QueuedBanner {
        BannerInfo info;
        std::function<void()> on_tap;
    };
    std::deque<QueuedBanner> banners_;  // waiting, the next first
    lv_obj_t* banner_ = nullptr;
    std::function<void()> banner_tap_;
    lv_timer_t* banner_timer_ = nullptr;
    int banner_event_depth_ = 0;
};

namespace detail {
// The keyboard's pinyin dictionary, generated by tools/gen_pinyin_dict.py
// into pinyin_dict.cpp: sorted by pinyin, ended by {nullptr, nullptr}.
const lv_pinyin_dict_t* pinyin_dict();
}  // namespace detail

}  // namespace wisp::ui
