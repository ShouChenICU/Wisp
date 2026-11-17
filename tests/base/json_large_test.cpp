// parse_json_large(): a parsed document whose every byte comes from an
// injected allocator (on the device, Mem::Large through wisp_net's
// large_json_alloc()), read through JsonView without building a Value.
//
// This binary replaces the global operator new/delete to prove that
// reading a document does not fall back to the default heap, so it holds
// only these tests.

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdlib>
#include <new>
#include <string>

#include "wisp/base/json.hpp"

namespace {

struct alignas(std::max_align_t) Header {
    size_t size;
    bool counted;
};

thread_local bool t_counting = false;
std::atomic<long> g_new_blocks{0};

void* counted_new(size_t n) {
    auto* h = static_cast<Header*>(std::malloc(sizeof(Header) + n));
    if (h == nullptr) std::abort();
    h->size = n;
    h->counted = t_counting;
    if (h->counted) g_new_blocks.fetch_add(1);
    return h + 1;
}

void counted_delete(void* p) {
    if (p == nullptr) return;
    std::free(static_cast<Header*>(p) - 1);
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

using wisp::ErrorCode;
using wisp::JsonAlloc;
using wisp::JsonView;
using wisp::LargeJson;
using wisp::parse_json_large;

std::atomic<long> g_allocs{0};
std::atomic<long> g_frees{0};
bool g_fail = false;
long g_fail_after = -1;  // >= 0: allocations beyond this many fail

bool refuse() { return g_fail || (g_fail_after >= 0 && g_allocs.load() >= g_fail_after); }

void* count_allocate(size_t n) {
    if (refuse()) return nullptr;
    g_allocs.fetch_add(1);
    return std::malloc(n);
}
void count_deallocate(void* p) {
    if (p == nullptr) return;
    g_frees.fetch_add(1);
    std::free(p);
}
void* count_reallocate(void* p, size_t n) {
    if (refuse()) return nullptr;
    // A fresh block counts as an allocation, the same as realloc(nullptr).
    if (p == nullptr) g_allocs.fetch_add(1);
    return std::realloc(p, n);
}

const JsonAlloc kCounting{count_allocate, count_deallocate, count_reallocate};

class JsonLarge : public ::testing::Test {
protected:
    void SetUp() override {
        g_allocs = 0;
        g_frees = 0;
        g_fail = false;
        g_fail_after = -1;
    }
    void TearDown() override {
        g_fail = false;
        g_fail_after = -1;
    }
};

LargeJson parse_ok(std::string_view text) {
    auto r = parse_json_large(text, kCounting);
    EXPECT_TRUE(r) << (r ? "" : r.error().message);
    return std::move(r.value());
}

TEST_F(JsonLarge, ReadsObjectsArraysAndScalars) {
    LargeJson doc = parse_ok(R"({"a":1,"b":"x","c":[true,null,2.5],"d":{"e":"f"}})");
    const JsonView root = doc.root();
    EXPECT_EQ(root.type(), JsonView::Type::Object);
    EXPECT_EQ(root.size(), 4u);
    EXPECT_EQ(root.find("a").type(), JsonView::Type::Int);
    EXPECT_EQ(root.find("a").integer(), 1);
    EXPECT_EQ(root.find("b").type(), JsonView::Type::String);
    EXPECT_EQ(root.find("b").string(), "x");
    const JsonView c = root.find("c");
    EXPECT_EQ(c.type(), JsonView::Type::Array);
    EXPECT_EQ(c.size(), 3u);
    EXPECT_EQ(c.at(0).type(), JsonView::Type::Bool);
    EXPECT_TRUE(c.at(0).boolean());
    EXPECT_EQ(c.at(1).type(), JsonView::Type::Null);
    EXPECT_EQ(c.at(2).type(), JsonView::Type::Double);
    EXPECT_FALSE(c.at(2).integer().has_value());
    EXPECT_EQ(root.find("d").find("e").string(), "f");
}

TEST_F(JsonLarge, MissingKeysAndWrongTypesGiveNullViews) {
    LargeJson doc = parse_ok(R"({"a":1,"c":[1,2]})");
    const JsonView root = doc.root();
    EXPECT_EQ(root.find("zz").type(), JsonView::Type::Null);
    EXPECT_EQ(root.find("c").at(9).type(), JsonView::Type::Null);
    EXPECT_EQ(root.find("c").find("a").type(), JsonView::Type::Null);
    EXPECT_EQ(root.at(0).type(), JsonView::Type::Null);
    EXPECT_EQ(root.find("a").string(), "");
    EXPECT_EQ(root.find("a").size(), 0u);
    EXPECT_FALSE(root.find("zz").integer().has_value());
    EXPECT_TRUE(root.find("a").boolean(true));
    EXPECT_EQ(JsonView{}.type(), JsonView::Type::Null);
}

TEST_F(JsonLarge, AppendToReserializesASubtree) {
    const std::string input = R"({"a":1,"b":"x","c":[true,null,2.5],"d":{"e":"f"}})";
    LargeJson doc = parse_ok(input);
    std::string out;
    doc.root().find("d").append_to(out);
    EXPECT_EQ(out, R"({"e":"f"})");
    std::string all;
    doc.root().append_to(all);
    EXPECT_EQ(all, input);
}

TEST_F(JsonLarge, AppendToEscapesControlBytes) {
    // A \u0001 escape parses to a raw byte; written back it must be an
    // escape again, or the output is not JSON.
    LargeJson doc = parse_ok(R"({"s":"a\u0001b\nc"})");
    std::string out;
    doc.root().append_to(out);
    EXPECT_EQ(out, R"({"s":"a\u0001b\nc"})");
}

TEST_F(JsonLarge, InvalidJsonIsInvalidArgument) {
    auto r = parse_json_large(R"({"a":)", kCounting);
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, ErrorCode::InvalidArgument);
    EXPECT_FALSE(r.error().message.empty());
}

TEST_F(JsonLarge, DeeperThanTheLimitIsQuotaExceeded) {
    const std::string deep = std::string(33, '[') + std::string(33, ']');
    auto r = parse_json_large(deep, kCounting);
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, ErrorCode::QuotaExceeded);
    const std::string ok = std::string(32, '[') + std::string(32, ']');
    EXPECT_TRUE(parse_json_large(ok, kCounting));
}

