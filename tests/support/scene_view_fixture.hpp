#pragma once

// Test-only: a UiRuntime over a fake display with a SystemLayer and a
// SceneView built on its UI thread, recording the events the view
// reports, plus helpers for building the UiOps the scene task would send.

#include <gtest/gtest.h>

#include <functional>
#include <initializer_list>
#include <memory>
#include <mutex>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "lvgl.h"
#include "support/fake_devices.hpp"
#include "support/lvgl_probe.hpp"
#include "wisp/hal/display_geometry.hpp"
#include "wisp/scene/scene_view.hpp"
#include "wisp/scene/schema.hpp"
#include "wisp/scene/ui_ops.hpp"
#include "wisp/ui/system_layer.hpp"
#include "wisp/ui/ui_runtime.hpp"

namespace wisp::test {

inline scene::PropValue str(const char* s) { return scene::PropValue{os::LargeString(s)}; }
inline scene::PropValue num(int64_t v) { return scene::PropValue{v}; }

inline scene::NodeDesc node(
    scene::NodeId id, scene::NodeId parent, scene::Widget w,
    std::initializer_list<std::pair<scene::Prop, scene::PropValue>> props = {},
    uint32_t events = 0) {
    scene::NodeDesc n;
    n.id = id;
    n.parent = parent;
    n.widget = static_cast<scene::WidgetId>(w);
    n.events = events;
    for (const auto& [p, v] : props) n.props.emplace_back(static_cast<scene::PropId>(p), v);
    return n;
}

inline scene::UiOp build(std::initializer_list<scene::NodeDesc> nodes) {
    scene::Build b;
    for (const auto& n : nodes) b.nodes.push_back(n);
    return b;
}

inline scene::UiOp set(scene::NodeId node, scene::Prop p, scene::PropValue v) {
    return scene::SetProp{node, static_cast<scene::PropId>(p), std::move(v)};
}

struct RecordedEvent {
    scene::NodeId node;
    scene::UiEvent ev;
    scene::PropValue value;
};

struct RecordedRange {
    scene::NodeId list;
    uint32_t first;
    uint32_t count;
    bool operator==(const RecordedRange&) const = default;
};

class SceneViewTest : public ::testing::Test {
protected:
    // `rgb565_be` gives the display the panel byte order (LVGL's
    // RGB565_SWAPPED); `ui_config` reaches the UiRuntime as it is.
    void start(const std::string& display_shape = "circle", bool rgb565_be = false,
               ui::UiConfig ui_config = {}) {
        display = std::make_shared<FakeDisplay>("display.main", 480, 480, 1, display_shape);
        if (rgb565_be) display->set_cap("rgb565_be", Value(true));
        ui = std::make_unique<ui::UiRuntime>(display, nullptr, std::move(ui_config));
        ASSERT_TRUE(ui->start());
        auto parsed = hal::screen_shape_from_caps(display->info().caps);
        ASSERT_TRUE(parsed);
        shape = parsed.value();
        scene::ViewCallbacks cb;
        cb.on_event = [this](scene::NodeId n, scene::UiEvent ev, scene::PropValue v) {
            std::lock_guard<std::mutex> lock(mutex);
            events.push_back({n, ev, std::move(v)});
        };
        cb.on_list_range = [this](scene::NodeId list, uint32_t first, uint32_t count) {
            std::lock_guard<std::mutex> lock(mutex);
            ranges.push_back({list, first, count});
        };
        cb.on_warning = [this](scene::NodeId, std::string_view message) {
            std::lock_guard<std::mutex> lock(mutex);
            warnings.emplace_back(message);
        };
        on_ui([&] {
            layer = std::make_unique<ui::SystemLayer>(shape);
            view = std::make_unique<scene::SceneView>(*layer, shape, cb);
        });
    }

    void TearDown() override {
        if (ui) {
            on_ui([&] {
                view.reset();
                layer.reset();
            });
            ui->stop();
        }
    }

    void on_ui(const std::function<void()>& fn) { run_on_ui_blocking(ui.get(), fn); }

    void apply(const scene::UiOp& op) {
        on_ui([&] { view->apply(op); });
    }

    std::vector<RecordedEvent> take_events() {
        std::lock_guard<std::mutex> lock(mutex);
        return std::exchange(events, {});
    }

    std::vector<RecordedRange> take_ranges() {
        std::lock_guard<std::mutex> lock(mutex);
        return std::exchange(ranges, {});
    }

    std::vector<std::string> take_warnings() {
        std::lock_guard<std::mutex> lock(mutex);
        return std::exchange(warnings, {});
    }

    std::shared_ptr<FakeDisplay> display;
    std::unique_ptr<ui::UiRuntime> ui;
    std::unique_ptr<ui::SystemLayer> layer;
    std::unique_ptr<scene::SceneView> view;
    hal::ScreenShape shape;
    std::mutex mutex;
    std::vector<RecordedEvent> events;
    std::vector<RecordedRange> ranges;
    std::vector<std::string> warnings;
};

}  // namespace wisp::test
