// SceneRuntime::check_build(): a package's entry file is run and its tree
// built in a VM of its own, outside the scenes' shared budget and with a
// store that never reaches flash (agent-core spec 4.1).

#include <gtest/gtest.h>

#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <utility>

#include "support/scene_runtime_fixture.hpp"
#include "support/wait.hpp"
#include "wisp/core/scene_manifest.hpp"
#include "wisp/core/scene_repository.hpp"
#include "wisp/os/os.hpp"
#include "wisp/scene/scene_runtime.hpp"

namespace {

using wisp::ErrorCode;
using wisp::test::Files;
using wisp::test::manifest;
using wisp::test::wait_until;

std::shared_ptr<const wisp::core::ScenePackage> package_of(const std::string& id,
                                                           const std::string& main_lua,
                                                           const std::string& json = "") {
    auto pkg = std::make_shared<wisp::core::ScenePackage>();
    const std::string m = json.empty() ? manifest(id) : json;
    auto parsed = wisp::core::parse_scene_manifest(m);
    EXPECT_TRUE(parsed) << parsed.error().message;
    pkg->manifest = parsed.value();
    auto add = [&](const std::string& path, const std::string& body) {
        auto buf = wisp::os::LargeBuffer::allocate(body.size());
        if (!body.empty()) std::memcpy(buf.data(), body.data(), body.size());
        pkg->files.emplace_back(path, std::move(buf));
    };
    add("scene.json", m);
    add("main.lua", main_lua);
    return pkg;
}

std::string read_file(const std::filesystem::path& p) {
    std::ifstream f(p, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

class CheckBuild : public wisp::test::SceneRuntimeFixture {};

}  // namespace

TEST_F(CheckBuild, AcceptsAValidScene) {
    start();
    auto r = rt->check_build(package_of("demo", R"(
local n = state(0)
return ui.screen { ui.column { ui.label { text = bind(n, tostring) }, ui.button { text = "+1" } } }
)"));
    EXPECT_TRUE(r) << r.error().message;
    // Nothing came up for it.
    EXPECT_FALSE(rt->foreground().has_value());
}

TEST_F(CheckBuild, UnknownWidgetHasFileAndLine) {
    start();
    auto r = rt->check_build(package_of(
        "demo", "return ui.screen {\n  ui.column {\n    ui.labl { text = 'x' },\n  },\n}\n"));
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(r.error().message.rfind("main.lua:3:", 0), 0u) << r.error().message;
    EXPECT_NE(r.error().message.find("ui.label"), std::string::npos) << r.error().message;
}

TEST_F(CheckBuild, UnknownPropertyHasFileAndLine) {
    start();
    auto r =
        rt->check_build(package_of("demo", "return ui.screen {\n  ui.label { txet = 'x' },\n}\n"));
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().message.rfind("main.lua:2:", 0), 0u) << r.error().message;
    EXPECT_NE(r.error().message.find("text"), std::string::npos) << r.error().message;
}

TEST_F(CheckBuild, RuntimeErrorInEntryFails) {
    start();
    auto r = rt->check_build(package_of("demo", "local x = nil\nreturn x.y\n"));
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().message.rfind("main.lua:2:", 0), 0u) << r.error().message;
    // The entry must return a screen.
    auto bad = rt->check_build(package_of("demo", "return ui.column {}\n"));
    ASSERT_FALSE(bad);
    EXPECT_NE(bad.error().message.find("ui.screen"), std::string::npos) << bad.error().message;
}

TEST_F(CheckBuild, LeavesTheRealStoreAlone) {
    install("demo", "wisp.store.set('k', 1)\nreturn ui.screen {}\n");
    start();
    ASSERT_TRUE(rt->open("demo"));
    ASSERT_TRUE(rt->exit("demo"));
    const auto log = std::filesystem::path(in) / "scenes" / "demo" / "data" / "kv.log";
    const std::string before = read_file(log);
    ASSERT_FALSE(before.empty());

    auto r =
        rt->check_build(package_of("demo",
                                   "assert(wisp.store.get('k') == nil)\nwisp.store.set('k', 2)\n"
                                   "wisp.store.set('other', 'x')\nreturn ui.screen {}\n"));
    EXPECT_TRUE(r) << r.error().message;
    ASSERT_TRUE(worker.drain(2000));
    // Past the store's coalescing delay: any write it had queued would be on disk.
    wisp::os::sleep_ms(400);
    ASSERT_TRUE(worker.drain(2000));
    EXPECT_EQ(read_file(log), before);
    EXPECT_FALSE(
        std::filesystem::exists(std::filesystem::path(in) / "scenes" / "demo" / "data" / ".check"));
}

TEST_F(CheckBuild, OutsideTheSharedVmBudget) {
    configure_config = [](wisp::scene::SceneRuntimeConfig& c) {
        c.vm.memory_bytes = 1024 * 1024;
        c.vm_budget = 1600 * 1024;
    };
    const std::string bg =
        R"({"id":"ID","name":"B","version":"1.0.0","schema_version":1,"background":true})";
    for (const char* id : {"b1", "b2", "b3"}) {
        std::string m = bg;
        m.replace(m.find("ID"), 2, id);
        install(id, "return ui.screen {}\n", {{"scene.json", m}});
    }
    // The handler keeps the string alive as long as the scene runs.
    install("big",
            "local big = string.rep('x', 700 * 1024)\n"
            "return ui.screen { ui.button { on_click = function() print(#big) end } }\n");
    start();
    for (const char* id : {"b1", "b2", "b3"})
        ASSERT_TRUE(rt->launch(id, wisp::scene::LaunchReason::Boot));
    ASSERT_TRUE(rt->open("big")) << rt->status("big").error;
    ASSERT_EQ(rt->background_scenes().size(), 3u);

    auto r = rt->check_build(
        package_of("demo", "local s = string.rep('y', 700 * 1024)\nreturn ui.screen {}\n"));
    EXPECT_TRUE(r) << r.error().message;
    EXPECT_EQ(rt->foreground(), "big");
    EXPECT_EQ(rt->background_scenes().size(), 3u);
}

TEST_F(CheckBuild, EndlessLoopTimesOut) {
    start();
    const auto t0 = std::chrono::steady_clock::now();
    auto r = rt->check_build(package_of("demo", "while true do end\n"));
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
            .count();
    ASSERT_FALSE(r);
    EXPECT_NE(r.error().message.find("exceeded 200 ms"), std::string::npos) << r.error().message;
    EXPECT_LT(ms, 1000);
}

TEST_F(CheckBuild, BusyWhenNotRunning) {
    start();
    rt->stop();
    auto r = rt->check_build(package_of("demo", "return ui.screen {}\n"));
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, ErrorCode::Busy);
}
