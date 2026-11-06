#pragma once

// WispView: the wisp (spec 6.1) as an LVGL object. It owns a WispModel
// (wisp/ui/wisp_model.hpp), steps and builds it from an lv_timer whose
// period follows WispModel::frame_interval_ms(), and invalidates only the
// dirty box. In LV_EVENT_DRAW_MAIN it draws the shape straight onto the
// layer: on colour panels as anti-aliased lines and rounded fills in the
// theme colour, on mono and e-paper panels (wisp::ui::display_panel()) as
// plain pixel runs, white or black. No off-screen frame. E-paper gets no
// timer at all: one still picture per mood.
//
// The object draws no background; put it on black (on e-paper, white).
//
// UI thread only, like every lv_* call. Deleting obj() frees the view.

#include <cstdint>

#include "lvgl.h"
#include "wisp/hal/display.hpp"
#include "wisp/ui/wisp_model.hpp"

namespace wisp::ui {

// The wisp's width for `percent` of the safe area's short side (spec 6.1).
int wisp_size_px(const hal::Area& safe, int percent);

// The wisp's colour on a panel, and the background it is meant for.
lv_color_t wisp_colour(hal::PanelKind panel);
lv_color_t wisp_background(hal::PanelKind panel);

class WispView {
public:
    // A view size_px wide (and size_px * 5 / 4 high), centred in `parent`.
    // nullptr when it cannot be allocated.
    static WispView* create(lv_obj_t* parent, int size_px);

    void set_mood(WispMood mood);
    void set_level(uint8_t level);
    void set_busy(bool fast);
    // Paused: no frames and no timer (screen off).
    void set_paused(bool paused);
    // Rebuilds the model at the new size, keeping the mood, level, busy and
    // paused state and the object's centre.
    void set_size(int size_px);

    lv_obj_t* obj() const { return obj_; }
    // Frames built since create(), the first one included.
    uint32_t frames() const { return frames_; }
    const WispModel& model() const;

    ~WispView();
    WispView(const WispView&) = delete;
    WispView& operator=(const WispView&) = delete;

private:
    struct Impl;
    explicit WispView(lv_obj_t* obj);

    static void on_draw(lv_event_t* e);
    static void on_delete(lv_event_t* e);
    static void on_timer(lv_timer_t* t);
    bool build(int size_px);
    void frame();
    void still_frame();
    void schedule();
    void draw(lv_layer_t* layer);

    lv_obj_t* obj_ = nullptr;
    Impl* impl_ = nullptr;
    lv_timer_t* timer_ = nullptr;
    uint32_t period_ = 0;
    uint64_t last_ms_ = 0;
    uint32_t frames_ = 0;
    uint32_t draw_us_ = 0;  // the last draw event's time, added to the next frame's
    uint8_t level_ = 0;
    bool busy_ = false;
    bool paused_ = false;
    bool stale_ = false;  // e-paper: the mood changed while paused
};

}  // namespace wisp::ui
