#pragma once

// Test-only: a scene VM (prelude and ui table installed) driven on the
// test thread the way the scene task drives it, wired to a SceneView on a
// UiRuntime (scene_view_fixture.hpp). open() runs main.lua, collects the
// tree, sends the Build and flushes; message() runs one more chunk and
// flushes; pump() applies what the batch holds to the view and answers
// the list ranges the view asks for, until nothing is left. Chunks get the
// internal table as `...`.

#include <gtest/gtest.h>

#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "support/scene_view_fixture.hpp"
#include "wisp/scene/patch_batch.hpp"
#include "wisp/scene/reactive.hpp"
#include "wisp/scene/schema.hpp"
#include "wisp/scene/vm.hpp"

namespace wisp::test {

inline std::string describe(const std::optional<scene::ScriptError>& err) {
    return err ? err->text() : std::string("(no error)");
}

class SceneHarness : public SceneViewTest {
protected:
    static constexpr uint32_t kInstance = 1;

    // Creates the VM; start() must have built the view.
    void make_vm() {
        vm = scene::Vm::create("t");
        ASSERT_TRUE(vm);
        auto err = scene::install_prelude(*vm, batch);
        if (!err) err = scene::install_ui(*vm);
        ASSERT_FALSE(err) << describe(err);
    }

    // Runs `src` as main.lua and builds what it returns. The error, if
    // any, is the scene's.
    std::optional<scene::ScriptError> open(std::string_view src) {
        if (!vm) make_vm();
        lua_State* L = vm->L();
        if (auto err = vm->load("main.lua", src)) return err;
        if (!scene::push_wisp_internal(L)) lua_pushnil(L);
        if (auto err = vm->call(1, 1)) return err;
        scene::Build tree;
        auto err = scene::collect_tree(*vm, -1, next_id, tree);
        lua_pop(L, 1);
        if (err) return err;
        auto pushed = batch.push(std::move(tree));
        EXPECT_TRUE(pushed.is_ok());
        if (auto flushed = scene::flush_reactive(*vm)) return flushed;
        return pump();
    }

    std::optional<scene::ScriptError> message(std::string_view src) {
        lua_State* L = vm->L();
        if (auto err = vm->load("main.lua", src)) return err;
        if (!scene::push_wisp_internal(L)) lua_pushnil(L);
        if (auto err = vm->call(1, 0)) return err;
        if (auto err = scene::flush_reactive(*vm)) return err;
        return pump();
    }

    // Applies the batch to the view and answers its list ranges, until
    // neither has anything left.
    std::optional<scene::ScriptError> pump() {
        for (int round = 0; round < 100; ++round) {
            scene::UiBatch b = batch.take(kInstance);
            if (!b.ops.empty()) {
                ops_applied += b.ops.size();
                on_ui([&] {
                    for (const auto& op : b.ops) view->apply(op);
                });
                for (auto& op : b.ops) applied.push_back(std::move(op));
            }
            auto asked = take_ranges();
            for (const auto& r : asked) all_ranges.push_back(r);
            if (asked.empty() && b.ops.empty()) return std::nullopt;
            for (const auto& r : asked) {
                if (auto err = scene::build_list_rows(*vm, r.list, r.first, r.count, next_id))
                    return err;
                if (auto err = scene::flush_reactive(*vm)) return err;
            }
        }
        ADD_FAILURE() << "the view and the scene kept asking each other for updates";
        return std::nullopt;
    }

    scene::PatchBatch batch;
    std::unique_ptr<scene::Vm> vm;
    scene::NodeId next_id = 1;
    size_t ops_applied = 0;
    std::vector<RecordedRange> all_ranges;
    // Every op applied so far, in order.
    std::vector<scene::UiOp> applied;
};

}  // namespace wisp::test
