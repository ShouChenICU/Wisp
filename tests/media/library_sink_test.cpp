// media::LibrarySink (wisp/media/library_sink.hpp): a stream from another
// task into a library file, its appends on the card's worker; a full Bulk
// lane, a refused finish and a sink that outlives its library.

#include "wisp/media/library_sink.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <memory>
#include <optional>
#include <string>
#include <thread>

#include "support/fake_devices.hpp"
#include "support/temp_dir.hpp"
#include "support/wait.hpp"
#include "wisp/core/device_registry.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/core/storage_manager.hpp"
#include "wisp/core/storage_router.hpp"
#include "wisp/core/storage_worker.hpp"
#include "wisp/media/library.hpp"
#include "wisp/os/os.hpp"

using wisp::Result;
using wisp::core::IoLane;
using wisp::core::StorageWorker;
using wisp::media::LibrarySink;
using wisp::media::MediaLibrary;
using wisp::test::wait_until;

namespace fs = std::filesystem;

namespace {

constexpr uint32_t kWaitMs = 5000;

std::string read_raw(const fs::path& path) {
    std::ifstream f(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

std::span<const uint8_t> bytes(const std::string& s) {
    return {reinterpret_cast<const uint8_t*>(s.data()), s.size()};
}

// An SD I/O worker whose Bulk lane holds one job, so a stream finds it
// full as soon as the worker is busy.
wisp::core::StorageWorkerConfig small_bulk() {
    auto c = StorageWorker::sd_io_config();
    c.bulk_capacity = 1;
    return c;
}

struct Fixture {
    Fixture() : in(wisp::test::temp_dir().string()), sd(wisp::test::temp_dir().string()) {
        registry.add(std::make_shared<wisp::test::FakeStorage>("storage.int", in, false));
        registry.add(std::make_shared<wisp::test::FakeStorage>("storage.sd", sd, true));
        EXPECT_TRUE(bus.start());
        storage = std::make_unique<wisp::core::StorageManager>(registry, bus);
        EXPECT_TRUE(storage->init());
        EXPECT_TRUE(flash.start());
        EXPECT_TRUE(sd_worker.start());
        router = std::make_unique<wisp::core::StorageRouter>(flash, &sd_worker);
        lib = std::make_unique<MediaLibrary>(wisp::media::MediaLibraryDeps{
            .storage = *storage, .router = router.get(), .bus = bus, .root = sd + "/media"});
        lib->start();
        EXPECT_TRUE(wait_until(
            [this] {
                auto s = lib->state().get();
                return s.available && !s.scanning;
            },
            kWaitMs));
        EXPECT_TRUE(router->drain(kWaitMs));
    }

    ~Fixture() {
        release();
        lib.reset();
        router.reset();
        sd_worker.stop();
        flash.stop();
        storage.reset();
        bus.stop();
    }

    std::shared_ptr<LibrarySink> open(const std::string& path) {
        std::shared_ptr<LibrarySink> out;
        wisp::os::Semaphore done{1, 0};
        LibrarySink::open(*lib, path, [&](Result<std::shared_ptr<LibrarySink>> r) {
            EXPECT_TRUE(r) << r.error().message;
            if (r) out = std::move(r.value());
            done.give();
        });
        EXPECT_TRUE(done.take(kWaitMs));
        return out;
    }

    // Holds the card's worker in a job until release().
    void hold() {
        held = true;
        std::atomic<bool> in_job{false};
        EXPECT_TRUE(sd_worker.submit(
            [this, &in_job] {
                in_job = true;
                while (held.load()) wisp::os::sleep_ms(1);
            },
            IoLane::Normal));
        EXPECT_TRUE(wait_until([&] { return in_job.load(); }, kWaitMs));
    }
    void release() { held = false; }

    fs::path file(const std::string& rel) const { return fs::path(sd) / "media" / rel; }

    std::string in, sd;
    wisp::core::DeviceRegistry registry;
    wisp::core::EventBus bus;
    std::unique_ptr<wisp::core::StorageManager> storage;
    StorageWorker flash;
    StorageWorker sd_worker{small_bulk()};
    std::unique_ptr<wisp::core::StorageRouter> router;
    std::unique_ptr<MediaLibrary> lib;
    std::atomic<bool> held{false};
};

// The finish's answer, waited for.
struct Finished {
    std::optional<Result<uint64_t>> got;
    wisp::os::Semaphore sem{1, 0};
    LibrarySink::Finished cb() {
        return [this](Result<uint64_t> r) {
            got.emplace(std::move(r));
            sem.give();
        };
    }
};

}  // namespace

// The Bulk lane holds far fewer chunks than push() lets wait: with the
// worker busy, push() waits for room instead of failing the transfer.
TEST(LibrarySink, FullBulkLaneIsWaitedFor) {
    Fixture f;
    auto sink = f.open("download/big.bin");
    ASSERT_TRUE(sink);
    const std::string body(5 * LibrarySink::kChunk + 100, 'b');
    f.hold();
    std::atomic<bool> pushed{false};
    std::atomic<bool> ok{false};
    std::thread producer([&] {
        ok = sink->push(bytes(body));
        pushed = true;
    });
    wisp::os::sleep_ms(100);
    EXPECT_FALSE(pushed.load());
    EXPECT_FALSE(sink->error());
    f.release();
    producer.join();
    EXPECT_TRUE(ok.load());
    Finished fin;
    ASSERT_TRUE(sink->finish(true, fin.cb()));
    ASSERT_TRUE(fin.sem.take(kWaitMs));
    ASSERT_TRUE(*fin.got);
    EXPECT_EQ(fin.got->value(), body.size());
    EXPECT_EQ(read_raw(f.file("download/big.bin")), body);
}

// A finish refused by a full lane is not final: the call the transfer's
// end makes later goes through.
TEST(LibrarySink, RefusedFinishCanBeRetried) {
    Fixture f;
    auto sink = f.open("download/r.bin");
    ASSERT_TRUE(sink);
    ASSERT_TRUE(sink->push(bytes("hello")));
    f.hold();
    // The one Bulk place taken.
    ASSERT_TRUE(f.sd_worker.submit([] {}, IoLane::Bulk));
    Finished refused;
    EXPECT_FALSE(sink->finish(false, refused.cb(), 0));
    f.release();
    Finished fin;
    ASSERT_TRUE(sink->finish(true, fin.cb()));
    ASSERT_TRUE(fin.sem.take(kWaitMs));
    ASSERT_TRUE(*fin.got);
    EXPECT_EQ(fin.got->value(), 5u);
    EXPECT_FALSE(refused.got.has_value());
    EXPECT_EQ(read_raw(f.file("download/r.bin")), "hello");
    // Once one is queued, later calls do nothing.
    EXPECT_FALSE(sink->finish(true, fin.cb()));
}

// The library goes while a sink still holds its writer: the sink queues
// nothing more, and letting go of it afterwards touches nothing.
TEST(LibrarySink, SinkOutlivesTheLibrary) {
    Fixture f;
    auto sink = f.open("download/late.bin");
    ASSERT_TRUE(sink);
    ASSERT_TRUE(sink->push(bytes(std::string(LibrarySink::kChunk + 10, 'x'))));
    ASSERT_TRUE(f.router->drain(kWaitMs));
    f.lib.reset();
    EXPECT_FALSE(sink->push(bytes(std::string(LibrarySink::kChunk, 'y'))));
    Finished fin;
    EXPECT_FALSE(sink->finish(true, fin.cb(), 0));
    sink.reset();
    EXPECT_FALSE(fin.got.has_value());
    EXPECT_FALSE(fs::exists(f.file("download/late.bin")));
}
