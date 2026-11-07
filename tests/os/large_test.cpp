#include "wisp/os/large.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
#include <memory>
#include <string>
#include <utility>

#include "wisp/os/os.hpp"

using namespace wisp::os;

TEST(LargeBuffer, AllocateMoveAndRelease) {
    LargeBuffer a = LargeBuffer::allocate(4096);
    ASSERT_TRUE(a.ok());
    EXPECT_EQ(a.size(), 4096u);
    ASSERT_NE(a.data(), nullptr);
    std::memset(a.data(), 0x5a, a.size());
    EXPECT_EQ(a.span().size(), 4096u);

    LargeBuffer b = std::move(a);
    EXPECT_FALSE(a.ok());  // NOLINT(bugprone-use-after-move): moved-from state is specified
    EXPECT_EQ(a.size(), 0u);
    ASSERT_TRUE(b.ok());
    EXPECT_EQ(b.size(), 4096u);
    EXPECT_EQ(b.data()[4095], 0x5a);

    b.truncate(10);
    EXPECT_EQ(b.size(), 10u);
    const LargeBuffer& cb = b;
    EXPECT_EQ(cb.span().size(), 10u);
    EXPECT_EQ(cb.data()[9], 0x5a);

    LargeBuffer c;
    EXPECT_FALSE(c.ok());
    c = std::move(b);
    EXPECT_TRUE(c.ok());
    EXPECT_FALSE(b.ok());  // NOLINT(bugprone-use-after-move)
}

TEST(LargeAllocator, VectorAndStringGrow) {
    LargeVector<int> v;
    for (int i = 0; i < 10000; ++i) {
        v.push_back(i);
    }
    ASSERT_EQ(v.size(), 10000u);
    for (int i = 0; i < 10000; ++i) {
        ASSERT_EQ(v[static_cast<size_t>(i)], i);
    }

    LargeString s;
    while (s.size() < 100 * 1024) {
        s += "0123456789abcdef";
    }
    ASSERT_EQ(s.size(), 100u * 1024u);
    for (size_t i = 0; i < s.size(); i += 16) {
        ASSERT_EQ(s.compare(i, 16, "0123456789abcdef"), 0) << "at " << i;
    }

    struct Pair {
        int a;
        int b;
        Pair(int x, int y) : a(x), b(y) {}
    };
    LargeUnique<Pair> p = make_large<Pair>(3, 4);
    ASSERT_TRUE(p);
    EXPECT_EQ(p->a, 3);
    EXPECT_EQ(p->b, 4);
}

TEST(OsRealloc, PreservesPrefixAndHandlesEdges) {
    auto* p = static_cast<uint8_t*>(alloc(16, Mem::Large));
    ASSERT_NE(p, nullptr);
    for (uint8_t i = 0; i < 16; ++i) {
        p[i] = i;
    }
    auto* q = static_cast<uint8_t*>(realloc(p, 64 * 1024, Mem::Large));
    ASSERT_NE(q, nullptr);
    for (uint8_t i = 0; i < 16; ++i) {
        EXPECT_EQ(q[i], i);
    }

    void* r = realloc(nullptr, 32, Mem::Large);
    EXPECT_NE(r, nullptr);

    EXPECT_EQ(realloc(q, 0, Mem::Large), nullptr);
    EXPECT_EQ(realloc(r, 0, Mem::Large), nullptr);
}

TEST(LargeFunction, CallsCopiesMovesAndEmpties) {
    LargeFunction<int(int, std::string&)> empty;
    EXPECT_FALSE(empty);
    LargeFunction<int(int, std::string&)> null = nullptr;
    EXPECT_FALSE(null);

    // A shared_ptr capture: what std::function would put on the default heap.
    auto count = std::make_shared<int>(0);
    LargeFunction<int(int, std::string&)> f = [count](int x, std::string& out) {
        ++*count;
        out += "x";
        return x * 2;
    };
    ASSERT_TRUE(f);
    std::string s;
    EXPECT_EQ(f(21, s), 42);
    EXPECT_EQ(s, "x");

    LargeFunction<int(int, std::string&)> copy = f;
    EXPECT_EQ(count.use_count(), 3);
    EXPECT_EQ(copy(1, s), 2);
    EXPECT_EQ(*count, 2);

    LargeFunction<int(int, std::string&)> moved = std::move(f);
    EXPECT_FALSE(f);  // NOLINT(bugprone-use-after-move): moved-from state is specified
    EXPECT_EQ(moved(2, s), 4);
    EXPECT_EQ(count.use_count(), 3);

    copy = nullptr;
    EXPECT_FALSE(copy);
    EXPECT_EQ(count.use_count(), 2);
    copy = moved;
    EXPECT_EQ(count.use_count(), 3);
    moved = std::move(copy);
    EXPECT_EQ(count.use_count(), 2);
    moved = LargeFunction<int(int, std::string&)>();
    EXPECT_EQ(count.use_count(), 1);
}

TEST(LargeFunction, VoidResult) {
    int hits = 0;
    LargeFunction<void()> f = [&hits] { ++hits; };
    f();
    f();
    EXPECT_EQ(hits, 2);
}
