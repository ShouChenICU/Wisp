// core::SceneFiles: each scene's private files (scene-device spec 7.1, 7.2).

#include "wisp/core/scene_files.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <system_error>
#include <thread>
#include <type_traits>
#include <vector>

#include "support/fake_devices.hpp"
#include "support/temp_dir.hpp"
#include "wisp/base/error.hpp"
#include "wisp/core/device_registry.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/core/log.hpp"
#include "wisp/core/storage_manager.hpp"

using wisp::ErrorCode;
using wisp::core::FilesLocation;
using wisp::core::FilesWant;
using wisp::core::SceneFiles;
using wisp::core::SceneFilesConfig;
using wisp::core::valid_file_name;

namespace fs = std::filesystem;

namespace {

std::span<const uint8_t> as_bytes(const std::string& s) {
    return {reinterpret_cast<const uint8_t*>(s.data()), s.size()};
}

std::string text(const wisp::os::LargeBuffer& b) {
    return {reinterpret_cast<const char*>(b.data()), b.size()};
}

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

// Sum of the regular files directly in `dir`.
uint64_t disk_bytes(const fs::path& dir) {
    uint64_t n = 0;
    std::error_code ec;
    for (const auto& e : fs::directory_iterator(dir, ec))
        if (e.is_regular_file(ec)) n += e.file_size(ec);
    return n;
}

// Collects the "scene_files" log lines while it lives.
struct LogTap : wisp::LogSink {
    LogTap() { wisp::log::add_sink(this); }
    ~LogTap() override { wisp::log::remove_sink(this); }
    LogTap(const LogTap&) = delete;
    LogTap& operator=(const LogTap&) = delete;
    void write(const wisp::LogRecord& rec) override {
        std::lock_guard<std::mutex> lock(mu);
        lines.push_back(std::string(rec.tag) + "|" + rec.msg);
    }
    // Lines containing every one of `parts`.
    size_t count(std::initializer_list<std::string_view> parts) {
        std::lock_guard<std::mutex> lock(mu);
        size_t n = 0;
        for (const std::string& l : lines) {
            bool all = true;
            for (std::string_view p : parts) all = all && l.find(p) != std::string::npos;
            n += all ? 1 : 0;
        }
        return n;
    }
    std::mutex mu;
    std::vector<std::string> lines;
};

// The SceneFiles error code a failed call carries.
template <class T>
std::string code_of(const wisp::Result<T>& r) {
    return r ? std::string("ok") : std::string(wisp::core::scene_files_code(r.error()));
}

struct Fixture {
    explicit Fixture(bool sd_online, SceneFilesConfig config = {})
        : in(wisp::test::temp_dir().string()), sd(wisp::test::temp_dir().string()) {
        int_dev = std::make_shared<wisp::test::FakeStorage>("storage.int", in, false);
        sd_dev = std::make_shared<wisp::test::FakeStorage>("storage.sd", sd, true);
        sd_dev->online_flag = sd_online;
        registry.add(int_dev);
        registry.add(sd_dev);
        storage = std::make_unique<wisp::core::StorageManager>(registry, bus);
        EXPECT_TRUE(storage->init());
        config.data_root = in + "/scenes";
        // Scenes not in `declared` are not installed.
        config.declared = [this](std::string_view id) -> wisp::Result<FilesWant> {
            std::lock_guard<std::mutex> lock(declared_mu);
            auto it = declared.find(std::string(id));
            if (it == declared.end())
                return wisp::Result<FilesWant>::err(ErrorCode::NotFound, "not installed");
            return wisp::Result<FilesWant>::ok(it->second);
        };
        cfg = config;
        files = std::make_unique<SceneFiles>(*storage, cfg);
    }

    void set_sd(bool online) {
        sd_dev->online_flag = online;
        storage->refresh();
    }

    // A fresh SceneFiles over the same disks: a reboot.
    void reboot() { files = std::make_unique<SceneFiles>(*storage, cfg); }

    fs::path int_files(const std::string& id) const {
        return fs::path(in) / "scenes" / id / "files";
    }
    fs::path sd_files(const std::string& id) const {
        return fs::path(sd) / "wisp" / "library" / "scenes" / id / "files";
    }
    fs::path record(const std::string& id) const {
        return fs::path(in) / "scenes" / id / "data" / "files_loc.json";
    }

    std::string in;
    std::string sd;
    wisp::core::DeviceRegistry registry;
    wisp::core::EventBus bus;
    std::shared_ptr<wisp::test::FakeStorage> int_dev;
    std::shared_ptr<wisp::test::FakeStorage> sd_dev;
    std::unique_ptr<wisp::core::StorageManager> storage;
    SceneFilesConfig cfg;
    std::mutex declared_mu;
    std::map<std::string, FilesWant> declared;
    std::unique_ptr<SceneFiles> files;
};

}  // namespace

