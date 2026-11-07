// The todo tools (mvp spec 3.2, 5.2): todo.list, add, update, done and
// remove over ReminderScheduler, all on the flash thread.

#include <gtest/gtest.h>

#include <atomic>
#include <functional>
#include <memory>
#include <string>
#include <utility>

#include "support/scene_runtime_fixture.hpp"
#include "wisp/agent/tool_registry.hpp"
#include "wisp/agent/tools.hpp"
#include "wisp/base/json.hpp"
#include "wisp/core/reminders.hpp"

namespace {

using wisp::Value;
using wisp::agent::ToolCtx;
using wisp::agent::ToolDef;
using wisp::agent::ToolOutput;
using wisp::agent::ToolRisk;
using wisp::agent::ToolThread;
using wisp::core::ReminderScheduler;
using wisp::core::TodoFilter;

// 2026-11-06T08:00 at UTC+8.
constexpr int64_t kT = 1793923200;

std::string str(const Value& v, std::string_view key) {
    const Value* f = v.find(key);
    return f != nullptr && f->type() == Value::Type::String ? f->as_string() : std::string();
}

class TodoTools : public wisp::test::SceneRuntimeFixture {
protected:
    void SetUp() override {
        SceneRuntimeFixture::SetUp();
        ctx.cancelled = [] { return false; };
        ctx.lang = "zh";
        sched = std::make_unique<ReminderScheduler>(
            wisp::core::ReminderDeps{
                .storage = *storage,
                .bus = bus,
                .notes = nullptr,
                .submit_io =
                    [this](std::function<void()> j) { return worker.submit(std::move(j)); },
                .wall_s = [this] { return now.load(); },
                .utc_offset_min = [] { return 480; },
                .missed_title = {},
                .make_timer =
                    [](std::function<void()>) {
                        return wisp::core::ReminderTimer{.arm = [](uint32_t) {}, .cancel = [] {}};
                    },
            },
            storage->write_root(wisp::core::Area::System).value() + "/todo/todos.json");
        ASSERT_TRUE(worker.run_sync([this] { ASSERT_TRUE(sched->load()); }, wisp::os::kForever));
        tools = std::make_unique<wisp::agent::ToolRegistry>(wisp::agent::ToolExecutors{
            .fs = [this](std::function<void()> fn) { return worker.submit(std::move(fn)); },
        });
        ASSERT_TRUE(
            wisp::agent::add_todo_tools(*tools, wisp::agent::TodoToolDeps{.scheduler = *sched}));
    }
    void TearDown() override {
        tools.reset();
        ASSERT_TRUE(worker.run_sync([this] { sched.reset(); }, wisp::os::kForever));
        SceneRuntimeFixture::TearDown();
    }

    ToolOutput call(const char* wire, const std::string& args) {
        return tools->call(wire, args, ctx);
    }

    std::atomic<int64_t> now{kT};
    std::unique_ptr<ReminderScheduler> sched;
    std::unique_ptr<wisp::agent::ToolRegistry> tools;
    ToolCtx ctx;
};

}  // namespace

TEST_F(TodoTools, AddParsesDueAndRepeat) {
    struct Case {
        const char* wire;
        ToolRisk risk;
    };
    for (const Case& c : {Case{"todo_list", ToolRisk::Normal}, Case{"todo_add", ToolRisk::Note},
                          Case{"todo_update", ToolRisk::Note}, Case{"todo_done", ToolRisk::Note},
                          Case{"todo_remove", ToolRisk::Note}}) {
        const ToolDef* def = tools->find_wire(c.wire);
        ASSERT_NE(def, nullptr) << c.wire;
        EXPECT_EQ(def->risk, c.risk) << c.wire;
        EXPECT_EQ(def->thread, ToolThread::Fs) << c.wire;
        EXPECT_EQ(def->timeout_ms, 5000u) << c.wire;
    }

    ToolOutput added = call("todo_add", R"({"title":"带伞","due":"2026-11-07T08:00"})");
    ASSERT_TRUE(added.ok) << wisp::to_json(added.result);
    EXPECT_EQ(added.note, "已添加待办「带伞」");
    EXPECT_EQ(added.result.find("id")->as_int(), 1);
    auto all = sched->list(TodoFilter::All);
    ASSERT_EQ(all.size(), 1u);
    EXPECT_EQ(all[0].due, std::optional<int64_t>(1794009600));

    ToolOutput weekly = call(
        "todo_add", R"({"title":"周会","due":"2026-11-09T10:00","repeat":{"weekly":[1,3,5]}})");
    ASSERT_TRUE(weekly.ok) << wisp::to_json(weekly.result);
    EXPECT_EQ(sched->list(TodoFilter::All)[1].repeat.days, 0b10101);
    ToolOutput daily =
        call("todo_add", R"({"title":"吃药","due":"2026-11-07T09:00","repeat":"daily"})");
    ASSERT_TRUE(daily.ok) << wisp::to_json(daily.result);
    ToolOutput undated = call("todo_add", R"({"title":"买牛奶"})");
    ASSERT_TRUE(undated.ok) << wisp::to_json(undated.result);

    for (const char* args :
         {R"({"title":"x","due":"tomorrow 8am"})", R"({"title":"x","due":"2026-02-30T08:00"})",
          R"({"title":"x","due":1794009600})"}) {
        ToolOutput bad = call("todo_add", args);
        ASSERT_FALSE(bad.ok) << args;
        EXPECT_EQ(str(bad.result, "error"), "bad_args") << args;
        EXPECT_EQ(str(bad.result, "field"), "due") << args;
    }
    for (const char* args : {R"({"title":"x","due":"2026-11-07T08:00","repeat":"hourly"})",
                             R"({"title":"x","due":"2026-11-07T08:00","repeat":{"weekly":[]}})",
                             R"({"title":"x","due":"2026-11-07T08:00","repeat":{"weekly":[9]}})"}) {
        ToolOutput bad = call("todo_add", args);
        ASSERT_FALSE(bad.ok) << args;
        EXPECT_EQ(str(bad.result, "error"), "bad_args") << args;
        EXPECT_EQ(str(bad.result, "field"), "repeat") << args;
    }
    EXPECT_EQ(sched->list(TodoFilter::All).size(), 4u);

    ToolOutput updated =
        call("todo_update", R"({"id":1,"fields":{"title":"带雨伞","due":"2026-11-07T07:30"}})");
    ASSERT_TRUE(updated.ok) << wisp::to_json(updated.result);
    EXPECT_EQ(updated.note, "已修改待办「带雨伞」");
    EXPECT_EQ(sched->list(TodoFilter::All)[0].due, std::optional<int64_t>(1794009600 - 1800));
    ToolOutput cleared = call("todo_update", R"({"id":1,"fields":{"due":null}})");
    ASSERT_TRUE(cleared.ok) << wisp::to_json(cleared.result);
    EXPECT_EQ(sched->list(TodoFilter::All)[0].due, std::nullopt);
    ToolOutput bad_update = call("todo_update", R"({"id":1,"fields":{"due":"soon"}})");
    ASSERT_FALSE(bad_update.ok);
    EXPECT_EQ(str(bad_update.result, "field"), "fields.due");

    ToolOutput done = call("todo_done", R"({"id":1})");
    ASSERT_TRUE(done.ok) << wisp::to_json(done.result);
    EXPECT_EQ(done.note, "已完成「带雨伞」");
    ToolOutput removed = call("todo_remove", R"({"id":4})");
    ASSERT_TRUE(removed.ok) << wisp::to_json(removed.result);
    EXPECT_EQ(removed.note, "已删除待办「买牛奶」");
}

