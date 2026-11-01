#include "wisp/core/storage_manager.hpp"

#include <gtest/gtest.h>
#include <sys/stat.h>
#include <unistd.h>

#include <cstdint>
#include <cstdio>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "support/fake_devices.hpp"
#include "support/temp_dir.hpp"
#include "wisp/base/error.hpp"
#include "wisp/core/device_registry.hpp"
#include "wisp/core/event_bus.hpp"

using wisp::core::Area;
using wisp::core::DeviceRegistry;
using wisp::core::Event;
using wisp::core::EventBus;
using wisp::core::StorageConfig;
using wisp::core::StorageManager;
using wisp::test::FakeStorage;

namespace {

std::vector<uint8_t> bytes(const std::string& s) {
    return std::vector<uint8_t>(s.begin(), s.end());
}

std::string str(const std::vector<uint8_t>& b) { return std::string(b.begin(), b.end()); }

bool exists(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return f.good();
}

void write_raw(const std::string& path, const std::string& contents) {
    std::ofstream f(path, std::ios::binary | std::ios::trunc);
    f << contents;
}

// registry has two FakeStorage devices, "storage.int" (always online, not
// removable, mount point `in`) and "storage.sd" (removable, mount point
// `sd`, online() reflecting sd_dev->online_flag -- starts at `sd_online`).
// bus is started and subscribed to every topic, collecting them (in
// delivery order) into `topics`. `topics` is only written by the bus's
// dispatcher thread and is read by tests only after bus.wait_idle()
// confirms that thread is idle, so no extra locking is needed here -- same
// pattern as button_service_test.cpp's Fixture.
struct Fixture {
    explicit Fixture(bool sd_online, StorageConfig config = {})
        : in(wisp::test::temp_dir().string()), sd(wisp::test::temp_dir().string()) {
        int_dev = std::make_shared<FakeStorage>("storage.int", in, /*removable=*/false);
        sd_dev = std::make_shared<FakeStorage>("storage.sd", sd, /*removable=*/true);
        sd_dev->online_flag = sd_online;
        registry.add(int_dev);
        registry.add(sd_dev);
        bus.start();
        bus.subscribe("", [this](const Event& e) {
            std::lock_guard<std::mutex> lock(mutex);
            topics.push_back(e.topic);
        });
        mgr = std::make_unique<StorageManager>(registry, bus, config);
    }

    std::vector<std::string> collected_topics() {
        std::lock_guard<std::mutex> lock(mutex);
        return topics;
    }

    std::string in;
    std::string sd;
    DeviceRegistry registry;
    EventBus bus;
    std::shared_ptr<FakeStorage> int_dev;
    std::shared_ptr<FakeStorage> sd_dev;
    std::unique_ptr<StorageManager> mgr;

    std::mutex mutex;
    std::vector<std::string> topics;
};

}  // namespace

TEST(StorageManager, RootsWithAndWithoutSd) {
    Fixture f(/*sd_online=*/true);
    ASSERT_TRUE(f.mgr->init());

    EXPECT_EQ(f.mgr->roots(Area::Library),
              (std::vector<std::string>{f.sd + "/wisp/library", f.in + "/library"}));
    EXPECT_EQ(f.mgr->write_root(Area::Library).value(), f.sd + "/wisp/library");

    f.sd_dev->online_flag = false;
    f.mgr->refresh();

    EXPECT_EQ(f.mgr->roots(Area::Library), (std::vector<std::string>{f.in + "/library"}));
    EXPECT_TRUE(f.mgr->roots(Area::Backup).empty());
    EXPECT_EQ(f.mgr->write_root(Area::Backup).error().code, wisp::ErrorCode::StorageUnavailable);
}

TEST(StorageManager, AtomicWriteLeavesNoTmpAndOverwrites) {
    Fixture f(false);
    ASSERT_TRUE(f.mgr->init());

    auto p = f.in + "/system/a.json";
    ASSERT_TRUE(f.mgr->write_atomic(p, bytes("one")));
    ASSERT_TRUE(f.mgr->write_atomic(p, bytes("two")));  // target already exists (FAT scenario)
    EXPECT_EQ(str(f.mgr->read_file(p).value()), "two");
    EXPECT_FALSE(exists(p + ".tmp"));
    EXPECT_FALSE(exists(p + ".new"));
}

