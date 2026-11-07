// MemoryStore (mvp spec 5.1): capacity, format, reload, concurrency and
// that neither the event nor the log ever carries a note's text.

#include "wisp/core/memory_store.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <vector>

#include "support/fake_devices.hpp"
#include "support/temp_dir.hpp"
#include "wisp/base/error.hpp"
#include "wisp/base/json.hpp"
#include "wisp/core/device_registry.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/core/log.hpp"
#include "wisp/core/storage_manager.hpp"

using wisp::ErrorCode;
using wisp::List;
using wisp::Map;
using wisp::Value;
using wisp::core::Event;
using wisp::core::EventBus;
using wisp::core::MemoryNote;
using wisp::core::MemoryStore;
using wisp::core::StorageManager;

namespace {

std::string str(const wisp::os::LargeString& s) { return std::string(s.data(), s.size()); }

std::span<const uint8_t> span_of(const std::string& s) {
    return {reinterpret_cast<const uint8_t*>(s.data()), s.size()};
}

struct Harness {
    Harness() : root(wisp::test::temp_dir().string()) {
        registry.add(std::make_shared<wisp::test::FakeStorage>("storage.int", root, false));
        storage = std::make_unique<StorageManager>(registry, bus);
        EXPECT_TRUE(storage->init());
        path = storage->write_root(wisp::core::Area::System).value() + "/memory/notes.json";
    }

    std::unique_ptr<MemoryStore> open(EventBus* events = nullptr) {
        auto s = std::make_unique<MemoryStore>(*storage, path, events, wall_s);
        EXPECT_TRUE(s->load());
        return s;
    }

    std::string root;
    wisp::core::DeviceRegistry registry;
    EventBus bus;
    std::unique_ptr<StorageManager> storage;
    std::string path;
    std::function<int64_t()> wall_s = [] { return int64_t{1700000000}; };
};

class MemoryStoreTest : public ::testing::Test {
protected:
    void SetUp() override {
        wisp::log::reset_for_test();
        wisp::log::init();
    }
    void TearDown() override { wisp::log::reset_for_test(); }
};

}  // namespace

TEST_F(MemoryStoreTest, AddListUpdateRemove) {
    Harness h;
    auto store = h.open();

    auto id1 = store->add("称呼：小陈");
    ASSERT_TRUE(id1);
    EXPECT_EQ(id1.value(), 1u);
    EXPECT_EQ(store->count(), 1u);

    auto id2 = store->add("住在上海");
    ASSERT_TRUE(id2);
    EXPECT_EQ(id2.value(), 2u);

    wisp::os::LargeVector<MemoryNote> notes = store->list();
    ASSERT_EQ(notes.size(), 2u);
    EXPECT_EQ(notes[0].id, 1u);
    EXPECT_EQ(str(notes[0].text), "称呼：小陈");
    EXPECT_EQ(notes[1].id, 2u);
    EXPECT_EQ(str(notes[1].text), "住在上海");

    ASSERT_TRUE(store->update(1, "称呼：陈先生"));
    notes = store->list();
    EXPECT_EQ(str(notes[0].text), "称呼：陈先生");

    ASSERT_TRUE(store->remove(2));
    EXPECT_EQ(store->count(), 1u);
    notes = store->list();
    ASSERT_EQ(notes.size(), 1u);
    EXPECT_EQ(notes[0].id, 1u);

    auto bad_update = store->update(99, "x");
    ASSERT_FALSE(bad_update);
    EXPECT_EQ(bad_update.error().code, ErrorCode::NotFound);
    EXPECT_EQ(bad_update.error().message, "not_found");

    auto bad_remove = store->remove(99);
    ASSERT_FALSE(bad_remove);
    EXPECT_EQ(bad_remove.error().code, ErrorCode::NotFound);
    EXPECT_EQ(bad_remove.error().message, "not_found");

    ASSERT_TRUE(store->clear());
    EXPECT_EQ(store->count(), 0u);
}

TEST_F(MemoryStoreTest, IdsAreNotReused) {
    Harness h;
    auto store = h.open();

    ASSERT_EQ(store->add("a").value(), 1u);
    ASSERT_EQ(store->add("b").value(), 2u);
    ASSERT_TRUE(store->remove(2));
    EXPECT_EQ(store->add("c").value(), 3u);
}

TEST_F(MemoryStoreTest, SixtyFourThenFull) {
    Harness h;
    auto store = h.open();

    for (int i = 0; i < 64; ++i) {
        auto r = store->add("note " + std::to_string(i));
        ASSERT_TRUE(r) << i;
    }
    EXPECT_EQ(store->count(), 64u);

    auto r = store->add("one more");
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, ErrorCode::QuotaExceeded);
    EXPECT_EQ(r.error().message, "memory_full");
    EXPECT_EQ(store->count(), 64u);
}

