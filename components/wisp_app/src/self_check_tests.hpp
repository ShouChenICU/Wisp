#pragma once

// The self-check screen's interactive tests card: the touch test area,
// the last button event and the "录音回放" (record and play back) button
// with its progress. Each test shows "未测" until it passes, then "✓
// 已通过"; the pass state itself lives with the owner (InteractiveTests),
// so it survives a rebuild.
//
// UI thread only.

#include <functional>
#include <string>

#include "loopback.hpp"
#include "lvgl.h"
#include "self_check_model.hpp"

namespace wisp::app {

class TestSection {
public:
    struct Callbacks {
        // Any press inside the touch test area.
        std::function<void()> on_touch;
        // The "录音回放" button was clicked.
        std::function<void()> on_loopback;
    };

    // Appends the card to `col` (which owns the widgets); call again after
    // the column was rebuilt.
    void build(lv_obj_t* col, Callbacks callbacks, const LoopbackStatus& loop_status);

    // Shows each test's pass state.
    void show(const InteractiveTests& tests);
    // "按键: <topic>" for the last button event.
    void set_button_event(const std::string& topic);
    void set_loopback(const LoopbackStatus& status);

private:
    static void touch_cb(lv_event_t* e);
    static void loopback_cb(lv_event_t* e);

    Callbacks callbacks_;
    lv_obj_t* touch_label_ = nullptr;
    lv_obj_t* button_label_ = nullptr;
    lv_obj_t* loop_label_ = nullptr;
    // Indexed by InteractiveTest.
    lv_obj_t* status_[kInteractiveTestCount] = {};
};

}  // namespace wisp::app
