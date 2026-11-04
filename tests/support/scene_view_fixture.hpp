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
    Value value;
};

class SceneViewTest : public ::testing::Test {
protected:
    void start(const std::string& display_shape = "circle") {
        display = std::make_shared<FakeDisplay>("display.main", 480, 480, 1, display_shape);
        ui = std::make_unique<ui::UiRuntime>(display, nullptr);
        ASSERT_TRUE(ui->start());
        auto parsed = hal::screen_shape_from_caps(display->info().caps);
        ASSERT_TRUE(parsed);
        shape = parsed.value();
        scene::ViewCallbacks cb;
        cb.on_event = [this](scene::NodeId n, scene::UiEvent ev, Value v) {
            std::lock_guard<std::mutex> lock(mutex);
            events.push_back({n, ev, std::move(v)});
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

    std::shared_ptr<FakeDisplay> display;
    std::unique_ptr<ui::UiRuntime> ui;
    std::unique_ptr<ui::SystemLayer> layer;
    std::unique_ptr<scene::SceneView> view;
    hal::ScreenShape shape;
    std::mutex mutex;
    std::vector<RecordedEvent> events;
};

}  // namespace wisp::test
