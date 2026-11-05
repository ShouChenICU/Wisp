#pragma once

// The self-check screen's interactive tests card: the touch test area,
// the last button event and the "Mic test" (record and play back) button
// with its progress. Each test shows "Not tested" until it passes, then
// "✓ Passed"; the pass state itself lives with the owner
// (InteractiveTests).
//
// UI thread only.

#include <functional>
#include <string>

#include "loopback.hpp"
#include "lvgl.h"
#include "self_check_layout.hpp"
#include "self_check_model.hpp"

namespace wisp::app {

class TestSection {
public:
    struct Callbacks {
        // Any press inside the touch test area.
        std::function<void()> on_touch;
        // The "Mic test" button was clicked.
        std::function<void()> on_loopback;
    };

    // Appends the card to `col` (which owns the widgets). `text` must
    // outlive the widgets.
    void build(lv_obj_t* col, PageText& text, Callbacks callbacks,
               const LoopbackStatus& loop_status);

    // Shows each test's pass state.
    void show(const InteractiveTests& tests);
    // "Button: <topic>" for the last button event.
    void set_button_event(const std::string& topic);
    void set_loopback(const LoopbackStatus& status);
    // Re-renders the button line in the page's current language (the
    // owner re-shows the statuses).
    void retext();

private:
    static void touch_cb(lv_event_t* e);
    static void loopback_cb(lv_event_t* e);

    std::string button_text() const;

    const PageText* text_ = nullptr;
    Callbacks callbacks_;
    // The last button event's topic.
    std::string button_topic_ = "--";
    lv_obj_t* touch_label_ = nullptr;
    lv_obj_t* button_label_ = nullptr;
    lv_obj_t* loop_label_ = nullptr;
    // Indexed by InteractiveTest.
    lv_obj_t* status_[kInteractiveTestCount] = {};
};

}  // namespace wisp::app
