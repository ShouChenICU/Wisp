// SceneRuntime::eval(), store_dump() and store_set(): the agent runs a chunk
// of Lua in a running scene's VM and reads or writes its store, each as a
// message of the scene task (agent-core spec 4.2).

#include <gtest/gtest.h>

#include <chrono>
#include <string>

#include "support/scene_runtime_fixture.hpp"
#include "support/wait.hpp"
#include "wisp/base/json.hpp"
#include "wisp/core/scene_host.hpp"
#include "wisp/os/os.hpp"
#include "wisp/scene/scene_runtime.hpp"

namespace {

using wisp::ErrorCode;
using wisp::core::SceneState;
using wisp::test::wait_until;

// The count state is published in the scene table, which eval code can
// reach (an undeclared global would not compile).
const char* kCounter = R"(local count = state(0)
scene.count = count
return ui.screen {
  ui.column { pad = 60, gap = 12,
    ui.label { text = bind(count, function(v) return "count " .. v end) },
  },
}
)";

class SceneEval : public wisp::test::SceneRuntimeFixture {
protected:
    void begin() {
        install("counter", kCounter);
        start();
        ASSERT_TRUE(rt->open("counter"));
        ASSERT_TRUE(wait_label("count 0"));
    }

    std::string eval_ok(const char* lua) {
        auto r = rt->eval("counter", lua);
        EXPECT_TRUE(r) << lua << ": " << (r ? "" : r.error().message);
        return r ? std::string(r.value().data(), r.value().size()) : std::string();
    }
};

using SceneStoreAccess = SceneEval;

}  // namespace

TEST_F(SceneEval, ReturnsJsonOfResults) {
    begin();
    EXPECT_EQ(eval_ok("return 1+1"), "2");
    EXPECT_EQ(eval_ok("return {a=1}"), R"({"a":1})");
    EXPECT_EQ(eval_ok("return 1, 'x'"), R"([1,"x"])");
    EXPECT_EQ(eval_ok("local x = 1"), "null");
    EXPECT_EQ(eval_ok("return nil"), "null");
    // A value JSON cannot hold is an error, and the scene goes on.
    auto fn = rt->eval("counter", "return function() end");
    ASSERT_FALSE(fn);
    EXPECT_NE(fn.error().message.find("not_serializable"), std::string::npos);
    EXPECT_EQ(rt->status("counter").state, SceneState::Running);
}

TEST_F(SceneEval, ErrorsDoNotFaultTheScene) {
    begin();
    auto r = rt->eval("counter", "error('x')");
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(r.error().message, "eval:1: x");
    auto syntax = rt->eval("counter", "return +");
    ASSERT_FALSE(syntax);
    EXPECT_EQ(syntax.error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(rt->status("counter").state, SceneState::Running);
    EXPECT_EQ(eval_ok("return scene.count:get()"), "0");
}

TEST_F(SceneEval, BudgetIs200ms) {
    begin();
    const uint64_t t0 = wisp::os::now_ms();
    auto r = rt->eval("counter", "while true do end");
    const uint64_t took = wisp::os::now_ms() - t0;
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, ErrorCode::InvalidArgument);
    EXPECT_NE(r.error().message.find("200"), std::string::npos) << r.error().message;
    EXPECT_GE(took, 190u);
    EXPECT_LT(took, 1000u);
    EXPECT_EQ(rt->status("counter").state, SceneState::Running);
    // The VM goes on with a full budget for the next message.
    EXPECT_EQ(eval_ok("return 3"), "3");
    EXPECT_EQ(eval_ok("scene.count:set(2) return scene.count:get()"), "2");
    EXPECT_TRUE(wait_label("count 2"));
    // A shorter budget of its own.
    auto quick = rt->eval("counter", "while true do end", 50);
    ASSERT_FALSE(quick);
    EXPECT_NE(quick.error().message.find("50"), std::string::npos) << quick.error().message;
    EXPECT_EQ(rt->status("counter").state, SceneState::Running);
}

TEST_F(SceneEval, UiChangesAreApplied) {
    begin();
    EXPECT_EQ(eval_ok("scene.count:set(7)"), "null");
    EXPECT_TRUE(wait_label("count 7")) << label("count");
}

TEST_F(SceneEval, NotRunningIsNotFound) {
    install("counter", kCounter);
    start();
    auto r = rt->eval("counter", "return 1");
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, ErrorCode::NotFound);
    EXPECT_EQ(rt->store_dump("counter", 4096).error().code, ErrorCode::NotFound);
    EXPECT_EQ(rt->store_set("counter", "k", "1").error().code, ErrorCode::NotFound);
}

TEST_F(SceneStoreAccess, DumpAndSetWhileRunning) {
    begin();
    eval_ok("wisp.store.set('a', {b = 1}) wisp.store.collection('notes'):insert({t = 'hi'})");
    auto dump = rt->store_dump("counter", 4096);
    ASSERT_TRUE(dump) << dump.error().message;
    EXPECT_EQ(std::string(dump.value().data(), dump.value().size()),
              R"({"kv":{"a":{"b":1}},"collections":{"notes":[{"id":1,"t":"hi"}]}})");

    ASSERT_TRUE(rt->store_set("counter", "k", R"({"x":[1,2]})"));
    EXPECT_EQ(eval_ok("return wisp.store.get('k')"), R"({"x":[1,2]})");
    auto bad = rt->store_set("counter", "k", "{nope");
    ASSERT_FALSE(bad);
    EXPECT_EQ(bad.error().code, ErrorCode::InvalidArgument);

    // Cut short: never longer than asked, and says so.
    eval_ok("for i = 1, 40 do wisp.store.set('key' .. i, string.rep('v', 40)) end");
    auto cut = rt->store_dump("counter", 512);
    ASSERT_TRUE(cut);
    const std::string text(cut.value().data(), cut.value().size());
    EXPECT_LE(text.size(), 512u);
    auto parsed = wisp::from_json(text);
    ASSERT_TRUE(parsed) << text;
    ASSERT_NE(parsed.value().find("truncated"), nullptr);
    EXPECT_TRUE(parsed.value().find("truncated")->as_bool());
}
