#include "wisp/os/os.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <string>

#include "wisp/os/queue.hpp"

using namespace wisp::os;

TEST(Os, ClockIsMonotonic) {
    auto a = now_ms();
    sleep_ms(20);
    EXPECT_GE(now_ms() - a, 19u);
}
TEST(Os, SemaphoreTimeout) {
    Semaphore s(1, 0);
    auto t0 = now_ms();
    EXPECT_FALSE(s.take(50));
    EXPECT_GE(now_ms() - t0, 45u);
    s.give();
    EXPECT_TRUE(s.take(0));
}
TEST(Os, TaskRunsAndJoins) {
    std::atomic<int> x{0};
    auto t = Task::spawn({.name = "t"}, [&] { x = 1; });
    t->join();
    EXPECT_EQ(x, 1);
}
TEST(Os, TaskIsCurrent) {
    std::atomic<bool> inside{false};
    std::unique_ptr<Task> t;
    Semaphore ready(1, 0);
    t = Task::spawn({}, [&] {
        ready.take(kForever);
        inside = t->is_current();
    });
    ready.give();
    t->join();
    EXPECT_TRUE(inside);
    EXPECT_FALSE(t->is_current());
}
TEST(Os, PeriodicTimerFiresRepeatedly) {
    std::atomic<int> n{0};
    Timer tm("tm", 10, true, [&] { ++n; });
    tm.start();
    sleep_ms(105);
    tm.stop();
    int seen = n;
    EXPECT_GE(seen, 8);
    sleep_ms(30);
    EXPECT_EQ(n, seen);
}
TEST(Queue, FifoAndTimeout) {
    Queue<std::string> q(2);
    EXPECT_TRUE(q.push("a", 0));
    EXPECT_TRUE(q.push("b", 0));
    EXPECT_FALSE(q.push("c", 10));
    std::string s;
    EXPECT_TRUE(q.pop(s, 0));
    EXPECT_EQ(s, "a");
}
TEST(Queue, DropOldestWhenFull) {
    Queue<int> q(2);
    q.push(1, 0);
    q.push(2, 0);
    EXPECT_TRUE(q.push_drop_oldest(3));
    int v;
    q.pop(v, 0);
    EXPECT_EQ(v, 2);
    q.pop(v, 0);
    EXPECT_EQ(v, 3);
}
TEST(Os, AllocKinds) {
    for (auto m : {Mem::Fast, Mem::Large, Mem::Dma}) {
        void* p = alloc(64, m);
        ASSERT_NE(p, nullptr);
        // Qualified: under `using namespace wisp::os;`, unqualified free(p)
        // is ambiguous against ::free(void*) from <cstdlib> (pulled in
        // transitively by <memory>, which os.hpp itself needs for
        // std::unique_ptr) — both are exact-match candidates in scope.
        wisp::os::free(p);
    }
}
