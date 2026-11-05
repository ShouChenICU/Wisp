// DirWatcher: the simulator's --scenes-dir change detection
// (scene-runtime-core spec 6.4).

#include "wisp/scene/dir_watcher.hpp"

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <system_error>
#include <vector>

#include "support/fake_devices.hpp"
#include "support/temp_dir.hpp"
#include "wisp/core/device_registry.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/core/storage_manager.hpp"

using wisp::scene::DirWatcher;

namespace fs = std::filesystem;

namespace {

void write_raw(const fs::path& path, const std::string& body) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream(path, std::ios::binary | std::ios::trunc) << body;
}

struct Fixture {
    Fixture() : in(wisp::test::temp_dir().string()) {
        registry.add(std::make_shared<wisp::test::FakeStorage>("storage.int", in + "/int", false));
        storage = std::make_unique<wisp::core::StorageManager>(registry, bus);
        EXPECT_TRUE(storage->init());
        root = fs::path(in) / "work";
        write_raw(root / "counter" / "scene.json",
                  R"({"id":"counter","name":"C","version":"1.0.0","schema_version":1})");
        write_raw(root / "counter" / "main.lua", "return ui.screen {}\n");
        write_raw(root / "other" / "scene.json",
                  R"({"id":"other","name":"O","version":"1.0.0","schema_version":1})");
        write_raw(root / "other" / "main.lua", "return ui.screen {}\n");
    }

    std::string in;
    fs::path root;
    wisp::core::DeviceRegistry registry;
    wisp::core::EventBus bus;
    std::unique_ptr<wisp::core::StorageManager> storage;
};

using Ids = std::vector<std::string>;

}  // namespace

TEST(DirWatcher, FirstPollIsTheBaseline) {
    Fixture f;
    DirWatcher w(*f.storage, f.root.string());
    EXPECT_EQ(w.poll(), Ids{});
    EXPECT_EQ(w.poll(), Ids{});
}

TEST(DirWatcher, DetectsChangeEvenWithSameSize) {
    Fixture f;
    DirWatcher w(*f.storage, f.root.string());
    EXPECT_EQ(w.poll(), Ids{});
    // One byte changed, same size: only the checksum tells.
    write_raw(f.root / "counter" / "main.lua", "return ui.screen {}\t");
    // The change is reported once the files have held still for a poll, so
    // a file still being written is never loaded half done.
    EXPECT_EQ(w.poll(), Ids{});
    EXPECT_EQ(w.poll(), Ids{"counter"});
    EXPECT_EQ(w.poll(), Ids{});
}

TEST(DirWatcher, WaitsWhileFilesKeepChanging) {
    Fixture f;
    DirWatcher w(*f.storage, f.root.string());
    EXPECT_EQ(w.poll(), Ids{});
    write_raw(f.root / "counter" / "main.lua", "-- 1\n");
    EXPECT_EQ(w.poll(), Ids{});
    write_raw(f.root / "counter" / "main.lua", "-- 12\n");
    EXPECT_EQ(w.poll(), Ids{});
    write_raw(f.root / "counter" / "lib" / "a.lua", "return 1\n");
    EXPECT_EQ(w.poll(), Ids{});
    EXPECT_EQ(w.poll(), Ids{"counter"});
}

TEST(DirWatcher, ReportsOnlyTheSceneThatChanged) {
    Fixture f;
    DirWatcher w(*f.storage, f.root.string());
    EXPECT_EQ(w.poll(), Ids{});
    write_raw(f.root / "other" / "assets" / "x.png", "png");
    EXPECT_EQ(w.poll(), Ids{});
    EXPECT_EQ(w.poll(), Ids{"other"});
    // A removed file is a change too.
    std::error_code ec;
    fs::remove(f.root / "other" / "assets" / "x.png", ec);
    EXPECT_EQ(w.poll(), Ids{});
    EXPECT_EQ(w.poll(), Ids{"other"});
}

TEST(DirWatcher, IgnoresDataDirectory) {
    Fixture f;
    DirWatcher w(*f.storage, f.root.string());
    EXPECT_EQ(w.poll(), Ids{});
    write_raw(f.root / "counter" / "data" / "kv.log", R"({"op":"put"})");
    EXPECT_EQ(w.poll(), Ids{});
    EXPECT_EQ(w.poll(), Ids{});
    write_raw(f.root / "counter" / "data" / "kv.log", R"({"op":"del"})");
    EXPECT_EQ(w.poll(), Ids{});
    EXPECT_EQ(w.poll(), Ids{});
}

TEST(DirWatcher, IgnoresHiddenEntries) {
    // Editors' swap files and a .git directory are not scene files.
    Fixture f;
    DirWatcher w(*f.storage, f.root.string());
    EXPECT_EQ(w.poll(), Ids{});
    write_raw(f.root / "counter" / ".main.lua.swp", "swap");
    write_raw(f.root / "counter" / ".git" / "HEAD", "ref");
    write_raw(f.root / ".hidden" / "main.lua", "x");
    EXPECT_EQ(w.poll(), Ids{});
    EXPECT_EQ(w.poll(), Ids{});
}

TEST(DirWatcher, MissingRootIsEmpty) {
    Fixture f;
    DirWatcher w(*f.storage, (f.root / "nope").string());
    EXPECT_EQ(w.poll(), Ids{});
    EXPECT_EQ(w.poll(), Ids{});
}