TEST_F(JsonLarge, StringsOver64KiBKeepTheirLength) {
    const std::string big(70000, 'q');
    LargeJson doc = parse_ok("{\"s\":\"" + big + "\"}");
    const std::string_view s = doc.root().find("s").string();
    EXPECT_EQ(s.size(), 70000u);
    EXPECT_EQ(s, big);
}

TEST_F(JsonLarge, EveryAllocationGoesThroughTheAllocator) {
    std::string text = "{";
    for (int i = 0; i < 200; ++i) {
        if (i > 0) text += ",";
        text += "\"key" + std::to_string(i) + "\":\"value number " + std::to_string(i) + "\"";
    }
    text += "}";
    std::string out;
    out.reserve(text.size() + 16);
    {
        g_new_blocks = 0;
        t_counting = true;
        auto r = parse_json_large(text, kCounting);
        bool ok = static_cast<bool>(r);
        size_t members = 0;
        std::string_view v;
        if (ok) {
            const JsonView root = r.value().root();
            members = root.size();
            v = root.find("key199").string();
            (void)root.at(0);
            root.append_to(out);
        }
        t_counting = false;
        ASSERT_TRUE(ok);
        EXPECT_EQ(members, 200u);
        EXPECT_EQ(v, "value number 199");
        EXPECT_EQ(out, text);
        EXPECT_EQ(g_new_blocks.load(), 0);
        EXPECT_GT(g_allocs.load(), 0);
    }
    EXPECT_EQ(g_allocs.load(), g_frees.load());
}

TEST_F(JsonLarge, MovedFromDocumentIsEmptyAndFreesOnce) {
    {
        LargeJson a = parse_ok(R"({"a":1})");
        LargeJson b = std::move(a);
        EXPECT_EQ(b.root().find("a").integer(), 1);
        EXPECT_EQ(a.root().type(), JsonView::Type::Null);  // NOLINT(bugprone-use-after-move)
        LargeJson c = parse_ok("[1]");
        c = std::move(b);
        EXPECT_EQ(c.root().find("a").integer(), 1);
    }
    EXPECT_EQ(g_allocs.load(), g_frees.load());
}

TEST_F(JsonLarge, AllocatorFailureIsNoMemory) {
    g_fail = true;
    auto r = parse_json_large(R"({"a":"some text"})", kCounting);
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, ErrorCode::NoMemory);
}

TEST_F(JsonLarge, AllocatorFailureWhileParsingIsNoMemoryAndLeaksNothing) {
    g_fail_after = 1;  // the document's own block, then nothing
    {
        auto r = parse_json_large(R"({"a":"some text"})", kCounting);
        ASSERT_FALSE(r);
        EXPECT_EQ(r.error().code, ErrorCode::NoMemory);
    }
    EXPECT_EQ(g_allocs.load(), 1);
    EXPECT_EQ(g_allocs.load(), g_frees.load());
}

}  // namespace