// Fix round 1, item 1 (controller ruling): a plain ".tmp" can be a torn
// write from any point -- including the very first write to a path, with
// no prior target and no ".new" ever created -- so it must never be
// promoted. read_file() recovers only from a leftover ".new" (always a
// complete, synced write); a lone ".tmp" is discarded as garbage and the
// file is reported missing.
TEST(StorageManager, RecoversFromPendingNewNotTmp) {
    Fixture f(false);
    ASSERT_TRUE(f.mgr->init());

    auto p = f.in + "/system/b.json";
    write_raw(p + ".new", "pending");  // simulate power loss after deleting target, before rename
    EXPECT_EQ(str(f.mgr->read_file(p).value()), "pending");
    EXPECT_TRUE(exists(p));
    EXPECT_FALSE(exists(p + ".new"));
}

TEST(StorageManager, LoneTmpIsNotPromotedAndReadsAsNotFound) {
    Fixture f(false);
    ASSERT_TRUE(f.mgr->init());

    auto p = f.in + "/system/c.json";
    write_raw(p + ".tmp", "half-written garbage");  // power loss mid-fwrite on a first write

    auto r = f.mgr->read_file(p);
    EXPECT_FALSE(r);
    EXPECT_EQ(r.error().code, wisp::ErrorCode::NotFound);
    EXPECT_FALSE(exists(p));
    // The stray .tmp is discarded opportunistically.
    EXPECT_FALSE(exists(p + ".tmp"));
}

TEST(StorageManager, InternalLibraryQuota) {
    Fixture f(false, StorageConfig{.internal_library_quota = 1000});
    ASSERT_TRUE(f.mgr->init());

    ASSERT_TRUE(f.mgr->write_atomic(f.in + "/library/x.bin", std::vector<uint8_t>(600)));
    ASSERT_TRUE(f.mgr->write_atomic(f.in + "/library/x.bin",
                                    std::vector<uint8_t>(900)));  // overwriting itself doesn't
                                                                  // double-count
    EXPECT_EQ(f.mgr->write_atomic(f.in + "/library/y.bin", std::vector<uint8_t>(200)).error().code,
              wisp::ErrorCode::QuotaExceeded);
}

TEST(StorageManager, OfflineSdWriteFails) {
    Fixture f(true);
    ASSERT_TRUE(f.mgr->init());
    f.sd_dev->online_flag = false;
    f.mgr->refresh();

    EXPECT_EQ(f.mgr->write_atomic(f.sd + "/wisp/library/z", bytes("z")).error().code,
              wisp::ErrorCode::StorageUnavailable);
}

TEST(StorageManager, HotPlugEvents) {
    Fixture f(false);
    ASSERT_TRUE(f.mgr->init());

    f.sd_dev->online_flag = true;
    f.mgr->refresh();
    f.sd_dev->online_flag = false;
    f.mgr->refresh();
    f.bus.wait_idle(1000);

    EXPECT_EQ(f.collected_topics(),
              (std::vector<std::string>{"storage.sd.inserted", "storage.sd.removed"}));
    EXPECT_TRUE(exists(f.sd + "/wisp/library"));
}

TEST(StorageManager, MissingInternalFailsInit) {
    wisp::core::DeviceRegistry reg;
    wisp::core::EventBus bus;
    wisp::core::StorageManager m(reg, bus);
    EXPECT_EQ(m.init().error().code, wisp::ErrorCode::StorageUnavailable);
}

TEST(StorageManager, RejectsDotDotPath) {
    Fixture f(false);
    ASSERT_TRUE(f.mgr->init());

    auto p = f.in + "/library/../system/x";
    EXPECT_EQ(f.mgr->write_atomic(p, bytes("x")).error().code, wisp::ErrorCode::InvalidArgument);
}

// Fix round 1, item 2 (controller ruling): a non-canonical alias of a
// quota-checked path (a doubled slash, or a "." component) must not
// bypass the internal library quota -- write_atomic() normalizes the
// path before any prefix check or size accounting.
TEST(StorageManager, NonCanonicalPathStillCountsAgainstQuota) {
    Fixture f(false, StorageConfig{.internal_library_quota = 1000});
    ASSERT_TRUE(f.mgr->init());

    ASSERT_TRUE(f.mgr->write_atomic(f.in + "/library/x.bin", std::vector<uint8_t>(600)));

    // A doubled-slash alias of the same library directory must still be
    // accounted against the same quota, not treated as some other root.
    EXPECT_EQ(f.mgr->write_atomic(f.in + "//library/y.bin", std::vector<uint8_t>(500)).error().code,
              wisp::ErrorCode::QuotaExceeded);

    // Likewise a "./" alias.
    EXPECT_EQ(
        f.mgr->write_atomic(f.in + "/./library/z.bin", std::vector<uint8_t>(500)).error().code,
        wisp::ErrorCode::QuotaExceeded);
}

