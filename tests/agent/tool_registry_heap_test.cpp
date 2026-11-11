// Where ToolRegistry keeps its data. On the device an ordinary new/malloc
// under 16 KB lands in internal SRAM (AGENTS.md section 8), so the tool
// definitions, their index and the request's "tools" JSON must all go
// through os::alloc(Mem::Large). The simulator's os::alloc is plain
// malloc, which this binary does not count, so every byte the counting
// operator new below still sees alive after registration is a byte that
// would stay in internal RAM.
//
// This binary replaces the global operator new/delete, so it holds only
// these tests.

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <new>
#include <string>

#include "support/scene_runtime_fixture.hpp"
#include "wisp/agent/tool_index.hpp"
#include "wisp/agent/tool_registry.hpp"
#include "wisp/agent/tools.hpp"
#include "wisp/base/json.hpp"

namespace {

// Every block carries a header saying whether it was allocated while
// counting, so a delete can tell its own blocks from everyone else's.
struct alignas(std::max_align_t) Header {
    size_t size;
    bool counted;
};

thread_local bool t_counting = false;
std::atomic<long> g_live_blocks{0};
std::atomic<long> g_live_bytes{0};
std::atomic<long> g_total_blocks{0};
std::atomic<long> g_peak_bytes{0};

void* counted_new(size_t n) {
    auto* h = static_cast<Header*>(std::malloc(sizeof(Header) + n));
    if (h == nullptr) std::abort();
    h->size = n;
    h->counted = t_counting;
    if (h->counted) {
        g_live_blocks.fetch_add(1);
        const long live = g_live_bytes.fetch_add(static_cast<long>(n)) + static_cast<long>(n);
        long peak = g_peak_bytes.load();
        while (peak < live && !g_peak_bytes.compare_exchange_weak(peak, live)) {
        }
        g_total_blocks.fetch_add(1);
    }
    return h + 1;
}

void counted_delete(void* p) {
    if (p == nullptr) return;
    Header* h = static_cast<Header*>(p) - 1;
    if (h->counted) {
        g_live_blocks.fetch_sub(1);
        g_live_bytes.fetch_sub(static_cast<long>(h->size));
    }
    std::free(h);
}

}  // namespace

void* operator new(size_t n) { return counted_new(n); }
void* operator new[](size_t n) { return counted_new(n); }
void* operator new(size_t n, const std::nothrow_t&) noexcept { return counted_new(n); }
void* operator new[](size_t n, const std::nothrow_t&) noexcept { return counted_new(n); }
void operator delete(void* p) noexcept { counted_delete(p); }
void operator delete[](void* p) noexcept { counted_delete(p); }
void operator delete(void* p, size_t) noexcept { counted_delete(p); }
void operator delete[](void* p, size_t) noexcept { counted_delete(p); }

namespace {

using wisp::Value;
using wisp::agent::ToolCtx;
using wisp::agent::ToolOutput;
using wisp::agent::ToolRegistry;

Value output_value(const ToolOutput& output) {
    if (output.result_json.empty()) return output.result;
    auto parsed =
        wisp::from_json(std::string_view(output.result_json.data(), output.result_json.size()));
    EXPECT_TRUE(parsed) << parsed.error().message;
    return parsed ? std::move(parsed.value()) : Value{};
}

struct Counting {
    Counting() {
        g_live_blocks = 0;
        g_live_bytes = 0;
        g_total_blocks = 0;
        g_peak_bytes = 0;
        t_counting = true;
    }
    ~Counting() { t_counting = false; }
};

struct Deps {
    int calls = 0;
};

// Shaped like the real tool sets: literal text, a schema with a few
// properties, and a run function holding a shared_ptr (which does not fit
// std::function's inline buffer).
wisp::Result<void> add_sample_tools(ToolRegistry& reg, const std::shared_ptr<Deps>& d) {
    static constexpr const char* kNames[] = {"sample.one",   "sample.two",  "sample.three",
                                             "sample.four",  "sample.five", "sample.six",
                                             "sample.seven", "sample.eight"};
    for (const char* name : kNames) {
        WISP_TRY(reg.add({
            .name = name,
            .description = "A sample tool whose description is long enough to leave the "
                           "small-string buffer, like every real tool description does.",
            .schema_json = R"({"type":"object","properties":{"id":{"type":"string",)"
                           R"("maxLength":64},"text":{"type":"string","maxLength":2000},)"
                           R"("due":{"type":["string","null"]},"n":{"type":"integer",)"
                           R"("minimum":0,"maximum":100}},"required":["id"]})",
            .risk = wisp::agent::ToolRisk::Note,
            .thread = wisp::agent::ToolThread::Agent,
            .timeout_ms = 1000,
            .run =
                [d](const Value&, ToolCtx&) {
                    ++d->calls;
                    ToolOutput out;
                    out.result = Value(wisp::Map{{"ok", Value(true)}});
                    return out;
                },
        }));
    }
    return wisp::Result<void>::ok();
}

}  // namespace

