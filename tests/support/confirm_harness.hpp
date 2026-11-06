#pragma once

// Test-only: an agent::ConfirmService over a real UiRuntime (fake 480 x
// 480 display of a chosen shape) and SystemLayer, with helpers to find
// and answer its dialog from the test thread.

#include <atomic>
#include <functional>
#include <memory>
#include <string>

#include "lvgl.h"
#include "support/fake_devices.hpp"
#include "support/lvgl_probe.hpp"
#include "support/wait.hpp"
#include "wisp/agent/confirm.hpp"
#include "wisp/hal/display_geometry.hpp"
#include "wisp/ui/strings.hpp"
#include "wisp/ui/system_layer.hpp"
#include "wisp/ui/ui_runtime.hpp"

namespace wisp::test {

struct ConfirmHarness {
    // `shape` is a Display caps shape ("rect", "rounded_rect", "circle").
    explicit ConfirmHarness(const std::string& shape = "rect", int corner_radius = 0,
                            uint32_t timeout_ms = agent::kConfirmMs) {
        display = std::make_shared<FakeDisplay>("display.main", 480, 480, 1, shape);
        display->set_cap("corner_radius", corner_radius);
        ui = std::make_unique<ui::UiRuntime>(display, nullptr);
        ok = ui->start().is_ok();
        if (!ok) return;
        auto parsed = hal::screen_shape_from_caps(display->info().caps);
        if (parsed) screen = parsed.value();
        on_ui([&] { layer = std::make_unique<ui::SystemLayer>(screen); });
        confirm = std::make_unique<agent::ConfirmService>(agent::ConfirmDeps{
            .ui = *ui,
            .layer = *layer,
            .wake =
                [this] {
                    // On the UI thread, before the dialog opens.
                    dialog_at_wake = layer->confirm_open();
                    ++wakes;
                    if (on_wake) on_wake();
                },
            .timeout_ms = timeout_ms,
        });
    }

    ~ConfirmHarness() {
        confirm.reset();
        if (ok) {
            on_ui([&] { layer.reset(); });
            ui->stop();
        }
    }

    ConfirmHarness(const ConfirmHarness&) = delete;
    ConfirmHarness& operator=(const ConfirmHarness&) = delete;

    void on_ui(const std::function<void()>& fn) { run_on_ui_blocking(ui.get(), fn); }

    bool dialog_open() {
        bool open = false;
        on_ui([&] { open = layer->confirm_open(); });
        return open;
    }

    // Whether a label on the top layer contains `text`.
    bool showing(const std::string& text) {
        bool found = false;
        on_ui([&] { found = find_label_containing(lv_layer_top(), text.c_str()) != nullptr; });
        return found;
    }

    bool wait_showing(const std::string& text, uint32_t ms = 3000) {
        return wait_until([&] { return showing(text); }, ms);
    }

    // Clicks the dialog's Allow (true) or Deny button; false if there is
    // none.
    bool press(bool allow) {
        bool pressed = false;
        on_ui([&] {
            lv_obj_t* btn = find_button_with_label(
                lv_layer_top(), ui::tr(allow ? ui::StrId::AgentAllow : ui::StrId::AgentDeny));
            if (btn == nullptr) return;
            lv_obj_send_event(btn, LV_EVENT_CLICKED, nullptr);
            pressed = true;
        });
        return pressed;
    }

    std::shared_ptr<FakeDisplay> display;
    std::unique_ptr<ui::UiRuntime> ui;
    std::unique_ptr<ui::SystemLayer> layer;
    std::unique_ptr<agent::ConfirmService> confirm;
    hal::ScreenShape screen;
    bool ok = false;
    std::atomic<int> wakes{0};
    std::atomic<bool> dialog_at_wake{false};
    std::function<void()> on_wake;  // set before the first ask()
};

}  // namespace wisp::test