TEST(SceneFiles, NameRules) {
    EXPECT_TRUE(valid_file_name("a.txt"));
    EXPECT_TRUE(valid_file_name("A-b_9.bin"));
    EXPECT_TRUE(valid_file_name(std::string(64, 'x')));
    EXPECT_FALSE(valid_file_name(""));
    EXPECT_FALSE(valid_file_name(".x"));
    EXPECT_FALSE(valid_file_name("a/b"));
    EXPECT_FALSE(valid_file_name("\xc3\xa9"));
    EXPECT_FALSE(valid_file_name(std::string(65, 'x')));
    // StorageManager's own bookkeeping suffixes.
    EXPECT_FALSE(valid_file_name("a.tmp"));
    EXPECT_FALSE(valid_file_name("a.new"));
    // In any case: on FAT "x.TMP" is "x.tmp".
    EXPECT_FALSE(valid_file_name("x.TMP"));
    EXPECT_FALSE(valid_file_name("x.New"));
    EXPECT_FALSE(valid_file_name("x.tMp"));
    EXPECT_TRUE(valid_file_name("x.tmpl"));

    Fixture f(false);
    EXPECT_EQ(code_of(f.files->write("s", "x.TMP", as_bytes("x"))), "bad_name");
    EXPECT_EQ(code_of(f.files->write("s", "../x", as_bytes("x"))), "bad_name");
    EXPECT_EQ(code_of(f.files->read("s", ".x", 0, 1)), "bad_name");
}

TEST(SceneFiles, LocationChosenOnFirstWriteAndRemembered) {
    Fixture f(/*sd_online=*/true);
    auto none = f.files->info("a");
    ASSERT_TRUE(none);
    EXPECT_EQ(none.value().location, FilesLocation::None);

    ASSERT_TRUE(f.files->write("a", "x.txt", as_bytes("hi")));
    EXPECT_EQ(read_raw(f.sd_files("a") / "x.txt"), "hi");
    EXPECT_NE(read_raw(f.record("a")).find("\"sd\""), std::string::npos);
    EXPECT_EQ(f.files->info("a").value().location, FilesLocation::Sd);
    EXPECT_EQ(f.files->info("a").value().quota, 64ull * 1024 * 1024);

    f.set_sd(false);
    f.files->on_sd_changed();
    EXPECT_EQ(code_of(f.files->write("a", "y.txt", as_bytes("y"))), "unavailable");
    EXPECT_EQ(code_of(f.files->read("a", "x.txt", 0, 2)), "unavailable");
    EXPECT_EQ(code_of(f.files->list("a")), "unavailable");
    EXPECT_EQ(code_of(f.files->info("a")), "unavailable");

    // Without the card a scene starts on internal storage and stays there.
    ASSERT_TRUE(f.files->write("b", "x.txt", as_bytes("int")));
    EXPECT_EQ(read_raw(f.int_files("b") / "x.txt"), "int");
    EXPECT_NE(read_raw(f.record("b")).find("\"int\""), std::string::npos);
    f.set_sd(true);
    f.reboot();
    ASSERT_TRUE(f.files->scan());
    EXPECT_EQ(f.files->info("b").value().location, FilesLocation::Internal);
    EXPECT_EQ(f.files->info("b").value().quota, 512u * 1024);
    ASSERT_TRUE(f.files->write("b", "z.txt", as_bytes("z")));
    EXPECT_TRUE(on_disk(f.int_files("b") / "z.txt"));
    EXPECT_FALSE(on_disk(f.sd_files("b")));
    // The card is back: "a" works again.
    EXPECT_EQ(text(f.files->read("a", "x.txt", 0, 10).value()), "hi");
}

TEST(SceneFiles, InternalQuota512KiB) {
    Fixture f(false);
    const std::string half(256 * 1024, 'q');
    ASSERT_TRUE(f.files->write("s", "a.bin", as_bytes(half)));
    ASSERT_TRUE(f.files->write("s", "b.bin", as_bytes(half)));
    EXPECT_EQ(f.files->info("s").value().used, 512u * 1024);
    EXPECT_EQ(code_of(f.files->append("s", "a.bin", as_bytes("1"))), "quota_exceeded");
    EXPECT_EQ(code_of(f.files->write("s", "c.bin", as_bytes("1"))), "quota_exceeded");
    EXPECT_EQ(fs::file_size(f.int_files("s") / "a.bin"), half.size());
    EXPECT_FALSE(on_disk(f.int_files("s") / "c.bin"));
    // Overwriting counts the file it replaces.
    ASSERT_TRUE(f.files->write("s", "a.bin", as_bytes(std::string(1000, 'r'))));
    EXPECT_EQ(f.files->info("s").value().used, 256u * 1024 + 1000);
}

