// GET/DELETE /memory and /todos (mvp spec 5.3): token and CSRF, listing and
// deleting, the local due time and the 503 when the services are missing.
// A real core::StorageWorker runs here (started, so it is the registered
// flash thread) so that a delete route which forgot to hop to it would
// abort under WISP_ASSERT_FLASH_THREAD rather than pass by accident.

#include <gtest/gtest.h>

#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <utility>

#include "admin/admin_fixture.hpp"
#include "support/fake_http.hpp"
#include "wisp/core/memory_store.hpp"
#include "wisp/core/reminders.hpp"
#include "wisp/core/storage_worker.hpp"

using wisp::ErrorCode;
using wisp::Result;
using wisp::Value;
using wisp::core::MemoryStore;
using wisp::core::ReminderDeps;
using wisp::core::ReminderScheduler;
using wisp::core::Repeat;
using wisp::core::RepeatRule;
using wisp::core::StorageWorker;
using wisp::net::HttpResponse;
using wisp::net::Method;
using wisp::test::AdminFixture;
using wisp::test::body_json;
using wisp::test::error_code;
using wisp::test::make_req;
using wisp::test::with_csrf;
using wisp::test::with_token;

namespace {

const Value& at(const Value& v, const char* key) {
    static const Value null;
    const Value* found = v.find(key);
    EXPECT_NE(found, nullptr) << "missing \"" << key << "\" in " << wisp::to_json(v);
    return found != nullptr ? *found : null;
}

// Runs `fn` on `worker` (the flash thread) and returns its Result,
// whether `fn` ran or the wait itself failed.
template <typename Fn>
Result<void> run_on_flash(StorageWorker& worker, Fn fn) {
    Result<void> r = Result<void>::err(ErrorCode::Busy, "not run");
    if (auto ran = worker.run_sync([&] { r = fn(); }, 5000); !ran) r = ran;
    return r;
}

class MemoryRoutes : public ::testing::Test {
protected:
    void SetUp() override {
        f.srv.reset();
        worker = std::make_unique<StorageWorker>();

        memory = std::make_unique<MemoryStore>(*f.storage, f.dir + "/memory/notes.json", &f.bus,
                                               [this] { return now; });
        reminders = std::make_unique<ReminderScheduler>(
            ReminderDeps{
                .storage = *f.storage,
                .bus = f.bus,
                .notes = nullptr,
                .submit_io = {},
                .wall_s = [this] { return now; },
                .utc_offset_min = [this] { return offset; },
                .missed_title = [](size_t n) { return std::to_string(n); },
                .make_timer = {},
            },
            f.dir + "/todo/todos.json");
        // Loaded, and the token fetched, before the worker starts: from
        // here on every flash touch (the routes', and this fixture's own
        // seeding helpers below) must hop to it, as at boot (see
        // AdminSceneFiles.DownloadReadsOnTheFlashThread for the same
        // ordering).
        ASSERT_TRUE(memory->load());
        ASSERT_TRUE(reminders->load());
        token = f.token();
        ASSERT_TRUE(worker->start());

        rebuild(/*with_services=*/true);
    }

    // Rebuilds the server; `with_services` false leaves deps.memory/todos
    // null (AdminFixture::make_deps()'s default).
    void rebuild(bool with_services) {
        wisp::admin::AdminDeps deps = f.make_deps();
        if (with_services) {
            deps.memory = memory.get();
            deps.todos = reminders.get();
            deps.worker = worker.get();
        }
        f.srv = std::make_unique<wisp::admin::AdminServer>(std::move(deps));
    }

    void TearDown() override {
        f.srv.reset();
        reminders.reset();
        memory.reset();
        if (worker) worker->stop();
    }

    HttpResponse get(const std::string& path) {
        return f.srv->handle(with_token(make_req(Method::Get, path), token));
    }
    HttpResponse del(const std::string& path) {
        return f.srv->handle(with_csrf(with_token(make_req(Method::Delete, path), token)));
    }

    // Seeding helpers: like the route handlers, every mutating call runs
    // on the worker -- `worker` is the registered flash thread once
    // started, so calling these services directly from the test body
    // would trip WISP_ASSERT_FLASH_THREAD just as a bug in the route
    // would.
    Result<uint32_t> add_note(std::string_view text) {
        Result<uint32_t> out = Result<uint32_t>::err(ErrorCode::Busy, "not run");
        if (auto ran = worker->run_sync([&] { out = memory->add(text); }, 5000); !ran) {
            out = Result<uint32_t>::err(ran.error().code, ran.error().message);
        }
        return out;
    }
    Result<uint32_t> add_todo(std::string_view title, std::optional<int64_t> due, RepeatRule r) {
        Result<uint32_t> out = Result<uint32_t>::err(ErrorCode::Busy, "not run");
        if (auto ran = worker->run_sync([&] { out = reminders->add(title, due, r); }, 5000); !ran) {
            out = Result<uint32_t>::err(ran.error().code, ran.error().message);
        }
        return out;
    }
    Result<void> snooze_todo(uint32_t id) {
        return run_on_flash(*worker, [&] { return reminders->snooze(id); });
    }

