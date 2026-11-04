#pragma once

// Test-only: a SceneRuntime on a UiRuntime with a fake 480x480 display, a
// storage worker, an event bus and a SceneRepository in a temp dir, plus
// helpers to install packages and read what the scenes logged and showed.
// configure_deps() lets a test fill in the optional SceneRuntimeDeps
// fields before start() makes the runtime.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <system_error>
#include <utility>
#include <vector>

#include "lvgl.h"
#include "support/fake_devices.hpp"
#include "support/lvgl_probe.hpp"
#include "support/temp_dir.hpp"
#include "support/wait.hpp"
#include "wisp/core/device_registry.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/core/log.hpp"
#include "wisp/core/scene_repository.hpp"
#include "wisp/core/storage_manager.hpp"
#include "wisp/core/storage_worker.hpp"
#include "wisp/hal/display_geometry.hpp"
#include "wisp/scene/scene_runtime.hpp"
#include "wisp/ui/system_layer.hpp"
#include "wisp/ui/ui_runtime.hpp"

namespace wisp::test {

using Files = std::vector<std::pair<std::string, std::string>>;

inline std::string manifest(const std::string& id, const std::string& name = "Demo") {
    return R"({"id":")" + id + R"(","name":")" + name +
           R"(","version":"1.0.0","schema_version":1})";
}

struct StateEvent {
    std::string id;
    std::string state;
    bool operator==(const StateEvent&) const = default;
};

class SceneRuntimeFixture : public ::testing::Test {
protected:
    void SetUp() override {
        wisp::log::reset_for_test();
        wisp::log::init(256 * 1024);
        in = wisp::test::temp_dir().string();
        registry.add(std::make_shared<wisp::test::FakeStorage>("storage.int", in, false));
        storage = std::make_unique<wisp::core::StorageManager>(registry, bus);
        ASSERT_TRUE(storage->init());
        ASSERT_TRUE(worker.start());
        ASSERT_TRUE(bus.start());
        bus.subscribe("scene.state", [this](const wisp::core::Event& e) {
            std::lock_guard<std::mutex> lock(mutex);
            EXPECT_EQ(e.source, "scene");
            state_events.push_back(
                {e.payload.at("id").as_string(), e.payload.at("state").as_string()});
        });
        wisp::core::SceneRepoConfig rc;
        rc.code_root = in + "/scenes";
        rc.data_root = in + "/scenes";
        repo = std::make_unique<wisp::core::SceneRepository>(*storage, rc);
    }

    void TearDown() override {
        rt.reset();
        if (ui) {
            on_ui([&] { layer.reset(); });
            ui->stop();
        }
        worker.stop();
        bus.stop();
        wisp::log::reset_for_test();
    }

    void start(const std::string& display_shape = "circle") {
        display =
            std::make_shared<wisp::test::FakeDisplay>("display.main", 480, 480, 1, display_shape);
        touch = std::make_shared<wisp::test::FakeTouch>("touch.main");
        ui = std::make_unique<wisp::ui::UiRuntime>(display, touch);
        ASSERT_TRUE(ui->start());
        auto parsed = wisp::hal::screen_shape_from_caps(display->info().caps);
        ASSERT_TRUE(parsed);
        shape = parsed.value();
        on_ui([&] {
            layer = std::make_unique<wisp::ui::SystemLayer>(shape);
            home = lv_screen_active();
            indev = lv_indev_get_next(nullptr);
        });
        wisp::scene::SceneRuntimeDeps deps{bus, *storage, worker, *repo, *ui, *layer, shape};
        if (configure_deps) configure_deps(deps);
        rt = std::make_unique<wisp::scene::SceneRuntime>(std::move(deps));
        ASSERT_TRUE(rt->start());
    }

    void on_ui(const std::function<void()>& fn) { run_on_ui_blocking(ui.get(), fn); }

    // Writes a package into the code root, the way an install leaves it.
    // A scene.json in `extra` replaces the default manifest.
    void install(const std::string& id, const std::string& main_lua, Files extra = {}) {
        Files files = {{"main.lua", main_lua}};
        bool has_manifest = false;
        for (auto& f : extra) {
            has_manifest = has_manifest || f.first == "scene.json";
            files.push_back(std::move(f));
        }
        if (!has_manifest) files.insert(files.begin(), {"scene.json", manifest(id)});
        for (const auto& [path, body] : files) {
            std::filesystem::path p = std::filesystem::path(in) / "scenes" / id / path;
            std::error_code ec;
            std::filesystem::create_directories(p.parent_path(), ec);
            std::ofstream f(p, std::ios::binary | std::ios::trunc);
            f << body;
        }
    }

    // The text of a label on the active screen that contains `needle`.
    std::string label(const char* needle) {
        std::string text;
        on_ui([&] { text = wisp::test::find_label_text_containing(lv_screen_active(), needle); });
        return text;
    }

    bool wait_label(const char* needle, uint32_t ms = 1000) {
        return wait_until([&] { return !label(needle).empty(); }, ms);
    }

    // Every label text on the active screen.
    std::vector<std::string> labels() {
        std::vector<std::pair<std::string, lv_area_t>> found;
        on_ui([&] { wisp::test::collect_labels(lv_screen_active(), found); });
        std::vector<std::string> out;
        for (auto& [text, area] : found) out.push_back(text);
        return out;
    }

    bool error_panel_has(const char* needle) {
        bool found = false;
        on_ui([&] {
            found = layer->error_visible() &&
                    wisp::test::find_label_containing(lv_layer_top(), needle) != nullptr;
        });
        return found;
    }

    // What the scene logged (print), in order.
    std::vector<std::string> scene_log(const std::string& id) {
        std::vector<std::string> out;
        for (auto& e : wisp::log::ring_entries())
            if (e.tag == "scene." + id) out.push_back(e.msg);
        return out;
    }

    bool log_has(const std::string& tag, const std::string& needle) {
        for (auto& e : wisp::log::ring_entries())
            if (e.tag == tag && e.msg.find(needle) != std::string::npos) return true;
        return false;
    }

    std::vector<StateEvent> events() {
        bus.wait_idle(1000);
        std::lock_guard<std::mutex> lock(mutex);
        return state_events;
    }

    // One touch read: `points` empty is a release.
    void feed(std::vector<wisp::hal::TouchPoint> points) {
        touch->set_points(std::move(points));
        on_ui([&] { lv_indev_read(indev); });
    }

    lv_obj_t* active_screen() {
        lv_obj_t* s = nullptr;
        on_ui([&] { s = lv_screen_active(); });
        return s;
    }

    std::string in;
    wisp::core::DeviceRegistry registry;
    wisp::core::EventBus bus;
    std::unique_ptr<wisp::core::StorageManager> storage;
    wisp::core::StorageWorker worker;
    std::unique_ptr<wisp::core::SceneRepository> repo;
    std::shared_ptr<wisp::test::FakeDisplay> display;
    std::shared_ptr<wisp::test::FakeTouch> touch;
    std::unique_ptr<wisp::ui::UiRuntime> ui;
    std::unique_ptr<wisp::ui::SystemLayer> layer;
    wisp::hal::ScreenShape shape;
    lv_obj_t* home = nullptr;
    lv_indev_t* indev = nullptr;
    std::unique_ptr<wisp::scene::SceneRuntime> rt;
    // Called with the deps before start() makes the runtime.
    std::function<void(wisp::scene::SceneRuntimeDeps&)> configure_deps;
    std::mutex mutex;
    std::vector<StateEvent> state_events;
};

}  // namespace wisp::test
