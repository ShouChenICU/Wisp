#include "wisp/core/scene_store.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string>
#include <system_error>
#include <thread>
#include <utility>
#include <vector>

#include "support/fake_devices.hpp"
#include "support/temp_dir.hpp"
#include "wisp/base/error.hpp"
#include "wisp/base/json.hpp"
#include "wisp/core/device_registry.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/core/log.hpp"
#include "wisp/core/storage_manager.hpp"
#include "wisp/core/storage_worker.hpp"

using wisp::ErrorCode;
using wisp::Result;
using wisp::Value;
using wisp::core::FindQuery;
using wisp::core::SceneStore;
using wisp::core::SceneStoreDeps;
using wisp::core::SceneStoreLimits;
using wisp::core::StoreRecord;

namespace fs = std::filesystem;

namespace {

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

std::vector<std::string> lines_of(const fs::path& path) {
    std::vector<std::string> out;
    std::string all = read_raw(path);
    size_t pos = 0;
    while (pos < all.size()) {
        size_t nl = all.find('\n', pos);
        if (nl == std::string::npos) {
            out.push_back(all.substr(pos));
            break;
        }
        out.push_back(all.substr(pos, nl - pos));
        pos = nl + 1;
    }
    return out;
}

// Every line parses as a whole log record.
void expect_well_formed(const fs::path& path) {
    std::string all = read_raw(path);
    ASSERT_TRUE(all.empty() || all.back() == '\n') << path;
    for (const auto& line : lines_of(path)) {
        auto v = wisp::from_json(line);
        ASSERT_TRUE(v) << "bad line in " << path << ": " << line;
        ASSERT_NE(v.value().find("op"), nullptr) << line;
    }
}

std::string str(const std::optional<wisp::os::LargeString>& s) {
    return s ? std::string(s->data(), s->size()) : std::string("<none>");
}

std::string str(const wisp::os::LargeString& s) { return std::string(s.data(), s.size()); }

std::vector<int64_t> ids(const wisp::os::LargeVector<StoreRecord>& records) {
    std::vector<int64_t> out;
    for (const auto& r : records) {
        out.push_back(r.id);
    }
    return out;
}

// A JSON object of exactly `n` bytes: {"p":"xxx..."}.
std::string object_of(size_t n) { return R"({"p":")" + std::string(n - 8, 'x') + R"("})"; }

struct Harness {
    Harness() : in(wisp::test::temp_dir().string()) {
        registry.add(std::make_shared<wisp::test::FakeStorage>("storage.int", in, false));
        storage = std::make_unique<wisp::core::StorageManager>(registry, bus);
        EXPECT_TRUE(storage->init());
    }

    fs::path dir() const { return fs::path(in) / "scenes" / "todo" / "data"; }

    SceneStoreDeps deps() {
        return SceneStoreDeps{*storage,
                              [this](std::function<void()> job) {
                                  ++submits;
                                  if (sync_io) {
                                      job();
                                  } else {
                                      queued.push_back(std::move(job));
                                  }
                                  return true;
                              },
                              [this](uint32_t ms, std::function<void()> cb) {
                                  timers.emplace_back(ms, std::move(cb));
                              }};
    }

    std::shared_ptr<SceneStore> open(SceneStoreLimits limits = {}) {
        auto s = SceneStore::create(deps(), dir().string(), limits);
        auto r = s->load();
        EXPECT_TRUE(r) << r.error().message;
        return s;
    }

    void fire_timers() {
        auto due = std::move(timers);
        timers.clear();
        for (auto& t : due) {
            t.second();
        }
    }

    void run_queued() {
        auto jobs = std::move(queued);
        queued.clear();
        for (auto& j : jobs) {
            j();
        }
    }

    // flush() and report what the job returned.
    static Result<void> flush_now(SceneStore& s) {
        Result<void> out = Result<void>::err(ErrorCode::Timeout, "not run");
        s.flush([&](Result<void> r) { out = std::move(r); });
        return out;
    }

    std::string in;
    wisp::core::DeviceRegistry registry;
    wisp::core::EventBus bus;
    std::unique_ptr<wisp::core::StorageManager> storage;
    std::vector<std::pair<uint32_t, std::function<void()>>> timers;
    std::vector<std::function<void()>> queued;
    bool sync_io = true;
    int submits = 0;
};

FindQuery all() { return FindQuery{}; }

}  // namespace

