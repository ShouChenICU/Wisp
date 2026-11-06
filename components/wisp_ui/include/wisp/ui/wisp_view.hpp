#pragma once

// WispView: the wisp (spec 6.1) as an LVGL object. It owns a WispModel
// (wisp/ui/wisp_model.hpp) plus its luminance map and RGB565 frame in
// Mem::Large, steps and renders it from an lv_timer whose period follows
// WispModel::frame_interval_ms(), and invalidates only the dirty box. The
// object draws the frame in LV_EVENT_DRAW_MAIN; the frame is opaque, black
// where the wisp is not, so it belongs on a black background.
//
// UI thread only, like every lv_* call. Deleting obj() frees the view.

#include <cstdint>

#include "lvgl.h"
#include "wisp/hal/display.hpp"
#include "wisp/ui/wisp_model.hpp"

namespace wisp::ui {

// The wisp's width for `percent` of the safe area's short side (spec 6.1).
int wisp_size_px(const hal::Area& safe, int percent);

class WispView {
public:
    // A view size_px wide (and size_px * 5 / 4 high), centred in `parent`.
    // nullptr when its buffers cannot be allocated.
    static WispView* create(lv_obj_t* parent, int size_px);

    void set_mood(WispMood mood);
    void set_level(uint8_t level);
    void set_busy(bool fast);
    // Paused: no frames and no timer (screen off).
    void set_paused(bool paused);
    // Rebuilds the model and buffers at the new size, keeping the mood,
    // level, busy and paused state and the object's centre. Keeps the old
    // size if the new buffers cannot be allocated.
    void set_size(int size_px);

    lv_obj_t* obj() const { return obj_; }
    // Frames drawn since create().
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
    void schedule();

    lv_obj_t* obj_ = nullptr;
    Impl* impl_ = nullptr;
    lv_timer_t* timer_ = nullptr;
    uint32_t period_ = 0;
    uint64_t last_ms_ = 0;
    uint32_t frames_ = 0;
    uint8_t level_ = 0;
    bool busy_ = false;
    bool paused_ = false;
};

}  // namespace wisp::ui
