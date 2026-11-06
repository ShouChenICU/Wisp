// Scene data migration on a running SceneRuntime (scene-device spec 7.3):
// scene.migrate(old) runs between main.lua and the tree when the data
// version is below the manifest's, and its writes land together with the
// new version or not at all.

#include <gtest/gtest.h>

#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <memory>
#include <mutex>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "support/scene_runtime_fixture.hpp"
#include "support/wait.hpp"
#include "wisp/base/json.hpp"
#include "wisp/base/value.hpp"
#include "wisp/core/scene_store.hpp"
#include "wisp/scene/scene_runtime.hpp"

namespace {

using wisp::Map;
using wisp::Value;
using wisp::core::SceneState;
using wisp::scene::SceneNotice;
using wisp::test::wait_until;

namespace fs = std::filesystem;

constexpr const char* kId = "migrate";

std::string read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    std::stringstream ss;
    ss << f.rdbuf();
    return ss.str();
}

Value mode(const char* m) {
    Map a;
    a["mode"] = Value(m);
    return Value(std::move(a));
}

class Migrate : public wisp::test::SceneRuntimeFixture {
protected:
    void SetUp() override {
        SceneRuntimeFixture::SetUp();
        list_path = in + "/system/scenes/background.json";
        configure_deps = [this](wisp::scene::SceneRuntimeDeps& d) {
            d.background_list_path = list_path;
            d.on_notice = [this](const SceneNotice& n) {
                std::lock_guard<std::mutex> lock(notice_mu);
                notices.push_back(n);
            };
        };
    }

    // Copies tests/scene/fixtures/<name> over the scene's package.
    void install_fixture(const char* name) {
        const fs::path from = fs::path(WISP_TEST_DIR) / "scene" / "fixtures" / name;
        install(kId, read_file(from / "main.lua"),
                {{"scene.json", read_file(from / "scene.json")}});
    }

    // Version 1 data on disk: the v1 scene opened once and exited.
    void make_v1_data() {
        install_fixture("migrate_v1");
        ASSERT_TRUE(rt->open(kId));
        ASSERT_TRUE(rt->exit(kId));
        ASSERT_TRUE(worker.drain(2000));
        ASSERT_EQ(stored().version, 1);
        ASSERT_EQ(stored().items.size(), 2u);
    }

    struct Stored {
        int version = -1;
        std::vector<std::string> items;
    };

    // What the scene's data directory holds, read by a store of its own.
    // Call it once the scene has exited and the worker drained.
    Stored stored() {
        wisp::core::SceneStoreDeps deps{*storage,
                                        [](std::function<void()> job) {
                                            job();
                                            return true;
                                        },
                                        [](uint32_t, std::function<void()>) {}};
        auto s = wisp::core::SceneStore::create(deps, repo->data_dir(kId));
        Stored out;
        if (!s->load()) return out;
        out.version = s->data_version();
        auto found = s->find("items", {});
        // Re-encoded with sorted keys: a Lua table's field order is not
        // fixed.
        if (found) {
            for (const auto& r : found.value()) {
                auto v = wisp::from_json(std::string_view(r.json.data(), r.json.size()));
                out.items.push_back(v ? wisp::to_json(v.value()) : std::string("<bad>"));
            }
        }
        s->close();
        return out;
    }

    void write_meta(int version) {
        const fs::path p = fs::path(repo->data_dir(kId)) / "meta.json";
        std::ofstream(p, std::ios::trunc) << R"({"schema_version":)" << version << "}";
    }

    std::vector<std::string> v1_items() const { return {R"({"name":"a"})", R"({"name":"b"})"}; }

    bool printed(const std::string& line) {
        for (const auto& l : scene_log(kId))
            if (l == line) return true;
        return false;
    }

    std::string list_path;
    std::mutex notice_mu;
    std::vector<SceneNotice> notices;
};

TEST_F(Migrate, SuccessCommitsAndBumps) {
    start();
    make_v1_data();
    install_fixture("migrate_v2");
    ASSERT_TRUE(rt->open(kId));
    EXPECT_EQ(rt->status(kId).state, SceneState::Running);
    EXPECT_TRUE(printed("migrate from 1"));
    EXPECT_TRUE(wait_label("v2"));
    ASSERT_TRUE(rt->exit(kId));
    ASSERT_TRUE(worker.drain(2000));
    Stored s = stored();
    EXPECT_EQ(s.version, 2);
    EXPECT_EQ(s.items, (std::vector<std::string>{R"({"done":false,"name":"a"})",
                                                 R"({"done":false,"name":"b"})"}));
    EXPECT_FALSE(fs::exists(fs::path(repo->data_dir(kId)) / "journal.json"));
}