TEST(ToolRegistryHeap, SearchResultsStayInLargeMemory) {
    wisp::agent::ToolIndex index;
    std::vector<wisp::core::SceneToolDecl> declarations;
    declarations.reserve(200);
    for (int i = 0; i < 200; ++i) {
        declarations.push_back(
            {"tool_" + std::to_string(i), "A description for an indexed tool", "", 30000});
    }
    index.replace_scene("many", "Many", declarations);
    std::string query;
    for (int i = 0; i < 200; ++i) query += " description";

    long blocks = 0;
    long bytes = 0;
    long total = 0;
    size_t result_count = 0;
    {
        Counting counting;
        const auto results = index.search(query, "", 200);
        result_count = results.size();
        t_counting = false;
        blocks = g_live_blocks.load();
        bytes = g_live_bytes.load();
        total = g_total_blocks.load();
    }
    EXPECT_EQ(result_count, 200u);
    EXPECT_EQ(blocks, 0) << bytes << " bytes in " << blocks << " blocks stay on the default heap";
    EXPECT_EQ(total, 0) << total << " temporary blocks were allocated on the default heap";
}

class ToolRegistryHeapSceneTest : public wisp::test::SceneRuntimeFixture {};

TEST_F(ToolRegistryHeapSceneTest, FindResponsesUseLargeMemory) {
    start();
    wisp::agent::ToolIndex index;
    std::vector<wisp::core::SceneToolDecl> declarations;
    declarations.reserve(200);
    std::string description;
    for (int i = 0; i < 512; ++i) description += "é";
    std::string large_enum = "\"";
    large_enum.append(3600, 'x');
    large_enum += "\"";
    const std::string schema =
        R"({"type":"object","properties":{"value":{"type":"string","enum":[)" + large_enum + "]}}}";
    for (int i = 0; i < 200; ++i) {
        declarations.push_back({"tool_" + std::to_string(i), description, schema, 30000});
    }
    index.replace_scene("many", "Many", declarations);
    wisp::agent::ToolIndex empty_index;
    wisp::agent::ToolRegistry empty_registry(wisp::agent::ToolExecutors{});
    ASSERT_TRUE(wisp::agent::add_scene_tool_tools(
        empty_registry, wisp::agent::SceneToolDeps{.index = empty_index, .runtime = *rt}));
    wisp::agent::ToolRegistry registry(wisp::agent::ToolExecutors{});
    ASSERT_TRUE(wisp::agent::add_scene_tool_tools(
        registry, wisp::agent::SceneToolDeps{.index = index, .runtime = *rt}));
    wisp::agent::ToolCtx ctx;
    ctx.cancelled = [] { return false; };

    long list_baseline = 0;
    {
        Counting counting;
        const ToolOutput empty = empty_registry.call("tools_find", "{}", ctx);
        t_counting = false;
        ASSERT_TRUE(empty.ok);
        list_baseline = g_peak_bytes.load();
    }
    ToolOutput listed;
    long list_peak = 0;
    {
        Counting counting;
        listed = registry.call("tools_find", "{}", ctx);
        t_counting = false;
        list_peak = g_peak_bytes.load();
    }
    ASSERT_TRUE(listed.ok);
    EXPECT_LE(list_peak - list_baseline, 512)
        << "list output added " << list_peak - list_baseline << " peak bytes on the default heap";
    const Value listed_value = output_value(listed);
    ASSERT_EQ(listed_value.find("tools")->as_list().size(), 200u);
    listed = ToolOutput{};

    ToolOutput filtered;
    long filtered_peak = 0;
    long filtered_baseline = 0;
    {
        Counting counting;
        const ToolOutput empty = empty_registry.call("tools_find", R"({"query":"tool"})", ctx);
        t_counting = false;
        ASSERT_TRUE(empty.ok);
        filtered_baseline = g_peak_bytes.load();
    }
    {
        Counting counting;
        filtered = registry.call("tools_find", R"({"query":"tool"})", ctx);
        t_counting = false;
        filtered_peak = g_peak_bytes.load();
    }
    ASSERT_TRUE(filtered.ok);
    const Value filtered_value = output_value(filtered);
    ASSERT_EQ(filtered_value.find("tools")->as_list().size(), 20u);
    ASSERT_EQ(filtered_value.find("tools")
                  ->as_list()
                  .front()
                  .find("parameters")
                  ->find("properties")
                  ->find("value")
                  ->find("enum")
                  ->as_list()
                  .front()
                  .as_string()
                  .size(),
              3600u);
    EXPECT_LE(filtered_peak, filtered_baseline + 512)
        << "filtered output added " << filtered_peak - filtered_baseline
        << " peak bytes on the default heap";
}

