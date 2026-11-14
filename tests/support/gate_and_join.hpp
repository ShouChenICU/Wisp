#pragma once

// Test-only guard for tests that park a thread (or a dispatcher's run) on a
// gate. Declared after a test's threads (and before it starts them): on
// every way out of the test, a failed ASSERT included, it opens the gate
// and joins the threads, so the failure is reported by gtest instead of
// std::terminate on a joinable thread or a run parked forever.

#include <initializer_list>
#include <thread>
#include <vector>

#include "wisp/os/os.hpp"

namespace wisp::test {

class GateAndJoin {
public:
    // `opens`: how many times the gate is given, one per waiter it may
    // still hold.
    GateAndJoin(os::Semaphore& gate, std::initializer_list<std::thread*> threads, int opens = 1)
        : gate_(gate), threads_(threads), opens_(opens) {}
    // For threads a test creates into a vector after declaring the guard.
    GateAndJoin(os::Semaphore& gate, std::vector<std::thread>& pool, int opens = 1)
        : gate_(gate), pool_(&pool), opens_(opens) {}
    ~GateAndJoin() {
        for (int i = 0; i < opens_; ++i) {
            gate_.give();
        }
        for (std::thread* t : threads_) {
            if (t->joinable()) {
                t->join();
            }
        }
        if (pool_ != nullptr) {
            for (std::thread& t : *pool_) {
                if (t.joinable()) {
                    t.join();
                }
            }
        }
    }
    GateAndJoin(const GateAndJoin&) = delete;
    GateAndJoin& operator=(const GateAndJoin&) = delete;

private:
    os::Semaphore& gate_;
    std::vector<std::thread*> threads_;
    std::vector<std::thread>* pool_ = nullptr;
    int opens_;
};

}  // namespace wisp::test
