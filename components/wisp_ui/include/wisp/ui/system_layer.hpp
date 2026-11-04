#pragma once

// The system layer (scene runtime spec 3.5/3.6/8): what the system shows
// above whatever screen is up -- the back gesture zone, the scene error
// panel, the on-screen keyboard and the modal confirm dialogs.
//
// There is no persistent container. Each piece is a direct child of
// lv_layer_top() that exists (or is shown) only while it is in use: a
// shown, unmarked top-layer object stops wisp/ui/overlay.hpp from cutting
// the areas it touches, so one full-screen container would make every
// scroll under the provisioning card redraw the card as well.
//   - The gesture strip is a narrow transparent bar along the left edge of
//     the safe area. It is unmarked, and shown only while a back handler
//     is set; its hit test follows the visible edge row by row, so the
//     rest of its pixels still reach the screen below.
//   - The error panel and the keyboard are opaque and marked as opaque
//     overlays.
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

    // Sets what a back swipe (is_back_swipe()) does and shows the gesture
    // strip; an empty function hides it again. Presses inside the zone
    // never reach the screen below while the strip is shown.
    void set_back_handler(std::function<void()> on_back);

    // Shows the error panel inside the safe area, replacing one already
    // up, and hides the keyboard. The panel takes every touch on it; its
    // buttons hide it and then call `on_reload` or `on_back`.
    void show_error(ErrorPanelInfo info, std::function<void()> on_reload,
                    std::function<void()> on_back);
    void hide_error();
    bool error_visible() const { return error_panel_ != nullptr; }

    // Shows the keyboard over the lower half of the safe area, typing
    // into `textarea`; for another textarea than the one it types into,
    // a fresh keyboard replaces it. It hides itself on LV_EVENT_READY /
    // LV_EVENT_CANCEL and when `textarea` is deleted; its language key
    // switches between pinyin and plain Latin input (pinyin first when
    // the UI language is Chinese, then whichever was used last).
    void show_keyboard(lv_obj_t* textarea);
    void hide_keyboard();
    // The lv_keyboard while it is up, else nullptr.
    lv_obj_t* keyboard() const { return kb_; }

    // Opens a modal Yes/No dialog and returns its box. Answering closes
    // it and then calls `on_answer`. Only one dialog is open at a time:
    // while one is, this returns nullptr and never calls `on_answer`.
    lv_obj_t* open_confirm(StrId title, const std::string& text,
                           std::function<void(bool yes)> on_answer);
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
    // Whether a press from `start` released at `end` is a back swipe: it
    // started in the zone and moved right by more than a quarter of the
    // screen width, more sideways than up or down.
    static bool is_back_swipe(const hal::ScreenShape& s, lv_point_t start, lv_point_t end);

private:
    void build_keyboard(lv_obj_t* textarea);
    // Hides the keyboard at once and deletes it now or, from inside one
    // of its own events, on the next timer pass.
    void discard_keyboard(bool now);

    static void strip_event_cb(lv_event_t* e);
    static void error_btn_cb(lv_event_t* e);
    static void key_cb(lv_event_t* e);
    static void kb_done_cb(lv_event_t* e);
    static void ta_delete_cb(lv_event_t* e);
    static void confirm_btn_cb(lv_event_t* e);

    hal::ScreenShape shape_;
    hal::Area safe_;

    lv_obj_t* strip_ = nullptr;
    std::function<void()> on_back_;
    lv_point_t press_start_{};
    bool pressing_ = false;

    lv_obj_t* error_panel_ = nullptr;
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
