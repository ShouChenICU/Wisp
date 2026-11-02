#include "wisp/core/button_service.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <memory>
#include <string>
#include <vector>

#include "support/fake_devices.hpp"
#include "wisp/core/device_registry.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/os/os.hpp"

using wisp::core::ButtonService;
using wisp::core::DeviceRegistry;
using wisp::core::Event;
using wisp::core::EventBus;
using wisp::test::FakeButton;

namespace {

// registry has one FakeButton "button.power"; bus is started and
// subscribed to every topic, collecting them (in delivery order) into
// `topics`. `topics` is only written by the bus's dispatcher thread and
// is read by tests only after bus.wait_idle() confirms that thread is
// idle, so no extra locking is needed here.
struct Fixture {
    Fixture() {
        btn = std::make_shared<FakeButton>("button.power");
        registry.add(btn);
        bus.start();
        bus.subscribe("", [this](const Event& e) { topics.push_back(e.topic); });
    }

    std::shared_ptr<FakeButton> add_button(const std::string& id) {
        auto b = std::make_shared<FakeButton>(id);
        registry.add(b);
        return b;
    }

    DeviceRegistry registry;
    EventBus bus;
    ButtonService svc{registry, bus};
    std::shared_ptr<FakeButton> btn;
    std::vector<std::string> topics;
};

}  // namespace

TEST(ButtonService, ShortPress) {
    Fixture f;
    f.btn->down = true;
    f.svc.poll(0);
    f.btn->down = false;
    f.svc.poll(300);
    f.bus.wait_idle(1000);
    EXPECT_EQ(f.topics, (std::vector<std::string>{"button.power.down", "button.power.up",
                                                  "button.power.short"}));
}

TEST(ButtonService, LongPressFiresOnceWhileHeld) {
    Fixture f;
    f.btn->down = true;
    f.svc.poll(0);
    f.svc.poll(999);
    f.svc.poll(1000);
    f.svc.poll(1500);
    f.svc.poll(3000);
    f.btn->down = false;
    f.svc.poll(3100);
    f.bus.wait_idle(1000);
    EXPECT_EQ(f.topics, (std::vector<std::string>{"button.power.down", "button.power.long",
                                                  "button.power.up"}));
}

// Task 18b: PowerManager widens the poll period to 50ms while in
// standby. The short/long press semantics (including the 1000ms long
// threshold) must keep working identically at the new period -- only the
// detection resolution (how quickly a transition is noticed) changes.
TEST(ButtonService, PollIntervalChangeKeepsShortAndLongDetection) {
    Fixture f;
    EXPECT_EQ(f.svc.poll_interval_ms(), 20u);
    f.svc.set_poll_interval_ms(50);
    EXPECT_EQ(f.svc.poll_interval_ms(), 50u);

    f.btn->down = true;
    f.svc.poll(0);
    f.btn->down = false;
    f.svc.poll(300);
    f.bus.wait_idle(1000);
    EXPECT_EQ(f.topics, (std::vector<std::string>{"button.power.down", "button.power.up",
                                                  "button.power.short"}));
}

TEST(ButtonService, PollIntervalChangeLongPressThresholdUnchanged) {
    Fixture f;
    f.svc.set_poll_interval_ms(50);

    f.btn->down = true;
    f.svc.poll(0);
    f.svc.poll(999);
    f.svc.poll(1000);
    f.svc.poll(1500);
    f.btn->down = false;
    f.svc.poll(1550);
    f.bus.wait_idle(1000);
    EXPECT_EQ(f.topics, (std::vector<std::string>{"button.power.down", "button.power.long",
                                                  "button.power.up"}));
}

// set_poll_interval_ms() must be safe to call while the real Timer is
// running (not just against synthetic poll() calls), and must not
// disturb per-button in-progress state (buttons_ is never touched by
// it) -- the held button keeps timing its hold across the switch.
TEST(ButtonService, SetPollIntervalWhileRunningSwitchesLiveTimer) {
    DeviceRegistry registry;
    auto btn = std::make_shared<FakeButton>("button.power");
    registry.add(btn);
    EventBus bus;
    bus.start();
    std::vector<std::string> topics;
    bus.subscribe("", [&](const Event& e) { topics.push_back(e.topic); });

    ButtonService svc(registry, bus, /*long_press_ms=*/200, /*poll_ms=*/5);
    svc.start();
    btn->down = true;
    wisp::os::sleep_ms(30);  // several 5ms polls have observed `down`
    svc.set_poll_interval_ms(50);
    EXPECT_EQ(svc.poll_interval_ms(), 50u);
    wisp::os::sleep_ms(250);  // cross the 200ms long_press_ms threshold at 50ms resolution
    btn->down = false;
    wisp::os::sleep_ms(100);
    svc.stop();
    ASSERT_TRUE(bus.wait_idle(1000));

    EXPECT_EQ(std::count(topics.begin(), topics.end(), "button.power.down"), 1);
    EXPECT_EQ(std::count(topics.begin(), topics.end(), "button.power.long"), 1);
    EXPECT_EQ(topics, (std::vector<std::string>{"button.power.down", "button.power.long",
                                                "button.power.up"}));
}

TEST(ButtonService, IndependentButtons) {
    Fixture f;
    auto up = f.add_button("button.vol_up");
    up->down = true;
    f.svc.poll(0);
    up->down = false;
    f.svc.poll(50);
    f.bus.wait_idle(1000);
    EXPECT_EQ(f.topics.back(), "button.vol_up.short");
}

// Review fix round 1, item 1: a second start() call while the Timer is
// already running (and the button is mid-hold) used to re-run
// refresh_buttons() -- unsynchronized against the Timer thread's
// concurrent poll(), and discarding every button's in-progress
// pressed/down_at/long_sent state, which made the next poll() see a
// "fresh" press and re-publish `down` (and, if already past threshold,
// `long` again too). start() must now be a true no-op whenever the
// Timer is already active, and poll()/the buttons_ it touches must be
// safe against being invoked by the Timer thread while another thread
// calls start() at the same time. This test drives the real Timer (not
// synthetic poll() calls) so the two threads genuinely race.
TEST(ButtonService, StartCalledTwiceWhileHeldDoesNotDuplicateEvents) {
    DeviceRegistry registry;
    auto btn = std::make_shared<FakeButton>("button.power");
    registry.add(btn);
    EventBus bus;
    bus.start();
    std::vector<std::string> topics;
    bus.subscribe("", [&](const Event& e) { topics.push_back(e.topic); });

    ButtonService svc(registry, bus, /*long_press_ms=*/50, /*poll_ms=*/5);
    svc.start();
    btn->down = true;
    wisp::os::sleep_ms(20);  // several polls have observed `down` and are timing the hold
    svc.start();             // idempotent while mid-hold: must not reset state
    wisp::os::sleep_ms(60);  // cross the 50ms long_press_ms threshold
    svc.start();             // idempotent again, now with `long` already latched
    btn->down = false;
    wisp::os::sleep_ms(20);
    svc.stop();
    ASSERT_TRUE(bus.wait_idle(1000));

    EXPECT_EQ(std::count(topics.begin(), topics.end(), "button.power.down"), 1);
    EXPECT_EQ(std::count(topics.begin(), topics.end(), "button.power.long"), 1);
    EXPECT_EQ(topics, (std::vector<std::string>{"button.power.down", "button.power.long",
                                                "button.power.up"}));
}