TEST_F(MemoryStoreTest, TwoHundredCodePoints) {
    Harness h;
    auto store = h.open();

    std::string two_hundred;
    for (int i = 0; i < 200; ++i) two_hundred += "\xE6\xB1\x89";  // U+6C49 "汉", 3 bytes
    ASSERT_TRUE(store->add(two_hundred));

    std::string two_hundred_one = two_hundred + "\xE6\xB1\x89";
    auto too_long = store->add(two_hundred_one);
    ASSERT_FALSE(too_long);
    EXPECT_EQ(too_long.error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(too_long.error().message, "too_long");

    auto empty = store->add("");
    ASSERT_FALSE(empty);
    EXPECT_EQ(empty.error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(empty.error().message, "empty");
}

TEST_F(MemoryStoreTest, FileFormatAndReload) {
    Harness h;
    {
        auto store = h.open();
        ASSERT_TRUE(store->add("one"));
        ASSERT_TRUE(store->add("two"));
        ASSERT_TRUE(store->remove(1));
    }

    auto bytes = h.storage->read_file(h.path);
    ASSERT_TRUE(bytes);
    const std::string text(reinterpret_cast<const char*>(bytes.value().data()),
                           bytes.value().size());
    auto doc = wisp::from_json(text);
    ASSERT_TRUE(doc);
    EXPECT_EQ(doc.value().find("version")->as_int(), 1);
    EXPECT_EQ(doc.value().find("next_id")->as_int(), 3);
    const auto& notes = doc.value().find("notes")->as_list();
    ASSERT_EQ(notes.size(), 1u);
    EXPECT_EQ(notes[0].find("id")->as_int(), 2);
    EXPECT_EQ(notes[0].find("text")->as_string(), "two");

    auto reloaded = h.open();
    wisp::os::LargeVector<MemoryNote> list1 = reloaded->list();
    ASSERT_EQ(list1.size(), 1u);
    EXPECT_EQ(list1[0].id, 2u);
    EXPECT_EQ(str(list1[0].text), "two");
    // The id counter survived the reload: it does not reuse 1.
    EXPECT_EQ(reloaded->add("three").value(), 3u);
}

TEST_F(MemoryStoreTest, EventAndLogsCarryNoContent) {
    Harness h;
    h.bus.start();
    std::vector<Value> payloads;
    h.bus.subscribe("memory.changed", [&](const Event& e) {
        EXPECT_EQ(e.source, "memory");
        payloads.push_back(Value(e.payload));
    });

    auto store = h.open(&h.bus);
    const std::string secret = "密语：不可说的事情";
    ASSERT_TRUE(store->add(secret));
    ASSERT_TRUE(h.bus.wait_idle(2000));
    h.bus.stop();

    ASSERT_EQ(payloads.size(), 1u);
    const wisp::Map& m = payloads[0].as_map();
    ASSERT_EQ(m.size(), 1u);
    ASSERT_NE(m.find("count"), m.end());
    EXPECT_EQ(m.at("count").as_int(), 1);

    for (const auto& e : wisp::log::ring_entries()) {
        EXPECT_EQ(e.msg.find(secret), std::string::npos) << e.msg;
    }
}

// A damaged file is never silently clobbered by the next write: load()
// quarantines it to "<path>.bad" first and continues empty and writable.
TEST_F(MemoryStoreTest, QuarantinesUnparsableFile) {
    Harness h;
    const std::string garbage = "not { valid json at all";
    ASSERT_TRUE(h.storage->write_atomic(h.path, span_of(garbage)));

    auto store = std::make_unique<MemoryStore>(*h.storage, h.path, nullptr, h.wall_s);
    ASSERT_TRUE(store->load());
    EXPECT_EQ(store->count(), 0u);

    ASSERT_TRUE(store->add("fresh note"));

    auto bad = h.storage->read_file(h.path + ".bad");
    ASSERT_TRUE(bad);
    const std::string bad_text(reinterpret_cast<const char*>(bad.value().data()),
                               bad.value().size());
    EXPECT_EQ(bad_text, garbage);

    auto fresh = h.storage->read_file(h.path);
    ASSERT_TRUE(fresh);
    const std::string fresh_text(reinterpret_cast<const char*>(fresh.value().data()),
                                 fresh.value().size());
    auto doc = wisp::from_json(fresh_text);
    ASSERT_TRUE(doc);
    const auto& notes = doc.value().find("notes")->as_list();
    ASSERT_EQ(notes.size(), 1u);
    EXPECT_EQ(notes[0].find("text")->as_string(), "fresh note");

    // A second corruption replaces the first ".bad" rather than failing
    // because one already exists.
    const std::string garbage2 = "still not json";
    ASSERT_TRUE(h.storage->write_atomic(h.path, span_of(garbage2)));
    auto store2 = std::make_unique<MemoryStore>(*h.storage, h.path, nullptr, h.wall_s);
    ASSERT_TRUE(store2->load());
    auto bad2 = h.storage->read_file(h.path + ".bad");
    ASSERT_TRUE(bad2);
    const std::string bad2_text(reinterpret_cast<const char*>(bad2.value().data()),
                                bad2.value().size());
    EXPECT_EQ(bad2_text, garbage2);
}

// A file in a future, unsupported format is quarantined the same way as
// one that fails to parse at all.
TEST_F(MemoryStoreTest, QuarantinesFutureVersion) {
    Harness h;
    const std::string future = R"({"version":2,"next_id":9,"notes":[{"id":1,"text":"x",)"
                               R"("updated":1}]})";
    ASSERT_TRUE(h.storage->write_atomic(h.path, span_of(future)));

    auto store = std::make_unique<MemoryStore>(*h.storage, h.path, nullptr, h.wall_s);
    ASSERT_TRUE(store->load());
    EXPECT_EQ(store->count(), 0u);

    auto bad = h.storage->read_file(h.path + ".bad");
    ASSERT_TRUE(bad);
    const std::string bad_text(reinterpret_cast<const char*>(bad.value().data()),
                               bad.value().size());
    EXPECT_EQ(bad_text, future);
}

// A file that exists but can't even be read (here: the path turns out to
// be a directory, so read_file_large() fails with something other than
// NotFound) must not be overwritten by a write that doesn't know what it
// would be destroying.
TEST_F(MemoryStoreTest, ReadErrorBlocksWritesUntilClear) {
    Harness h;
    ASSERT_TRUE(std::filesystem::create_directories(h.path));

    MemoryStore store(*h.storage, h.path, nullptr, h.wall_s);
    auto loaded = store.load();
    ASSERT_FALSE(loaded);
    EXPECT_NE(loaded.error().code, ErrorCode::NotFound);

    auto added = store.add("x");
    ASSERT_FALSE(added);
    EXPECT_EQ(added.error().message, "memory_unavailable");
    EXPECT_EQ(store.count(), 0u);
    EXPECT_TRUE(std::filesystem::is_directory(h.path)) << "a blocked write must not touch the path";

    // clear() is the explicit override: it recovers the store.
    ASSERT_TRUE(store.clear());
    EXPECT_TRUE(std::filesystem::is_regular_file(h.path));

    auto recovered = store.add("y");
    ASSERT_TRUE(recovered);
    EXPECT_EQ(recovered.value(), 1u);
}

// A hand-edited (or corrupted-then-partially-recovered) file is
// re-validated note by note on load, the same way
// ModelProfileService::load_models() re-validates stored profiles.
TEST_F(MemoryStoreTest, DropsInvalidRecordsOnLoad) {
    Harness h;

    auto note = [](int64_t id, std::string text) {
        return Value(Map{
            {"id", Value(id)}, {"text", Value(std::move(text))}, {"updated", Value(int64_t{1})}});
    };

    List notes;
    notes.push_back(note(0, "zero id"));              // dropped: id 0
    notes.push_back(note(1, "first"));                // kept
    notes.push_back(note(1, "duplicate"));            // dropped: duplicate id
    notes.push_back(note(2, ""));                     // dropped: empty text
    notes.push_back(note(3, std::string(201, 'x')));  // dropped: too long
    for (int i = 0; i < 70; ++i) {
        notes.push_back(note(10 + i, "note " + std::to_string(i)));  // first 63 kept, rest dropped
    }

    const std::string doc = wisp::to_json(
        Value(Map{{"version", Value(int64_t{1})}, {"notes", Value(std::move(notes))}}));
    ASSERT_TRUE(h.storage->write_atomic(h.path, span_of(doc)));

    auto store = std::make_unique<MemoryStore>(*h.storage, h.path, nullptr, h.wall_s);
    ASSERT_TRUE(store->load());

    // 1 (id 1) + 63 of the 70 "10.." notes = 64, MemoryStore::kMaxNotes.
    EXPECT_EQ(store->count(), MemoryStore::kMaxNotes);
    wisp::os::LargeVector<MemoryNote> list = store->list();
    ASSERT_FALSE(list.empty());
    EXPECT_EQ(list.front().id, 1u);
    EXPECT_EQ(str(list.front().text), "first");

    // The store is at kMaxNotes already; free a slot before checking that
    // next_id_ cleared every id the file named, including ones dropped
    // for capacity (highest id in the file is 10+69=79).
    ASSERT_TRUE(store->remove(1));
    auto added = store->add("new");
    ASSERT_TRUE(added);
    EXPECT_EQ(added.value(), 80u);
}

TEST_F(MemoryStoreTest, ConcurrentAddsGetDistinctIds) {
    Harness h;
    auto store = h.open();

    constexpr int kThreads = 2;
    constexpr int kPerThread = 32;
    std::vector<std::thread> threads;
    std::atomic<int> failures{0};
    for (int t = 0; t < kThreads; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < kPerThread; ++i) {
                if (!store->add("note")) ++failures;
            }
        });
    }
    for (auto& th : threads) th.join();

    EXPECT_EQ(failures.load(), 0);
    wisp::os::LargeVector<MemoryNote> notes = store->list();
    ASSERT_EQ(notes.size(), static_cast<size_t>(kThreads * kPerThread));
    std::vector<uint32_t> ids;
    for (const auto& n : notes) ids.push_back(n.id);
    std::sort(ids.begin(), ids.end());
    ids.erase(std::unique(ids.begin(), ids.end()), ids.end());
    EXPECT_EQ(ids.size(), notes.size());
}
