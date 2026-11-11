#include "wisp/core/scene_repository.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "support/fake_devices.hpp"
#include "support/temp_dir.hpp"
#include "wisp/base/error.hpp"
#include "wisp/core/device_registry.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/core/log.hpp"
#include "wisp/core/scene_files.hpp"
#include "wisp/core/storage_manager.hpp"
#include "wisp/core/storage_worker.hpp"
#include "wisp/core/zip.hpp"
#include "wisp/os/os.hpp"

using wisp::ErrorCode;
using wisp::Result;
using wisp::core::SceneInfo;
using wisp::core::ScenePackage;
using wisp::core::SceneRepoConfig;
using wisp::core::SceneRepository;
using wisp::core::SourceFile;
using wisp::core::ZipReader;
using wisp::core::ZipWriter;

namespace fs = std::filesystem;

namespace {

using Files = std::vector<std::pair<std::string, std::string>>;

std::span<const uint8_t> as_bytes(const std::string& s) {
    return {reinterpret_cast<const uint8_t*>(s.data()), s.size()};
}

std::string scene_json(const std::string& version = "1.0.0",
                       const std::string& hosts = R"(["api.example.com"])") {
    return R"({"id":"todo","name":"待办","version":")" + version +
           R"(","schema_version":1,"icon":"assets/icon.png","permissions":{"http":)" + hosts + "}}";
}

Files todo_files(const std::string& version = "1.0.0",
                 const std::string& hosts = R"(["api.example.com"])") {
    return {{"scene.json", scene_json(version, hosts)},
            {"main.lua", "local u = wisp.import('util')\n"},
            {"lib/util.lua", "return {}\n"},
            {"assets/icon.png", std::string("\x89PNG", 4)}};
}

std::vector<uint8_t> zip_of(const Files& files) {
    ZipWriter w;
    for (const auto& [path, body] : files) {
        w.add(path, as_bytes(body));
    }
    wisp::os::LargeBuffer zip = w.finish();
    return {zip.data(), zip.data() + zip.size()};
}

uint64_t total_size(const Files& files) {
    uint64_t n = 0;
    for (const auto& f : files) {
        n += f.second.size();
    }
    return n;
}

Result<void> accept_all(std::span<const SourceFile>) { return Result<void>::ok(); }

void write_raw(const fs::path& path, const std::string& contents) {
    std::error_code ec;
    fs::create_directories(path.parent_path(), ec);
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f << contents;
}

