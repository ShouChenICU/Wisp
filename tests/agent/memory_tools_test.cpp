// The memory tools (mvp spec 3.2, 5.1): memory.list, add, update and
// remove over MemoryStore, all on the flash thread.

#include <gtest/gtest.h>

#include <functional>
#include <memory>
#include <string>
#include <utility>

#include "support/scene_runtime_fixture.hpp"
#include "wisp/agent/tool_registry.hpp"
#include "wisp/agent/tools.hpp"
#include "wisp/base/json.hpp"
#include "wisp/core/memory_store.hpp"

namespace {

using wisp::Map;
using wisp::Value;
using wisp::agent::ToolCtx;
using wisp::agent::ToolDef;
using wisp::agent::ToolOutput;
using wisp::agent::ToolRisk;
using wisp::agent::ToolThread;
using wisp::core::MemoryStore;

std::string str(const Value& v, std::string_view key) {
    const Value* f = v.find(key);
    return f != nullptr && f->type() == Value::Type::String ? f->as_string() : std::string();
}

class MemoryTools : public wisp::test::SceneRuntimeFixture {
protected:
    void SetUp() override {
        SceneRuntimeFixture::SetUp();
        ctx.cancelled = [] { return false; };
        ctx.lang = "zh";
        const std::string path =
            storage->write_root(wisp::core::Area::System).value() + "/memory/notes.json";
        store =
            std::make_unique<MemoryStore>(*storage, path, &bus, [] { return int64_t{1700000000}; });
        ASSERT_TRUE(worker.run_sync([this] { ASSERT_TRUE(store->load()); }, wisp::os::kForever));
        tools = std::make_unique<wisp::agent::ToolRegistry>(wisp::agent::ToolExecutors{
            .fs = [this](std::function<void()> fn) { return worker.submit(std::move(fn)); },
        });
        ASSERT_TRUE(
            wisp::agent::add_memory_tools(*tools, wisp::agent::MemoryToolDeps{.store = *store}));
    }
    void TearDown() override {
        tools.reset();
        store.reset();
        SceneRuntimeFixture::TearDown();
    }

    ToolOutput call(const char* wire, const std::string& args) {
        return tools->call(wire, args, ctx);
    }

    std::unique_ptr<MemoryStore> store;
    std::unique_ptr<wisp::agent::ToolRegistry> tools;
    ToolCtx ctx;
};

}  // namespace

TEST_F(MemoryTools, RiskThreadTimeout) {
    struct Case {
        const char* wire;
        ToolRisk risk;
    };
    for (const Case& c :
         {Case{"memory_list", ToolRisk::Normal}, Case{"memory_add", ToolRisk::Note},
          Case{"memory_update", ToolRisk::Note}, Case{"memory_remove", ToolRisk::Note}}) {
        const ToolDef* def = tools->find_wire(c.wire);
        ASSERT_NE(def, nullptr) << c.wire;
        EXPECT_EQ(def->risk, c.risk) << c.wire;
        EXPECT_EQ(def->thread, ToolThread::Fs) << c.wire;
        EXPECT_EQ(def->timeout_ms, 5000u) << c.wire;
    }
}

TEST_F(MemoryTools, ErrorsAndNotes) {
    ToolOutput added = call("memory_add", R"({"text":"称呼：小陈"})");
    ASSERT_TRUE(added.ok) << wisp::to_json(added.result);
    EXPECT_EQ(added.note, "已记住一条记忆");
    const int64_t id = added.result.find("id")->as_int();

    ToolOutput listed = call("memory_list", "");
    ASSERT_TRUE(listed.ok);
    ASSERT_EQ(listed.result.find("notes")->as_list().size(), 1u);

    ToolOutput updated =
        call("memory_update", R"({"id":)" + std::to_string(id) + R"(,"text":"称呼：陈先生"})");
    ASSERT_TRUE(updated.ok) << wisp::to_json(updated.result);
    EXPECT_EQ(updated.note, "已修改记忆 #" + std::to_string(id));

    ToolOutput too_long = call("memory_add", R"({"text":")" + std::string(201, 'a') + R"("})");
    ASSERT_FALSE(too_long.ok);
    EXPECT_EQ(str(too_long.result, "error"), "too_long");

    ToolOutput empty = call("memory_add", R"({"text":""})");
    ASSERT_FALSE(empty.ok);
    EXPECT_EQ(str(empty.result, "error"), "empty");

    for (int i = 0; i < 63; ++i) {
        ToolOutput r = call("memory_add", R"({"text":"filler )" + std::to_string(i) + R"("})");
        ASSERT_TRUE(r.ok) << i;
    }
    ToolOutput full = call("memory_add", R"({"text":"one too many"})");
    ASSERT_FALSE(full.ok);
    EXPECT_EQ(str(full.result, "error"), "memory_full");

    ToolOutput removed = call("memory_remove", R"({"id":)" + std::to_string(id) + "}");
    ASSERT_TRUE(removed.ok) << wisp::to_json(removed.result);
    EXPECT_EQ(removed.note, "已删除记忆 #" + std::to_string(id));

    ToolOutput not_found = call("memory_remove", R"({"id":)" + std::to_string(id) + "}");
    ASSERT_FALSE(not_found.ok);
    EXPECT_EQ(str(not_found.result, "error"), "not_found");
}
