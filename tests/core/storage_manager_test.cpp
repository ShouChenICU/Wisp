#include "wisp/core/storage_manager.hpp"

#include <gtest/gtest.h>
#include <signal.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <unistd.h>

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <system_error>
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

// RAII guard that chmod()s `path` to `temp_mode` on
// construction and restores its original permissions on destruction --
// so a test that ASSERTs (and so returns early, skipping any manual
// cleanup written after the assertion) never leaves a read-only
// directory behind for temp_dir()'s process-exit cleanup to choke on.
class ChmodGuard {
public:
    ChmodGuard(std::string path, mode_t temp_mode) : path_(std::move(path)) {
        struct stat st{};
        stat(path_.c_str(), &st);
        original_mode_ = st.st_mode & 07777;
        chmod(path_.c_str(), temp_mode);
    }

    ~ChmodGuard() { chmod(path_.c_str(), original_mode_); }

    ChmodGuard(const ChmodGuard&) = delete;
    ChmodGuard& operator=(const ChmodGuard&) = delete;

private:
    std::string path_;
    mode_t original_mode_ = 0755;
};

// registry has two FakeStorage devices, "storage.int" (always online, not
// removable, mount point `in`) and "storage.sd" (removable, mount point
// `sd`, online() reflecting sd_dev->online_flag -- starts at `sd_online`).
// bus is started and subscribed to every topic, collecting them (in
// delivery order) into `topics`. `topics` is only written by the bus's
// dispatcher thread and is read by tests only after bus.wait_idle()
// confirms that thread is idle, so no extra locking is needed here.
//
// The explicit destructor stops the bus's dispatcher thread before
// `mutex`/`topics` are torn down. Members destruct in reverse declaration
// order, so without this, `topics`/`mutex` (declared last) would be
// destroyed while the dispatcher could still be delivering an event into
// the subscriber callback above -- a use-after-destroy race.
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

    ~Fixture() {
        mgr.reset();
        bus.stop();
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

// Services keep their files in subdirectories of an area root (e.g.
// <System>/net/wifi.json) that nothing else creates.
TEST(StorageManager, AtomicWriteCreatesMissingParentDirectories) {
    Fixture f(false);
    ASSERT_TRUE(f.mgr->init());

    auto p = f.in + "/system/net/sub/a.json";
    ASSERT_TRUE(f.mgr->write_atomic(p, bytes("one")));
    EXPECT_EQ(str(f.mgr->read_file(p).value()), "one");

    // A plain file where a parent directory should be is still an error.
    write_raw(f.in + "/system/blocker", "x");
    EXPECT_EQ(f.mgr->write_atomic(f.in + "/system/blocker/a.json", bytes("x")).error().code,
              wisp::ErrorCode::IoError);
}

// A plain ".tmp" can be a torn
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

// A non-canonical alias of a
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

// A driver that reports a mount point with a
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

// A write-protected/corrupt SD
// card must not fail init() for internal storage, which is otherwise
// fine -- it only logs a warning, the same as refresh().
TEST(StorageManager, InitContinuesWhenSdDirsCannotBeCreated) {
    if (getuid() == 0) {
        GTEST_SKIP() << "running as root: chmod-based read-only test would not be meaningful";
    }
    Fixture f(/*sd_online=*/true);
    ChmodGuard guard(f.sd,
                     0500);  // read-only: mkdir_p() under it must fail; restored on scope exit

    EXPECT_TRUE(f.mgr->init());

    // Internal storage is unaffected: the System root is usable.
    auto p = f.in + "/system/ok.txt";
    ASSERT_TRUE(f.mgr->write_atomic(p, bytes("ok")));
    EXPECT_EQ(str(f.mgr->read_file(p).value()), "ok");
}

// Before init() has succeeded, roots() must not
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

    // Use a fresh temp_dir() for the probe paths
    // instead of a hardcoded "/tmp/..." literal, so this test can't ever
    // collide with another process/run and self-cleans like every other
    // test here.
    auto scratch = wisp::test::temp_dir().string();
    auto p = scratch + "/should-not-be-created";

    EXPECT_TRUE(mgr.roots(Area::System).empty());
    EXPECT_EQ(mgr.write_root(Area::System).error().code, wisp::ErrorCode::StorageUnavailable);
    EXPECT_EQ(mgr.write_atomic(p, bytes("x")).error().code, wisp::ErrorCode::StorageUnavailable);
    EXPECT_EQ(mgr.read_file(scratch + "/should-not-exist").error().code,
              wisp::ErrorCode::StorageUnavailable);
    EXPECT_FALSE(exists(p));
}