TEST(SceneFiles, SdQuotaFromConfig) {
    SceneFilesConfig config;
    config.sd_quota = 1000;
    Fixture f(true, config);
    ASSERT_TRUE(f.files->write("s", "a", as_bytes(std::string(600, 'a'))));
    EXPECT_EQ(code_of(f.files->write("s", "b", as_bytes(std::string(500, 'b')))), "quota_exceeded");
    ASSERT_TRUE(f.files->write("s", "a", as_bytes(std::string(900, 'a'))));
    auto info = f.files->info("s");
    ASSERT_TRUE(info);
    EXPECT_EQ(info.value().quota, 1000u);
    EXPECT_EQ(info.value().used, 900u);
}

TEST(SceneFiles, WriteIsAtomicAppendExtends) {
    Fixture f(false);
    ASSERT_TRUE(f.files->write("s", "log.txt", as_bytes("abc")));
    ASSERT_TRUE(f.files->append("s", "log.txt", as_bytes("def")));
    EXPECT_EQ(text(f.files->read("s", "log.txt", 0, 100).value()), "abcdef");
    // append creates a missing file.
    ASSERT_TRUE(f.files->append("s", "new.txt", as_bytes("n")));
    EXPECT_EQ(text(f.files->read("s", "new.txt", 0, 100).value()), "n");
    ASSERT_TRUE(f.files->write("s", "log.txt", as_bytes("x")));
    EXPECT_EQ(text(f.files->read("s", "log.txt", 0, 100).value()), "x");
    EXPECT_FALSE(on_disk(f.int_files("s") / "log.txt.tmp"));
    EXPECT_FALSE(on_disk(f.int_files("s") / "log.txt.new"));
    EXPECT_EQ(f.files->info("s").value().used, 2u);
}

TEST(SceneFiles, ReadRangeAndLimits) {
    Fixture f(false);
    ASSERT_TRUE(f.files->write("s", "d.bin", as_bytes("0123456789")));
    EXPECT_EQ(code_of(f.files->read("s", "d.bin", 0, 65537)), "too_large");
    auto ok = f.files->read("s", "d.bin", 0, 65536);
    ASSERT_TRUE(ok);
    EXPECT_EQ(text(ok.value()), "0123456789");
    EXPECT_EQ(text(f.files->read("s", "d.bin", 7, 100).value()), "789");
    EXPECT_EQ(f.files->read("s", "d.bin", 100, 5).value().size(), 0u);
    EXPECT_EQ(code_of(f.files->read("s", "none", 0, 1)), "not_found");

    const std::string over(256 * 1024 + 1, 'o');
    EXPECT_EQ(code_of(f.files->write("s", "big", as_bytes(over))), "too_large");
    EXPECT_EQ(code_of(f.files->append("s", "big", as_bytes(over))), "too_large");
    EXPECT_FALSE(on_disk(f.int_files("s") / "big"));
}

// A full list holds up to max_list names; past 15 bytes a std::string
// would put each one in internal RAM on the device (AGENTS.md section 8).
static_assert(std::is_same_v<decltype(wisp::core::SceneFileEntry::name), wisp::os::LargeString>,
              "listed file names live in PSRAM");

TEST(SceneFiles, ListRemoveInfo) {
    Fixture f(false);
    auto empty = f.files->list("s");
    ASSERT_TRUE(empty);
    EXPECT_TRUE(empty.value().entries.empty());
    EXPECT_EQ(code_of(f.files->remove("s", "a.txt")), "not_found");

    ASSERT_TRUE(f.files->write("s", "b.txt", as_bytes("bb")));
    ASSERT_TRUE(f.files->write("s", "a.txt", as_bytes("a")));
    // Download bookkeeping is never listed.
    write_raw(f.int_files("s") / ".c.dl", "partial");
    auto listed = f.files->list("s");
    ASSERT_TRUE(listed);
    ASSERT_EQ(listed.value().entries.size(), 2u);
    EXPECT_EQ(listed.value().entries[0].name, "a.txt");
    EXPECT_EQ(listed.value().entries[0].size, 1u);
    EXPECT_GT(listed.value().entries[0].mtime, 0);
    EXPECT_EQ(listed.value().entries[1].name, "b.txt");
    EXPECT_EQ(listed.value().entries[1].size, 2u);

    ASSERT_TRUE(f.files->remove("s", "a.txt"));
    EXPECT_FALSE(on_disk(f.int_files("s") / "a.txt"));
    EXPECT_EQ(code_of(f.files->remove("s", "a.txt")), "not_found");
    EXPECT_EQ(f.files->list("s").value().entries.size(), 1u);

    auto info = f.files->info("s");
    ASSERT_TRUE(info);
    EXPECT_EQ(info.value().location, FilesLocation::Internal);
    EXPECT_EQ(info.value().quota, 512u * 1024);
    EXPECT_EQ(info.value().used, 2u);
    EXPECT_STREQ(wisp::core::to_string(FilesLocation::Internal), "int");
    EXPECT_STREQ(wisp::core::to_string(FilesLocation::Sd), "sd");
    EXPECT_STREQ(wisp::core::to_string(FilesLocation::None), "none");
}

