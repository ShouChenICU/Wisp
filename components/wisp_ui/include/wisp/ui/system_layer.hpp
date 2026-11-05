#pragma once

// The system layer (scene runtime spec 3.5/3.6/8): what the system shows
// above whatever screen is up -- the back gesture, the scene error panel,
// the on-screen keyboard and the modal confirm dialogs.
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
//   - A confirm dialog is an lv_msgbox under LVGL's full-screen backdrop.
//     The dialogs guard dangerous actions (factory reset, firmware
//     install, deleting a scene), so the backdrop keeps every touch
//     outside the box from reaching the UI below. Neither the box nor the
//     backdrop is marked (the backdrop is translucent), so while a dialog
//     is open nothing under it skips a redraw; dialogs are rare and short.
//
// Every member is UI thread only, like every lv_* call.

#include <cstdint>
#include <functional>
#include <string>

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

    // Moves every piece that is up to the front of the top layer, the
    // dialog (with its backdrop) last. Whoever adds other top-layer
    // objects calls this afterwards.
    void raise();

    // Whether (x, y) is in the back gesture zone: a row of the safe area,
    // within kBackZoneWidth pixels of that row's visible left edge.
    static bool in_back_zone(const hal::ScreenShape& s, int32_t x, int32_t y);
    // Whether a press from `start` now at `end` is a back swipe: it
    // started in the zone and moved right by at least kBackTriggerDx, at
    // any angle.
    static bool is_back_swipe(const hal::ScreenShape& s, lv_point_t start, lv_point_t end);

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
    bool pressed_ = false;    // the last read was a press
    bool candidate_ = false;  // the press started in the zone
    bool taken_ = false;      // it crossed the trigger and was taken
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
};

namespace detail {
// The keyboard's pinyin dictionary, generated by tools/gen_pinyin_dict.py
// into pinyin_dict.cpp: sorted by pinyin, ended by {nullptr, nullptr}.
const lv_pinyin_dict_t* pinyin_dict();
}  // namespace detail

}  // namespace wisp::ui