// Reading a directory must fail cleanly rather than
// attempting fopen()/fread() on it.
TEST(StorageManager, ReadFileOnDirectoryIsInvalidArgument) {
    Fixture f(false);
    ASSERT_TRUE(f.mgr->init());

    EXPECT_EQ(f.mgr->read_file(f.in + "/system").error().code, wisp::ErrorCode::InvalidArgument);
}

// Mkdir_p() must not silently treat an existing
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

// A caller must not be able to write/read a path
// ending in the reserved ".tmp"/".new" suffix -- see has_reserved_suffix()
// in the .cpp for the three ways that would otherwise go wrong (invisible
// to the quota, clobbered by write_atomic()'s own bookkeeping, or
// mis-subtracted from "old_target_size").
TEST(StorageManager, RejectsReservedSuffixOnWrite) {
    Fixture f(false);
    ASSERT_TRUE(f.mgr->init());

    EXPECT_EQ(f.mgr->write_atomic(f.in + "/library/a.new", bytes("x")).error().code,
              wisp::ErrorCode::InvalidArgument);
    EXPECT_EQ(f.mgr->write_atomic(f.in + "/library/a.tmp", bytes("x")).error().code,
              wisp::ErrorCode::InvalidArgument);
}

TEST(StorageManager, RejectsReservedSuffixOnRead) {
    Fixture f(false);
    ASSERT_TRUE(f.mgr->init());

    EXPECT_EQ(f.mgr->read_file(f.in + "/library/a.new").error().code,
              wisp::ErrorCode::InvalidArgument);
    EXPECT_EQ(f.mgr->read_file(f.in + "/library/a.tmp").error().code,
              wisp::ErrorCode::InvalidArgument);
}

// A "<path>.new" orphaned by a double power cut (one
// write completed and synced but its final rename never happened) must
// be recovered, not silently discarded by a later write_atomic() call to
// the same path. write_atomic() promotes it to `path` before doing
// anything else, so the data is never lost even though this particular
// call immediately overwrites it with fresh data afterwards.
TEST(StorageManager, WriteAtomicPromotesPendingNewBeforeOverwriting) {
    Fixture f(false);
    ASSERT_TRUE(f.mgr->init());

    auto p = f.in + "/system/d.json";
    write_raw(p + ".new", "orphaned-from-double-power-cut");

    ASSERT_TRUE(f.mgr->write_atomic(p, bytes("fresh")));
    EXPECT_EQ(str(f.mgr->read_file(p).value()), "fresh");
    EXPECT_FALSE(exists(p + ".new"));
    EXPECT_FALSE(exists(p + ".tmp"));
}

// A stray ".tmp" sitting in the library directory
// (e.g. left over from an interrupted write to some other path) must not
// inflate the quota accounting -- dir_size_recursive() excludes it.
TEST(StorageManager, StrayTmpInLibraryDoesNotCountTowardQuota) {
    Fixture f(false, StorageConfig{.internal_library_quota = 1000});
    ASSERT_TRUE(f.mgr->init());

    write_raw(f.in + "/library/leftover.tmp", std::string(900, 'x'));

    // If the 900-byte stray ".tmp" counted, this 900-byte write would
    // push the total to 1800 and exceed the 1000-byte quota.
    ASSERT_TRUE(f.mgr->write_atomic(f.in + "/library/real.bin", std::vector<uint8_t>(900)));
}

// On the host the quota walk follows symlinks (stat, for ESP-IDF's sake),
// so a directory symlink pointing back up must not recurse until the stack
// overflows: the walk stops descending at a fixed depth.
TEST(StorageManager, SymlinkLoopInLibraryDoesNotOverflowTheQuotaWalk) {
    Fixture f(false, StorageConfig{.internal_library_quota = 1000});
    ASSERT_TRUE(f.mgr->init());
    ASSERT_TRUE(f.mgr->write_atomic(f.in + "/library/a.bin", std::vector<uint8_t>(10)));
    // Two links back to the parent: the kernel's per-lookup symlink limit
    // (ELOOP) alone would still leave 2^40 paths to walk.
    for (const char* name : {"/library/loop1", "/library/loop2"}) {
        std::error_code ec;
        std::filesystem::create_directory_symlink(f.in + "/library", f.in + name, ec);
        ASSERT_FALSE(ec) << ec.message();
    }

    // The walk sees a.bin again on every path through the links, so the
    // write is refused -- but it returns.
    auto r = f.mgr->write_atomic(f.in + "/library/b.bin", std::vector<uint8_t>(10));
    EXPECT_FALSE(r);
    EXPECT_EQ(r.error().code, wisp::ErrorCode::QuotaExceeded);
}