// One file's entry, for a download that announces its length; on the card
// the spelling on disk.
TEST(SceneFiles, StatOneFile) {
    Fixture f(false);
    EXPECT_EQ(code_of(f.files->stat("s", "a.txt")), "not_found");
    ASSERT_TRUE(f.files->write("s", "a.txt", as_bytes("abc")));
    auto st = f.files->stat("s", "a.txt");
    ASSERT_TRUE(st);
    EXPECT_EQ(st.value().name, "a.txt");
    EXPECT_EQ(st.value().size, 3u);
    EXPECT_GT(st.value().mtime, 0);
    EXPECT_EQ(code_of(f.files->stat("s", "b.txt")), "not_found");
    EXPECT_EQ(code_of(f.files->stat("s", ".a.txt")), "bad_name");
    // Bookkeeping is never a file of the scene's.
    write_raw(f.int_files("s") / ".c.dl", "partial");
    EXPECT_EQ(code_of(f.files->stat("s", ".c.dl")), "bad_name");

    Fixture card(true);
    ASSERT_TRUE(card.files->write("c", "Song.MP3", as_bytes("12345")));
    auto folded = card.files->stat("c", "song.mp3");
    ASSERT_TRUE(folded);
    EXPECT_EQ(folded.value().name, "Song.MP3");
    EXPECT_EQ(folded.value().size, 5u);
    card.set_sd(false);
    EXPECT_EQ(code_of(card.files->stat("c", "Song.MP3")), "unavailable");
}

// Two-phase naming of downloads (".<name>.dl" -> ".<name>.done" -> name):
// at boot a ".dl" is garbage and a ".done" is a finished download.
TEST(SceneFiles, UsageScannedAtBootAndRecoversDownloads) {
    Fixture f(false);
    ASSERT_TRUE(f.files->write("s", "keep.txt", as_bytes("12345")));
    write_raw(f.int_files("s") / ".x.dl", "half a download");
    write_raw(f.int_files("s") / ".y.done", "1234567");
    // Cut off after the ".done" rename, before the old file went.
    write_raw(f.int_files("s") / "z", "old");
    write_raw(f.int_files("s") / ".z.done", "newer");

    f.reboot();
    ASSERT_TRUE(f.files->scan());
    EXPECT_FALSE(on_disk(f.int_files("s") / ".x.dl"));
    EXPECT_FALSE(on_disk(f.int_files("s") / ".y.done"));
    EXPECT_EQ(read_raw(f.int_files("s") / "y"), "1234567");
    EXPECT_FALSE(on_disk(f.int_files("s") / ".z.done"));
    EXPECT_EQ(read_raw(f.int_files("s") / "z"), "newer");
    EXPECT_EQ(f.files->info("s").value().used, 5u + 7u + 5u);
    EXPECT_EQ(f.files->info("s").value().used, disk_bytes(f.int_files("s")));
}

TEST(SceneFiles, RemoveAllChecksBothLocations) {
    Fixture f(true);
    ASSERT_TRUE(f.files->write("r", "on_sd.txt", as_bytes("sd")));
    // Left on internal storage by an earlier install, say.
    write_raw(f.int_files("r") / "old.txt", "int");
    ASSERT_TRUE(f.files->remove_all("r"));
    EXPECT_FALSE(on_disk(f.sd_files("r")));
    EXPECT_FALSE(on_disk(f.int_files("r")));
    EXPECT_FALSE(on_disk(f.record("r")));
    EXPECT_EQ(f.files->info("r").value().location, FilesLocation::None);
    // Nothing there is not an error.
    ASSERT_TRUE(f.files->remove_all("nothing"));
}