    AdminFixture f;
    std::unique_ptr<StorageWorker> worker;
    std::unique_ptr<MemoryStore> memory;
    std::unique_ptr<ReminderScheduler> reminders;
    std::string token;
    int64_t now = 1793923200;  // 2026-11-06T08:00 at UTC+8 (a Friday)
    int offset = 480;
};

}  // namespace

TEST_F(MemoryRoutes, NeedTokenAndCsrf) {
    for (const char* path : {"/api/v1/memory", "/api/v1/todos"}) {
        HttpResponse res = f.srv->handle(make_req(Method::Get, path));
        EXPECT_EQ(res.status, 401) << path;
        EXPECT_EQ(error_code(res), "unauthorized") << path;
    }
    for (const char* path : {"/api/v1/memory/1", "/api/v1/todos/1"}) {
        HttpResponse res =
            f.srv->handle(with_token(make_req(Method::Delete, path), token));  // no X-Wisp
        EXPECT_EQ(res.status, 403) << path;
        EXPECT_EQ(error_code(res), "csrf") << path;
    }
}

TEST_F(MemoryRoutes, ListAndDelete) {
    auto id = add_note("称呼：小陈");
    ASSERT_TRUE(id);

    Value notes = body_json(get("/api/v1/memory"));
    const Value& list = at(notes, "notes");
    ASSERT_EQ(list.type(), Value::Type::List);
    ASSERT_EQ(list.as_list().size(), 1u);
    EXPECT_EQ(at(list.as_list()[0], "id"), Value(static_cast<int64_t>(id.value())));
    EXPECT_EQ(at(list.as_list()[0], "text"), Value(std::string("称呼：小陈")));

    HttpResponse deleted = del("/api/v1/memory/" + std::to_string(id.value()));
    EXPECT_EQ(deleted.status, 204);
    EXPECT_TRUE(deleted.body.empty());

    Value after = body_json(get("/api/v1/memory"));
    EXPECT_TRUE(at(after, "notes").as_list().empty());

    HttpResponse again = del("/api/v1/memory/" + std::to_string(id.value()));
    EXPECT_EQ(again.status, 404);
    EXPECT_EQ(error_code(again), "not_found");

    HttpResponse bad_id = del("/api/v1/memory/not-a-number");
    EXPECT_EQ(bad_id.status, 404);

    // Todos: the same shape of round trip.
    auto todo_id = add_todo("买菜", std::nullopt, {});
    ASSERT_TRUE(todo_id);
    Value todos = body_json(get("/api/v1/todos"));
    const Value& todo_list = at(todos, "todos");
    ASSERT_EQ(todo_list.as_list().size(), 1u);
    EXPECT_EQ(at(todo_list.as_list()[0], "title"), Value(std::string("买菜")));
    EXPECT_EQ(at(todo_list.as_list()[0], "done"), Value(false));

    HttpResponse todo_deleted = del("/api/v1/todos/" + std::to_string(todo_id.value()));
    EXPECT_EQ(todo_deleted.status, 204);
    EXPECT_TRUE(body_json(get("/api/v1/todos")).find("todos")->as_list().empty());

    HttpResponse todo_again = del("/api/v1/todos/" + std::to_string(todo_id.value()));
    EXPECT_EQ(todo_again.status, 404);
    EXPECT_EQ(error_code(todo_again), "not_found");
}

TEST_F(MemoryRoutes, TodosShowLocalDue) {
    const auto due = wisp::core::parse_local_minute("2026-11-07T08:00", offset);
    ASSERT_TRUE(due.has_value());
    auto id = add_todo("取药", due, RepeatRule{Repeat::Daily, 0});
    ASSERT_TRUE(id);
    ASSERT_TRUE(snooze_todo(id.value()));

    Value todos = body_json(get("/api/v1/todos"));
    const Value& list = at(todos, "todos");
    ASSERT_EQ(list.as_list().size(), 1u);
    const Value& t = list.as_list()[0];
    EXPECT_EQ(at(t, "id"), Value(static_cast<int64_t>(id.value())));
    EXPECT_EQ(at(t, "title"), Value(std::string("取药")));
    EXPECT_EQ(at(t, "done"), Value(false));
    EXPECT_EQ(at(t, "due_local"), Value(std::string("2026-11-07T08:00")));
    EXPECT_EQ(at(t, "repeat"), Value("daily"));
    // snooze() rings kSnoozeS (600 s) from `now`, formatted the same way.
    EXPECT_EQ(at(t, "snooze_until"),
              Value(wisp::core::format_local_minute(now + ReminderScheduler::kSnoozeS, offset)));
}

TEST_F(MemoryRoutes, UnavailableWithoutServices) {
    rebuild(/*with_services=*/false);
    for (const char* path : {"/api/v1/memory", "/api/v1/todos"}) {
        HttpResponse res = get(path);
        EXPECT_EQ(res.status, 503) << path;
        EXPECT_EQ(error_code(res), "unavailable") << path;
    }
    for (const char* path : {"/api/v1/memory/1", "/api/v1/todos/1"}) {
        HttpResponse res = del(path);
        EXPECT_EQ(res.status, 503) << path;
        EXPECT_EQ(error_code(res), "unavailable") << path;
    }
}