// set_standby()
// forwards to storage.sd only -- storage.int has no background polling
// to pause, so it must never see a call at all.
TEST(StorageManager, SetStandbyForwardsToSdOnlyAndIsIdempotentToObserve) {
    Fixture f(true);
    ASSERT_TRUE(f.mgr->init());

    f.mgr->set_standby(true);
    EXPECT_TRUE(f.sd_dev->standby());
    EXPECT_EQ(f.sd_dev->set_standby_calls(), 1);
    EXPECT_EQ(f.int_dev->set_standby_calls(), 0);

    f.mgr->set_standby(false);
    EXPECT_FALSE(f.sd_dev->standby());
    EXPECT_EQ(f.sd_dev->set_standby_calls(), 2);
    EXPECT_EQ(f.int_dev->set_standby_calls(), 0);
}

// No "storage.sd" registered at all: set_standby() must not crash or
// otherwise misbehave -- mirrors refresh()'s own "no-op if storage.sd
// isn't registered" contract.
TEST(StorageManager, SetStandbyIsANoOpWithoutAnSdCard) {
    DeviceRegistry registry;
    EventBus bus;
    bus.start();
    auto int_dev = std::make_shared<FakeStorage>("storage.int", wisp::test::temp_dir().string(),
                                                 /*removable=*/false);
    registry.add(int_dev);
    StorageManager mgr(registry, bus);
    ASSERT_TRUE(mgr.init());

    mgr.set_standby(true);
    mgr.set_standby(false);
    EXPECT_EQ(int_dev->set_standby_calls(), 0);
}

namespace {

std::span<const uint8_t> view(const std::string& s) {
    return {reinterpret_cast<const uint8_t*>(s.data()), s.size()};
}

}  // namespace

TEST(StorageManager, AppendCreatesAndExtends) {
    Fixture f(false);
    ASSERT_TRUE(f.mgr->init());
    std::string p = f.in + "/system/logs/a.log";

    ASSERT_TRUE(f.mgr->append(p, view("a\n")));
    ASSERT_TRUE(f.mgr->append(p, view("a\n")));

    EXPECT_EQ(str(f.mgr->read_file(p).value()), "a\na\n");
    EXPECT_FALSE(exists(p + ".tmp"));
    EXPECT_FALSE(exists(p + ".new"));
}

TEST(StorageManager, AppendRejectsReservedSuffixAndDotDot) {
    Fixture f(false);
    ASSERT_TRUE(f.mgr->init());

    EXPECT_EQ(f.mgr->append(f.in + "/system/x.tmp", view("x")).error().code,
              wisp::ErrorCode::InvalidArgument);
    EXPECT_EQ(f.mgr->append(f.in + "/system/x.new", view("x")).error().code,
              wisp::ErrorCode::InvalidArgument);
    EXPECT_EQ(f.mgr->append(f.in + "/system/a/../b", view("x")).error().code,
              wisp::ErrorCode::InvalidArgument);
    EXPECT_FALSE(exists(f.in + "/system/x.tmp"));
    EXPECT_FALSE(exists(f.in + "/system/b"));
}

TEST(StorageManager, AppendCountsAgainstLibraryQuota) {
    Fixture f(false, StorageConfig{.internal_library_quota = 1024});
    ASSERT_TRUE(f.mgr->init());
    std::string p = f.in + "/library/s/data.log";

    ASSERT_TRUE(f.mgr->append(p, std::vector<uint8_t>(1000, 'a')));
    ASSERT_TRUE(f.mgr->append(p, std::vector<uint8_t>(24, 'b')));  // exactly at the quota

    auto r = f.mgr->append(p, std::vector<uint8_t>(1, 'c'));
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, wisp::ErrorCode::QuotaExceeded);
    EXPECT_EQ(f.mgr->read_file(p).value().size(), 1024u);
}

