// TlsGate: one TLS handshake at a time in the process (wisp/net/tls_gate.hpp).

#include "wisp/net/tls_gate.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <thread>
#include <vector>

#include "wisp/os/os.hpp"

namespace {

using wisp::net::TlsGate;

TEST(TlsGate, OneHolderAtATime) {
    TlsGate gate;
    {
        TlsGate::Hold first(gate, 0);
        EXPECT_TRUE(first.held());
        const uint64_t t0 = wisp::os::now_ms();
        TlsGate::Hold second(gate, 50);
        EXPECT_FALSE(second.held());
        EXPECT_GE(wisp::os::now_ms() - t0, 40u);
    }
    // Released when the holder went out of scope; a failed hold released
    // nothing it did not have.
    TlsGate::Hold again(gate, 0);
    EXPECT_TRUE(again.held());
    TlsGate::Hold blocked(gate, 0);
    EXPECT_FALSE(blocked.held());
}

TEST(TlsGate, WaiterGetsItWhenReleased) {
    TlsGate gate;
    auto first = std::make_unique<TlsGate::Hold>(gate, 0);
    ASSERT_TRUE(first->held());
    std::thread releaser([&] {
        wisp::os::sleep_ms(50);
        first.reset();
    });
    TlsGate::Hold waiting(gate, 2000);
    EXPECT_TRUE(waiting.held());
    releaser.join();
}

TEST(TlsGate, NeverTwoAtOnce) {
    TlsGate gate;
    std::atomic<int> inside{0};
    std::atomic<int> most{0};
    std::atomic<int> entered{0};
    std::vector<std::thread> threads;
    for (int t = 0; t < 4; ++t) {
        threads.emplace_back([&] {
            for (int i = 0; i < 20; ++i) {
                TlsGate::Hold hold(gate, 5000);
                if (!hold.held()) continue;
                ++entered;
                const int now = ++inside;
                int seen = most.load();
                while (now > seen && !most.compare_exchange_weak(seen, now)) {
                }
                wisp::os::sleep_ms(1);
                --inside;
            }
        });
    }
    for (auto& t : threads) t.join();
    EXPECT_EQ(entered.load(), 80);
    EXPECT_EQ(most.load(), 1);
}

TEST(TlsGate, ProcessGateIsShared) {
    EXPECT_EQ(&TlsGate::process(), &TlsGate::process());
    TlsGate::Hold hold(TlsGate::process(), 0);
    EXPECT_TRUE(hold.held());
    TlsGate::Hold other(TlsGate::process(), 0);
    EXPECT_FALSE(other.held());
}

}  // namespace