// Fix round 1, item 2: a driver that reports a mount point with a
// trailing slash must not break the quota's prefix check -- init() strips
// it before storing int_mount_/sd_mount_.
TEST(StorageManager, TrailingSlashMountPointIsNormalized) {
    auto in_raw = wisp::test::temp_dir().string();
    DeviceRegistry registry;
    EventBus bus;
    auto int_dev = std::make_shared<FakeStorage>("storage.int", in_raw + "/", /*removable=*/false);
    registry.add(int_dev);
    StorageManager mgr(registry, bus, StorageConfig{.internal_library_quota = 1000});
    ASSERT_TRUE(mgr.init());

    ASSERT_TRUE(mgr.write_atomic(in_raw + "/library/x.bin", std::vector<uint8_t>(600)));
    EXPECT_EQ(mgr.write_atomic(in_raw + "/library/y.bin", std::vector<uint8_t>(500)).error().code,
              wisp::ErrorCode::QuotaExceeded);
}

// Fix round 1, item 3 (controller ruling): a write-protected/corrupt SD
// card must not fail init() for internal storage, which is otherwise
// fine -- it only logs a warning, the same as refresh().
TEST(StorageManager, InitContinuesWhenSdDirsCannotBeCreated) {
    if (getuid() == 0) {
        GTEST_SKIP() << "running as root: chmod-based read-only test would not be meaningful";
    }
    Fixture f(/*sd_online=*/true);
    ASSERT_EQ(chmod(f.sd.c_str(), 0500), 0);  // read-only: mkdir_p() under it must fail

    EXPECT_TRUE(f.mgr->init());

    // Internal storage is unaffected: the System root is usable.
    auto p = f.in + "/system/ok.txt";
    ASSERT_TRUE(f.mgr->write_atomic(p, bytes("ok")));
    EXPECT_EQ(str(f.mgr->read_file(p).value()), "ok");

    chmod(f.sd.c_str(), 0700);  // restore so temp_dir()'s cleanup can remove it
}

// Fix round 1, item 8: before init() has succeeded, roots() must not
// fall back to a bogus "/system" built from an empty mount point, and
// write_atomic()/read_file() must refuse outright.
TEST(StorageManager, OperationsBeforeInitFail) {
    DeviceRegistry registry;
    EventBus bus;
    auto int_dev =
        std::make_shared<FakeStorage>("storage.int", wisp::test::temp_dir().string(), false);
    registry.add(int_dev);
    StorageManager mgr(registry, bus);
    // init() deliberately not called.

    EXPECT_TRUE(mgr.roots(Area::System).empty());
    EXPECT_EQ(mgr.write_root(Area::System).error().code, wisp::ErrorCode::StorageUnavailable);
    EXPECT_EQ(mgr.write_atomic("/tmp/wisp-should-not-be-created", bytes("x")).error().code,
              wisp::ErrorCode::StorageUnavailable);
    EXPECT_EQ(mgr.read_file("/tmp/wisp-should-not-exist").error().code,
              wisp::ErrorCode::StorageUnavailable);
    EXPECT_FALSE(exists("/tmp/wisp-should-not-be-created"));
}

// Fix round 1, item 12: reading a directory must fail cleanly rather than
// attempting fopen()/fread() on it.
TEST(StorageManager, ReadFileOnDirectoryIsInvalidArgument) {
    Fixture f(false);
    ASSERT_TRUE(f.mgr->init());

    EXPECT_EQ(f.mgr->read_file(f.in + "/system").error().code, wisp::ErrorCode::InvalidArgument);
}

// Fix round 1, item 13: mkdir_p() must not silently treat an existing
// non-directory file at a path component as "already there".
TEST(StorageManager, MkdirPFailsOnNonDirectoryComponent) {
    auto base = wisp::test::temp_dir().string();
    write_raw(base + "/system", "not a directory");  // occupies where <int>/system should go

    DeviceRegistry registry;
    EventBus bus;
    auto int_dev = std::make_shared<FakeStorage>("storage.int", base, false);
    registry.add(int_dev);
    StorageManager mgr(registry, bus);

    EXPECT_EQ(mgr.init().error().code, wisp::ErrorCode::IoError);
}