TEST(StorageManager, ListDirSortedWithKinds) {
    Fixture f(false);
    ASSERT_TRUE(f.mgr->init());
    std::string d = f.in + "/system/ls";
    ASSERT_TRUE(f.mgr->write_atomic(d + "/b", view("hello")));
    ASSERT_EQ(mkdir((d + "/a").c_str(), 0755), 0);
    ASSERT_EQ(symlink((d + "/a").c_str(), (d + "/link").c_str()), 0);

    auto r = f.mgr->list_dir(d);
    ASSERT_TRUE(r) << r.error().message;
    const auto& v = r.value();
    ASSERT_EQ(v.size(), 3u);
    EXPECT_EQ(v[0].name, "a");
    EXPECT_EQ(v[0].kind, wisp::core::EntryKind::Dir);
    EXPECT_EQ(v[1].name, "b");
    EXPECT_EQ(v[1].kind, wisp::core::EntryKind::File);
    EXPECT_EQ(v[1].size, 5u);
    EXPECT_EQ(v[2].name, "link");
    EXPECT_EQ(v[2].kind, wisp::core::EntryKind::Other);

    EXPECT_EQ(f.mgr->list_dir(f.in + "/system/missing").error().code, wisp::ErrorCode::NotFound);
    EXPECT_EQ(f.mgr->list_dir(d + "/b").error().code, wisp::ErrorCode::InvalidArgument);
}

// visit_dir(): the entries one at a time, stopping when asked.
TEST(StorageManager, VisitDirStopsEarly) {
    Fixture f(false);
    ASSERT_TRUE(f.mgr->init());
    std::string d = f.in + "/system/vd";
    for (const char* n : {"a", "b", "c"}) ASSERT_TRUE(f.mgr->write_atomic(d + "/" + n, view("xy")));
    std::vector<std::string> seen;
    ASSERT_TRUE(f.mgr->visit_dir(d, [&](const wisp::core::DirEntry& e) {
        EXPECT_EQ(e.kind, wisp::core::EntryKind::File);
        EXPECT_EQ(e.size, 2u);
        seen.push_back(e.name);
        return true;
    }));
    std::sort(seen.begin(), seen.end());
    EXPECT_EQ(seen, (std::vector<std::string>{"a", "b", "c"}));
    size_t calls = 0;
    ASSERT_TRUE(f.mgr->visit_dir(d, [&](const wisp::core::DirEntry&) {
        ++calls;
        return false;
    }));
    EXPECT_EQ(calls, 1u);
    EXPECT_EQ(f.mgr->visit_dir(d + "/missing", [](const wisp::core::DirEntry&) { return true; })
                  .error()
                  .code,
              wisp::ErrorCode::NotFound);
}

TEST(StorageManager, RemoveTreeRecursiveAndMissingOk) {
    Fixture f(false);
    ASSERT_TRUE(f.mgr->init());
    std::string d = f.in + "/library/tree";
    ASSERT_TRUE(f.mgr->write_atomic(d + "/x/y/z.bin", view("1")));
    ASSERT_TRUE(f.mgr->write_atomic(d + "/top.bin", view("2")));
    write_raw(d + "/x/leftover.tmp", "garbage");
    // A symlink inside the tree is removed, not followed.
    std::string outside = f.in + "/system/keep";
    ASSERT_TRUE(f.mgr->write_atomic(outside + "/k.bin", view("k")));
    ASSERT_EQ(symlink(outside.c_str(), (d + "/x/link").c_str()), 0);

    ASSERT_TRUE(f.mgr->remove_tree(d));
    struct stat st{};
    EXPECT_NE(lstat(d.c_str(), &st), 0);
    EXPECT_TRUE(exists(outside + "/k.bin"));

    EXPECT_TRUE(f.mgr->remove_tree(d));  // already gone

    // A single file goes together with its write bookkeeping, so a later
    // read_file() cannot resurrect it from a leftover ".new".
    std::string file = f.in + "/system/one.json";
    ASSERT_TRUE(f.mgr->write_atomic(file, view("x")));
    write_raw(file + ".new", "stale");
    ASSERT_TRUE(f.mgr->remove_tree(file));
    EXPECT_FALSE(exists(file));
    EXPECT_FALSE(exists(file + ".new"));
    EXPECT_EQ(f.mgr->read_file(file).error().code, wisp::ErrorCode::NotFound);

    EXPECT_EQ(f.mgr->remove_tree(f.in).error().code, wisp::ErrorCode::InvalidArgument);
}

