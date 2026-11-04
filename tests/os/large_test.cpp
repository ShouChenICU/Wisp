#include "wisp/os/large.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <cstring>
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
