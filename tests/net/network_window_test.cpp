// NetworkWindow: deep standby's network windows (spec 4.4).

#include "wisp/net/network_window.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <optional>
#include <string>

#include "wisp/core/event_bus.hpp"
#include "wisp/core/log.hpp"
#include "wisp/core/wakelock.hpp"

namespace {

using wisp::core::EventBus;
using wisp::core::WakeLock;
using wisp::core::WakeLockKind;
using wisp::core::WakeLockRegistry;
using wisp::hal::PowerLevel;
using wisp::net::NetworkWindow;

constexpr uint64_t kInterval = NetworkWindow::kIntervalMs;

struct Fixture {
    Fixture() {
        wisp::log::reset_for_test();
        wisp::log::init(64 * 1024);
        bus.start();
    }
    ~Fixture() {
        bus.stop();
        wisp::log::reset_for_test();
    }

    void tick(uint64_t now, PowerLevel level) { win.tick(now, level); }

    bool window_lock() const {
        for (const auto& l : locks.list())
            if (l.holder == "system.netwindow" && l.kind == WakeLockKind::Network &&
                l.reason == "deferred requests")
                return true;
        return false;
    }

    bool logged(const std::string& text) const {
        return wisp::log::ring_snapshot().find(text) != std::string::npos;
    }

    // Opens a window at `at`, deep standby entered at `at - kInterval`.
    void open_at(uint64_t at) {
        tick(at - kInterval, PowerLevel::DeepStandby);
        pending = 2;
        tick(at, PowerLevel::DeepStandby);
        ASSERT_TRUE(win.open());
    }