std::string read_raw(const fs::path& path) {
    std::ifstream f(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

bool on_disk(const fs::path& path) {
    std::error_code ec;
    return fs::exists(path, ec);
}

// Writes `files` straight into `dir`, the way an interrupted install
// leaves them.
void lay_out(const fs::path& dir, const Files& files) {
    for (const auto& [path, body] : files) {
        write_raw(dir / path, body);
    }
}

struct Fixture {
    // `data_root` is appended to the storage root; by default it is the
    // code root.
    explicit Fixture(bool dev = false, const std::string& data_root = "/scenes",
                     wisp::core::StorageWorker* worker = nullptr)
        : in(wisp::test::temp_dir().string()) {
        registry.add(std::make_shared<wisp::test::FakeStorage>("storage.int", in, false));
        storage = std::make_unique<wisp::core::StorageManager>(registry, bus);
        EXPECT_TRUE(storage->init());
        SceneRepoConfig config;
        config.code_root = in + "/scenes";
        config.data_root = in + data_root;
        config.dev = dev;
        config.worker = worker;
        repo = std::make_unique<SceneRepository>(*storage, config);
    }

    fs::path root() const { return fs::path(in) / "scenes"; }

    Result<SceneInfo> install(const Files& files) {
        std::vector<uint8_t> zip = zip_of(files);
        return repo->install(zip, accept_all);
    }

    std::string in;
    wisp::core::DeviceRegistry registry;
    wisp::core::EventBus bus;
    std::unique_ptr<wisp::core::StorageManager> storage;
    std::unique_ptr<SceneRepository> repo;
};

std::vector<std::string> zip_paths(const wisp::os::LargeBuffer& zip) {
    std::vector<std::string> out;
    auto r = ZipReader::open(zip.span(), {.max_entries = 256, .max_total = 4 * 1024 * 1024});
    EXPECT_TRUE(r) << r.error().message;
    if (r) {
        for (const auto& e : r.value().entries()) {
            out.emplace_back(e.path.begin(), e.path.end());
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

}  // namespace

TEST(SceneRepository, InstallRejectsUnsupportedSceneToolSchema) {
    Fixture fx;
    Files files = todo_files();
    files.front().second =
        R"({"id":"todo","name":"待办","version":"1.0.0","schema_version":1,)"
        R"("icon":"assets/icon.png","tools":[{"name":"lookup",)"
        R"("description":"Lookup a value","parameters":{"type":"object","oneOf":[]}}]})";

    const auto installed = fx.install(files);

    ASSERT_FALSE(installed);
    EXPECT_EQ(installed.error().code, ErrorCode::InvalidArgument);
    EXPECT_NE(installed.error().message.find("tools[0].parameters"), std::string::npos);
    EXPECT_TRUE(fx.repo->list().value().empty());
}

TEST(SceneRepository, InstallsAndLists) {
    Fixture f;
    std::vector<std::string> checked;
    std::vector<uint8_t> zip = zip_of(todo_files());
    auto r = f.repo->install(zip, [&](std::span<const SourceFile> files) {
        for (const auto& s : files) {
            checked.push_back(std::string(s.path) + "=" + std::string(s.text));
        }
        return Result<void>::ok();
    });
    ASSERT_TRUE(r) << r.error().message;
    std::sort(checked.begin(), checked.end());
    EXPECT_EQ(checked, (std::vector<std::string>{"lib/util.lua=return {}\n",
                                                 "main.lua=local u = wisp.import('util')\n"}));

    EXPECT_EQ(r.value().manifest.id, "todo");
    EXPECT_EQ(r.value().code_bytes, total_size(todo_files()));
    EXPECT_EQ(r.value().data_bytes, 0u);

    auto list = f.repo->list();
    ASSERT_TRUE(list);
    ASSERT_EQ(list.value().size(), 1u);
    EXPECT_EQ(list.value()[0].manifest.name, "待办");
    EXPECT_EQ(list.value()[0].code_bytes, total_size(todo_files()));

    auto pkg = f.repo->load("todo");
    ASSERT_TRUE(pkg) << pkg.error().message;
    EXPECT_EQ(pkg.value().manifest.version, "1.0.0");
    std::vector<std::string> paths;
    for (const auto& [path, buf] : pkg.value().files) {
        paths.push_back(path);
        if (path == "lib/util.lua") {
            EXPECT_EQ(std::string(buf.data(), buf.data() + buf.size()), "return {}\n");
        }
    }
    std::sort(paths.begin(), paths.end());
    EXPECT_EQ(paths, (std::vector<std::string>{"assets/icon.png", "lib/util.lua", "main.lua",
                                               "scene.json"}));

    EXPECT_FALSE(f.repo->info("nope"));
    EXPECT_EQ(f.repo->info("nope").error().code, ErrorCode::NotFound);
}

TEST(SceneRepository, ListSkipsHiddenAndInvalid) {
    Fixture f;
    ASSERT_TRUE(f.install(todo_files()));
    write_raw(f.root() / "broken" / "scene.json", "{");
    write_raw(f.root() / "other" / "scene.json", scene_json());  // id "todo" in dir "other"
    write_raw(f.root() / ".hidden" / "scene.json", scene_json());
    write_raw(f.root() / "stray.txt", "x");
    auto list = f.repo->list();
    ASSERT_TRUE(list);
    ASSERT_EQ(list.value().size(), 1u);
    EXPECT_EQ(list.value()[0].manifest.id, "todo");
}

// manifests(): list() without sizing anything, so one read per scene.
TEST(SceneRepository, ManifestsReadsOnlyTheManifests) {
    Fixture f;
    ASSERT_TRUE(f.install(todo_files()));
    write_raw(f.root() / "todo" / "data" / "kv.log", "x\n");
    write_raw(f.root() / "broken" / "scene.json", "{");
    write_raw(f.root() / "other" / "scene.json", scene_json());
    write_raw(f.root() / ".hidden" / "scene.json", scene_json());
    write_raw(f.root() / "kept" / "data" / "kv.log", "x\n");  // data only
    const uint64_t before = f.storage->calls();
    auto got = f.repo->manifests();
    ASSERT_TRUE(got) << got.error().message;
    ASSERT_EQ(got.value().size(), 1u);
    EXPECT_EQ(got.value()[0].id, "todo");
    EXPECT_EQ(got.value()[0].name, "待办");
    // The root, then scene.json in each of todo, broken, other and kept.
    EXPECT_EQ(f.storage->calls() - before, 5u);
}

// changed_since(): the ids each replace or remove touched, with no file
// work, so a cached listing knows what to read again.
TEST(SceneRepository, ChangedSinceNamesWhatChanged) {
    Fixture f;
    uint64_t gen = 0;
    EXPECT_TRUE(f.repo->changed_since(0, gen).empty());
    const uint64_t start = gen;
    ASSERT_TRUE(f.install(todo_files()));
    Files clock = todo_files();
    clock[0].second = R"({"id":"clock","name":"时钟","version":"1.0.0","schema_version":1,)"
                      R"("icon":"assets/icon.png"})";
    ASSERT_TRUE(f.install(clock));
    uint64_t now = 0;
    std::vector<std::string> changed = f.repo->changed_since(start, now);
    std::sort(changed.begin(), changed.end());
    EXPECT_EQ(changed, (std::vector<std::string>{"clock", "todo"}));
    EXPECT_GT(now, start);

    // Nothing since: nothing, and the same generation.
    const uint64_t calls = f.storage->calls();
    uint64_t again = 0;
    EXPECT_TRUE(f.repo->changed_since(now, again).empty());
    EXPECT_EQ(again, now);
    EXPECT_EQ(f.storage->calls(), calls);

    // An update, then a remove: each names its own scene only.
    ASSERT_TRUE(f.install(todo_files("1.1.0")));
    EXPECT_EQ(f.repo->changed_since(now, again), (std::vector<std::string>{"todo"}));
    now = again;
    ASSERT_TRUE(f.repo->remove("clock", false));
    EXPECT_EQ(f.repo->changed_since(now, again), (std::vector<std::string>{"clock"}));
    now = again;
    // A rejected package changes nothing.
    Files bad = todo_files();
    bad.pop_back();  // its icon is missing
    ASSERT_FALSE(f.install(bad));
    EXPECT_TRUE(f.repo->changed_since(now, again).empty());
    // From an older generation, every id once.
    changed = f.repo->changed_since(start, again);
    std::sort(changed.begin(), changed.end());
    EXPECT_EQ(changed, (std::vector<std::string>{"clock", "todo"}));
}

// recover() finishing an interrupted install counts as a change too.
TEST(SceneRepository, ChangedSinceIncludesRecoveredInstalls) {
    Fixture f;
    uint64_t gen = 0;
    (void)f.repo->changed_since(0, gen);
    lay_out(f.root() / ".todo.newdir", todo_files());
    ASSERT_TRUE(f.repo->recover());
    uint64_t now = 0;
    EXPECT_EQ(f.repo->changed_since(gen, now), (std::vector<std::string>{"todo"}));
}

TEST(SceneRepository, RejectsDeeplyNestedManifest) {
    Fixture f;
    Files files = todo_files();
    files[0].second.insert(files[0].second.size() - 1, R"(,"x":[[[[[[[[[1]]]]]]]]])");
    auto r = f.install(files);
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(r.error().message, "scene.json: nested deeper than 8 levels");
    EXPECT_FALSE(fs::exists(f.root() / "todo"));

    // One already on disk (put there before the cap) is skipped by the
    // listing instead of being parsed.
    write_raw(f.root() / "todo" / "scene.json", files[0].second);
    auto list = f.repo->list();
    ASSERT_TRUE(list) << list.error().message;
    EXPECT_TRUE(list.value().empty());
}

TEST(SceneRepository, ListOfMissingRootIsEmpty) {
    Fixture f;
    auto list = f.repo->list();
    ASSERT_TRUE(list) << list.error().message;
    EXPECT_TRUE(list.value().empty());
}

TEST(SceneRepository, ReinstallKeepsData) {
    Fixture f;
    ASSERT_TRUE(f.install(todo_files()));
    const std::string kv = "{\"op\":\"put\",\"id\":\"a\",\"v\":1}\n";
    ASSERT_TRUE(f.storage->write_atomic(f.repo->data_dir("todo") + "/kv.log", as_bytes(kv)));

    // 1.0.1 drops the icon, so its file must not survive the replace.
    Files v2 = todo_files("1.0.1");
    v2.pop_back();
    v2[0].second = R"({"id":"todo","name":"待办","version":"1.0.1","schema_version":1})";
    auto r = f.install(v2);
    ASSERT_TRUE(r) << r.error().message;
    EXPECT_EQ(r.value().manifest.version, "1.0.1");
    EXPECT_EQ(r.value().data_bytes, kv.size());
    EXPECT_EQ(r.value().code_bytes, total_size(v2));
    EXPECT_EQ(read_raw(f.root() / "todo" / "data" / "kv.log"), kv);
    // The old package's files are gone, not merged.
    EXPECT_FALSE(on_disk(f.root() / "todo" / "assets" / "icon.png"));
    EXPECT_FALSE(on_disk(f.root() / ".todo.tmpdir"));
    EXPECT_FALSE(on_disk(f.root() / ".todo.newdir"));
}

TEST(SceneRepository, RejectsCheckerErrorWithFileAndLine) {
    Fixture f;
    std::vector<uint8_t> zip = zip_of(todo_files());
    auto r = f.repo->install(zip, [](std::span<const SourceFile>) {
        return Result<void>::err(ErrorCode::InvalidArgument, "lib/util.lua:3: x");
    });
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(r.error().message, "lib/util.lua:3: x");
    EXPECT_FALSE(on_disk(f.root() / "todo"));
    EXPECT_FALSE(on_disk(f.root() / ".todo.tmpdir"));
    EXPECT_FALSE(on_disk(f.root() / ".todo.newdir"));
}

TEST(SceneRepository, CodeLimitIs512KB) {
    Fixture f;
    Files files = todo_files();
    const uint64_t rest = total_size(files) - files.back().second.size();
    files.back().second = std::string(512 * 1024 - rest, 'x');
    ASSERT_EQ(total_size(files), 512u * 1024u);
    auto ok = f.install(files);
    ASSERT_TRUE(ok) << ok.error().message;
    EXPECT_EQ(ok.value().code_bytes, 512u * 1024u);

    files.back().second.push_back('x');
    auto big = f.install(files);
    ASSERT_FALSE(big);
    EXPECT_EQ(big.error().code, ErrorCode::QuotaExceeded);
}

TEST(SceneRepository, RejectsUnexpectedFilesAndData) {
    Fixture f;
    for (const char* extra : {"README.md", "data/kv.log", "lib/x.txt", "other.lua", "assets/a.tmp",
                              "lib/x.new/y.lua", "assets/a.tmp/b.png"}) {
        Files files = todo_files();
        files.emplace_back(extra, "x");
        auto r = f.install(files);
        ASSERT_FALSE(r) << extra;
        EXPECT_EQ(r.error().code, ErrorCode::InvalidArgument) << extra;
        EXPECT_NE(r.error().message.find(extra), std::string::npos) << r.error().message;
    }
    for (const char* dir : {"data/", "lib/x.new/", "assets/a.tmp/"}) {
        Files dir_entry = todo_files();
        dir_entry.emplace_back(dir, "");
        EXPECT_FALSE(f.install(dir_entry)) << dir;
    }

    Files no_entry = todo_files();
    no_entry.erase(no_entry.begin() + 1);  // main.lua
    EXPECT_FALSE(f.install(no_entry));

    Files no_icon = todo_files();
    no_icon.pop_back();  // assets/icon.png, named by scene.json
    EXPECT_FALSE(f.install(no_icon));

    Files no_manifest = todo_files();
    no_manifest.erase(no_manifest.begin());
    EXPECT_FALSE(f.install(no_manifest));

    std::vector<uint8_t> garbage = {1, 2, 3};
    EXPECT_FALSE(f.repo->install(garbage, accept_all));

    EXPECT_FALSE(on_disk(f.root() / "todo"));

    Files with_dirs = todo_files();
    with_dirs.emplace_back("lib/", "");
    with_dirs.emplace_back("assets/img/", "");
    with_dirs.emplace_back("lib/sub/deep.lua", "return 1\n");
    EXPECT_TRUE(f.install(with_dirs));
}

TEST(SceneRepository, RecoverFinishesInterruptedReplace) {
    // Interrupted after the new package was staged complete, before the
    // data directory moved over.
    {
        Fixture f;
        ASSERT_TRUE(f.install(todo_files()));
        write_raw(f.root() / "todo" / "data" / "kv.log", "kv\n");
        lay_out(f.root() / ".todo.newdir", todo_files("1.0.1"));
        lay_out(f.root() / ".todo.tmpdir", {{"scene.json", "{"}});
        write_raw(f.root() / ".zzz.tmpdir" / "main.lua", "half");

        ASSERT_TRUE(f.repo->recover());
        auto info = f.repo->info("todo");
        ASSERT_TRUE(info) << info.error().message;
        EXPECT_EQ(info.value().manifest.version, "1.0.1");
        EXPECT_EQ(read_raw(f.root() / "todo" / "data" / "kv.log"), "kv\n");
        EXPECT_FALSE(on_disk(f.root() / ".todo.newdir"));
        EXPECT_FALSE(on_disk(f.root() / ".todo.tmpdir"));
        EXPECT_FALSE(on_disk(f.root() / ".zzz.tmpdir"));
    }
    // Interrupted after the old directory was removed.
    {
        Fixture f;
        lay_out(f.root() / ".todo.newdir", todo_files("1.0.1"));
        write_raw(f.root() / ".todo.newdir" / "data" / "kv.log", "kv\n");
        ASSERT_TRUE(f.repo->recover());
        auto info = f.repo->info("todo");
        ASSERT_TRUE(info) << info.error().message;
        EXPECT_EQ(info.value().manifest.version, "1.0.1");
        EXPECT_EQ(read_raw(f.root() / "todo" / "data" / "kv.log"), "kv\n");
        EXPECT_FALSE(on_disk(f.root() / ".todo.newdir"));
    }
    // Interrupted after the data moved, before the old directory went.
    {
        Fixture f;
        lay_out(f.root() / "todo", todo_files());
        lay_out(f.root() / ".todo.newdir", todo_files("1.0.1"));
        write_raw(f.root() / ".todo.newdir" / "data" / "kv.log", "kv\n");
        ASSERT_TRUE(f.repo->recover());
        auto info = f.repo->info("todo");
        ASSERT_TRUE(info) << info.error().message;
        EXPECT_EQ(info.value().manifest.version, "1.0.1");
        EXPECT_EQ(read_raw(f.root() / "todo" / "data" / "kv.log"), "kv\n");
        EXPECT_FALSE(on_disk(f.root() / ".todo.newdir"));
    }
    // Both directories hold data (the scene wrote again after a failed
    // replace): neither copy is deleted, the replace reports an error.
    {
        Fixture f;
        lay_out(f.root() / "todo", todo_files());
        write_raw(f.root() / "todo" / "data" / "kv.log", "newer\n");
        lay_out(f.root() / ".todo.newdir", todo_files("1.0.1"));
        write_raw(f.root() / ".todo.newdir" / "data" / "kv.log", "older\n");
        EXPECT_FALSE(f.repo->recover());
        EXPECT_EQ(read_raw(f.root() / "todo" / "data" / "kv.log"), "newer\n");
        EXPECT_EQ(read_raw(f.root() / ".todo.newdir" / "data" / "kv.log"), "older\n");
        EXPECT_FALSE(f.install(todo_files("1.0.2")));
        EXPECT_EQ(read_raw(f.root() / "todo" / "data" / "kv.log"), "newer\n");
    }
    // Leftovers whose names are only a suffix are ignored.
    {
        Fixture f;
        write_raw(f.root() / ".newdir" / "x", "x");
        write_raw(f.root() / ".tmpdir" / "x", "x");
        EXPECT_TRUE(f.repo->recover());
        EXPECT_TRUE(on_disk(f.root() / ".newdir" / "x"));
    }
    // Nothing to do, including no scenes directory at all.
    {
        Fixture f;
        EXPECT_TRUE(f.repo->recover());
    }
}

// The data directory is found inside the package directory however the
// roots are spelled, so a reinstall never deletes it.
TEST(SceneRepository, ReinstallKeepsDataWhenRootsAreSpelledDifferently) {
    Fixture f(/*dev=*/false, "//scenes/.");
    ASSERT_TRUE(f.install(todo_files()));
    ASSERT_TRUE(f.storage->write_atomic(f.repo->data_dir("todo") + "/kv.log", as_bytes("kv\n")));
    ASSERT_TRUE(f.install(todo_files("1.0.1")));
    EXPECT_EQ(read_raw(f.root() / "todo" / "data" / "kv.log"), "kv\n");

    ASSERT_TRUE(f.repo->remove("todo", false));
    EXPECT_EQ(read_raw(f.root() / "todo" / "data" / "kv.log"), "kv\n");
    ASSERT_TRUE(f.install(todo_files()));
    ASSERT_TRUE(f.repo->remove("todo", true));
    EXPECT_FALSE(on_disk(f.root() / "todo"));
}

TEST(SceneRepository, ExportRoundTrips) {
    Fixture f;
    ASSERT_TRUE(f.install(todo_files()));
    ASSERT_TRUE(f.storage->write_atomic(f.repo->data_dir("todo") + "/kv.log", as_bytes("kv\n")));
    // Bookkeeping of an interrupted atomic write is never exported.
    write_raw(f.root() / "todo" / "data" / "items.log.tmp", "half");

    auto plain = f.repo->export_zip("todo", false);
    ASSERT_TRUE(plain) << plain.error().message;
    EXPECT_EQ(zip_paths(plain.value()), (std::vector<std::string>{"assets/icon.png", "lib/util.lua",
                                                                  "main.lua", "scene.json"}));

    auto full = f.repo->export_zip("todo", true);
    ASSERT_TRUE(full) << full.error().message;
    EXPECT_EQ(zip_paths(full.value()),
              (std::vector<std::string>{"assets/icon.png", "data/kv.log", "lib/util.lua",
                                        "main.lua", "scene.json"}));

    // The code-only export installs again (on another device here).
    Fixture g;
    auto again = g.repo->install(plain.value().span(), accept_all);
    ASSERT_TRUE(again) << again.error().message;
    EXPECT_EQ(again.value().code_bytes, total_size(todo_files()));

    EXPECT_EQ(f.repo->export_zip("nope", false).error().code, ErrorCode::NotFound);
}

TEST(SceneRepository, RemoveKeepsOrDeletesData) {
    Fixture f;
    ASSERT_TRUE(f.install(todo_files()));
    ASSERT_TRUE(f.storage->write_atomic(f.repo->data_dir("todo") + "/kv.log", as_bytes("kv\n")));

    ASSERT_TRUE(f.repo->remove("todo", false));
    EXPECT_EQ(f.repo->info("todo").error().code, ErrorCode::NotFound);
    EXPECT_TRUE(f.repo->list().value().empty());
    EXPECT_EQ(read_raw(f.root() / "todo" / "data" / "kv.log"), "kv\n");
    EXPECT_FALSE(on_disk(f.root() / "todo" / "main.lua"));
    EXPECT_FALSE(on_disk(f.root() / "todo" / "lib"));
    EXPECT_EQ(f.repo->remove("todo", false).error().code, ErrorCode::NotFound);

    // Installing again picks the kept data up.
    auto again = f.install(todo_files());
    ASSERT_TRUE(again);
    EXPECT_EQ(again.value().data_bytes, 3u);

    ASSERT_TRUE(f.repo->remove("todo", true));
    EXPECT_FALSE(on_disk(f.root() / "todo"));
    EXPECT_EQ(f.repo->remove("todo", true).error().code, ErrorCode::NotFound);
    EXPECT_EQ(f.repo->remove("Bad", true).error().code, ErrorCode::InvalidArgument);
}

TEST(SceneRepository, DevModeIsReadOnly) {
    Fixture f(/*dev=*/true);
    lay_out(f.root() / "todo", todo_files());
    write_raw(f.root() / "todo" / "README.md", "ignored in dev mode");

    std::vector<uint8_t> zip = zip_of(todo_files());
    EXPECT_EQ(f.repo->install(zip, accept_all).error().code, ErrorCode::Unsupported);
    EXPECT_EQ(f.repo->remove("todo", true).error().code, ErrorCode::Unsupported);
    EXPECT_EQ(f.repo->data_dir("todo"), f.in + "/scenes/todo/data");

    auto pkg = f.repo->load("todo");
    ASSERT_TRUE(pkg) << pkg.error().message;
    EXPECT_EQ(pkg.value().files.size(), 4u);
    EXPECT_TRUE(on_disk(f.root() / "todo" / "main.lua"));
}

TEST(SceneRepository, LogsHostChanges) {
    wisp::log::reset_for_test();
    wisp::log::init(8 * 1024);
    Fixture f;
    ASSERT_TRUE(f.install(todo_files()));
    ASSERT_TRUE(f.install(todo_files("1.0.1")));  // same hosts: no line

    auto count = [] {
        int n = 0;
        for (const auto& e : wisp::log::ring_entries()) {
            if (e.tag == "scene.todo" && e.msg.find("http hosts changed") != std::string::npos) {
                ++n;
            }
        }
        return n;
    };
    EXPECT_EQ(count(), 0);

    ASSERT_TRUE(f.install(todo_files("1.0.2", R"(["b.example.com"])")));
    EXPECT_EQ(count(), 1);
    bool found = false;
    for (const auto& e : wisp::log::ring_entries()) {
        if (e.tag == "scene.todo" &&
            e.msg == "http hosts changed: [api.example.com] -> [b.example.com]") {
            found = true;
        }
    }
    EXPECT_TRUE(found);
    wisp::log::reset_for_test();
}

// Installs arrive on HTTP server threads while the storage worker loads and
// removes scenes: every method serializes on the repository's own lock. Two
// installs of the same scene at once do not interleave: the later one is
// refused with Busy while the first runs.
TEST(SceneRepository, ConcurrentCallsAreSerialized) {
    Fixture fx;
    ASSERT_TRUE(fx.install(todo_files("1.0.0")));
    std::atomic<int> failures{0};
    std::atomic<int> installed{0};
    auto installer = [&](const char* version) {
        for (int i = 0; i < 15; ++i) {
            auto r = fx.install(todo_files(version));
            if (r) {
                ++installed;
            } else if (r.error().code != ErrorCode::Busy) {
                ++failures;
            }
        }
    };
    auto reader = [&] {
        for (int i = 0; i < 30; ++i) {
            if (!fx.repo->load("todo")) ++failures;
            auto listed = fx.repo->list();
            if (!listed || listed.value().size() != 1) ++failures;
        }
    };
    std::thread a(installer, "1.0.1");
    std::thread b(installer, "1.0.2");
    std::thread c(reader);
    a.join();
    b.join();
    c.join();
    EXPECT_EQ(failures.load(), 0);
    EXPECT_GT(installed.load(), 0);
}

// The checker may take as long as it likes (it waits for the scene task),
// so the lock is not held while it runs: it can use the repository itself.
TEST(SceneRepository, CheckerRunsOutsideTheLock) {
    Fixture fx;
    ASSERT_TRUE(fx.install(todo_files("1.0.0")));
    std::vector<uint8_t> zip = zip_of(todo_files("1.1.0"));
    bool listed = false;
    auto r = fx.repo->install(zip, [&](std::span<const SourceFile>) {
        listed = static_cast<bool>(fx.repo->list());
        return Result<void>::ok();
    });
    ASSERT_TRUE(r) << r.error().message;
    EXPECT_TRUE(listed);
}

// installing(id) holds from the moment the upload names its scene until the
// install returns, failed or not; a second install of the same id meanwhile
// is refused rather than interleaved with the first.
TEST(SceneRepository, InstallingMarksTheIdUntilItReturns) {
    Fixture fx;
    EXPECT_FALSE(fx.repo->installing("todo"));
    std::vector<uint8_t> zip = zip_of(todo_files("1.0.0"));
    bool during = false;
    Result<SceneInfo> nested = Result<SceneInfo>::err(ErrorCode::IoError, "not run");
    auto r = fx.repo->install(zip, [&](std::span<const SourceFile>) {
        during = fx.repo->installing("todo");
        nested = fx.repo->install(zip, accept_all);
        return Result<void>::ok();
    });
    ASSERT_TRUE(r) << r.error().message;
    EXPECT_TRUE(during);
    ASSERT_FALSE(nested);
    EXPECT_EQ(nested.error().code, ErrorCode::Busy);
    EXPECT_FALSE(fx.repo->installing("todo"));

    // A refused package leaves no mark behind.
    auto refused = fx.repo->install(zip, [](std::span<const SourceFile>) {
        return Result<void>::err(ErrorCode::InvalidArgument, "main.lua:1: no");
    });
    EXPECT_FALSE(refused);
    EXPECT_FALSE(fx.repo->installing("todo"));
    EXPECT_FALSE(fx.repo->installing("other"));
}

// What a delete finds: the package and its name, and data left behind once
// the package itself is gone.
// The delete flow marks the id from the user's yes until the files are
// gone; marks nest, so two deletes of one id overlap safely.
TEST(SceneRepository, RemovingMarksTheIdUntilEnded) {
    Fixture f;
    EXPECT_FALSE(f.repo->removing("todo"));
    f.repo->begin_removing("todo");
    f.repo->begin_removing("todo");
    EXPECT_TRUE(f.repo->removing("todo"));
    EXPECT_FALSE(f.repo->removing("other"));
    f.repo->end_removing("todo");
    EXPECT_TRUE(f.repo->removing("todo"));
    f.repo->end_removing("todo");
    EXPECT_FALSE(f.repo->removing("todo"));
    f.repo->end_removing("todo");  // unbalanced: ignored
    EXPECT_FALSE(f.repo->removing("todo"));
}

TEST(SceneRepository, RemovalSeesDataLeftBehind) {
    Fixture fx;
    EXPECT_EQ(fx.repo->removal("todo", true).error().code, ErrorCode::NotFound);
    ASSERT_TRUE(fx.install(todo_files()));
    auto r = fx.repo->removal("todo", false);
    ASSERT_TRUE(r);
    EXPECT_EQ(r.value().name, "待办");
    EXPECT_TRUE(r.value().package);
    EXPECT_FALSE(r.value().data);
    ASSERT_TRUE(fx.storage->write_atomic(fx.repo->data_dir("todo") + "/kv.log", as_bytes("kv\n")));
    EXPECT_TRUE(fx.repo->removal("todo", false).value().data);

    ASSERT_TRUE(fx.repo->remove("todo", false));
    EXPECT_EQ(fx.repo->info("todo").error().code, ErrorCode::NotFound);
    // Only the data is left: found when it is to go too.
    EXPECT_EQ(fx.repo->removal("todo", false).error().code, ErrorCode::NotFound);
    r = fx.repo->removal("todo", true);
    ASSERT_TRUE(r);
    EXPECT_FALSE(r.value().package);
    EXPECT_TRUE(r.value().data);
    EXPECT_EQ(r.value().name, "todo");
    ASSERT_TRUE(fx.repo->remove("todo", true));
    EXPECT_EQ(fx.repo->removal("todo", true).error().code, ErrorCode::NotFound);
    EXPECT_EQ(fx.repo->removal("Bad", true).error().code, ErrorCode::InvalidArgument);

    // A broken manifest is still a package, named by its id.
    ASSERT_TRUE(fx.install(todo_files()));
    std::ofstream(fx.root() / "todo" / "scene.json") << "{not json";
    r = fx.repo->removal("todo", false);
    ASSERT_TRUE(r);
    EXPECT_EQ(r.value().name, "todo");
}

// files/ (wisp.files, scene-device spec 7.2) is the scene's like data/:
// a replace carries it over, and the package's own files never include it.
TEST(SceneRepository, ReplaceKeepsFilesDir) {
    Fixture f;
    ASSERT_TRUE(f.install(todo_files()));
    write_raw(f.root() / "todo" / "files" / "a.txt", "kept");
    auto r = f.install(todo_files("1.0.1"));
    ASSERT_TRUE(r) << r.error().message;
    EXPECT_EQ(r.value().code_bytes, total_size(todo_files("1.0.1")));
    EXPECT_EQ(read_raw(f.root() / "todo" / "files" / "a.txt"), "kept");
    EXPECT_FALSE(on_disk(f.root() / ".todo.newdir"));
}

TEST(SceneRepository, LoadAndExportSkipFiles) {
    Fixture f;
    ASSERT_TRUE(f.install(todo_files()));
    write_raw(f.root() / "todo" / "files" / "a.txt", "not code");
    ASSERT_TRUE(f.storage->write_atomic(f.repo->data_dir("todo") + "/kv.log", as_bytes("kv\n")));

    auto info = f.repo->info("todo");
    ASSERT_TRUE(info);
    EXPECT_EQ(info.value().code_bytes, total_size(todo_files()));

    auto pkg = f.repo->load("todo");
    ASSERT_TRUE(pkg) << pkg.error().message;
    for (const auto& file : pkg.value().files) EXPECT_NE(file.first.rfind("files/", 0), 0u);

    auto full = f.repo->export_zip("todo", true);
    ASSERT_TRUE(full);
    EXPECT_EQ(zip_paths(full.value()),
              (std::vector<std::string>{"assets/icon.png", "data/kv.log", "lib/util.lua",
                                        "main.lua", "scene.json"}));
}

// Without data, files/ stays in both places; with data it goes from both:
// <int>/scenes/<id>/files and <sd>/wisp/library/scenes/<id>/files.
TEST(SceneRepository, RemoveWithDataRemovesFiles) {
    const std::string in = wisp::test::temp_dir().string();
    const std::string sd = wisp::test::temp_dir().string();
    wisp::core::DeviceRegistry registry;
    wisp::core::EventBus bus;
    registry.add(std::make_shared<wisp::test::FakeStorage>("storage.int", in, false));
    registry.add(std::make_shared<wisp::test::FakeStorage>("storage.sd", sd, true));
    wisp::core::StorageManager storage(registry, bus);
    ASSERT_TRUE(storage.init());
    SceneRepoConfig config;
    config.code_root = in + "/scenes";
    config.data_root = in + "/scenes";
    SceneRepository repo(storage, config);
    const fs::path int_files = fs::path(in) / "scenes" / "todo" / "files";
    const fs::path sd_files = fs::path(sd) / "wisp" / "library" / "scenes" / "todo" / "files";

    ASSERT_TRUE(repo.install(zip_of(todo_files()), accept_all));
    write_raw(int_files / "a.txt", "a");
    write_raw(sd_files / "b.txt", "b");
    // Files alone (no data/) count as the scene's data.
    EXPECT_TRUE(repo.removal("todo", false).value().data);
    ASSERT_TRUE(repo.remove("todo", false));
    EXPECT_EQ(read_raw(int_files / "a.txt"), "a");
    EXPECT_EQ(read_raw(sd_files / "b.txt"), "b");
    EXPECT_FALSE(on_disk(fs::path(in) / "scenes" / "todo" / "main.lua"));

    ASSERT_TRUE(repo.install(zip_of(todo_files()), accept_all));
    EXPECT_EQ(read_raw(int_files / "a.txt"), "a");
    ASSERT_TRUE(repo.remove("todo", true));
    EXPECT_FALSE(on_disk(fs::path(in) / "scenes" / "todo"));
    EXPECT_FALSE(on_disk(fs::path(sd) / "wisp" / "library" / "scenes" / "todo"));
    EXPECT_TRUE(on_disk(fs::path(sd) / "wisp" / "library"));
}

namespace {

struct RepoLogTap : wisp::LogSink {
    RepoLogTap() { wisp::log::add_sink(this); }
    ~RepoLogTap() override { wisp::log::remove_sink(this); }
    RepoLogTap(const RepoLogTap&) = delete;
    RepoLogTap& operator=(const RepoLogTap&) = delete;
    void write(const wisp::LogRecord& rec) override {
        std::lock_guard<std::mutex> lock(mu);
        lines.push_back(std::string(rec.tag) + "|" + rec.msg);
    }
    size_t count(std::string_view a, std::string_view b) {
        std::lock_guard<std::mutex> lock(mu);
        size_t n = 0;
        for (const std::string& l : lines)
            n += l.find(a) != std::string::npos && l.find(b) != std::string::npos ? 1 : 0;
        return n;
    }
    std::mutex mu;
    std::vector<std::string> lines;
};

// A repository and SceneFiles over an internal disk and a card, wired the
// way SceneWiring wires them.
struct FilesRig {
    FilesRig() : in(wisp::test::temp_dir().string()), sd(wisp::test::temp_dir().string()) {
        sd_dev = std::make_shared<wisp::test::FakeStorage>("storage.sd", sd, true);
        registry.add(std::make_shared<wisp::test::FakeStorage>("storage.int", in, false));
        registry.add(sd_dev);
        storage = std::make_unique<wisp::core::StorageManager>(registry, bus);
        EXPECT_TRUE(storage->init());
        SceneRepoConfig config;
        config.code_root = in + "/scenes";
        config.data_root = in + "/scenes";
        repo = std::make_unique<SceneRepository>(*storage, config);
        files_cfg.data_root = in + "/scenes";
        files_cfg.declared = [this](std::string_view id) -> Result<wisp::core::FilesWant> {
            auto m = repo->manifest(id);
            if (!m) return Result<wisp::core::FilesWant>::err(m.error().code, m.error().message);
            return Result<wisp::core::FilesWant>::ok(m.value().files);
        };
        files = std::make_unique<wisp::core::SceneFiles>(*storage, files_cfg);
    }
    void set_sd(bool online) {
        sd_dev->online_flag = online;
        storage->refresh();
        files->on_sd_changed();
    }
    fs::path sd_files(const std::string& id) const {
        return fs::path(sd) / "wisp" / "library" / "scenes" / id / "files";
    }

    std::string in;
    std::string sd;
    wisp::core::DeviceRegistry registry;
    wisp::core::EventBus bus;
    std::shared_ptr<wisp::test::FakeStorage> sd_dev;
    std::unique_ptr<wisp::core::StorageManager> storage;
    std::unique_ptr<SceneRepository> repo;
    wisp::core::SceneFilesConfig files_cfg;
    std::unique_ptr<wisp::core::SceneFiles> files;
};

}  // namespace

// Deleting with data while the card is out cannot reach the card: its
// files stay, that is logged, and nothing removes them later on its own.
TEST(SceneRepository, RemoveWithCardOutKeepsCardFiles) {
    FilesRig r;
    ASSERT_TRUE(r.repo->install(zip_of(todo_files()), accept_all));
    ASSERT_TRUE(r.files->write("todo", "song.mp3", as_bytes("music")));
    EXPECT_EQ(read_raw(r.sd_files("todo") / "song.mp3"), "music");

    r.set_sd(false);
    RepoLogTap tap;
    ASSERT_TRUE(r.repo->remove("todo", true));
    ASSERT_TRUE(r.files->remove_all("todo"));
    EXPECT_EQ(tap.count("scene|", "SD card is out"), 1u);
    EXPECT_FALSE(on_disk(fs::path(r.in) / "scenes" / "todo"));

    r.set_sd(true);
    ASSERT_TRUE(r.files->scan());
    EXPECT_EQ(read_raw(r.sd_files("todo") / "song.mp3"), "music");
    EXPECT_EQ(tap.count("scene_files|", "todo/files"), 2u);  // on_sd_changed(), scan()
}

// Without data the files stay, and the same id installed again finds them
// where they were, counted.
TEST(SceneRepository, ReinstallAdoptsFiles) {
    FilesRig r;
    ASSERT_TRUE(r.repo->install(zip_of(todo_files()), accept_all));
    ASSERT_TRUE(r.files->write("todo", "a.txt", as_bytes("12345")));
    ASSERT_TRUE(r.repo->remove("todo", false));
    ASSERT_TRUE(r.repo->install(zip_of(todo_files("1.1.0")), accept_all));

    for (bool reboot : {false, true}) {
        if (reboot) {
            r.files = std::make_unique<wisp::core::SceneFiles>(*r.storage, r.files_cfg);
            ASSERT_TRUE(r.files->scan());
        }
        auto info = r.files->info("todo");
        ASSERT_TRUE(info);
        EXPECT_EQ(info.value().location, wisp::core::FilesLocation::Sd);
        EXPECT_EQ(info.value().used, 5u);
        auto listed = r.files->list("todo");
        ASSERT_TRUE(listed);
        ASSERT_EQ(listed.value().entries.size(), 1u);
        EXPECT_EQ(listed.value().entries[0].name, "a.txt");
        EXPECT_EQ(read_raw(r.sd_files("todo") / "a.txt"), "12345");
    }
}

// A delete with data while the card was out left the card's files; the
// same id installed again takes them over on its first, read-only, look.
TEST(SceneRepository, ReinstallAdoptsCardFilesAfterCardOutDelete) {
    FilesRig r;
    ASSERT_TRUE(r.repo->install(zip_of(todo_files()), accept_all));
    ASSERT_TRUE(r.files->write("todo", "song.mp3", as_bytes("music")));
    r.set_sd(false);
    ASSERT_TRUE(r.repo->remove("todo", true));
    ASSERT_TRUE(r.files->remove_all("todo"));
    ASSERT_TRUE(r.repo->install(zip_of(todo_files("1.1.0")), accept_all));
    r.set_sd(true);

    auto listed = r.files->list("todo");
    ASSERT_TRUE(listed);
    ASSERT_EQ(listed.value().entries.size(), 1u);
    EXPECT_EQ(listed.value().entries[0].name, "song.mp3");
    auto data = r.files->read("todo", "song.mp3", 0, 100);
    ASSERT_TRUE(data);
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(data.value().data()), data.value().size()),
              "music");
    EXPECT_EQ(r.files->info("todo").value().location, wisp::core::FilesLocation::Sd);
    EXPECT_EQ(r.files->info("todo").value().used, 5u);
    EXPECT_TRUE(on_disk(fs::path(r.in) / "scenes" / "todo" / "data" / "files_loc.json"));
}

namespace {

// A package whose files take several flash-thread jobs to stage: three
// assets, one larger than a job writes at once.
Files multi_file_package() {
    Files files = todo_files();
    files.push_back({"assets/a.bin", std::string(wisp::core::kIoChunk + 100, 'a')});
    files.push_back({"assets/b.bin", std::string(1000, 'b')});
    return files;
}

// Parks `worker` until the returned semaphore is given.
struct ParkedWorker {
    explicit ParkedWorker(wisp::core::StorageWorker& w) {
        EXPECT_TRUE(w.submit([this] {
            started.give();
            (void)release.take(wisp::os::kForever);
        }));
        EXPECT_TRUE(started.take(5000));
    }
    ~ParkedWorker() { release.give(); }
    wisp::os::Semaphore started{1, 0};
    wisp::os::Semaphore release{1, 0};
};

bool wait_for_pending(wisp::core::StorageWorker& w, size_t n) {
    for (int i = 0; i < 5000; ++i) {
        if (w.pending() >= n) return true;
        wisp::os::sleep_ms(1);
    }
    return false;
}

}  // namespace

// An install goes to the flash thread in small jobs, so other work queued
// meanwhile (a settings save, a download chunk) runs before it completes
// instead of waiting for the whole package to be written.
TEST(SceneRepository, InstallLetsOtherFlashWorkInterleave) {
    wisp::core::StorageWorker worker;
    Fixture f(false, "/scenes", &worker);
    ASSERT_TRUE(worker.start());
    std::vector<uint8_t> zip = zip_of(multi_file_package());

    std::atomic<bool> installed_when_ran{true};
    std::atomic<bool> ran{false};
    Result<SceneInfo> result = Result<SceneInfo>::err(ErrorCode::Busy, "not run");
    {
        ParkedWorker parked(worker);
        std::thread installer([&] { result = f.repo->install(zip, accept_all); });
        // The install's first job waits behind the parked one; queue
        // another behind it.
        ASSERT_TRUE(wait_for_pending(worker, 2));
        ASSERT_TRUE(worker.submit([&] {
            installed_when_ran = on_disk(f.root() / "todo" / "scene.json");
            ran = true;
        }));
        parked.release.give();
        installer.join();
    }
    ASSERT_TRUE(result) << result.error().message;
    EXPECT_TRUE(ran.load());
    EXPECT_FALSE(installed_when_ran.load());
    EXPECT_EQ(read_raw(f.root() / "todo" / "assets" / "a.bin"),
              std::string(wisp::core::kIoChunk + 100, 'a'));
    EXPECT_FALSE(on_disk(f.root() / ".todo.tmpdir"));
    worker.stop();
}

// An export reads on the flash thread in small jobs too: a job queued
// after it starts runs before its later reads.
TEST(SceneRepository, ExportLetsOtherFlashWorkInterleave) {
    wisp::core::StorageWorker worker;
    Fixture f(false, "/scenes", &worker);
    ASSERT_TRUE(worker.start());
    ASSERT_TRUE(f.install(multi_file_package()));

    Result<wisp::os::LargeBuffer> exported =
        Result<wisp::os::LargeBuffer>::err(ErrorCode::Busy, "not run");
    {
        ParkedWorker parked(worker);
        std::thread exporter([&] { exported = f.repo->export_zip("todo", false); });
        ASSERT_TRUE(wait_for_pending(worker, 2));
        // Runs after the export's first job and before its reads, so the
        // export sees this content.
        ASSERT_TRUE(worker.submit([&] {
            EXPECT_TRUE(f.storage->write_atomic((f.root() / "todo" / "assets" / "b.bin").string(),
                                                as_bytes("changed")));
        }));
        parked.release.give();
        exporter.join();
    }
    ASSERT_TRUE(exported) << exported.error().message;
    auto reader = ZipReader::open(exported.value().span(),
                                  {.max_entries = 256, .max_total = 4 * 1024 * 1024});
    ASSERT_TRUE(reader);
    std::string b;
    std::string a;
    for (const auto& e : reader.value().entries()) {
        auto body = reader.value().read(e);
        ASSERT_TRUE(body);
        std::string text(reinterpret_cast<const char*>(body.value().data()), body.value().size());
        if (e.path == "assets/b.bin") b = text;
        if (e.path == "assets/a.bin") a = text;
    }
    EXPECT_EQ(b, "changed");
    EXPECT_EQ(a, std::string(wisp::core::kIoChunk + 100, 'a'));
    worker.stop();
}

// A replace that lands between an export's reads fails the export rather
// than mixing two versions.
TEST(SceneRepository, ExportFailsWhenThePackageIsReplacedMidway) {
    wisp::core::StorageWorker worker;
    Fixture f(false, "/scenes", &worker);
    ASSERT_TRUE(worker.start());
    ASSERT_TRUE(f.install(multi_file_package()));
    std::vector<uint8_t> v2 = zip_of(todo_files("2.0.0"));

    Result<wisp::os::LargeBuffer> exported =
        Result<wisp::os::LargeBuffer>::err(ErrorCode::IoError, "not run");
    {
        ParkedWorker parked(worker);
        std::thread exporter([&] { exported = f.repo->export_zip("todo", false); });
        ASSERT_TRUE(wait_for_pending(worker, 2));
        // A whole reinstall on the worker itself: it runs inline there.
        ASSERT_TRUE(worker.submit([&] { EXPECT_TRUE(f.repo->install(v2, accept_all)); }));
        parked.release.give();
        exporter.join();
    }
    ASSERT_FALSE(exported);
    EXPECT_EQ(exported.error().code, ErrorCode::Busy);
    worker.stop();
}

// The concurrency test above, with the file work on a flash thread.
TEST(SceneRepository, ConcurrentCallsThroughTheWorkerAreSerialized) {
    wisp::core::StorageWorker worker;
    Fixture fx(false, "/scenes", &worker);
    ASSERT_TRUE(worker.start());
    ASSERT_TRUE(fx.install(todo_files("1.0.0")));
    std::atomic<int> failures{0};
    std::atomic<int> installed{0};
    auto installer = [&](const char* version) {
        for (int i = 0; i < 10; ++i) {
            auto r = fx.install(todo_files(version));
            if (r) {
                ++installed;
            } else if (r.error().code != ErrorCode::Busy) {
                ++failures;
            }
        }
    };
    auto reader = [&] {
        for (int i = 0; i < 20; ++i) {
            if (!fx.repo->load("todo")) ++failures;
            auto listed = fx.repo->list();
            if (!listed || listed.value().size() != 1) ++failures;
            auto exported = fx.repo->export_zip("todo", false);
            if (!exported && exported.error().code != ErrorCode::Busy) ++failures;
        }
    };
    std::thread a(installer, "1.0.1");
    std::thread b(installer, "1.0.2");
    std::thread c(reader);
    a.join();
    b.join();
    c.join();
    EXPECT_EQ(failures.load(), 0);
    EXPECT_GT(installed.load(), 0);
    worker.stop();
}

namespace {

Result<SceneInfo> install_keeping_prev(Fixture& f, const Files& files) {
    std::vector<uint8_t> zip = zip_of(files);
    return f.repo->install(zip, accept_all, {}, {.keep_prev = true});
}

}  // namespace

TEST(SceneRepositoryTest, KeepPrevMovesOldPackageAside) {
    Fixture f;
    ASSERT_TRUE(f.install(todo_files("1.0.0")));
    ASSERT_TRUE(f.storage->write_atomic(f.repo->data_dir("todo") + "/kv.log", as_bytes("kv\n")));
    EXPECT_FALSE(f.repo->has_prev("todo"));

    auto r = install_keeping_prev(f, todo_files("2.0.0"));
    ASSERT_TRUE(r) << r.error().message;
    EXPECT_EQ(r.value().manifest.version, "2.0.0");
    EXPECT_TRUE(f.repo->has_prev("todo"));
    EXPECT_EQ(read_raw(f.root() / ".prev" / "todo" / "scene.json"), scene_json("1.0.0"));
    EXPECT_EQ(read_raw(f.root() / ".prev" / "todo" / "lib" / "util.lua"), "return {}\n");
    // The data stays with the installed package, never in .prev.
    EXPECT_EQ(read_raw(f.root() / "todo" / "data" / "kv.log"), "kv\n");
    EXPECT_FALSE(on_disk(f.root() / ".prev" / "todo" / "data"));
    EXPECT_EQ(f.repo->info("todo").value().manifest.version, "2.0.0");

    // Without keep_prev an install leaves .prev alone.
    ASSERT_TRUE(f.install(todo_files("3.0.0")));
    EXPECT_EQ(read_raw(f.root() / ".prev" / "todo" / "scene.json"), scene_json("1.0.0"));
}

TEST(SceneRepositoryTest, PrevReplacedOnEachInstall) {
    Fixture f;
    ASSERT_TRUE(f.install(todo_files("1.0.0")));
    ASSERT_TRUE(install_keeping_prev(f, todo_files("2.0.0")));
    ASSERT_TRUE(install_keeping_prev(f, todo_files("3.0.0")));
    EXPECT_EQ(read_raw(f.root() / ".prev" / "todo" / "scene.json"), scene_json("2.0.0"));
    EXPECT_EQ(f.repo->info("todo").value().manifest.version, "3.0.0");
    // A first install has nothing to keep.
    Fixture g;
    ASSERT_TRUE(install_keeping_prev(g, todo_files("1.0.0")));
    EXPECT_FALSE(g.repo->has_prev("todo"));
}

TEST(SceneRepositoryTest, RemoveDeletesPrev) {
    Fixture f;
    ASSERT_TRUE(f.install(todo_files("1.0.0")));
    ASSERT_TRUE(install_keeping_prev(f, todo_files("2.0.0")));
    ASSERT_TRUE(f.repo->remove("todo", false));
    EXPECT_FALSE(f.repo->has_prev("todo"));
    EXPECT_FALSE(on_disk(f.root() / ".prev" / "todo"));

    ASSERT_TRUE(f.install(todo_files("1.0.0")));
    ASSERT_TRUE(install_keeping_prev(f, todo_files("2.0.0")));
    ASSERT_TRUE(f.repo->remove("todo", true));
    EXPECT_FALSE(on_disk(f.root() / ".prev" / "todo"));
}

TEST(SceneRepositoryTest, ListSkipsPrevAndDrafts) {
    Fixture f;
    ASSERT_TRUE(f.install(todo_files("1.0.0")));
    ASSERT_TRUE(install_keeping_prev(f, todo_files("2.0.0")));
    lay_out(f.root() / ".drafts" / "todo", todo_files("3.0.0"));
    auto list = f.repo->list();
    ASSERT_TRUE(list);
    ASSERT_EQ(list.value().size(), 1u);
    EXPECT_EQ(list.value()[0].manifest.version, "2.0.0");
    // Neither does a boot's recovery touch them.
    ASSERT_TRUE(f.repo->recover());
    EXPECT_TRUE(f.repo->has_prev("todo"));
    EXPECT_TRUE(on_disk(f.root() / ".drafts" / "todo" / "scene.json"));
}

TEST(SceneRepositoryTest, PrevZipReinstallsTheOldVersion) {
    Fixture f;
    EXPECT_EQ(f.repo->prev_zip("todo").error().code, ErrorCode::NotFound);
    ASSERT_TRUE(f.install(todo_files("1.0.0")));
    EXPECT_EQ(f.repo->prev_zip("todo").error().code, ErrorCode::NotFound);
    ASSERT_TRUE(install_keeping_prev(f, todo_files("2.0.0")));

    auto zip = f.repo->prev_zip("todo");
    ASSERT_TRUE(zip) << zip.error().message;
    EXPECT_EQ(zip_paths(zip.value()), (std::vector<std::string>{"assets/icon.png", "lib/util.lua",
                                                                "main.lua", "scene.json"}));
    // Reinstalling it keeping the previous one swaps the two.
    auto back = f.repo->install(zip.value().span(), accept_all, {}, {.keep_prev = true});
    ASSERT_TRUE(back) << back.error().message;
    EXPECT_EQ(back.value().manifest.version, "1.0.0");
    EXPECT_EQ(read_raw(f.root() / ".prev" / "todo" / "scene.json"), scene_json("2.0.0"));
}

TEST(SceneRepositoryTest, PrevWorksThroughTheWorker) {
    wisp::core::StorageWorker worker;
    Fixture f(false, "/scenes", &worker);
    ASSERT_TRUE(worker.start());
    ASSERT_TRUE(f.install(todo_files("1.0.0")));
    ASSERT_TRUE(install_keeping_prev(f, todo_files("2.0.0")));
    EXPECT_TRUE(f.repo->has_prev("todo"));
    auto zip = f.repo->prev_zip("todo");
    ASSERT_TRUE(zip) << zip.error().message;
    ASSERT_TRUE(f.repo->remove("todo", false));
    EXPECT_FALSE(f.repo->has_prev("todo"));
    worker.stop();
}

TEST(SceneRepositoryTest, ValidPackagePath) {
    using wisp::core::valid_package_path;
    for (const char* ok : {"scene.json", "main.lua", "other.lua", "lib/u.lua", "lib/a/b.lua",
                           "assets/a.png", "assets/x/y.bin"})
        EXPECT_TRUE(valid_package_path(ok)) << ok;
    for (const char* bad : {"", "../x", "/x", "data/k", "files/k", "x.txt", "lib/u.txt", "lib/.lua",
                            "assets/", "assets/../x", "assets//a", "lib/a.tmp/b.lua",
                            "assets/a.new", "a/main.lua", ".lua", "assets/a\\b", "assets/./a"})
        EXPECT_FALSE(valid_package_path(bad)) << bad;
}

// read_asset: a file under assets/ only, whole, and refused past the limit.
TEST(SceneRepository, ReadAssetUnderAssetsOnly) {
    Fixture fx;
    ASSERT_TRUE(fx.install(todo_files()));
    auto icon = fx.repo->read_asset("todo", "assets/icon.png", 64);
    ASSERT_TRUE(icon) << icon.error().message;
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(icon.value().data()), icon.value().size()),
              std::string("\x89PNG", 4));
    // Exactly the limit is fine; one byte less is too large.
    EXPECT_TRUE(fx.repo->read_asset("todo", "assets/icon.png", 4));
    auto big = fx.repo->read_asset("todo", "assets/icon.png", 3);
    ASSERT_FALSE(big);
    EXPECT_EQ(big.error().code, ErrorCode::QuotaExceeded);
    EXPECT_EQ(big.error().message, "too_large");

    for (const char* path : {"scene.json", "main.lua", "assets/../scene.json", "../todo/scene.json",
                             "/assets/icon.png", "assets", ""}) {
        auto r = fx.repo->read_asset("todo", path, 64);
        ASSERT_FALSE(r) << path;
        EXPECT_EQ(r.error().code, ErrorCode::InvalidArgument) << path;
    }
    EXPECT_EQ(fx.repo->read_asset("Bad", "assets/icon.png", 64).error().code,
              ErrorCode::InvalidArgument);
    EXPECT_EQ(fx.repo->read_asset("todo", "assets/none.png", 64).error().code, ErrorCode::NotFound);
    EXPECT_EQ(fx.repo->read_asset("other", "assets/icon.png", 64).error().code,
              ErrorCode::NotFound);
}