TEST_F(TodoTools, ListShowsLocalTimesAndErrorsMap) {
    ASSERT_TRUE(call("todo_add", R"({"title":"带伞","due":"2026-11-07T08:00"})").ok);
    ASSERT_TRUE(
        call("todo_add", R"({"title":"今天","due":"2026-11-06T20:00","repeat":"daily"})").ok);
    ASSERT_TRUE(call("todo_add", R"({"title":"买牛奶"})").ok);
    ASSERT_TRUE(call("todo_done", R"({"id":3})").ok);

    ToolOutput listed = call("todo_list", "");
    ASSERT_TRUE(listed.ok) << wisp::to_json(listed.result);
    const auto& open = listed.result.find("todos")->as_list();
    ASSERT_EQ(open.size(), 2u);
    EXPECT_EQ(str(open[0], "title"), "带伞");
    EXPECT_EQ(str(open[0], "due"), "2026-11-07T08:00");
    EXPECT_EQ(wisp::to_json(*open[0].find("repeat")), R"("none")");
    EXPECT_EQ(wisp::to_json(*open[1].find("repeat")), R"("daily")");
    EXPECT_FALSE(open[0].find("done")->as_bool());

    EXPECT_EQ(call("todo_list", R"({"filter":"done"})").result.find("todos")->as_list().size(), 1u);
    EXPECT_EQ(call("todo_list", R"({"filter":"all"})").result.find("todos")->as_list().size(), 3u);
    const auto today = call("todo_list", R"({"filter":"due_today"})");
    ASSERT_EQ(today.result.find("todos")->as_list().size(), 1u);
    EXPECT_EQ(str(today.result.find("todos")->as_list()[0], "title"), "今天");
    const ToolOutput bad_filter = call("todo_list", R"({"filter":"soon"})");
    EXPECT_EQ(str(bad_filter.result, "error"), "bad_args");

    ToolOutput too_long = call("todo_add", R"({"title":")" + std::string(101, 'a') + R"("})");
    EXPECT_EQ(str(too_long.result, "error"), "too_long");
    ToolOutput empty = call("todo_add", R"({"title":""})");
    EXPECT_EQ(str(empty.result, "error"), "empty");
    ToolOutput not_found = call("todo_done", R"({"id":99})");
    EXPECT_EQ(str(not_found.result, "error"), "not_found");
    EXPECT_EQ(str(call("todo_remove", R"({"id":99})").result, "error"), "not_found");
    EXPECT_EQ(str(call("todo_update", R"({"id":99,"fields":{"title":"x"}})").result, "error"),
              "not_found");

    for (int i = 3; i < 200; ++i) {
        ASSERT_TRUE(call("todo_add", R"({"title":"t)" + std::to_string(i) + R"("})").ok) << i;
    }
    EXPECT_EQ(str(call("todo_add", R"({"title":"full"})").result, "error"), "todo_full");

    now = 0;
    ASSERT_TRUE(call("todo_remove", R"({"id":5})").ok);
    ToolOutput no_clock = call("todo_add", R"({"title":"x","due":"2026-11-07T08:00"})");
    EXPECT_EQ(str(no_clock.result, "error"), "clock_not_set");
    // Without a clock the list still answers, without due_today.
    EXPECT_TRUE(call("todo_list", R"({"filter":"due_today"})").ok);
}