TEST(StorageManager, RenameRefusesExistingTarget) {
    Fixture f(false);
    ASSERT_TRUE(f.mgr->init());
    std::string a = f.in + "/system/r/a.txt";
    std::string b = f.in + "/system/r/b.txt";
    ASSERT_TRUE(f.mgr->write_atomic(a, view("A")));
    ASSERT_TRUE(f.mgr->write_atomic(b, view("B")));

    EXPECT_EQ(f.mgr->rename(a, b).error().code, wisp::ErrorCode::AlreadyExists);
    EXPECT_EQ(str(f.mgr->read_file(b).value()), "B");
    EXPECT_EQ(f.mgr->rename(f.in + "/system/r/none", f.in + "/system/r/c").error().code,
              wisp::ErrorCode::NotFound);

    std::string d1 = f.in + "/system/d1";
    std::string d2 = f.in + "/system/d2";
    ASSERT_TRUE(f.mgr->write_atomic(d1 + "/inner.txt", view("I")));
    ASSERT_TRUE(f.mgr->rename(d1, d2));
    EXPECT_FALSE(exists(d1 + "/inner.txt"));
    EXPECT_EQ(str(f.mgr->read_file(d2 + "/inner.txt").value()), "I");
}

TEST(StorageManager, RenameIntoLibraryCountsAgainstQuota) {
    Fixture f(false, StorageConfig{.internal_library_quota = 100});
    ASSERT_TRUE(f.mgr->init());
    std::string src = f.in + "/system/big";
    ASSERT_TRUE(f.mgr->write_atomic(src + "/a.bin", std::vector<uint8_t>(101)));

    EXPECT_EQ(f.mgr->rename(src, f.in + "/library/big").error().code,
              wisp::ErrorCode::QuotaExceeded);
    EXPECT_TRUE(exists(src + "/a.bin"));
}

TEST(StorageManager, TreeSizeExcludesTmpAndNew) {
    Fixture f(false);
    ASSERT_TRUE(f.mgr->init());
    std::string d = f.in + "/library/sz";
    ASSERT_TRUE(f.mgr->write_atomic(d + "/a.bin", std::vector<uint8_t>(10)));
    ASSERT_TRUE(f.mgr->write_atomic(d + "/sub/b.bin", std::vector<uint8_t>(20)));
    write_raw(d + "/c.bin.tmp", std::string(100, 'x'));
    write_raw(d + "/sub/d.bin.new", std::string(200, 'x'));

    EXPECT_EQ(f.mgr->tree_size(d).value(), 30u);
    EXPECT_EQ(f.mgr->tree_size(d + "/a.bin").value(), 10u);
    EXPECT_EQ(f.mgr->tree_size(f.in + "/library/missing").value(), 0u);
}

TEST(StorageManager, InternalRootIsIntMount) {
    Fixture f(false);
    EXPECT_EQ(f.mgr->internal_root().error().code, wisp::ErrorCode::StorageUnavailable);
    ASSERT_TRUE(f.mgr->init());
    EXPECT_EQ(f.mgr->internal_root().value(), f.in);
}

TEST(StorageManager, ReadFileLargeMatchesReadFile) {
    Fixture f(false);
    ASSERT_TRUE(f.mgr->init());
    std::string p = f.in + "/system/big.bin";
    std::vector<uint8_t> data(70 * 1024);
    for (size_t i = 0; i < data.size(); ++i) data[i] = static_cast<uint8_t>(i * 31 + 7);
    ASSERT_TRUE(f.mgr->write_atomic(p, data));

    auto large = f.mgr->read_file_large(p);
    ASSERT_TRUE(large) << large.error().message;
    auto small = f.mgr->read_file(p);
    ASSERT_TRUE(small);
    ASSERT_EQ(large.value().size(), small.value().size());
    EXPECT_TRUE(std::equal(small.value().begin(), small.value().end(), large.value().data()));

    EXPECT_EQ(f.mgr->read_file_large(f.in + "/system/none").error().code,
              wisp::ErrorCode::NotFound);
    EXPECT_EQ(f.mgr->read_file_large(f.in + "/system/x.tmp").error().code,
              wisp::ErrorCode::InvalidArgument);
}

