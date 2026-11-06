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
#include "wisp/os/os.hpp"
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
        int version = -100;
        std::vector<std::string> items;
        bool v2_seen = false;  // main.lua v2's top-level write landed
    };

    // What the scene's data directory holds, read by a store of its own
    // on the worker (the flash thread). Call it once the scene has exited
    // and the worker drained.
    Stored stored() {
        Stored out;
        EXPECT_TRUE(worker.run_sync([&] { out = stored_here(); }, wisp::os::kForever));
        return out;
    }

    Stored stored_here() {
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
        out.v2_seen = s->kv_get("v2_seen").has_value();
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
        write_meta_text(R"({"schema_version":)" + std::to_string(version) + "}");
    }

    void write_meta_text(const std::string& text) {
        const fs::path p = fs::path(repo->data_dir(kId)) / "meta.json";
        std::ofstream(p, std::ios::trunc) << text;
    }

    // The v2 manifest with a main.lua of the test's own.
    void install_v2_with(const std::string& main_lua) {
        const fs::path from = fs::path(WISP_TEST_DIR) / "scene" / "fixtures" / "migrate_v2";
        install(kId, main_lua, {{"scene.json", read_file(from / "scene.json")}});
    }

    size_t count_printed(const std::string& line) {
        size_t n = 0;
        for (const auto& l : scene_log(kId)) n += l == line ? 1 : 0;
        return n;
    }

    std::vector<SceneNotice> seen_notices() {
        std::lock_guard<std::mutex> lock(notice_mu);
        return notices;
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
    EXPECT_TRUE(s.v2_seen);
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
    // main.lua's top-level write went into the migration's batch too.
    EXPECT_FALSE(s.v2_seen);
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
    EXPECT_FALSE(s.v2_seen);
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

TEST_F(Migrate, UnreadableVersionRefused) {
    start();
    make_v1_data();
    write_meta_text("{\"schema_version\":");
    install_fixture("migrate_v2");
    auto r = rt->open(kId);
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().message, "data version unreadable");
    EXPECT_EQ(rt->status(kId).state, SceneState::Error);
    EXPECT_TRUE(wait_until([&] { return error_panel_has("data version unreadable"); }, 2000));
    EXPECT_FALSE(printed("v2 main"));
    ASSERT_TRUE(worker.drain(2000));
    EXPECT_EQ(read_file(fs::path(repo->data_dir(kId)) / "meta.json"), "{\"schema_version\":");
    Stored s = stored();
    EXPECT_EQ(s.items, v1_items());
    EXPECT_FALSE(s.v2_seen);
}

// A scene that never stores anything has no data: no data directory, and
// the repository does not list it as having any.
TEST_F(Migrate, SceneWithoutWritesLeavesNoData) {
    start();
    install("plain", "return ui.screen { ui.label { text = 'plain' } }");
    ASSERT_TRUE(rt->open("plain"));
    ASSERT_TRUE(rt->exit("plain"));
    ASSERT_TRUE(worker.drain(2000));
    EXPECT_FALSE(fs::exists(repo->data_dir("plain")));
    auto removal = repo->removal("plain", true);
    ASSERT_TRUE(removal);
    EXPECT_FALSE(removal.value().data);
    EXPECT_EQ(repo->info("plain").value().data_bytes, 0u);
}

// A migration that fails while the background is restored: the scene is
// in Error, the restore reports it once, the list drops it, and nothing
// retries it.
TEST_F(Migrate, FailedMigrationOnRestoreIsReportedOnce) {
    start();
    make_v1_data();
    install_v2_with(R"(
scene.migrate = function(old)
  print("migrate from " .. old)
  error("boom")
end
return ui.screen { ui.label { text = "v2" } }
)");
    fs::create_directories(fs::path(list_path).parent_path());
    std::ofstream(list_path, std::ios::trunc) << R"({"ids":["migrate"]})";
    rt->restore_background();
    EXPECT_TRUE(wait_until([&] { return rt->status(kId).state == SceneState::Error; }, 5000));
    EXPECT_NE(rt->status(kId).error.find("migrate failed: "), std::string::npos)
        << rt->status(kId).error;
    EXPECT_TRUE(wait_until(
        [&] {
            std::ifstream f(list_path);
            std::stringstream ss;
            ss << f.rdbuf();
            return ss.str().find("migrate") == std::string::npos;
        },
        2000));
    // Long enough for a retry to have shown up.
    wisp::os::sleep_ms(500);
    EXPECT_EQ(count_printed("migrate from 1"), 1u);
    auto seen = seen_notices();
    ASSERT_EQ(seen.size(), 1u);
    EXPECT_EQ(seen[0].kind, wisp::scene::SceneNoticeKind::RestoreFailed);
    EXPECT_TRUE(rt->background_scenes().empty());
    ASSERT_TRUE(worker.drain(2000));
    Stored s = stored();
    EXPECT_EQ(s.version, 1);
    EXPECT_EQ(s.items, v1_items());
}

// wisp.store.batch inside migrate nests in the migration's batch: a
// completed one commits with it, an aborted one fails the migration.
TEST_F(Migrate, NestedBatchInsideMigrate) {
    const char* lua = R"(
local mode = (wisp.scene.args() or {}).mode
scene.migrate = function(old)
  local items = wisp.store.collection("items")
  if mode == "abort" then
    pcall(wisp.store.batch, function()
      items:update(1, { name = "x" })
      error("inner")
    end)
    return
  end
  wisp.store.batch(function()
    for _, r in ipairs(items:find()) do
      r.done = false
      items:update(r.id, r)
    end
  end)
end
return ui.screen { ui.label { text = "v2" } }
)";
    start();
    make_v1_data();
    install_v2_with(lua);
    auto r = rt->open(kId, mode("abort"));
    ASSERT_FALSE(r);
    EXPECT_NE(r.error().message.find("migrate failed: batch_aborted"), std::string::npos)
        << r.error().message;
    ASSERT_TRUE(worker.drain(2000));
    Stored s = stored();
    EXPECT_EQ(s.version, 1);
    EXPECT_EQ(s.items, v1_items());

    ASSERT_TRUE(rt->open(kId));
    ASSERT_TRUE(rt->exit(kId));
    ASSERT_TRUE(worker.drain(2000));
    s = stored();
    EXPECT_EQ(s.version, 2);
    EXPECT_EQ(s.items, (std::vector<std::string>{R"({"done":false,"name":"a"})",
                                                 R"({"done":false,"name":"b"})"}));
}

TEST_F(Migrate, MemoryFaultRollsBack) {
    start();
    make_v1_data();
    install_v2_with(R"(
scene.migrate = function(old)
  wisp.store.collection("items"):update(1, { name = "x" })
  local t = {}
  for i = 1, 1000000000 do t[i] = string.rep("x", 4096) .. i end
end
return ui.screen { ui.label { text = "v2" } }
)");
    auto r = rt->open(kId);
    ASSERT_FALSE(r);
    EXPECT_NE(r.error().message.find("migrate failed: "), std::string::npos) << r.error().message;
    EXPECT_NE(r.error().message.find("memory"), std::string::npos) << r.error().message;
    EXPECT_EQ(rt->status(kId).state, SceneState::Error);
    ASSERT_TRUE(worker.drain(2000));
    Stored s = stored();
    EXPECT_EQ(s.version, 1);
    EXPECT_EQ(s.items, v1_items());
}

}  // namespace