TEST(SceneFiles, SdRemovedMidDownload) {
    Fixture f(true);
    auto begun = f.files->begin_download("d", "big.bin");
    ASSERT_TRUE(begun) << begun.error().message;
    std::shared_ptr<wisp::core::FileDownload> dl = begun.value();
    const std::string chunk(64 * 1024, 'c');
    EXPECT_TRUE(dl->push(as_bytes(chunk)));
    EXPECT_TRUE(on_disk(f.sd_files("d") / ".big.bin.dl"));
    // Not listed while it downloads.
    EXPECT_TRUE(f.files->list("d").value().entries.empty());

    f.set_sd(false);
    f.files->on_sd_changed();
    EXPECT_FALSE(dl->push(as_bytes(chunk)));
    EXPECT_EQ(dl->error(), "unavailable");
    // Once failed it stays failed.
    EXPECT_FALSE(dl->push(as_bytes("x")));
    auto fin = dl->finish(false);
    EXPECT_FALSE(fin);
    EXPECT_EQ(code_of(f.files->write("d", "after.txt", as_bytes("a"))), "unavailable");

    f.set_sd(true);
    f.files->on_sd_changed();
    ASSERT_TRUE(f.files->write("d", "after.txt", as_bytes("after")));
    EXPECT_FALSE(on_disk(f.sd_files("d") / ".big.bin.dl"));
    EXPECT_FALSE(on_disk(f.sd_files("d") / "big.bin"));
    EXPECT_EQ(f.files->info("d").value().used, 5u);
    EXPECT_EQ(f.files->info("d").value().used, disk_bytes(f.sd_files("d")));
}

// A finished download replaces its target and counts once.
TEST(SceneFiles, DownloadFinishReplacesTarget) {
    Fixture f(false);
    ASSERT_TRUE(f.files->write("d", "f.bin", as_bytes("old contents")));
    auto dl = f.files->begin_download("d", "f.bin").value();
    const std::string body(100 * 1024 + 7, 'n');
    EXPECT_TRUE(dl->push(as_bytes(body.substr(0, 50000))));
    EXPECT_TRUE(dl->push(as_bytes(body.substr(50000))));
    auto fin = dl->finish(true);
    ASSERT_TRUE(fin) << fin.error().message;
    EXPECT_EQ(fin.value(), body.size());
    EXPECT_EQ(read_raw(f.int_files("d") / "f.bin"), body);
    EXPECT_FALSE(on_disk(f.int_files("d") / ".f.bin.dl"));
    EXPECT_FALSE(on_disk(f.int_files("d") / ".f.bin.done"));
    EXPECT_EQ(f.files->info("d").value().used, body.size());

    // Over the quota: push refuses, nothing is left.
    auto big = f.files->begin_download("d", "g.bin").value();
    const std::string piece(250 * 1024, 'g');
    EXPECT_TRUE(big->push(as_bytes(piece)));
    EXPECT_FALSE(big->push(as_bytes(piece)));
    EXPECT_EQ(big->error(), "quota_exceeded");
    EXPECT_FALSE(big->finish(true));
    EXPECT_FALSE(on_disk(f.int_files("d") / "g.bin"));
    EXPECT_FALSE(on_disk(f.int_files("d") / ".g.bin.dl"));
    EXPECT_EQ(f.files->info("d").value().used, body.size());
}

// The manifest's "files" (spec 7.2): "int" ignores the card, "sd" never
// falls back to internal storage, "auto" takes the card when it is there.
TEST(SceneFiles, DeclarationPicksTheLocation) {
    Fixture f(true);
    f.declared = {{"i", FilesWant::Int}, {"s", FilesWant::Sd}, {"a", FilesWant::Auto}};
    EXPECT_EQ(f.files->info("i").value().want, FilesWant::Int);
    EXPECT_EQ(f.files->info("i").value().quota, 512u * 1024);
    ASSERT_TRUE(f.files->write("i", "x", as_bytes("int")));
    EXPECT_EQ(read_raw(f.int_files("i") / "x"), "int");
    EXPECT_FALSE(on_disk(f.sd_files("i")));
    EXPECT_EQ(f.files->info("i").value().location, FilesLocation::Internal);

    ASSERT_TRUE(f.files->write("a", "x", as_bytes("auto")));
    EXPECT_EQ(read_raw(f.sd_files("a") / "x"), "auto");
    EXPECT_EQ(f.files->info("a").value().want, FilesWant::Auto);

    // "sd" without a card: unavailable, and nothing is chosen.
    f.set_sd(false);
    f.files->on_sd_changed();
    EXPECT_EQ(code_of(f.files->write("s", "x", as_bytes("sd"))), "unavailable");
    EXPECT_EQ(code_of(f.files->append("s", "x", as_bytes("sd"))), "unavailable");
    EXPECT_EQ(code_of(f.files->begin_download("s", "x")), "unavailable");
    EXPECT_EQ(code_of(f.files->list("s")), "unavailable");
    EXPECT_EQ(code_of(f.files->info("s")), "unavailable");
    EXPECT_FALSE(on_disk(f.record("s")));
    EXPECT_FALSE(on_disk(f.int_files("s")));
    // "auto" without a card goes inside.
    f.declared["b"] = FilesWant::Auto;
    ASSERT_TRUE(f.files->write("b", "x", as_bytes("b")));
    EXPECT_TRUE(on_disk(f.int_files("b") / "x"));

    f.set_sd(true);
    f.files->on_sd_changed();
    auto info = f.files->info("s");
    ASSERT_TRUE(info);
    EXPECT_EQ(info.value().location, FilesLocation::None);
    EXPECT_EQ(info.value().want, FilesWant::Sd);
    EXPECT_EQ(info.value().quota, 64ull * 1024 * 1024);
    ASSERT_TRUE(f.files->write("s", "x", as_bytes("sd")));
    EXPECT_EQ(read_raw(f.sd_files("s") / "x"), "sd");
    EXPECT_STREQ(wisp::core::to_string(FilesWant::Sd), "sd");
}