namespace {

// `levels` nested one-letter directories under `base`, e.g. base/d/d/d.
std::string nested(const std::string& base, int levels) {
    std::string p = base;
    for (int i = 0; i < levels; ++i) p += "/d";
    return p;
}

}  // namespace

// The walks run on the 4 KB wisp.fs stack; trees are capped at 16 levels
// below the mount so no path can be built that the walks refuse.
TEST(StorageManager, PathsDeeperThanSixteenLevelsAreRefused) {
    Fixture f(false);
    ASSERT_TRUE(f.mgr->init());

    // 16 levels below the mount (15 directories + the file) is fine.
    ASSERT_TRUE(f.mgr->append(nested(f.in, 15) + "/ok.log", view("x")));
    ASSERT_TRUE(f.mgr->write_atomic(nested(f.in, 15) + "/ok.bin", view("x")));

    // One more level is refused before mkdir_p creates anything.
    std::string deep = nested(f.in + "/system", 16);
    EXPECT_EQ(f.mgr->append(deep + "/x.log", view("x")).error().code,
              wisp::ErrorCode::InvalidArgument);
    EXPECT_EQ(f.mgr->write_atomic(deep + "/x.bin", view("x")).error().code,
              wisp::ErrorCode::InvalidArgument);
    ASSERT_TRUE(f.mgr->write_atomic(f.in + "/system/r/a.txt", view("A")));
    EXPECT_EQ(f.mgr->rename(f.in + "/system/r/a.txt", deep + "/a.txt").error().code,
              wisp::ErrorCode::InvalidArgument);
    EXPECT_FALSE(exists(f.in + "/system/d"));
}

TEST(StorageManager, RemoveTreeFailsOnTreeDeeperThanLimitAndLeavesIt) {
    Fixture f(false);
    ASSERT_TRUE(f.mgr->init());
    // Built behind the manager's back: a tree the manager never made.
    std::string root = f.in + "/system/t";
    std::error_code ec;
    std::filesystem::create_directories(nested(root, 17), ec);
    ASSERT_FALSE(ec) << ec.message();

    auto r = f.mgr->remove_tree(root);
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, wisp::ErrorCode::IoError);
    EXPECT_TRUE(exists(root));

    // Within the limit the same walk succeeds.
    std::filesystem::remove_all(nested(root, 1), ec);
    std::filesystem::create_directories(nested(root, 14), ec);
    EXPECT_TRUE(f.mgr->remove_tree(root));
}

TEST(StorageManager, TreeSizeOfDeepTreeIsBounded) {
    Fixture f(false);
    ASSERT_TRUE(f.mgr->init());
    std::string root = f.in + "/system/t";
    ASSERT_TRUE(f.mgr->write_atomic(nested(root, 10) + "/a.bin", std::vector<uint8_t>(7)));
    ASSERT_TRUE(f.mgr->write_atomic(root + "/b.bin", std::vector<uint8_t>(5)));
    EXPECT_EQ(f.mgr->tree_size(root).value(), 12u);
}

// A write that fails partway (here: the process file-size limit, which
// makes the kernel accept only part of the data) must not leave the
// accepted part behind for the next append to land after.
TEST(StorageManager, FailedAppendLeavesFileUnchanged) {
    Fixture f(false);
    ASSERT_TRUE(f.mgr->init());
    std::string p = f.in + "/system/j.log";
    ASSERT_TRUE(f.mgr->append(p, view("first\n")));

    struct sigaction ignore{};
    struct sigaction old_action{};
    ignore.sa_handler = SIG_IGN;
    ASSERT_EQ(sigaction(SIGXFSZ, &ignore, &old_action), 0);
    struct rlimit old_limit{};
    ASSERT_EQ(getrlimit(RLIMIT_FSIZE, &old_limit), 0);
    struct rlimit small = old_limit;
    small.rlim_cur = 10;
    ASSERT_EQ(setrlimit(RLIMIT_FSIZE, &small), 0);

    auto r = f.mgr->append(p, view("a record that does not fit\n"));

    setrlimit(RLIMIT_FSIZE, &old_limit);
    sigaction(SIGXFSZ, &old_action, nullptr);

    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, wisp::ErrorCode::IoError);
    EXPECT_EQ(str(f.mgr->read_file(p).value()), "first\n");

    ASSERT_TRUE(f.mgr->append(p, view("second\n")));
    EXPECT_EQ(str(f.mgr->read_file(p).value()), "first\nsecond\n");
}