TEST(SceneStore, KvSetGetDelete) {
    Harness h;
    auto s = h.open();
    EXPECT_FALSE(s->kv_get("name"));
    ASSERT_TRUE(s->kv_set("name", R"("Ada")"));
    EXPECT_EQ(str(s->kv_get("name")), R"("Ada")");
    ASSERT_TRUE(s->kv_set("name", R"({"first":"Ada","n":[1,2]})"));
    EXPECT_EQ(str(s->kv_get("name")), R"({"first":"Ada","n":[1,2]})");
    // Keys are arbitrary bytes, escaped in the log.
    const std::string odd = std::string("a\"b\\c\n\x01", 7);
    ASSERT_TRUE(s->kv_set(odd, "42"));
    ASSERT_TRUE(s->kv_set("gone", "true"));
    ASSERT_TRUE(s->kv_del("gone"));
    EXPECT_FALSE(s->kv_get("gone"));
    EXPECT_TRUE(s->kv_del("never"));  // missing: no-op

    EXPECT_EQ(s->kv_set("", "1").error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(s->kv_set(std::string(65, 'k'), "1").error().code, ErrorCode::InvalidArgument);
    EXPECT_TRUE(s->kv_set(std::string(64, 'k'), "1"));
    EXPECT_EQ(s->kv_set("bad", "{").error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(s->kv_set("bad", "1 2").error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(s->kv_set("bad", "{\"a\":\n1}").error().code, ErrorCode::InvalidArgument);
    EXPECT_FALSE(s->kv_get("bad"));

    ASSERT_TRUE(Harness::flush_now(*s));
    expect_well_formed(h.dir() / "kv.log");
    auto again = h.open();
    EXPECT_EQ(str(again->kv_get("name")), R"({"first":"Ada","n":[1,2]})");
    EXPECT_EQ(str(again->kv_get(odd)), "42");
    EXPECT_FALSE(again->kv_get("gone"));
    EXPECT_EQ(again->used_bytes(), fs::file_size(h.dir() / "kv.log"));
}

TEST(SceneStore, CollectionCrudAndMonotonicIds) {
    Harness h;
    auto s = h.open();
    EXPECT_EQ(s->insert("todos", R"({"t":"a"})").value(), 1);
    EXPECT_EQ(s->insert("todos", R"({"t":"b"})").value(), 2);
    EXPECT_EQ(s->insert("todos", R"({"t":"c"})").value(), 3);
    ASSERT_TRUE(s->remove("todos", 2));
    EXPECT_EQ(s->insert("todos", R"({"t":"d"})").value(), 4);
    ASSERT_TRUE(s->update("todos", 1, R"({"t":"A"})"));
    EXPECT_EQ(s->update("todos", 2, R"({"t":"x"})").error().code, ErrorCode::NotFound);
    EXPECT_EQ(s->remove("todos", 2).error().code, ErrorCode::NotFound);
    EXPECT_EQ(s->update("nope", 1, R"({})").error().code, ErrorCode::NotFound);
    EXPECT_EQ(s->insert("todos", "[1]").error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(s->insert("todos", "3").error().code, ErrorCode::InvalidArgument);

    auto found = s->find("todos", all());
    ASSERT_TRUE(found);
    EXPECT_EQ(ids(found.value()), (std::vector<int64_t>{1, 3, 4}));
    EXPECT_EQ(str(found.value()[0].json), R"({"t":"A"})");

    ASSERT_TRUE(Harness::flush_now(*s));
    expect_well_formed(h.dir() / "todos.log");
    auto again = h.open();
    EXPECT_EQ(again->insert("todos", R"({"t":"e"})").value(), 5);
    EXPECT_EQ(ids(again->find("todos", all()).value()), (std::vector<int64_t>{1, 3, 4, 5}));
}

TEST(SceneStore, IdsStayMonotonicAcrossCompaction) {
    Harness h;
    auto s = h.open();
    for (int i = 0; i < 3; ++i) {
        ASSERT_TRUE(s->insert("todos", R"({"t":"x"})"));
    }
    ASSERT_TRUE(s->remove("todos", 3));
    ASSERT_TRUE(s->remove("todos", 2));
    ASSERT_TRUE(Harness::flush_now(*s));  // dead > live / 2: rewritten
    auto lines = lines_of(h.dir() / "todos.log");
    ASSERT_EQ(lines.size(), 2u);
    EXPECT_EQ(lines[0], R"({"op":"put","id":1,"v":{"t":"x"}})");
    EXPECT_EQ(lines[1], R"({"op":"del","id":3})");

    auto again = h.open();
    EXPECT_EQ(again->insert("todos", R"({"t":"y"})").value(), 4);
    EXPECT_EQ(ids(again->find("todos", all()).value()), (std::vector<int64_t>{1, 4}));
}

TEST(SceneStore, FindFilterSortLimitOffset) {
    Harness h;
    auto s = h.open();
    ASSERT_TRUE(s->insert("todos", R"({"t":"a","done":false,"due":3})"));    // 1
    ASSERT_TRUE(s->insert("todos", R"({"t":"b","done":true,"due":5})"));     // 2
    ASSERT_TRUE(s->insert("todos", R"({"t":"c","done":false,"due":10})"));   // 3
    ASSERT_TRUE(s->insert("todos", R"({"t":"d","done":false})"));            // 4
    ASSERT_TRUE(s->insert("todos", R"({"t":"e","done":false,"due":7.5})"));  // 5

    FindQuery q;
    q.equals.emplace_back("done", Value(false));
    q.sort = "due";
    q.desc = true;
    q.limit = 2;
    q.offset = 1;
    auto r = s->find("todos", q);
    ASSERT_TRUE(r);
    EXPECT_EQ(ids(r.value()), (std::vector<int64_t>{5, 1}));

    // Ascending: numbers by value, the missing field last.
    q.desc = false;
    q.limit = SIZE_MAX;
    q.offset = 0;
    EXPECT_EQ(ids(s->find("todos", q).value()), (std::vector<int64_t>{1, 5, 3, 4}));
    q.desc = true;
    EXPECT_EQ(ids(s->find("todos", q).value()), (std::vector<int64_t>{3, 5, 1, 4}));

    // 3 == 3.0; strings by bytes; ties keep id order; offset past the end.
    FindQuery n;
    n.equals.emplace_back("due", Value(3.0));
    EXPECT_EQ(ids(s->find("todos", n).value()), (std::vector<int64_t>{1}));
    FindQuery t;
    t.equals.emplace_back("t", Value("c"));
    EXPECT_EQ(ids(s->find("todos", t).value()), (std::vector<int64_t>{3}));
    FindQuery by_done;
    by_done.sort = "done";  // booleans are not sortable: all tie, id order
    EXPECT_EQ(ids(s->find("todos", by_done).value()), (std::vector<int64_t>{1, 2, 3, 4, 5}));
    FindQuery by_t;
    by_t.sort = "t";
    by_t.desc = true;
    EXPECT_EQ(ids(s->find("todos", by_t).value()), (std::vector<int64_t>{5, 4, 3, 2, 1}));
    FindQuery past;
    past.offset = 9;
    EXPECT_TRUE(s->find("todos", past).value().empty());

    ASSERT_TRUE(s->insert("tags", R"({"k":"zé","list":[1,2],"o":{"a":1}})"));
    FindQuery esc;
    esc.equals.emplace_back("k", Value("z\xc3\xa9"));
    esc.equals.emplace_back("list", Value(wisp::List{Value(1), Value(2)}));
    esc.equals.emplace_back("o", Value(wisp::Map{{"a", Value(1)}}));
    EXPECT_EQ(ids(s->find("tags", esc).value()), (std::vector<int64_t>{1}));

    EXPECT_TRUE(s->find("missing", all()).value().empty());
    EXPECT_EQ(s->find("Bad", all()).error().code, ErrorCode::InvalidArgument);
}

TEST(SceneStore, FindByIdAndIdOrder) {
    Harness h;
    auto s = h.open();
    ASSERT_TRUE(s->insert("todos", R"({"t":"a","done":false})"));  // 1
    ASSERT_TRUE(s->insert("todos", R"({"t":"b","done":true})"));   // 2
    ASSERT_TRUE(s->insert("todos", R"({"t":"c","done":false})"));  // 3

    // No sort field: id order, reversed by desc.
    FindQuery q;
    q.desc = true;
    EXPECT_EQ(ids(s->find("todos", q).value()), (std::vector<int64_t>{3, 2, 1}));
    q.limit = 1;
    EXPECT_EQ(ids(s->find("todos", q).value()), (std::vector<int64_t>{3}));

    // id selects one record; the other equals still apply.
    FindQuery one;
    one.id = 2;
    EXPECT_EQ(ids(s->find("todos", one).value()), (std::vector<int64_t>{2}));
    one.equals.emplace_back("done", Value(false));
    EXPECT_TRUE(s->find("todos", one).value().empty());
    one.id = 3;
    EXPECT_EQ(ids(s->find("todos", one).value()), (std::vector<int64_t>{3}));
    one.id = 9;
    EXPECT_TRUE(s->find("todos", one).value().empty());

    // The same inside a batch, where the overlay holds new records.
    s->begin_batch();
    ASSERT_TRUE(s->insert("todos", R"({"t":"d","done":false})"));  // 4
    FindQuery in_batch;
    in_batch.id = 4;
    EXPECT_EQ(ids(s->find("todos", in_batch).value()), (std::vector<int64_t>{4}));
    in_batch.id.reset();
    in_batch.desc = true;
    EXPECT_EQ(ids(s->find("todos", in_batch).value()), (std::vector<int64_t>{4, 3, 2, 1}));
    s->abort_batch();
}

TEST(SceneStore, WritesCoalesceWithin300ms) {
    Harness h;
    auto s = h.open();
    for (int i = 0; i < 10; ++i) {
        ASSERT_TRUE(s->kv_set("k" + std::to_string(i), std::to_string(i)));
    }
    ASSERT_EQ(h.timers.size(), 1u);
    EXPECT_EQ(h.timers[0].first, 300u);
    EXPECT_FALSE(on_disk(h.dir() / "kv.log"));
    EXPECT_EQ(h.submits, 0);

    h.fire_timers();
    EXPECT_EQ(h.submits, 1);
    EXPECT_EQ(lines_of(h.dir() / "kv.log").size(), 10u);
    EXPECT_FALSE(on_disk(h.dir() / "journal.json"));

    // The next write arms a new timer.
    ASSERT_TRUE(s->kv_set("k10", "10"));
    EXPECT_EQ(h.timers.size(), 1u);
}

TEST(SceneStore, FlushWritesPendingNow) {
    Harness h;
    auto s = h.open();
    ASSERT_TRUE(s->kv_set("a", "1"));
    ASSERT_TRUE(s->insert("todos", R"({"t":"x"})"));
    EXPECT_FALSE(on_disk(h.dir() / "kv.log"));
    ASSERT_TRUE(Harness::flush_now(*s));
    EXPECT_EQ(lines_of(h.dir() / "kv.log").size(), 1u);
    EXPECT_EQ(lines_of(h.dir() / "todos.log").size(), 1u);
    EXPECT_EQ(s->used_bytes(),
              fs::file_size(h.dir() / "kv.log") + fs::file_size(h.dir() / "todos.log"));

    // The timer armed by the first write finds nothing left to do.
    int before = h.submits;
    h.fire_timers();
    EXPECT_EQ(h.submits, before + 1);
    EXPECT_EQ(lines_of(h.dir() / "kv.log").size(), 1u);

    // A flush that cannot be queued reports Busy and keeps the data.
    SceneStoreDeps deps = h.deps();
    deps.submit_io = [](std::function<void()>) { return false; };
    auto busy = SceneStore::create(deps, h.dir().string());
    ASSERT_TRUE(busy->load());
    ASSERT_TRUE(busy->kv_set("b", "2"));
    Result<void> out = Result<void>::ok();
    busy->flush([&](Result<void> r) { out = std::move(r); });
    EXPECT_EQ(out.error().code, ErrorCode::Busy);
    EXPECT_EQ(str(busy->kv_get("b")), "2");
}

TEST(SceneStore, CompactsWhenDeadExceedsHalfLive) {
    Harness h;
    auto s = h.open();
    for (int i = 0; i < 4; ++i) {
        ASSERT_TRUE(s->insert("todos", object_of(100)));
    }
    ASSERT_TRUE(Harness::flush_now(*s));
    ASSERT_EQ(lines_of(h.dir() / "todos.log").size(), 4u);

    // dead = 1 line, live = 4 lines: appended.
    ASSERT_TRUE(s->update("todos", 1, object_of(100)));
    ASSERT_TRUE(Harness::flush_now(*s));
    ASSERT_EQ(lines_of(h.dir() / "todos.log").size(), 5u);
    ASSERT_TRUE(s->update("todos", 1, object_of(100)));
    ASSERT_TRUE(Harness::flush_now(*s));
    ASSERT_EQ(lines_of(h.dir() / "todos.log").size(), 6u);  // dead = 2 lines = live / 2

    ASSERT_TRUE(s->update("todos", 1, R"({"p":"last"})"));
    ASSERT_TRUE(Harness::flush_now(*s));  // dead = 3 lines > live / 2: rewritten
    auto lines = lines_of(h.dir() / "todos.log");
    ASSERT_EQ(lines.size(), 4u);
    EXPECT_EQ(lines[0], R"({"op":"put","id":1,"v":{"p":"last"}})");
    EXPECT_EQ(s->used_bytes(), fs::file_size(h.dir() / "todos.log"));

    auto again = h.open();
    EXPECT_EQ(ids(again->find("todos", all()).value()), (std::vector<int64_t>{1, 2, 3, 4}));
    EXPECT_EQ(str(again->find("todos", all()).value()[0].json), R"({"p":"last"})");
}

TEST(SceneStore, CompactsAfter64KBAppendedOnly) {
    Harness h;
    auto s = h.open();
    // A put line is 24 bytes + the id's digits + the value.
    auto line_len = [](int64_t id, size_t json) { return 24 + std::to_string(id).size() + json; };
    int64_t next = 1;
    for (int i = 0; i < 100; ++i) {
        ASSERT_EQ(s->insert("big", object_of(1000)).value(), next++);
    }
    ASSERT_TRUE(Harness::flush_now(*s));  // ~100 KB of live data
    const uint64_t base = fs::file_size(h.dir() / "big.log");

    // A small update: appended, its dead line stays.
    ASSERT_TRUE(s->update("big", 1, object_of(50)));
    ASSERT_TRUE(Harness::flush_now(*s));
    ASSERT_EQ(lines_of(h.dir() / "big.log").size(), 101u);
    uint64_t appended = fs::file_size(h.dir() / "big.log") - base;

    // Exactly 64 KB appended since the last rewrite: still appended.
    while (appended < 64 * 1024) {
        const uint64_t want = 64 * 1024 - appended;
        const size_t overhead = line_len(next, 0);
        size_t json = 16000;
        if (want <= overhead + 16000) {
            json = static_cast<size_t>(want) - overhead;  // the last one fills it exactly
        } else if (want - (overhead + 16000) < 100) {
            json = 15000;  // leave room for a last object of at least 8 bytes
        }
        ASSERT_EQ(s->insert("big", object_of(json)).value(), next);
        appended += line_len(next, json);
        ++next;
    }
    ASSERT_EQ(appended, 64u * 1024);
    ASSERT_TRUE(Harness::flush_now(*s));
    EXPECT_EQ(fs::file_size(h.dir() / "big.log"), base + appended);
    const size_t lines_before = lines_of(h.dir() / "big.log").size();
    EXPECT_EQ(lines_before, static_cast<size_t>(next - 1) + 1);  // one dead line

    // One more line: past 64 KB, rewritten without the dead line.
    ASSERT_TRUE(s->insert("big", object_of(10)));
    ASSERT_TRUE(Harness::flush_now(*s));
    EXPECT_EQ(lines_of(h.dir() / "big.log").size(), static_cast<size_t>(next));
    EXPECT_EQ(s->used_bytes(), fs::file_size(h.dir() / "big.log"));
}

TEST(SceneStore, TruncatedTailLineDropped) {
    wisp::log::reset_for_test();
    wisp::log::init(8 * 1024);
    Harness h;
    const std::string good =
        "{\"op\":\"put\",\"id\":1,\"v\":{\"t\":\"a\"}}\n"
        "{\"op\":\"put\",\"id\":2,\"v\":{\"t\":\"b\"}}\n";
    write_raw(h.dir() / "todos.log", good + R"({"op":"put","id":1,"v":{)");
    write_raw(h.dir() / "kv.log", "{\"op\":\"put\",\"id\":\"k\",\"v\":1}\n{\"op\":\"pu");

    auto s = h.open();
    auto found = s->find("todos", all());
    ASSERT_TRUE(found);
    EXPECT_EQ(ids(found.value()), (std::vector<int64_t>{1, 2}));
    EXPECT_EQ(str(found.value()[0].json), R"({"t":"a"})");
    EXPECT_EQ(str(s->kv_get("k")), "1");

    bool warned = false;
    for (const auto& e : wisp::log::ring_entries()) {
        if (e.level == wisp::LogLevel::Warn && e.msg.find("todos.log") != std::string::npos) {
            warned = true;
        }
    }
    EXPECT_TRUE(warned) << wisp::log::ring_snapshot();

    // The torn bytes are gone from disk before anything is appended.
    EXPECT_EQ(read_raw(h.dir() / "todos.log"), good);
    expect_well_formed(h.dir() / "kv.log");
    wisp::log::reset_for_test();
}

TEST(SceneStore, TornTailIsRewrittenBeforeNextAppend) {
    Harness h;
    write_raw(
        h.dir() / "todos.log",
        "{\"op\":\"put\",\"id\":1,\"v\":{\"t\":\"a\"}}\n{\"op\":\"put\",\"id\":2,\"v\":{\"t\"");
    {
        auto s = h.open();
        EXPECT_EQ(s->insert("todos", R"({"t":"b"})").value(), 2);
        EXPECT_EQ(s->insert("todos", R"({"t":"c"})").value(), 3);
        ASSERT_TRUE(Harness::flush_now(*s));
    }
    expect_well_formed(h.dir() / "todos.log");
    auto again = h.open();
    auto found = again->find("todos", all());
    ASSERT_TRUE(found);
    ASSERT_EQ(ids(found.value()), (std::vector<int64_t>{1, 2, 3}));
    EXPECT_EQ(str(found.value()[0].json), R"({"t":"a"})");
    EXPECT_EQ(str(found.value()[1].json), R"({"t":"b"})");
    EXPECT_EQ(str(found.value()[2].json), R"({"t":"c"})");
}

TEST(SceneStore, QuotaIs1MB) {
    Harness h;
    auto s = h.open();
    const std::string value = "\"" + std::string(15000, 'v') + "\"";
    int n = 0;
    Result<void> last = Result<void>::ok();
    for (; n < 200; ++n) {
        last = s->kv_set("key" + std::to_string(n), value);
        if (!last) {
            break;
        }
        ASSERT_LE(s->used_bytes(), 1024u * 1024);
    }
    ASSERT_FALSE(last);
    EXPECT_EQ(last.error().code, ErrorCode::QuotaExceeded);
    EXPECT_EQ(last.error().message, "quota_exceeded");
    EXPECT_GT(s->used_bytes() + value.size(), 1024u * 1024 - 100);
    EXPECT_FALSE(s->kv_get("key" + std::to_string(n)));
    const uint64_t used = s->used_bytes();
    EXPECT_EQ(s->insert("todos", "{\"p\":" + value + "}").error().code, ErrorCode::QuotaExceeded);
    EXPECT_TRUE(s->find("todos", all()).value().empty());
    EXPECT_EQ(s->used_bytes(), used);

    // Replacing a value fits once the logs are rewritten: accepted, and
    // the next flush rewrites.
    ASSERT_TRUE(Harness::flush_now(*s));
    ASSERT_TRUE(s->kv_set("key0", value));
    EXPECT_GT(s->used_bytes(), 1024u * 1024);
    ASSERT_TRUE(Harness::flush_now(*s));
    EXPECT_LE(s->used_bytes(), 1024u * 1024);
    EXPECT_EQ(s->used_bytes(), fs::file_size(h.dir() / "kv.log"));
    EXPECT_EQ(lines_of(h.dir() / "kv.log").size(), static_cast<size_t>(n));

    // Deleting frees room.
    ASSERT_TRUE(s->kv_del("key1"));
    EXPECT_TRUE(s->kv_set("fresh", value));
}

TEST(SceneStore, RecordLimitIs16KB) {
    Harness h;
    auto s = h.open();
    const std::string ok = "\"" + std::string(16384 - 2, 'x') + "\"";
    const std::string big = "\"" + std::string(16385 - 2, 'x') + "\"";
    EXPECT_TRUE(s->kv_set("ok", ok));
    auto r = s->kv_set("big", big);
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(r.error().message, "record_too_large");
    EXPECT_FALSE(s->kv_get("big"));

    EXPECT_TRUE(s->insert("todos", object_of(16384)));
    auto ins = s->insert("todos", object_of(16385));
    ASSERT_FALSE(ins);
    EXPECT_EQ(ins.error().message, "record_too_large");
    auto up = s->update("todos", 1, object_of(16385));
    ASSERT_FALSE(up);
    EXPECT_EQ(up.error().message, "record_too_large");
}

TEST(SceneStore, SixteenCollectionsMax) {
    Harness h;
    auto s = h.open();
    for (int i = 0; i < 16; ++i) {
        ASSERT_TRUE(s->insert("c" + std::to_string(i), "{}")) << i;
    }
    auto r = s->insert("c16", "{}");
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, ErrorCode::QuotaExceeded);
    EXPECT_EQ(r.error().message, "too_many_collections");
    EXPECT_TRUE(s->insert("c0", "{}"));

    for (const char* bad : {"kv", "Bad", "", "a.b", "a/b"}) {
        EXPECT_EQ(s->insert(bad, "{}").error().code, ErrorCode::InvalidArgument) << bad;
    }
    EXPECT_EQ(s->insert(std::string(33, 'a'), "{}").error().code, ErrorCode::InvalidArgument);

    // The count survives a reload.
    ASSERT_TRUE(Harness::flush_now(*s));
    auto again = h.open();
    EXPECT_EQ(again->insert("c16", "{}").error().message, "too_many_collections");
}

TEST(SceneStore, BatchCommitUsesJournal) {
    Harness h;
    // A directory where b.log belongs makes its append fail, leaving the
    // flush half done: the journal must still be there.
    std::error_code ec;
    fs::create_directories(h.dir() / "b.log", ec);
    auto s = h.open();
    s->begin_batch();
    ASSERT_TRUE(s->kv_set("k", "1"));
    ASSERT_TRUE(s->insert("a", R"({"x":1})"));
    ASSERT_TRUE(s->insert("b", R"({"y":2})"));
    ASSERT_TRUE(s->commit_batch());
    EXPECT_FALSE(Harness::flush_now(*s));

    ASSERT_TRUE(on_disk(h.dir() / "journal.json"));
    auto journal = wisp::from_json(read_raw(h.dir() / "journal.json"));
    ASSERT_TRUE(journal);
    EXPECT_EQ(journal.value().as_list().size(), 3u);
    EXPECT_EQ(lines_of(h.dir() / "kv.log").size(), 1u);
    EXPECT_EQ(lines_of(h.dir() / "a.log").size(), 1u);

    // Fixed: the retry writes b, and the journal goes away.
    fs::remove(h.dir() / "b.log", ec);
    ASSERT_TRUE(Harness::flush_now(*s));
    EXPECT_FALSE(on_disk(h.dir() / "journal.json"));
    EXPECT_EQ(lines_of(h.dir() / "a.log").size(), 1u);
    EXPECT_EQ(lines_of(h.dir() / "b.log").size(), 1u);

    // A clean multi-file flush leaves no journal either.
    s->begin_batch();
    ASSERT_TRUE(s->kv_set("k", "2"));
    ASSERT_TRUE(s->insert("a", R"({"x":3})"));
    ASSERT_TRUE(s->commit_batch());
    ASSERT_TRUE(Harness::flush_now(*s));
    EXPECT_FALSE(on_disk(h.dir() / "journal.json"));

    auto again = h.open();
    EXPECT_EQ(str(again->kv_get("k")), "2");
    EXPECT_EQ(ids(again->find("a", all()).value()), (std::vector<int64_t>{1, 2}));
    EXPECT_EQ(str(again->find("b", all()).value()[0].json), R"({"y":2})");
}

TEST(SceneStore, BatchAbortLeavesNothing) {
    Harness h;
    auto s = h.open();
    ASSERT_TRUE(s->kv_set("a", "1"));
    ASSERT_TRUE(s->insert("todos", R"({"t":"x"})"));
    ASSERT_TRUE(Harness::flush_now(*s));
    const std::string kv_before = read_raw(h.dir() / "kv.log");
    const std::string todos_before = read_raw(h.dir() / "todos.log");
    const uint64_t used = s->used_bytes();
    h.timers.clear();

    s->begin_batch();
    ASSERT_TRUE(s->kv_set("a", "2"));
    ASSERT_TRUE(s->kv_set("b", "3"));
    EXPECT_EQ(s->insert("todos", R"({"t":"y"})").value(), 2);
    ASSERT_TRUE(s->update("todos", 1, R"({"t":"z"})"));
    ASSERT_TRUE(s->remove("todos", 1));
    ASSERT_TRUE(s->insert("other", R"({})"));
    s->abort_batch();

    EXPECT_EQ(str(s->kv_get("a")), "1");
    EXPECT_FALSE(s->kv_get("b"));
    auto found = s->find("todos", all());
    ASSERT_EQ(ids(found.value()), (std::vector<int64_t>{1}));
    EXPECT_EQ(str(found.value()[0].json), R"({"t":"x"})");
    EXPECT_TRUE(s->find("other", all()).value().empty());
    EXPECT_EQ(s->used_bytes(), used);
    EXPECT_TRUE(h.timers.empty());
    EXPECT_EQ(s->commit_batch().error().code, ErrorCode::InvalidArgument);

    ASSERT_TRUE(Harness::flush_now(*s));
    EXPECT_EQ(read_raw(h.dir() / "kv.log"), kv_before);
    EXPECT_EQ(read_raw(h.dir() / "todos.log"), todos_before);
    EXPECT_FALSE(on_disk(h.dir() / "other.log"));
    EXPECT_EQ(s->insert("todos", R"({"t":"w"})").value(), 2);
}

TEST(SceneStore, ReadsInsideBatchSeeOverlay) {
    Harness h;
    auto s = h.open();
    ASSERT_TRUE(s->kv_set("a", "1"));
    ASSERT_TRUE(s->kv_set("gone", "1"));
    ASSERT_TRUE(s->insert("todos", R"({"t":"x","n":1})"));
    ASSERT_TRUE(s->insert("todos", R"({"t":"y","n":2})"));

    s->begin_batch();
    ASSERT_TRUE(s->kv_set("a", "2"));
    ASSERT_TRUE(s->kv_del("gone"));
    EXPECT_EQ(str(s->kv_get("a")), "2");
    EXPECT_FALSE(s->kv_get("gone"));
    EXPECT_EQ(s->insert("todos", R"({"t":"z","n":0})").value(), 3);
    ASSERT_TRUE(s->update("todos", 1, R"({"t":"x","n":9})"));
    ASSERT_TRUE(s->remove("todos", 2));
    EXPECT_EQ(s->update("todos", 2, "{}").error().code, ErrorCode::NotFound);
    FindQuery q;
    q.sort = "n";
    EXPECT_EQ(ids(s->find("todos", q).value()), (std::vector<int64_t>{3, 1}));
    // Nested batches flatten into the outer one.
    s->begin_batch();
    ASSERT_TRUE(s->insert("tmp", "{}"));
    ASSERT_TRUE(s->commit_batch());
    EXPECT_EQ(s->find("tmp", all()).value().size(), 1u);
    ASSERT_TRUE(s->commit_batch());

    EXPECT_EQ(ids(s->find("todos", q).value()), (std::vector<int64_t>{3, 1}));
    ASSERT_TRUE(Harness::flush_now(*s));
    auto again = h.open();
    EXPECT_EQ(str(again->kv_get("a")), "2");
    EXPECT_FALSE(again->kv_get("gone"));
    EXPECT_EQ(ids(again->find("todos", q).value()), (std::vector<int64_t>{3, 1}));
    EXPECT_EQ(again->find("tmp", all()).value().size(), 1u);
}

TEST(SceneStore, JournalReplayedOnLoad) {
    Harness h;
    write_raw(h.dir() / "kv.log", "{\"op\":\"put\",\"id\":\"a\",\"v\":1}\n");
    write_raw(h.dir() / "todos.log", "{\"op\":\"put\",\"id\":1,\"v\":{\"t\":\"old\"}}\n");
    write_raw(h.dir() / "journal.json", R"([{"f":"kv","r":{"op":"put","id":"a","v":2}},)"
                                        R"({"f":"todos","r":{"op":"put","id":2,"v":{"t":"new"}}},)"
                                        R"({"f":"todos","r":{"op":"del","id":1}},)"
                                        R"({"f":"fresh","r":{"op":"put","id":7,"v":{}}}])");
    {
        auto s = h.open();
        EXPECT_EQ(str(s->kv_get("a")), "2");
        EXPECT_EQ(ids(s->find("todos", all()).value()), (std::vector<int64_t>{2}));
        EXPECT_EQ(ids(s->find("fresh", all()).value()), (std::vector<int64_t>{7}));
        EXPECT_FALSE(on_disk(h.dir() / "journal.json"));
    }
    auto again = h.open();
    EXPECT_EQ(str(again->kv_get("a")), "2");
    EXPECT_EQ(ids(again->find("todos", all()).value()), (std::vector<int64_t>{2}));
    EXPECT_EQ(again->insert("todos", "{}").value(), 3);
    EXPECT_EQ(again->insert("fresh", "{}").value(), 8);
}

TEST(SceneStore, FlushOutlivesOwner) {
    Harness h;
    auto s = h.open();
    std::weak_ptr<SceneStore> weak = s;
    ASSERT_TRUE(s->kv_set("a", "1"));
    s.reset();
    EXPECT_FALSE(weak.expired());  // the armed timer keeps it
    h.fire_timers();
    EXPECT_EQ(lines_of(h.dir() / "kv.log").size(), 1u);
    EXPECT_TRUE(weak.expired());

    // A queued flush job keeps it too.
    h.sync_io = false;
    auto t = h.open();
    weak = t;
    ASSERT_TRUE(t->insert("todos", "{}"));
    bool done = false;
    t->flush([&](Result<void> r) { done = r.is_ok(); });
    h.timers.clear();
    t.reset();
    EXPECT_FALSE(weak.expired());
    h.run_queued();
    EXPECT_TRUE(done);
    EXPECT_TRUE(weak.expired());
    EXPECT_EQ(lines_of(h.dir() / "todos.log").size(), 1u);
}

TEST(SceneStore, WritesDuringFlushesAreNeitherLostNorDuplicated) {
    Harness h;
    wisp::core::StorageWorker worker;
    SceneStoreLimits limits;
    limits.compact_append_bytes = 2048;  // compact often
    SceneStoreDeps deps{*h.storage,
                        [&](std::function<void()> job) { return worker.submit(std::move(job)); },
                        [](uint32_t, std::function<void()> cb) { cb(); }};
    auto s = SceneStore::create(deps, h.dir().string(), limits);
    // Loaded before the worker -- the flash thread -- starts.
    ASSERT_TRUE(s->load());
    ASSERT_TRUE(worker.start());

    std::atomic<bool> stop{false};
    std::thread flusher([&] {
        while (!stop.load()) {
            s->flush();
            std::this_thread::yield();
        }
    });
    std::map<int64_t, std::string> model;
    std::map<std::string, std::string> kv_model;
    for (int i = 0; i < 1500; ++i) {
        std::string v = R"({"i":)" + std::to_string(i) + "}";
        switch (i % 5) {
            case 0:
            case 1: {
                auto id = s->insert("items", v);
                ASSERT_TRUE(id);
                model[id.value()] = v;
                break;
            }
            case 2:
                if (!model.empty()) {
                    auto it = model.begin();
                    std::advance(it, i % model.size());
                    ASSERT_TRUE(s->update("items", it->first, v));
                    it->second = v;
                }
                break;
            case 3:
                if (model.size() > 4) {
                    auto it = model.begin();
                    std::advance(it, i % model.size());
                    ASSERT_TRUE(s->remove("items", it->first));
                    model.erase(it);
                }
                break;
            default: {
                std::string key = "k" + std::to_string(i % 7);
                s->begin_batch();
                ASSERT_TRUE(s->kv_set(key, v));
                auto id = s->insert("items", v);
                ASSERT_TRUE(id);
                ASSERT_TRUE(s->commit_batch());
                kv_model[key] = v;
                model[id.value()] = v;
                break;
            }
        }
    }
    stop = true;
    flusher.join();
    ASSERT_TRUE(worker.drain(10000));  // so the last flush finds room in the queue
    std::atomic<bool> flushed{false};
    s->flush([&](Result<void> r) {
        EXPECT_TRUE(r) << r.error().message;
        flushed = true;
    });
    ASSERT_TRUE(worker.drain(10000));
    ASSERT_TRUE(flushed.load());
    worker.stop();
    expect_well_formed(h.dir() / "items.log");
    EXPECT_EQ(s->used_bytes(),
              fs::file_size(h.dir() / "items.log") + fs::file_size(h.dir() / "kv.log"));
    s.reset();

    auto again = h.open();
    auto found = again->find("items", all());
    ASSERT_TRUE(found);
    std::map<int64_t, std::string> got;
    for (const auto& r : found.value()) {
        got[r.id] = str(r.json);
    }
    EXPECT_EQ(got, model);
    for (const auto& [k, v] : kv_model) {
        EXPECT_EQ(str(again->kv_get(k)), v) << k;
    }
}

TEST(SceneStore, WritesBeforeLoadAreRefused) {
    Harness h;
    auto s = SceneStore::create(h.deps(), h.dir().string());
    EXPECT_EQ(s->kv_set("a", "1").error().code, ErrorCode::StorageUnavailable);
    EXPECT_EQ(s->insert("todos", "{}").error().code, ErrorCode::StorageUnavailable);
}

TEST(SceneStore, SingleFileBatchIsAllOrNothing) {
    Harness h;
    // A directory where kv.log belongs makes the append fail, so the
    // journal the flush wrote first stays behind.
    std::error_code ec;
    fs::create_directories(h.dir() / "kv.log", ec);
    {
        auto s = h.open();
        s->begin_batch();
        ASSERT_TRUE(s->kv_set("a", "1"));
        ASSERT_TRUE(s->kv_set("b", "2"));
        ASSERT_TRUE(s->commit_batch());
        EXPECT_FALSE(Harness::flush_now(*s));
        ASSERT_TRUE(on_disk(h.dir() / "journal.json"));
        auto journal = wisp::from_json(read_raw(h.dir() / "journal.json"));
        ASSERT_TRUE(journal);
        EXPECT_EQ(journal.value().as_list().size(), 2u);
        h.timers.clear();
    }
    // Power lost mid-append: the first line of the batch landed, the
    // second is torn.
    fs::remove(h.dir() / "kv.log", ec);
    write_raw(h.dir() / "kv.log",
              "{\"op\":\"put\",\"id\":\"a\",\"v\":1}\n{\"op\":\"put\",\"id\":\"b\"");
    auto again = h.open();
    EXPECT_EQ(str(again->kv_get("a")), "1");
    EXPECT_EQ(str(again->kv_get("b")), "2");
    EXPECT_FALSE(on_disk(h.dir() / "journal.json"));
    expect_well_formed(h.dir() / "kv.log");

    // Single writes outside a batch are appended without a journal.
    ASSERT_TRUE(again->kv_set("c", "3"));
    ASSERT_TRUE(again->kv_set("d", "4"));
    fs::remove(h.dir() / "kv.log", ec);
    fs::create_directories(h.dir() / "kv.log", ec);
    EXPECT_FALSE(Harness::flush_now(*again));
    EXPECT_FALSE(on_disk(h.dir() / "journal.json"));
    h.timers.clear();
}

TEST(SceneStore, PendingIsCappedAboveQuota) {
    Harness h;
    auto s = h.open();
    const std::string value = "\"" + std::string(15000, 'v') + "\"";
    for (int n = 0; s->kv_set("key" + std::to_string(n), value); ++n) {
    }
    ASSERT_TRUE(Harness::flush_now(*s));
    const uint64_t disk = fs::file_size(h.dir() / "kv.log");
    h.timers.clear();

    // Replacing a value is accepted past the quota (rewriting fits), but
    // not without bound before a flush gets to rewrite.
    Result<void> last = Result<void>::ok();
    int accepted = 0;
    for (; accepted < 500; ++accepted) {
        last = s->kv_set("key0", value);
        if (!last) {
            break;
        }
    }
    ASSERT_FALSE(last);
    EXPECT_GT(accepted, 1);
    EXPECT_EQ(last.error().code, ErrorCode::QuotaExceeded);
    EXPECT_EQ(last.error().message, "quota_exceeded");
    EXPECT_LE(s->used_bytes(), disk + 1024 * 1024);

    // The flush rewrites, and writes are accepted again.
    ASSERT_TRUE(Harness::flush_now(*s));
    EXPECT_LE(s->used_bytes(), 1024u * 1024);
    EXPECT_TRUE(s->kv_set("key0", value));
}

TEST(SceneStore, NestedAbortFailsTheOuterBatch) {
    Harness h;
    auto s = h.open();
    ASSERT_TRUE(s->kv_set("keep", "1"));
    ASSERT_TRUE(Harness::flush_now(*s));
    const std::string kv_before = read_raw(h.dir() / "kv.log");
    h.timers.clear();

    s->begin_batch();
    ASSERT_TRUE(s->kv_set("x", "1"));
    s->begin_batch();
    ASSERT_TRUE(s->kv_set("y", "2"));
    s->abort_batch();  // a caught inner error
    // Still inside the outer batch: writes stay in the overlay.
    ASSERT_TRUE(s->kv_set("a", "3"));
    ASSERT_TRUE(s->insert("todos", "{}"));
    EXPECT_TRUE(h.timers.empty());
    auto r = s->commit_batch();
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(r.error().message, "batch_aborted");

    for (const char* k : {"x", "y", "a"}) {
        EXPECT_FALSE(s->kv_get(k)) << k;
    }
    EXPECT_TRUE(s->find("todos", all()).value().empty());
    EXPECT_EQ(s->commit_batch().error().message, "no_batch");
    ASSERT_TRUE(Harness::flush_now(*s));
    EXPECT_EQ(read_raw(h.dir() / "kv.log"), kv_before);
    EXPECT_FALSE(on_disk(h.dir() / "todos.log"));

    // The store is usable again afterwards.
    s->begin_batch();
    ASSERT_TRUE(s->kv_set("z", "4"));
    ASSERT_TRUE(s->commit_batch());
    EXPECT_EQ(str(s->kv_get("z")), "4");
}

TEST(SceneStore, IdsUsedInsideABatchAreNotReused) {
    Harness h;
    {
        auto s = h.open();
        ASSERT_EQ(s->insert("todos", "{}").value(), 1);
        s->begin_batch();
        ASSERT_EQ(s->insert("todos", "{}").value(), 2);
        ASSERT_TRUE(s->remove("todos", 2));
        ASSERT_EQ(s->insert("fresh", "{}").value(), 1);
        ASSERT_TRUE(s->remove("fresh", 1));
        ASSERT_TRUE(s->commit_batch());
        ASSERT_TRUE(Harness::flush_now(*s));
    }
    auto again = h.open();
    EXPECT_EQ(again->insert("todos", "{}").value(), 3);
    EXPECT_EQ(again->insert("fresh", "{}").value(), 2);
}

TEST(SceneStore, FailedFlushRetriesWithBackoff) {
    Harness h;
    std::error_code ec;
    // A non-empty directory: write_atomic() cannot replace it either.
    fs::create_directories(h.dir() / "kv.log" / "x", ec);
    auto s = h.open();
    ASSERT_TRUE(s->kv_set("a", "1"));
    ASSERT_EQ(h.timers.size(), 1u);
    std::vector<uint32_t> delays;
    for (int i = 0; i < 7; ++i) {
        h.fire_timers();  // the flush fails and schedules a retry
        ASSERT_EQ(h.timers.size(), 1u) << i;
        delays.push_back(h.timers[0].first);
    }
    EXPECT_EQ(delays, (std::vector<uint32_t>{300, 600, 1200, 2400, 4800, 5000, 5000}));

    fs::remove_all(h.dir() / "kv.log", ec);
    h.fire_timers();
    EXPECT_TRUE(h.timers.empty());
    EXPECT_EQ(lines_of(h.dir() / "kv.log").size(), 1u);

    // A later failure starts from the short delay again.
    fs::create_directories(h.dir() / "todos.log" / "x", ec);
    ASSERT_TRUE(s->insert("todos", "{}"));
    h.fire_timers();
    ASSERT_EQ(h.timers.size(), 1u);
    EXPECT_EQ(h.timers[0].first, 300u);
    fs::remove_all(h.dir() / "todos.log", ec);
    h.fire_timers();
    EXPECT_TRUE(h.timers.empty());
    EXPECT_EQ(lines_of(h.dir() / "todos.log").size(), 1u);
}

TEST(SceneStore, ClosedStoreStopsRetrying) {
    Harness h;
    std::error_code ec;
    fs::create_directories(h.dir() / "kv.log" / "x", ec);  // unwritable
    auto s = h.open();
    std::weak_ptr<SceneStore> weak = s;
    ASSERT_TRUE(s->kv_set("a", "1"));
    EXPECT_FALSE(Harness::flush_now(*s));  // the final flush fails
    s->close();
    EXPECT_EQ(s->kv_set("b", "2").error().code, ErrorCode::StorageUnavailable);
    Result<void> after = Result<void>::ok();
    s->flush([&](Result<void> r) { after = std::move(r); });
    EXPECT_EQ(after.error().code, ErrorCode::StorageUnavailable);
    s.reset();

    // Storage recovers and the scene is reopened: the newer store's value
    // must not be followed by the closed store's older line.
    fs::remove_all(h.dir() / "kv.log", ec);
    auto fresh = h.open();
    ASSERT_TRUE(fresh->kv_set("a", "2"));
    ASSERT_TRUE(Harness::flush_now(*fresh));
    for (int i = 0; i < 5 && !h.timers.empty(); ++i) {
        h.fire_timers();
    }
    EXPECT_TRUE(h.timers.empty());
    EXPECT_TRUE(weak.expired());
    auto lines = lines_of(h.dir() / "kv.log");
    ASSERT_EQ(lines.size(), 1u);
    EXPECT_EQ(lines[0], R"({"op":"put","id":"a","v":2})");
    EXPECT_FALSE(on_disk(h.dir() / "journal.json"));
    auto again = h.open();
    EXPECT_EQ(str(again->kv_get("a")), "2");
}

// A timer job queued before close() but run after it must not write: the
// final flush already took (or gave back) its lines, and a store reopened
// on the directory owns it now.
TEST(SceneStore, TimerJobQueuedBeforeCloseIsSkipped) {
    Harness h;
    h.sync_io = false;
    std::error_code ec;
    fs::create_directories(h.dir() / "kv.log" / "x", ec);  // unwritable
    auto s = h.open();
    ASSERT_TRUE(s->kv_set("a", "1"));
    ASSERT_EQ(h.timers.size(), 1u);
    Result<void> final_flush = Result<void>::ok();
    s->flush([&](Result<void> r) { final_flush = std::move(r); });
    h.fire_timers();  // queues the timer's job behind the final flush
    ASSERT_EQ(h.queued.size(), 2u);
    s->close();
    auto jobs = std::move(h.queued);
    h.queued.clear();
    jobs[0]();  // the final flush fails
    EXPECT_FALSE(final_flush);
    fs::remove_all(h.dir() / "kv.log", ec);  // storage recovers
    jobs[1]();
    EXPECT_FALSE(on_disk(h.dir() / "kv.log"));
    EXPECT_TRUE(h.timers.empty());
}

TEST(SceneStore, ExplicitFlushKeepsOneRetryChain) {
    Harness h;
    std::error_code ec;
    fs::create_directories(h.dir() / "kv.log" / "x", ec);
    auto s = h.open();
    ASSERT_TRUE(s->kv_set("a", "1"));  // arms the coalescing timer
    ASSERT_EQ(h.timers.size(), 1u);
    EXPECT_FALSE(Harness::flush_now(*s));
    EXPECT_EQ(h.timers.size(), 1u);  // the outstanding timer is the retry
    h.fire_timers();
    ASSERT_EQ(h.timers.size(), 1u);
    EXPECT_EQ(h.timers[0].first, 300u);
    EXPECT_FALSE(Harness::flush_now(*s));
    EXPECT_EQ(h.timers.size(), 1u);
    h.fire_timers();
    ASSERT_EQ(h.timers.size(), 1u);
    EXPECT_EQ(h.timers[0].first, 600u);

    fs::remove_all(h.dir() / "kv.log", ec);
    h.fire_timers();
    EXPECT_TRUE(h.timers.empty());
    EXPECT_EQ(lines_of(h.dir() / "kv.log").size(), 1u);
}

TEST(SceneStore, BatchIdMarkerCountsAgainstQuota) {
    Harness h;
    SceneStoreLimits limits;
    limits.quota_bytes = 1000;
    auto s = h.open(limits);
    // A kv put line is 24 bytes + the quoted key + the value.
    auto value = [](size_t line) { return "\"" + std::string(line - 28 - 2, 'v') + "\""; };
    ASSERT_TRUE(s->kv_set("k0", value(500)));
    s->begin_batch();
    ASSERT_EQ(s->insert("todos", "{}").value(), 1);
    ASSERT_TRUE(s->remove("todos", 1));  // commit writes {"op":"del","id":1}: 20 bytes
    // 500 + 481 fits the quota, but not with the 20-byte line on top.
    auto r = s->kv_set("k1", value(481));
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().message, "quota_exceeded");
    ASSERT_TRUE(s->kv_set("k1", value(480)));
    ASSERT_TRUE(s->commit_batch());
    EXPECT_EQ(s->used_bytes(), 1000u);
    ASSERT_TRUE(Harness::flush_now(*s));
    EXPECT_EQ(s->used_bytes(),
              fs::file_size(h.dir() / "kv.log") + fs::file_size(h.dir() / "todos.log"));
}

// Spec 7.3: the data version is meta.json's, else 1 for data an older
// firmware wrote without one, else 0 for a store never written.
TEST(SceneStore, DataVersionDefaults) {
    {
        Harness h;
        auto s = h.open();
        EXPECT_EQ(s->data_version(), 0);
        // A new store takes the manifest's version, but writes it only
        // with its first data: a scene that stores nothing leaves no
        // data directory behind.
        s->set_initial_version(4);
        EXPECT_EQ(s->data_version(), 4);
        EXPECT_TRUE(h.timers.empty());
        ASSERT_TRUE(Harness::flush_now(*s));
        EXPECT_FALSE(on_disk(h.dir()));
        EXPECT_EQ(h.open()->data_version(), 0);

        ASSERT_TRUE(s->kv_set("a", "1"));
        ASSERT_TRUE(Harness::flush_now(*s));
        EXPECT_EQ(read_raw(h.dir() / "meta.json"), R"({"schema_version":4})");
        EXPECT_FALSE(on_disk(h.dir() / "journal.json"));
        auto again = h.open();
        EXPECT_EQ(again->data_version(), 4);
        EXPECT_EQ(str(again->kv_get("a")), "1");
    }
    {
        Harness h;
        write_raw(h.dir() / "kv.log", "{\"op\":\"put\",\"id\":\"a\",\"v\":1}\n");
        auto s = h.open();
        EXPECT_EQ(s->data_version(), 1);
        // Only a new store takes an initial version.
        s->set_initial_version(4);
        EXPECT_EQ(s->data_version(), 1);
        ASSERT_TRUE(Harness::flush_now(*s));
        EXPECT_FALSE(on_disk(h.dir() / "meta.json"));
    }
    {
        Harness h;
        write_raw(h.dir() / "kv.log", "{\"op\":\"put\",\"id\":\"a\",\"v\":1}\n");
        write_raw(h.dir() / "meta.json", R"({"schema_version":3})");
        EXPECT_EQ(h.open()->data_version(), 3);
    }
    {
        Harness h;
        write_raw(h.dir() / "meta.json", R"({"schema_version":5})");
        EXPECT_EQ(h.open()->data_version(), 5);
    }
    // A meta.json that does not parse is not guessed at: the data could
    // be at any version.
    for (const char* bad : {"{\"schema_version\":", "garbage", R"({"schema_version":0})",
                            R"({"schema_version":"2"})", R"({"other":2})"}) {
        Harness h;
        write_raw(h.dir() / "kv.log", "{\"op\":\"put\",\"id\":\"a\",\"v\":1}\n");
        write_raw(h.dir() / "meta.json", bad);
        auto s = h.open();
        EXPECT_EQ(s->data_version(), wisp::core::kDataVersionUnreadable) << bad;
        s->set_initial_version(4);
        EXPECT_EQ(s->data_version(), wisp::core::kDataVersionUnreadable) << bad;
        EXPECT_EQ(read_raw(h.dir() / "meta.json"), bad);
    }
}

// Spec 7.3: the migrated records and the new version reach the disk
// together through journal.json. A directory where write_atomic() puts
// its ".tmp" makes that write fail before it changes anything, which
// stands in for a power cut at that point: the store is then dropped
// without another flush.
TEST(SceneStore, MigrationCommitIsAtomicWithVersion) {
    const std::string v1 =
        "{\"op\":\"put\",\"id\":1,\"v\":{\"name\":\"a\"}}\n"
        "{\"op\":\"put\",\"id\":2,\"v\":{\"name\":\"b\"}}\n";
    auto migrate = [](SceneStore& s) {
        s.begin_batch();
        ASSERT_TRUE(s.update("items", 1, R"({"name":"a","done":false})"));
        ASSERT_TRUE(s.update("items", 2, R"({"name":"b","done":false})"));
        ASSERT_TRUE(s.commit_migration(2));
        EXPECT_EQ(s.data_version(), 2);
    };
    std::error_code ec;

    // Cut after the journal: the reload replays records and version.
    {
        Harness h;
        write_raw(h.dir() / "items.log", v1);
        {
            auto s = h.open();
            EXPECT_EQ(s->data_version(), 1);
            fs::create_directories(h.dir() / "meta.json.tmp", ec);
            migrate(*s);
            EXPECT_FALSE(Harness::flush_now(*s));
            ASSERT_TRUE(on_disk(h.dir() / "journal.json"));
            EXPECT_EQ(read_raw(h.dir() / "items.log"), v1);
            h.timers.clear();
        }
        fs::remove(h.dir() / "meta.json.tmp", ec);
        auto again = h.open();
        EXPECT_EQ(again->data_version(), 2);
        auto found = again->find("items", all());
        ASSERT_TRUE(found);
        ASSERT_EQ(found.value().size(), 2u);
        EXPECT_EQ(str(found.value()[0].json), R"({"name":"a","done":false})");
        EXPECT_EQ(str(found.value()[1].json), R"({"name":"b","done":false})");
        EXPECT_FALSE(on_disk(h.dir() / "journal.json"));
        EXPECT_EQ(read_raw(h.dir() / "meta.json"), R"({"schema_version":2})");
        EXPECT_EQ(h.open()->data_version(), 2);
    }

    // Cut before the journal: nothing changed, records nor version.
    {
        Harness h;
        write_raw(h.dir() / "items.log", v1);
        {
            auto s = h.open();
            fs::create_directories(h.dir() / "journal.json.tmp", ec);
            migrate(*s);
            EXPECT_FALSE(Harness::flush_now(*s));
            h.timers.clear();
        }
        fs::remove(h.dir() / "journal.json.tmp", ec);
        auto again = h.open();
        EXPECT_EQ(again->data_version(), 1);
        EXPECT_EQ(str(again->find("items", all()).value()[0].json), R"({"name":"a"})");
        EXPECT_FALSE(on_disk(h.dir() / "meta.json"));
        EXPECT_EQ(read_raw(h.dir() / "items.log"), v1);
    }

    // Cut after meta.json, before the logs: the journal is still there,
    // and its replay brings the logs up to the version.
    {
        Harness h;
        write_raw(h.dir() / "items.log", v1);
        write_raw(h.dir() / "meta.json", R"({"schema_version":2})");
        write_raw(h.dir() / "journal.json",
                  R"([{"f":"meta.json","r":{"schema_version":2}},)"
                  R"({"f":"items","r":{"op":"put","id":1,"v":{"name":"a","done":false}}},)"
                  R"({"f":"items","r":{"op":"put","id":2,"v":{"name":"b","done":false}}}])");
        auto again = h.open();
        EXPECT_EQ(again->data_version(), 2);
        EXPECT_EQ(str(again->find("items", all()).value()[0].json), R"({"name":"a","done":false})");
        EXPECT_FALSE(on_disk(h.dir() / "journal.json"));
        EXPECT_EQ(h.open()->data_version(), 2);
    }

    // Nothing in the way: everything lands and the journal goes.
    {
        Harness h;
        write_raw(h.dir() / "items.log", v1);
        auto s = h.open();
        migrate(*s);
        ASSERT_TRUE(Harness::flush_now(*s));
        EXPECT_FALSE(on_disk(h.dir() / "journal.json"));
        EXPECT_EQ(read_raw(h.dir() / "meta.json"), R"({"schema_version":2})");
        auto again = h.open();
        EXPECT_EQ(again->data_version(), 2);
        EXPECT_EQ(str(again->find("items", all()).value()[1].json), R"({"name":"b","done":false})");
    }

    // Without an open batch there is nothing to commit.
    {
        Harness h;
        auto s = h.open();
        auto r = s->commit_migration(2);
        ASSERT_FALSE(r);
        EXPECT_EQ(r.error().code, ErrorCode::InvalidArgument);
        EXPECT_EQ(s->data_version(), 0);
    }
}

// dump_json() (agent-core spec 4.2, scene.data_read): the committed data as
// one JSON document, cut at a size it never passes.
TEST(SceneStore, DumpJson) {
    Harness h;
    auto s = h.open();
    auto dump = [&](size_t max) {
        const wisp::os::LargeString d = s->dump_json(max);
        return std::string(d.data(), d.size());
    };
    EXPECT_EQ(dump(4096), R"({"kv":{},"collections":{}})");
    ASSERT_TRUE(s->kv_set("b", "2"));
    ASSERT_TRUE(s->kv_set("a", R"({"x":"y"})"));
    ASSERT_TRUE(s->insert("notes", R"({"t":"hi"})"));
    ASSERT_TRUE(s->insert("notes", "{}"));
    ASSERT_TRUE(s->insert("alpha", R"({"n":1})"));
    EXPECT_EQ(dump(4096), R"({"kv":{"a":{"x":"y"},"b":2},"collections":{"alpha":[{"id":1,"n":1}],)"
                          R"("notes":[{"id":1,"t":"hi"},{"id":2}]}})");
    // An open batch is not seen.
    s->begin_batch();
    ASSERT_TRUE(s->kv_set("c", "3"));
    EXPECT_EQ(dump(4096).find("\"c\""), std::string::npos);
    s->abort_batch();

    for (int i = 0; i < 50; ++i) ASSERT_TRUE(s->kv_set("k" + std::to_string(i), object_of(100)));
    for (size_t max : {size_t{64}, size_t{100}, size_t{300}, size_t{1000}, size_t{4000}}) {
        const std::string text = dump(max);
        EXPECT_LE(text.size(), max) << max;
        auto v = wisp::from_json(text);
        ASSERT_TRUE(v) << max << ": " << text;
        ASSERT_NE(v.value().find("truncated"), nullptr) << text;
        EXPECT_TRUE(v.value().find("truncated")->as_bool());
        EXPECT_NE(v.value().find("collections"), nullptr) << text;
    }
    const std::string whole = dump(1024 * 1024);
    EXPECT_EQ(whole.find("truncated"), std::string::npos);
}