// A recorded location outlives a changed declaration: nothing moves, and
// loading the record says so once.
TEST(SceneFiles, RecordWinsOverDeclaration) {
    Fixture f(true);
    f.declared["r"] = FilesWant::Int;
    ASSERT_TRUE(f.files->write("r", "x", as_bytes("kept")));
    EXPECT_TRUE(on_disk(f.int_files("r") / "x"));

    // An update now declares "sd".
    f.declared["r"] = FilesWant::Sd;
    LogTap tap;
    f.reboot();
    ASSERT_TRUE(f.files->scan());
    EXPECT_EQ(tap.count({"scene_files|", "r:", "\"sd\"", "kept on int"}), 1u);
    auto info = f.files->info("r");
    ASSERT_TRUE(info);
    EXPECT_EQ(info.value().location, FilesLocation::Internal);
    EXPECT_EQ(info.value().want, FilesWant::Sd);
    ASSERT_TRUE(f.files->write("r", "y", as_bytes("y")));
    EXPECT_TRUE(on_disk(f.int_files("r") / "y"));
    EXPECT_FALSE(on_disk(f.sd_files("r")));
    // Loaded once: no more warnings.
    EXPECT_EQ(tap.count({"scene_files|", "kept on int"}), 1u);
}

// Files on the card whose scene is gone are logged and never removed.
TEST(SceneFiles, OrphansOnTheCardAreLoggedAndKept) {
    Fixture f(true);
    f.declared["live"] = FilesWant::Auto;
    ASSERT_TRUE(f.files->write("live", "x", as_bytes("live")));
    write_raw(f.sd_files("ghost") / "photo.jpg", "jpeg");

    LogTap tap;
    ASSERT_TRUE(f.files->scan());
    EXPECT_EQ(tap.count({"scene_files|", "ghost/files", "no installed scene"}), 1u);
    EXPECT_EQ(tap.count({"live/files"}), 0u);
    EXPECT_EQ(read_raw(f.sd_files("ghost") / "photo.jpg"), "jpeg");

    f.set_sd(false);
    f.files->on_sd_changed();
    f.set_sd(true);
    f.files->on_sd_changed();
    EXPECT_EQ(tap.count({"scene_files|", "ghost/files", "no installed scene"}), 2u);
    EXPECT_EQ(read_raw(f.sd_files("ghost") / "photo.jpg"), "jpeg");
    EXPECT_EQ(read_raw(f.sd_files("live") / "x"), "live");
}

// A finished download never removes a directory that has its name.
TEST(SceneFiles, DoneNeverReplacesADirectory) {
    Fixture f(false);
    ASSERT_TRUE(f.files->write("d", "keep.txt", as_bytes("k")));
    write_raw(f.int_files("d") / "t" / "inner.txt", "mine");
    write_raw(f.int_files("d") / ".t.done", "finished");

    LogTap tap;
    f.reboot();
    ASSERT_TRUE(f.files->scan());
    EXPECT_EQ(read_raw(f.int_files("d") / "t" / "inner.txt"), "mine");
    EXPECT_EQ(read_raw(f.int_files("d") / ".t.done"), "finished");
    EXPECT_EQ(tap.count({"scene_files|", "is not a file"}), 1u);
    EXPECT_EQ(f.files->info("d").value().used, 1u);

    // The same when the download finishes now.
    write_raw(f.int_files("d") / "u" / "inner.txt", "also mine");
    auto dl = f.files->begin_download("d", "u").value();
    EXPECT_TRUE(dl->push(as_bytes("new body")));
    auto fin = dl->finish(true);
    ASSERT_FALSE(fin);
    EXPECT_EQ(read_raw(f.int_files("d") / "u" / "inner.txt"), "also mine");
    EXPECT_EQ(read_raw(f.int_files("d") / ".u.done"), "new body");
    EXPECT_FALSE(on_disk(f.int_files("d") / ".u.dl"));
    EXPECT_EQ(tap.count({"scene_files|", "is not a file"}), 2u);
    EXPECT_EQ(f.files->info("d").value().used, 1u);
}