TEST(StorageManager, RemoveTreeAndRenameStayInsideStorage) {
    Fixture f(true);
    ASSERT_TRUE(f.mgr->init());
    std::string outside = wisp::test::temp_dir().string();
    write_raw(outside + "/keep.txt", "k");
    ASSERT_TRUE(f.mgr->write_atomic(f.in + "/system/a.txt", view("A")));
    const auto bad = wisp::ErrorCode::InvalidArgument;

    // Relative paths and paths outside every mount.
    EXPECT_EQ(f.mgr->remove_tree("system/a.txt").error().code, bad);
    EXPECT_EQ(f.mgr->remove_tree(outside).error().code, bad);
    EXPECT_EQ(f.mgr->remove_tree(outside + "/keep.txt").error().code, bad);
    EXPECT_TRUE(exists(outside + "/keep.txt"));
    EXPECT_EQ(f.mgr->rename("system/a.txt", f.in + "/system/b.txt").error().code, bad);
    EXPECT_EQ(f.mgr->rename(f.in + "/system/a.txt", "b.txt").error().code, bad);
    EXPECT_EQ(f.mgr->rename(outside + "/keep.txt", f.in + "/system/k.txt").error().code, bad);
    EXPECT_EQ(f.mgr->rename(f.in + "/system/a.txt", outside + "/a.txt").error().code, bad);

    // Mount points and area roots, as the remove target and as either
    // end of a rename.
    for (const std::string& root : {f.in, f.in + "/system", f.in + "/library", f.sd, f.sd + "/wisp",
                                    f.sd + "/wisp/library", f.sd + "/wisp/backup"}) {
        EXPECT_EQ(f.mgr->remove_tree(root).error().code, bad) << root;
        EXPECT_EQ(f.mgr->rename(root, f.in + "/system/moved").error().code, bad) << root;
    }
    EXPECT_TRUE(f.mgr->remove_tree(f.in + "/library/x"));  // a missing child of a root
    for (const std::string& root : {f.in + "/library", f.sd + "/wisp/backup"}) {
        ASSERT_EQ(rmdir(root.c_str()), 0);  // so only the root rule can refuse it
        EXPECT_EQ(f.mgr->rename(f.in + "/system/a.txt", root).error().code, bad) << root;
        ASSERT_EQ(mkdir(root.c_str(), 0755), 0);
    }
    EXPECT_TRUE(exists(f.in + "/system/a.txt"));

    // Inside a mount but outside the areas (e.g. <int>/scenes) is allowed.
    ASSERT_TRUE(f.mgr->rename(f.in + "/system/a.txt", f.in + "/scenes/a.txt"));
    EXPECT_TRUE(f.mgr->remove_tree(f.in + "/scenes"));
}

TEST(StorageManager, ReadRangeAndMtime) {
    Fixture f(/*sd_online=*/false);
    ASSERT_TRUE(f.mgr->init());
    const std::string path = f.in + "/scenes/a/files/x.bin";
    ASSERT_TRUE(f.mgr->write_atomic(path, bytes("0123456789")));

    auto mid = f.mgr->read_range(path, 2, 3);
    ASSERT_TRUE(mid) << mid.error().message;
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(mid.value().data()), mid.value().size()),
              "234");
    // Past the end: what is left, then nothing.
    auto tail = f.mgr->read_range(path, 8, 100);
    ASSERT_TRUE(tail);
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(tail.value().data()), tail.value().size()),
              "89");
    auto beyond = f.mgr->read_range(path, 50, 4);
    ASSERT_TRUE(beyond);
    EXPECT_EQ(beyond.value().size(), 0u);
    EXPECT_EQ(f.mgr->read_range(f.in + "/scenes/a/files/none", 0, 4).error().code,
              wisp::ErrorCode::NotFound);

    struct stat st{};
    ASSERT_EQ(stat(path.c_str(), &st), 0);
    auto listed = f.mgr->list_dir(f.in + "/scenes/a/files");
    ASSERT_TRUE(listed);
    ASSERT_EQ(listed.value().size(), 1u);
    EXPECT_EQ(listed.value()[0].name, "x.bin");
    EXPECT_EQ(listed.value()[0].size, 10u);
    EXPECT_EQ(listed.value()[0].mtime, static_cast<int64_t>(st.st_mtime));
    EXPECT_GT(listed.value()[0].mtime, 0);
}
