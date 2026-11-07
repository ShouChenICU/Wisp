#pragma once

// Shell (MVP spec 6.2, 6.3, 6.6): the device's own pages, native C++
// screens rather than scenes. The device boots into the home screen: the
// wisp centred in the safe area on the wisp's background (black; white on
// e-paper), kWispIdlePercent of the safe area's short side.
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
// Threading: every member is UI thread only, like every lv_* call. A back
// swipe's work is posted (it arrives from inside the pointer's read
// callback, where the screen under the finger must not be deleted); a
// post still queued after stop() does nothing.

#include <cstdint>
#include <functional>
#include <memory>

#include "lvgl.h"
#include "wisp/core/event_bus.hpp"
#include "wisp/hal/display_geometry.hpp"

namespace wisp::ui {
class SystemLayer;
class UiRuntime;
class WispView;
}  // namespace wisp::ui

namespace wisp::shell {

class ConversationController;

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

    // Logs "shell: page <name>". Home closes the diagnostics if they are
    // open and loads the home screen; the other pages have no screen of
    // their own yet and only change page().
    void show(Page p);
    Page page() const { return page_; }
    // nullptr before start() and after stop().
    lv_obj_t* home_screen() const { return home_; }
    // The wisp on the home screen; nullptr without one (no memory).
    ui::WispView* wisp() const { return wisp_; }

    // Opens the hardware diagnostics over the home screen; a no-op while
    // they are open. The back handler -> close_diagnostics(), show(Home).
    void open_diagnostics();
    bool diagnostics_open() const { return diag_ != nullptr; }

    // The conversation once the voice has started (it starts after the
    // home screen must be up); nullptr without voice.
    void set_conversation(ConversationController* conversation) {
        deps_.conversation = conversation;
    }

private:
    void close_diagnostics();
    void set_diag_back_handler();
    static void diag_loaded_cb(lv_event_t* e);

    ShellDeps deps_;
    Page page_ = Page::Home;
    // What was active before start(), loaded again by stop().
    lv_obj_t* prev_ = nullptr;
    lv_obj_t* home_ = nullptr;
    ui::WispView* wisp_ = nullptr;  // owned by its object, under home_
    // The diagnostics' screen while it is open (owned by the App's
    // self-check object).
    lv_obj_t* diag_ = nullptr;
    // Cleared by stop(): a back swipe's post queued before it does
    // nothing.
    std::shared_ptr<bool> alive_;
};

}  // namespace wisp::shell