TEST_F(Migrate, ErrorRollsBack) {
    start();
    make_v1_data();
    install_fixture("migrate_v2");
    auto r = rt->open(kId, mode("boom"));
    ASSERT_FALSE(r);
    EXPECT_NE(r.error().message.find("migrate failed: "), std::string::npos) << r.error().message;
    EXPECT_EQ(rt->status(kId).state, SceneState::Error);
    EXPECT_TRUE(wait_until([&] { return error_panel_has("migrate failed: "); }, 2000));
    EXPECT_TRUE(error_panel_has("boom"));
    EXPECT_TRUE(label("v2").empty());
    ASSERT_TRUE(worker.drain(2000));
    Stored s = stored();
    EXPECT_EQ(s.version, 1);
    EXPECT_EQ(s.items, v1_items());
}

TEST_F(Migrate, TimeoutAfterTwoSeconds) {
    start();
    make_v1_data();
    install_fixture("migrate_v2");
    const auto t0 = std::chrono::steady_clock::now();
    auto r = rt->open(kId, mode("loop"));
    const auto ms =
        std::chrono::duration_cast<std::chrono::milliseconds>(std::chrono::steady_clock::now() - t0)
            .count();
    ASSERT_FALSE(r);
    EXPECT_NE(r.error().message.find("migrate failed: "), std::string::npos) << r.error().message;
    EXPECT_NE(r.error().message.find("2000 ms"), std::string::npos) << r.error().message;
    EXPECT_EQ(rt->status(kId).state, SceneState::Error);
    // The 2 s are measured on the scene task; a loaded machine only adds.
    EXPECT_GE(ms, static_cast<long long>(wisp::scene::kMigrateMs) - 50);
    EXPECT_LT(ms, 9000);
    ASSERT_TRUE(worker.drain(2000));
    Stored s = stored();
    EXPECT_EQ(s.version, 1);
    EXPECT_EQ(s.items, v1_items());
}

TEST_F(Migrate, NoHookJustBumps) {
    start();
    make_v1_data();
    install_fixture("migrate_v2");
    ASSERT_TRUE(rt->open(kId, mode("none")));
    EXPECT_EQ(rt->status(kId).state, SceneState::Running);
    EXPECT_FALSE(printed("migrate from 1"));
    ASSERT_TRUE(rt->exit(kId));
    ASSERT_TRUE(worker.drain(2000));
    Stored s = stored();
    EXPECT_EQ(s.version, 2);
    EXPECT_EQ(s.items, v1_items());
}

TEST_F(Migrate, DowngradeRefused) {
    start();
    make_v1_data();
    write_meta(3);
    install_fixture("migrate_v2");
    auto r = rt->open(kId);
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().message, "data version 3 newer than scene 2");
    EXPECT_EQ(rt->status(kId).state, SceneState::Error);
    EXPECT_TRUE(
        wait_until([&] { return error_panel_has("data version 3 newer than scene 2"); }, 2000));
    // The scene never ran: main.lua did not even print.
    EXPECT_FALSE(printed("v2 main"));
    ASSERT_TRUE(worker.drain(2000));
    Stored s = stored();
    EXPECT_EQ(s.version, 3);
    EXPECT_EQ(s.items, v1_items());
}

TEST_F(Migrate, NewSceneStartsAtManifestVersion) {
    start();
    install_fixture("migrate_v2");
    // "boom" would fail a migration: none runs for a new scene.
    ASSERT_TRUE(rt->open(kId, mode("boom")));
    EXPECT_EQ(rt->status(kId).state, SceneState::Running);
    EXPECT_FALSE(printed("migrate from 0"));
    ASSERT_TRUE(rt->exit(kId));
    ASSERT_TRUE(worker.drain(2000));
    EXPECT_EQ(stored().version, 2);
}

TEST_F(Migrate, RunsOnBootRestore) {
    start();
    make_v1_data();
    install_fixture("migrate_v2");
    fs::create_directories(fs::path(list_path).parent_path());
    std::ofstream(list_path, std::ios::trunc) << R"({"ids":["migrate"]})";
    rt->restore_background();
    EXPECT_TRUE(wait_until([&] { return rt->status(kId).state == SceneState::Background; }, 5000));
    EXPECT_TRUE(printed("migrate from 1"));
    {
        std::lock_guard<std::mutex> lock(notice_mu);
        EXPECT_TRUE(notices.empty());
    }
    ASSERT_TRUE(rt->exit(kId));
    ASSERT_TRUE(worker.drain(2000));
    Stored s = stored();
    EXPECT_EQ(s.version, 2);
    EXPECT_EQ(s.items, (std::vector<std::string>{R"({"done":false,"name":"a"})",
                                                 R"({"done":false,"name":"b"})"}));
}

}  // namespace