    EventBus bus;
    WakeLockRegistry locks{bus};
    size_t pending = 0;
    bool idle = true;
    bool connected = false;
    std::optional<bool> deferring;
    NetworkWindow win{{
        .locks = locks,
        .pending = [this] { return pending; },
        .http_idle = [this] { return idle && pending == 0; },
        .set_deferring = [this](bool on) { deferring = on; },
        .wifi_connected = [this] { return connected; },
    }};
};

TEST(NetworkWindow, OpensOnlyWithPendingEvery15Minutes) {
    Fixture f;
    f.tick(0, PowerLevel::Active);
    EXPECT_EQ(f.deferring, false);

    // Entering deep standby starts deferring and the 15 minute clock.
    f.tick(1000, PowerLevel::DeepStandby);
    EXPECT_EQ(f.deferring, true);
    f.pending = 1;
    f.tick(1000 + kInterval - 1, PowerLevel::DeepStandby);
    EXPECT_FALSE(f.win.open());
    EXPECT_FALSE(f.window_lock());

    f.tick(1000 + kInterval, PowerLevel::DeepStandby);
    EXPECT_TRUE(f.win.open());
    EXPECT_TRUE(f.window_lock());
    EXPECT_TRUE(f.logged("netwindow: open"));
    EXPECT_EQ(f.deferring, true);  // not connected yet

    // Nothing waiting: no window, until something is.
    Fixture g;
    g.tick(0, PowerLevel::DeepStandby);
    g.tick(kInterval, PowerLevel::DeepStandby);
    g.tick(kInterval + 60000, PowerLevel::DeepStandby);
    EXPECT_FALSE(g.win.open());
    EXPECT_FALSE(g.window_lock());
    g.pending = 1;
    g.tick(kInterval + 120000, PowerLevel::DeepStandby);
    EXPECT_TRUE(g.win.open());
}

TEST(NetworkWindow, FlushesAfterConnectAndWaitsForTimeSync) {
    Fixture f;
    const uint64_t t = 2 * kInterval;
    f.open_at(t);

    // PowerManager answers the lock with Standby; until Wi-Fi is up the
    // requests keep waiting.
    f.tick(t + 250, PowerLevel::Standby);
    EXPECT_EQ(f.deferring, true);
    f.connected = true;
    f.idle = false;
    f.tick(t + 5000, PowerLevel::Standby);
    EXPECT_EQ(f.deferring, false);
    EXPECT_TRUE(f.win.open());

    // Sent, but no time result yet: still open.
    f.pending = 0;
    f.idle = true;
    f.tick(t + 6000, PowerLevel::Standby);
    EXPECT_TRUE(f.win.open());
    EXPECT_TRUE(f.window_lock());

    f.win.note_time_result();
    f.tick(t + 7000, PowerLevel::Standby);
    EXPECT_FALSE(f.win.open());
    EXPECT_FALSE(f.window_lock());
    EXPECT_TRUE(f.logged("netwindow: closed (done)"));
    EXPECT_EQ(f.deferring, true);

    // PowerManager has not gone back down yet: still deferring.
    f.tick(t + 7250, PowerLevel::Standby);
    EXPECT_EQ(f.deferring, true);
    f.connected = false;
    f.tick(t + 7500, PowerLevel::DeepStandby);
    EXPECT_EQ(f.deferring, true);

    // The next window is 15 minutes after this one closed.
    f.pending = 1;
    f.tick(t + 7000 + kInterval - 1, PowerLevel::DeepStandby);
    EXPECT_FALSE(f.win.open());
    f.tick(t + 7000 + kInterval, PowerLevel::DeepStandby);
    EXPECT_TRUE(f.win.open());

    // Without any time result the window waits 30 s after connecting.
    const uint64_t u = t + 7000 + kInterval;
    f.connected = true;
    f.tick(u + 1000, PowerLevel::Standby);
    f.pending = 0;
    f.tick(u + 1000 + NetworkWindow::kTimeSyncWaitMs - 1, PowerLevel::Standby);
    EXPECT_TRUE(f.win.open());
    f.tick(u + 1000 + NetworkWindow::kTimeSyncWaitMs, PowerLevel::Standby);
    EXPECT_FALSE(f.win.open());
    EXPECT_FALSE(f.window_lock());
}

// A router that is gone: the window gives up after a minute, the device
// goes back to deep standby and the requests keep waiting.
TEST(NetworkWindow, GivesUpAfterConnectTimeout) {
    Fixture f;
    const uint64_t t = 2 * kInterval;
    f.open_at(t);
    f.tick(t + NetworkWindow::kConnectTimeoutMs - 1, PowerLevel::Standby);
    EXPECT_TRUE(f.win.open());
    f.tick(t + NetworkWindow::kConnectTimeoutMs, PowerLevel::Standby);
    EXPECT_FALSE(f.win.open());
    EXPECT_FALSE(f.window_lock());
    EXPECT_TRUE(f.logged("netwindow: closed (no wifi)"));
    EXPECT_EQ(f.deferring, true);
    f.tick(t + NetworkWindow::kConnectTimeoutMs + 250, PowerLevel::Standby);
    EXPECT_EQ(f.deferring, true);
    f.tick(t + NetworkWindow::kConnectTimeoutMs + 500, PowerLevel::DeepStandby);
    EXPECT_EQ(f.deferring, true);

    const uint64_t next = t + NetworkWindow::kConnectTimeoutMs + kInterval;
    f.tick(next - 1, PowerLevel::DeepStandby);
    EXPECT_FALSE(f.win.open());
    f.tick(next, PowerLevel::DeepStandby);
    EXPECT_TRUE(f.win.open());
}

TEST(NetworkWindow, ClosesAfterFiveMinutes) {
    Fixture f;
    const uint64_t t = 2 * kInterval;
    f.open_at(t);
    f.connected = true;
    f.idle = false;  // a request that never finishes
    f.tick(t + 1000, PowerLevel::Standby);
    f.win.note_time_result();
    f.tick(t + NetworkWindow::kMaxOpenMs - 1, PowerLevel::Standby);
    EXPECT_TRUE(f.win.open());
    f.tick(t + NetworkWindow::kMaxOpenMs, PowerLevel::Standby);
    EXPECT_FALSE(f.win.open());
    EXPECT_FALSE(f.window_lock());
    EXPECT_TRUE(f.logged("netwindow: closed (timeout)"));
    EXPECT_EQ(f.deferring, true);
}

TEST(NetworkWindow, LeavingDeepStandbyStopsDeferring) {
    Fixture f;
    f.tick(0, PowerLevel::DeepStandby);
    EXPECT_EQ(f.deferring, true);
    f.tick(1000, PowerLevel::Active);
    EXPECT_EQ(f.deferring, false);

    // Back in deep standby the clock starts over.
    f.pending = 1;
    f.tick(2000, PowerLevel::DeepStandby);
    EXPECT_EQ(f.deferring, true);
    f.tick(2000 + kInterval - 1, PowerLevel::DeepStandby);
    EXPECT_FALSE(f.win.open());
    f.tick(2000 + kInterval, PowerLevel::DeepStandby);
    EXPECT_TRUE(f.win.open());

    // Woken while a window is open: it closes and requests go out.
    f.tick(2000 + kInterval + 1000, PowerLevel::Active);
    EXPECT_FALSE(f.win.open());
    EXPECT_FALSE(f.window_lock());
    EXPECT_EQ(f.deferring, false);

    // Standby kept up by someone else's network lock: Wi-Fi is on, so
    // nothing waits.
    WakeLock admin(f.locks, "system.admin", WakeLockKind::Network, "admin session");
    f.tick(3 * kInterval, PowerLevel::DeepStandby);
    EXPECT_EQ(f.deferring, true);
    f.tick(3 * kInterval + 250, PowerLevel::Standby);
    EXPECT_EQ(f.deferring, false);
}

}  // namespace
