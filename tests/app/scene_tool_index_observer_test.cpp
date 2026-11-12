// SceneToolIndexObserver (agent_wiring.hpp): the ToolIndex rebuilt from the
// installed scenes at boot, on a delete, and on a scene.state change that
// follows an install (agent-tools spec 5.3).

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <future>
#include <memory>
#include <string>

#include "agent_wiring.hpp"
#include "support/scene_runtime_fixture.hpp"
#include "support/wait.hpp"
#include "wisp/agent/tool_index.hpp"
#include "wisp/base/json.hpp"
#include "wisp/core/scene_install.hpp"
#include "wisp/core/scene_manifest.hpp"
#include "wisp/core/zip.hpp"

namespace {

using wisp::Value;
using wisp::test::wait_until;

std::span<const uint8_t> bytes(std::string_view text) {
    return {reinterpret_cast<const uint8_t*>(text.data()), text.size()};
}

std::string manifest(std::string_view id, std::string_view name, std::string_view tools) {
    return "{\"id\":\"" + std::string(id) + "\",\"name\":\"" + std::string(name) +
           "\",\"version\":\"1.0.0\",\"schema_version\":1,\"tools\":" + std::string(tools) + "}";
}

class SceneToolIndexObserverTest : public wisp::test::SceneRuntimeFixture {
protected:
    void SetUp() override {
        SceneRuntimeFixture::SetUp();
        index = std::make_unique<wisp::agent::ToolIndex>();
        observer = std::make_unique<wisp::app::SceneToolIndexObserver>(bus, worker, *repo, *index);
    }

    void TearDown() override {
        observer.reset();
        index.reset();
        SceneRuntimeFixture::TearDown();
    }

    // Writes a scene package into the code root, then loads the index the
    // way AgentWiring::start() does at boot.
    void boot_with(std::string_view id, std::string_view name, std::string_view tools) {
        install(std::string(id), "return ui.screen {}\n",
                {{"scene.json", manifest(id, name, tools)}});
        ASSERT_TRUE(observer->load_installed());
    }

    std::unique_ptr<wisp::agent::ToolIndex> index;
    std::unique_ptr<wisp::app::SceneToolIndexObserver> observer;
};

}  // namespace

TEST_F(SceneToolIndexObserverTest, RebuiltOnInstallAndDelete) {
    const std::string tools =
        R"([{"name":"lookup","description":"Local temperature for a saved city"}])";
    boot_with("weather", "Weather", tools);
    ASSERT_TRUE(index->find("weather.lookup"));

    // A delete reaches the bus as scene.state "deleted" and removes the
    // scene's tools from the index without a flash read.
    wisp::core::Event deleted;
    deleted.topic = "scene.state";
    deleted.source = "scene";
    deleted.payload = {{"id", Value("weather")}, {"state", Value("deleted")}};
    bus.publish(std::move(deleted));
    ASSERT_TRUE(wait_until([&] { return !index->find("weather.lookup").has_value(); }, 2000));
    EXPECT_EQ(index->size(), 0u);

    // A reinstall reaches the bus as "stopped"; the observer reads the new
    // manifest back on the worker and repopulates the index.
    const std::string newer =
        manifest("weather", "Weather", R"([{"name":"forecast","description":"Tomorrow's rain"}])");
    std::filesystem::path scene_json =
        std::filesystem::path(in) / "scenes" / "weather" / "scene.json";
    std::ofstream f(scene_json, std::ios::binary | std::ios::trunc);
    f << newer;
    f.close();

    wisp::core::Event stopped;
    stopped.topic = "scene.state";
    stopped.source = "scene";
    stopped.payload = {{"id", Value("weather")}, {"state", Value("stopped")}};
    bus.publish(std::move(stopped));
    ASSERT_TRUE(wait_until([&] { return index->find("weather.forecast").has_value(); }, 2000));
    EXPECT_FALSE(index->find("weather.lookup").has_value());
}

TEST_F(SceneToolIndexObserverTest, SuccessfulInstallWithReloadFailureRefreshesToolIndex) {
    start();
    boot_with("weather", "Weather", R"([{"name":"lookup","description":"Old forecast"}])");
    ASSERT_TRUE(rt->open("weather"));
    ASSERT_TRUE(wait_until(
        [&] { return rt->status("weather").state == wisp::core::SceneState::Running; }, 2000));

    const std::string replacement =
        manifest("weather", "Weather", R"([{"name":"forecast","description":"New forecast"}])");
    wisp::core::ZipWriter zip;
    zip.add("scene.json", bytes(replacement));
    zip.add("main.lua", bytes("error('reload failed')\n"));
    wisp::os::LargeBuffer archive = zip.finish();
    ASSERT_TRUE(archive.ok());

    const auto installed = wisp::core::install_scene(*repo, *rt, archive.span());
    ASSERT_TRUE(installed) << installed.error().message;
    ASSERT_TRUE(wait_until(
        [&] { return rt->status("weather").state == wisp::core::SceneState::Error; }, 2000));
    ASSERT_TRUE(wait_until([&] { return index->find("weather.forecast").has_value(); }, 2000));
    EXPECT_FALSE(index->find("weather.lookup").has_value());
}

TEST_F(SceneToolIndexObserverTest, DestructionDropsQueuedRefreshSafely) {
    boot_with("weather", "Weather", R"([{"name":"lookup","description":"Old forecast"}])");
    const std::string replacement =
        manifest("weather", "Weather", R"([{"name":"forecast","description":"New forecast"}])");
    const std::filesystem::path scene_json =
        std::filesystem::path(in) / "scenes" / "weather" / "scene.json";
    {
        std::ofstream f(scene_json, std::ios::binary | std::ios::trunc);
        f << replacement;
    }

    std::promise<void> blocker_started;
    std::future<void> blocker_started_future = blocker_started.get_future();
    std::promise<void> unblock_worker;
    std::shared_future<void> unblock_worker_future = unblock_worker.get_future().share();
    ASSERT_TRUE(worker.submit([&] {
        blocker_started.set_value();
        unblock_worker_future.wait();
    }));
    blocker_started_future.wait();

    wisp::core::Event stopped;
    stopped.topic = "scene.state";
    stopped.source = "scene";
    stopped.payload = {{"id", Value("weather")}, {"state", Value("stopped")}};
    bus.publish(std::move(stopped));
    const bool bus_idle = bus.wait_idle(1000);
    const bool refresh_queued = wait_until([&] { return worker.pending() >= 2; }, 2000);

    observer.reset();
    const bool index_unchanged_before_release = index->find("weather.lookup").has_value();
    unblock_worker.set_value();
    const bool drained = worker.drain(2000);

    EXPECT_TRUE(bus_idle);
    EXPECT_TRUE(refresh_queued);
    EXPECT_TRUE(drained);
    EXPECT_TRUE(index_unchanged_before_release);
    EXPECT_TRUE(index->find("weather.lookup").has_value());
    EXPECT_FALSE(index->find("weather.forecast").has_value());
}

TEST_F(SceneToolIndexObserverTest, LoadsEveryInstalledSceneAtBoot) {
    boot_with("weather", "Weather", R"([{"name":"lookup","description":"Temperature"}])");
    install("notes", "return ui.screen {}\n",
            {{"scene.json",
              manifest("notes", "Notes", R"([{"name":"add","description":"Add a note"}])")}});
    ASSERT_TRUE(observer->load_installed());
    ASSERT_TRUE(index->find("weather.lookup"));
    ASSERT_TRUE(index->find("notes.add"));
    EXPECT_EQ(index->size(), 2u);
}