TEST(ToolRegistryHeap, RegistrationKeepsNothingOnTheDefaultHeap) {
    auto deps = std::make_shared<Deps>();
    ToolRegistry reg(wisp::agent::ToolExecutors{});
    long blocks = 0;
    long bytes = 0;
    long total = 0;
    {
        Counting counting;
        const bool sample = static_cast<bool>(add_sample_tools(reg, deps));
        const bool docs = static_cast<bool>(wisp::agent::add_docs_tools(reg));
        // Built here if it is built lazily: it must land in Mem::Large too.
        const size_t json = reg.tools_json().size();
        const bool found = reg.find("sample.one") != nullptr &&
                           reg.find_wire("docs_read") == reg.find("docs.read");
        blocks = g_live_blocks.load();
        bytes = g_live_bytes.load();
        total = g_total_blocks.load();
        t_counting = false;
        ASSERT_TRUE(sample);
        ASSERT_TRUE(docs);
        ASSERT_GT(json, 0u);
        ASSERT_TRUE(found);
    }
    EXPECT_EQ(blocks, 0) << bytes << " bytes in " << blocks << " blocks stay on the default "
                         << "heap (" << total << " blocks allocated in all)";
}

TEST(ToolRegistryHeap, CallsReleaseWhatTheyAllocate) {
    auto deps = std::make_shared<Deps>();
    ToolRegistry reg(wisp::agent::ToolExecutors{});
    ASSERT_TRUE(add_sample_tools(reg, deps));
    ToolCtx ctx;
    ctx.cancelled = [] { return false; };
    long blocks = 0;
    {
        Counting counting;
        ToolOutput out = reg.call("sample_one", R"({"id":"x","n":3})", ctx);
        ToolOutput bad = reg.call("sample_one", R"({"n":3})", ctx);
        t_counting = false;
        EXPECT_TRUE(out.ok);
        EXPECT_FALSE(bad.ok);
        out = ToolOutput{};
        bad = ToolOutput{};
        blocks = g_live_blocks.load();
    }
    EXPECT_EQ(deps->calls, 1);
    EXPECT_EQ(blocks, 0);
}
