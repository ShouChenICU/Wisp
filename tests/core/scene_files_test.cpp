// core::SceneFiles: each scene's private files (scene-device spec 7.1, 7.2).

#include "wisp/core/scene_files.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <system_error>
#include <vector>

#include "support/fake_devices.hpp"
#include "support/temp_dir.hpp"
#include "wisp/base/error.hpp"
#include "wisp/core/device_registry.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/core/storage_manager.hpp"

using wisp::ErrorCode;
using wisp::core::FilesLocation;
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

    Fixture f(false);
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

TEST(SceneFiles, ListRemoveInfo) {
    Fixture f(false);
    auto empty = f.files->list("s");
    ASSERT_TRUE(empty);
    EXPECT_TRUE(empty.value().empty());
    EXPECT_EQ(code_of(f.files->remove("s", "a.txt")), "not_found");

    ASSERT_TRUE(f.files->write("s", "b.txt", as_bytes("bb")));
    ASSERT_TRUE(f.files->write("s", "a.txt", as_bytes("a")));
    // Download bookkeeping is never listed.
    write_raw(f.int_files("s") / ".c.dl", "partial");
    auto listed = f.files->list("s");
    ASSERT_TRUE(listed);
    ASSERT_EQ(listed.value().size(), 2u);
    EXPECT_EQ(listed.value()[0].name, "a.txt");
    EXPECT_EQ(listed.value()[0].size, 1u);
    EXPECT_GT(listed.value()[0].mtime, 0);
    EXPECT_EQ(listed.value()[1].name, "b.txt");
    EXPECT_EQ(listed.value()[1].size, 2u);

    ASSERT_TRUE(f.files->remove("s", "a.txt"));
    EXPECT_FALSE(on_disk(f.int_files("s") / "a.txt"));
    EXPECT_EQ(code_of(f.files->remove("s", "a.txt")), "not_found");
    EXPECT_EQ(f.files->list("s").value().size(), 1u);

    auto info = f.files->info("s");
    ASSERT_TRUE(info);
    EXPECT_EQ(info.value().location, FilesLocation::Internal);
    EXPECT_EQ(info.value().quota, 512u * 1024);
    EXPECT_EQ(info.value().used, 2u);
    EXPECT_STREQ(wisp::core::to_string(FilesLocation::Internal), "int");
    EXPECT_STREQ(wisp::core::to_string(FilesLocation::Sd), "sd");
    EXPECT_STREQ(wisp::core::to_string(FilesLocation::None), "none");
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
    EXPECT_TRUE(f.files->list("d").value().empty());

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