// list() returns at most max_list names.
TEST(SceneFiles, ListIsBounded) {
    SceneFilesConfig config;
    config.max_list = 3;
    Fixture f(false, config);
    // Written out of order: the directory's own order is not the answer's.
    for (const char* n : {"e", "c", "a"}) ASSERT_TRUE(f.files->write("l", n, as_bytes("1")));
    auto exact = f.files->list("l");
    ASSERT_TRUE(exact);
    EXPECT_EQ(exact.value().entries.size(), 3u);
    EXPECT_FALSE(exact.value().truncated);
    for (const char* n : {"d", "b"}) ASSERT_TRUE(f.files->write("l", n, as_bytes("1")));
    auto listed = f.files->list("l");
    ASSERT_TRUE(listed);
    EXPECT_TRUE(listed.value().truncated);
    std::vector<std::string> names;
    for (const auto& e : listed.value().entries) names.emplace_back(e.name);
    EXPECT_EQ(names, (std::vector<std::string>{"a", "b", "c"}));
    EXPECT_EQ(f.files->info("l").value().used, 5u);
}

// With the worker stalled, push() really waits past kMaxQueued; draining
// the queue lets it go on, and cancel() wakes it at once.
TEST(SceneFiles, PushWaitsForTheWorkerUntilCancelled) {
    std::mutex mu;
    std::vector<std::function<void()>> stalled;
    SceneFilesConfig config;
    config.submit_io = [&](std::function<void()> job) {
        std::lock_guard<std::mutex> lock(mu);
        stalled.push_back(std::move(job));
        return true;
    };
    Fixture f(true, config);
    auto run_stalled = [&] {
        std::vector<std::function<void()>> jobs;
        {
            std::lock_guard<std::mutex> lock(mu);
            jobs.swap(stalled);
        }
        for (auto& j : jobs) j();
    };
    using wisp::core::FileDownload;
    const std::string body(FileDownload::kMaxQueued + 2 * FileDownload::kChunk, 'b');

    for (bool by_cancel : {false, true}) {
        const std::string name = by_cancel ? "c.bin" : "d.bin";
        auto dl = f.files->begin_download("w", name).value();
        std::atomic<int> pushed{-1};
        std::thread http([&] { pushed = dl->push(as_bytes(body)) ? 1 : 0; });
        // Blocked: everything it could queue is queued, and it has not
        // returned.
        const auto until = std::chrono::steady_clock::now() + std::chrono::seconds(5);
        for (;;) {
            {
                std::lock_guard<std::mutex> lock(mu);
                if (stalled.size() * FileDownload::kChunk > FileDownload::kMaxQueued) break;
            }
            ASSERT_LT(std::chrono::steady_clock::now(), until);
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(150));
        EXPECT_EQ(pushed.load(), -1);
        if (by_cancel) {
            dl->cancel();
        } else {
            run_stalled();
        }
        http.join();
        EXPECT_EQ(pushed.load(), by_cancel ? 0 : 1);
        run_stalled();
        auto fin = dl->finish(!by_cancel);
        EXPECT_EQ(static_cast<bool>(fin), !by_cancel);
        EXPECT_FALSE(on_disk(f.sd_files("w") / ("." + name + ".dl")));
    }
    EXPECT_EQ(read_raw(f.sd_files("w") / "d.bin"), body);
    EXPECT_FALSE(on_disk(f.sd_files("w") / "c.bin"));
    EXPECT_EQ(f.files->info("w").value().used, body.size());
}

