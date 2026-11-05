#include "wisp/core/scene_repository.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
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
#include "wisp/core/storage_manager.hpp"
#include "wisp/core/zip.hpp"

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
    explicit Fixture(bool dev = false, const std::string& data_root = "/scenes")
        : in(wisp::test::temp_dir().string()) {
        registry.add(std::make_shared<wisp::test::FakeStorage>("storage.int", in, false));
        storage = std::make_unique<wisp::core::StorageManager>(registry, bus);
        EXPECT_TRUE(storage->init());
        SceneRepoConfig config;
        config.code_root = in + "/scenes";
        config.data_root = in + data_root;
        config.dev = dev;
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

TEST(SceneRepository, RejectsDeeplyNestedManifest) {
    Fixture f;
    Files files = todo_files();
    files[0].second.insert(files[0].second.size() - 1, R"(,"x":[[[[[[[[[1]]]]]]]]])");
    auto r = f.install(files);
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(r.error().message, "scene.json: nested deeper than 4 levels");
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
