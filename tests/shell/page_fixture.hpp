#pragma once

// PageHarness: a Shell on a live UiRuntime (a FakeDisplay of any shape and
// a FakeTouch the tests drive one read at a time) with a system layer, for
// the sub-page tests. The test fills ShellDeps' page services before
// start(); boot() then shows the home screen.

#include <gtest/gtest.h>

#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "support/fake_devices.hpp"
#include "support/lvgl_probe.hpp"
#include "support/visible.hpp"
#include "support/wait.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/hal/display_geometry.hpp"
#include "wisp/shell/shell.hpp"
#include "wisp/ui/strings.hpp"
#include "wisp/ui/system_layer.hpp"
#include "wisp/ui/ui_runtime.hpp"

namespace wisp::test {

inline lv_area_t coords_of(lv_obj_t* o) {
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    return a;
}

inline std::string text_of(lv_obj_t* label) {
    const char* t = label != nullptr ? lv_label_get_text(label) : nullptr;
    return t != nullptr ? t : "";
}

inline bool is_hidden(lv_obj_t* o) {
    for (; o != nullptr; o = lv_obj_get_parent(o))
        if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return true;
    return false;
}

class PageHarness {
public:
    explicit PageHarness(const std::string& shape = "rect", int32_t width = 480,
                         int32_t height = 480) {
        display_ = std::make_shared<FakeDisplay>("display.main", width, height, 1, shape);
        shape_ = hal::screen_shape_from_caps(display_->info().caps).value();
        touch_ = std::make_shared<FakeTouch>("touch.main");
        ui_ = std::make_unique<ui::UiRuntime>(display_, touch_);
        started_ = static_cast<bool>(ui_->start());
        if (!started_) return;
        (void)bus_.start();
        on_ui([&] {
            ui::set_lang(ui::Lang::Zh);
            layer_ = std::make_unique<ui::SystemLayer>(shape_);
            indev_ = lv_indev_get_next(nullptr);
        });
    }

    ~PageHarness() {
        if (!started_) return;
        on_ui([&] {
            if (shell_) shell_->stop();
            layer_.reset();
        });
        shell_.reset();
        bus_.stop();
        ui_->stop();
    }

    PageHarness(const PageHarness&) = delete;
    PageHarness& operator=(const PageHarness&) = delete;

    // The shell's deps with the harness's runtime; the page services are
    // the test's to fill in before boot().
    shell::ShellDeps& deps() {
        if (!deps_) {
            deps_ = std::make_unique<shell::ShellDeps>(shell::ShellDeps{
                .ui = *ui_,
                .layer = *layer_,
                .shape = shape_,
                .bus = bus_,
            });
        }
        return *deps_;
    }

    void boot() {
        shell_ = std::make_unique<shell::Shell>(deps());
        on_ui([&] { shell_->start(); });
    }

    void show(shell::Page p) {
        on_ui([&] {
            shell_->show(p);
            if (lv_obj_t* scr = shell_->page_screen()) lv_obj_update_layout(scr);
        });
    }

    bool started() const { return started_; }
    void on_ui(const std::function<void()>& fn) { run_on_ui_blocking(ui_.get(), fn); }
    bool ui_until(const std::function<bool()>& pred, uint32_t ms = 5000) {
        return wait_until(
            [&] {
                bool ok = false;
                on_ui([&] { ok = pred(); });
                return ok;
            },
            ms);
    }
    shell::Shell& shell() { return *shell_; }
    ui::SystemLayer& layer() { return *layer_; }
    ui::UiRuntime& ui() { return *ui_; }
    core::EventBus& bus() { return bus_; }
    const hal::ScreenShape& shape() const { return shape_; }

    lv_obj_t* active() {
        lv_obj_t* s = nullptr;
        on_ui([&] { s = lv_screen_active(); });
        return s;
    }

    // One touch read: `points` empty is a release.
    void feed(std::vector<hal::TouchPoint> points) {
        touch_->set_points(std::move(points));
        on_ui([&] { lv_indev_read(indev_); });
    }
    void drag(lv_point_t from, lv_point_t to) {
        constexpr int kSteps = 6;
        for (int i = 0; i <= kSteps; ++i) {
            feed({{static_cast<int16_t>(from.x + (to.x - from.x) * i / kSteps),
                   static_cast<int16_t>(from.y + (to.y - from.y) * i / kSteps), 0}});
        }
        feed({});
    }
    // A press at `p` held for `ms` before the release.
    void hold(lv_point_t p, uint32_t ms) {
        feed({{static_cast<int16_t>(p.x), static_cast<int16_t>(p.y), 0}});
        const uint64_t until = os::now_ms() + ms;
        while (os::now_ms() < until) {
            os::sleep_ms(20);
            feed({{static_cast<int16_t>(p.x), static_cast<int16_t>(p.y), 0}});
        }
        feed({});
    }
    void tap(lv_point_t p) {
        feed({{static_cast<int16_t>(p.x), static_cast<int16_t>(p.y), 0}});
        feed({});
    }
    lv_point_t centre_of(lv_obj_t* o) {
        lv_point_t p{};
        on_ui([&] {
            lv_obj_update_layout(lv_obj_get_screen(o));
            const lv_area_t a = coords_of(o);
            p = {(a.x1 + a.x2) / 2, (a.y1 + a.y2) / 2};
        });
        return p;
    }
    // Clicks the system dialog's button labelled `text`.
    bool click_dialog(const char* text) {
        bool ok = false;
        on_ui([&] {
            lv_obj_t* btn = find_button_with_label(lv_layer_top(), text);
            if (btn == nullptr) return;
            lv_obj_send_event(btn, LV_EVENT_CLICKED, nullptr);
            ok = true;
        });
        return ok;
    }

private:
    std::shared_ptr<FakeDisplay> display_;
    std::shared_ptr<FakeTouch> touch_;
    hal::ScreenShape shape_;
    std::unique_ptr<ui::UiRuntime> ui_;
    core::EventBus bus_;
    std::unique_ptr<ui::SystemLayer> layer_;
    std::unique_ptr<shell::ShellDeps> deps_;
    std::unique_ptr<shell::Shell> shell_;
    lv_indev_t* indev_ = nullptr;
    bool started_ = false;
};

}  // namespace wisp::test