// On the card (FAT) a name differing only in case is the file already
// there: every call acts on the spelling on disk. The simulator's card is
// case-sensitive, so a second spelling showing up would be a bug here.
TEST(SceneFiles, CardNamesMatchWithoutCase) {
    Fixture f(true);
    ASSERT_TRUE(f.files->write("c", "Song.MP3", as_bytes("12345")));
    ASSERT_TRUE(f.files->write("c", "song.mp3", as_bytes("abc")));
    ASSERT_TRUE(f.files->append("c", "SONG.mp3", as_bytes("d")));
    EXPECT_EQ(read_raw(f.sd_files("c") / "Song.MP3"), "abcd");
    EXPECT_FALSE(on_disk(f.sd_files("c") / "song.mp3"));
    EXPECT_FALSE(on_disk(f.sd_files("c") / "SONG.mp3"));
    EXPECT_EQ(text(f.files->read("c", "song.MP3", 0, 10).value()), "abcd");
    EXPECT_EQ(f.files->info("c").value().used, 4u);

    // A download: the in-flight check and the target follow the same rule.
    auto dl = f.files->begin_download("c", "SONG.MP3");
    ASSERT_TRUE(dl);
    EXPECT_EQ(code_of(f.files->begin_download("c", "song.mp3")), "busy");
    EXPECT_TRUE(dl.value()->push(as_bytes("new!!!")));
    ASSERT_TRUE(dl.value()->finish(true));
    EXPECT_EQ(read_raw(f.sd_files("c") / "Song.MP3"), "new!!!");
    EXPECT_FALSE(on_disk(f.sd_files("c") / "SONG.MP3"));
    EXPECT_EQ(f.files->info("c").value().used, 6u);

    ASSERT_TRUE(f.files->remove("c", "song.mp3"));
    EXPECT_FALSE(on_disk(f.sd_files("c") / "Song.MP3"));
    EXPECT_EQ(f.files->info("c").value().used, 0u);
    EXPECT_EQ(disk_bytes(f.sd_files("c")), 0u);

    // Internal storage keeps case: two files.
    f.declared["i"] = FilesWant::Int;
    ASSERT_TRUE(f.files->write("i", "A.txt", as_bytes("1")));
    ASSERT_TRUE(f.files->write("i", "a.txt", as_bytes("22")));
    EXPECT_EQ(read_raw(f.int_files("i") / "A.txt"), "1");
    EXPECT_EQ(read_raw(f.int_files("i") / "a.txt"), "22");
    EXPECT_EQ(f.files->info("i").value().used, 3u);
}

// No record, but the card holds files for the id: an installed scene that
// may use the card takes them over on its first call, even a read; one
// declaring "int" does not, and neither does an id with no scene.
TEST(SceneFiles, CardFilesAdoptedOnFirstLook) {
    Fixture f(true);
    write_raw(f.sd_files("a") / "ep1.mp3", "audio");
    write_raw(f.sd_files("i") / "x", "int?");
    write_raw(f.sd_files("ghost") / "x", "ghost");
    f.declared["a"] = FilesWant::Auto;
    f.declared["i"] = FilesWant::Int;

    EXPECT_EQ(text(f.files->read("a", "ep1.mp3", 0, 10).value()), "audio");
    EXPECT_NE(read_raw(f.record("a")).find("\"sd\""), std::string::npos);
    EXPECT_EQ(f.files->info("a").value().location, FilesLocation::Sd);
    EXPECT_EQ(f.files->info("a").value().used, 5u);

    EXPECT_EQ(f.files->info("i").value().location, FilesLocation::None);
    EXPECT_FALSE(on_disk(f.record("i")));

    ASSERT_TRUE(f.files->scan());
    EXPECT_EQ(f.files->info("ghost").value().location, FilesLocation::None);
    EXPECT_FALSE(on_disk(f.record("ghost")));
    EXPECT_EQ(read_raw(f.sd_files("ghost") / "x"), "ghost");
}

// The agent's own files (agent-tools spec 4.2): the "@agent" id, which no
// scene id can be, lives in <Library>/agent/files with a scene's quota.
TEST(SceneFiles, AgentAreaInTheLibrary) {
    Fixture f(/*sd_online=*/false);
    ASSERT_TRUE(f.files->write(wisp::core::kAgentFilesId, "a.txt", as_bytes("hi")));
    EXPECT_EQ(read_raw(fs::path(f.in) / "library" / "agent" / "files" / "a.txt"), "hi");
    EXPECT_FALSE(on_disk(fs::path(f.in) / "scenes" / "@agent"));
    auto info = f.files->info(wisp::core::kAgentFilesId);
    ASSERT_TRUE(info);
    EXPECT_EQ(info.value().location, FilesLocation::Internal);
    EXPECT_EQ(info.value().quota, 512u * 1024);
    EXPECT_EQ(text(f.files->read(wisp::core::kAgentFilesId, "a.txt", 0, 10).value()), "hi");

    // With a card, a first write goes to the card's library.
    Fixture g(/*sd_online=*/true);
    ASSERT_TRUE(g.files->write(wisp::core::kAgentFilesId, "b.txt", as_bytes("yo")));
    EXPECT_EQ(read_raw(fs::path(g.sd) / "wisp" / "library" / "agent" / "files" / "b.txt"), "yo");
    EXPECT_EQ(g.files->info(wisp::core::kAgentFilesId).value().location, FilesLocation::Sd);

    // Other ids outside the scene id rules stay refused.
    EXPECT_FALSE(f.files->write("@other", "a.txt", as_bytes("x")));
    EXPECT_FALSE(f.files->write("agent/..", "a.txt", as_bytes("x")));
}
