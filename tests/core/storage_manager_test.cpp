#include "wisp/core/storage_manager.hpp"

#include <gtest/gtest.h>

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
}

TEST(StorageManager, RecoversPendingTmp) {
    Fixture f(false);
    ASSERT_TRUE(f.mgr->init());

    auto p = f.in + "/system/b.json";
    write_raw(p + ".tmp", "pending");  // simulate power loss after deleting target, before rename
    EXPECT_EQ(str(f.mgr->read_file(p).value()), "pending");
    EXPECT_TRUE(exists(p));
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
