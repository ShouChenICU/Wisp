#include "wisp/core/button_service.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <vector>

#include "support/fake_devices.hpp"
#include "wisp/core/device_registry.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/os/os.hpp"
#include "wisp/port/sim/sim_os.hpp"

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
//
// The explicit destructor stops the service and the bus's dispatcher
// thread before `topics` is torn down. Members destruct in reverse
// declaration order, so without this, `topics` (declared last) would be
// destroyed while the dispatcher could still be delivering an event into
// the subscriber callback above -- a use-after-destroy race.
struct Fixture {
    Fixture() {
        btn = std::make_shared<FakeButton>("button.power");
        registry.add(btn);
        bus.start();
        bus.subscribe("", [this](const Event& e) { topics.push_back(e.topic); });
    }

    ~Fixture() {
        svc.stop();
        bus.stop();
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

class EdgeButton final : public wisp::hal::Button {
public:
    explicit EdgeButton(std::string id) {
        info_.id = std::move(id);
        info_.cls = kClass;
        info_.model = "EDGE_FAKE";
    }

    const wisp::hal::DeviceInfo& info() const override { return info_; }
    bool pressed() override {
        ++pressed_calls;
        return down.load();
    }
    wisp::hal::ButtonInputMode input_mode() const override {
        return wisp::hal::ButtonInputMode::EdgeEvent;
    }
    wisp::Result<void> set_edge_sink(wisp::hal::ButtonEdgeSink sink) override {
        if (sink.push == nullptr) {
            detach_entered.give();
        }
        std::lock_guard<std::mutex> lock(mutex_);
        if (sink.push != nullptr && reject_sink) {
            return wisp::Result<void>::err(wisp::ErrorCode::Unsupported);
        }
        sink_ = sink;
        return wisp::Result<void>::ok();
    }
    uint32_t standby_poll_ms() const override { return 100; }

    bool emit(bool pressed, uint64_t timestamp_us, wisp::os::Semaphore* entered = nullptr,
              wisp::os::Semaphore* resume = nullptr) {
        down.store(pressed);
        // Serialize detach with the entire callback, not just copying the sink.
        std::lock_guard<std::mutex> lock(mutex_);
        if (sink_.push == nullptr) {
            return false;
        }
        if (entered != nullptr) {
            entered->give();
            resume->take(wisp::os::kForever);
        }
        return sink_.push(sink_.context, pressed, timestamp_us);
    }

    bool has_sink() {
        std::lock_guard<std::mutex> lock(mutex_);
        return sink_.push != nullptr;
    }

    void service_input() override { ++maintenance_calls; }
    std::atomic<size_t> maintenance_calls{0};
    bool reject_sink = false;
    wisp::os::Semaphore detach_entered{1, 0};
    std::atomic<bool> down{false};
    std::atomic<size_t> pressed_calls{0};

private:
    wisp::hal::DeviceInfo info_;
    std::mutex mutex_;
    wisp::hal::ButtonEdgeSink sink_{};
};

class PollButton final : public FakeButton {
public:
    PollButton(std::string id, uint32_t standby_poll_ms)
        : FakeButton(std::move(id)), standby_poll_ms_(standby_poll_ms) {}

    bool pressed() override {
        ++pressed_calls;
        {
            std::lock_guard<std::mutex> lock(samples_mutex_);
            sample_times_us_.push_back(wisp::os::now_us());
            samples_changed_.notify_all();
        }
        return FakeButton::pressed();
    }
    uint32_t standby_poll_ms() const override { return standby_poll_ms_; }

    std::vector<uint64_t> sample_times() {
        std::lock_guard<std::mutex> lock(samples_mutex_);
        return sample_times_us_;
    }
    bool wait_for_samples(size_t count) {
        std::unique_lock<std::mutex> lock(samples_mutex_);
        return samples_changed_.wait_for(lock, std::chrono::seconds(2),
                                         [&] { return sample_times_us_.size() >= count; });
    }

    std::atomic<size_t> pressed_calls{0};

private:
    uint32_t standby_poll_ms_;
    std::mutex samples_mutex_;
    std::condition_variable samples_changed_;
    std::vector<uint64_t> sample_times_us_;
};

struct FailureReset {
    ~FailureReset() {
        wisp::port::sim::set_alloc_failure(wisp::os::Mem::Fast, false);
        wisp::port::sim::set_task_failure(false);
        wisp::port::sim::set_timer_failure(false);
    }
};

struct EventLog {
    void append(const Event& event) {
        std::lock_guard<std::mutex> lock(mutex);
        topics.push_back(event.topic);
        changed.notify_all();
    }
    bool wait_for(size_t count) {
        std::unique_lock<std::mutex> lock(mutex);
        return changed.wait_for(lock, std::chrono::seconds(2),
                                [&] { return topics.size() >= count; });
    }
    std::vector<std::string> snapshot() {
        std::lock_guard<std::mutex> lock(mutex);
        return topics;
    }
    std::mutex mutex;
    std::condition_variable changed;
    std::vector<std::string> topics;
};

void check_resource_fallback(bool fail_task) {
    FailureReset failures;
    DeviceRegistry registry;
    auto power = std::make_shared<EdgeButton>("button.power");
    auto ordinary = std::make_shared<PollButton>("button.vol_up", 0);
    ASSERT_TRUE(registry.add(power));
    ASSERT_TRUE(registry.add(ordinary));
    EventLog events;
    EventBus bus;
    ASSERT_TRUE(bus.start());
    bus.subscribe("", [&](const Event& event) { events.append(event); });
    ButtonService service(registry, bus);
    if (fail_task) {
        wisp::port::sim::set_task_failure(true);
    } else {
        wisp::port::sim::set_alloc_failure(wisp::os::Mem::Fast, true);
    }
    ASSERT_TRUE(service.start());
    EXPECT_FALSE(power->has_sink());
    service.set_standby(true);
    const size_t ordinary_calls = ordinary->pressed_calls.load();
    power->down.store(true);
    ASSERT_TRUE(events.wait_for(1));
    power->down.store(false);
    ASSERT_TRUE(events.wait_for(3));
    EXPECT_EQ(ordinary->pressed_calls.load(), ordinary_calls);
    service.set_standby(false);
    ordinary->down.store(true);
    ASSERT_TRUE(events.wait_for(4));
    service.stop();
    ASSERT_TRUE(bus.wait_idle(1000));
    EXPECT_EQ(events.snapshot(),
              (std::vector<std::string>{"button.power.down", "button.power.up",
                                        "button.power.short", "button.vol_up.down"}));
    bus.stop();
}

void check_fallback_transition_with_timer_failure(bool fail_task, bool target_standby) {
    FailureReset failures;
    DeviceRegistry registry;
    auto power = std::make_shared<PollButton>("button.power", 100);
    auto ordinary = std::make_shared<PollButton>("button.vol_up", 0);
    ASSERT_TRUE(registry.add(power));
    ASSERT_TRUE(registry.add(ordinary));
    EventLog events;
    EventBus bus;
    ASSERT_TRUE(bus.start());
    bus.subscribe("", [&](const Event& event) { events.append(event); });
    ButtonService service(registry, bus);
    wisp::port::sim::set_alloc_failure(wisp::os::Mem::Fast, !fail_task);
    wisp::port::sim::set_task_failure(fail_task);
    ASSERT_TRUE(service.start());
    service.set_standby(!target_standby);
    ASSERT_TRUE(power->wait_for_samples(power->sample_times().size() + 1));

    wisp::port::sim::set_timer_failure(true);
    service.set_standby(target_standby);
    const size_t ordinary_calls = ordinary->pressed_calls.load();
    const size_t first_sample = power->sample_times().size();
    ASSERT_TRUE(power->wait_for_samples(first_sample + 3));
    auto samples = power->sample_times();
    // Millisecond deadlines may be rounded relative to the microsecond clock.
    const uint64_t minimum_interval_us = target_standby ? 99'000 : 19'000;
    EXPECT_GE(samples[first_sample + 1] - samples[first_sample], minimum_interval_us);
    EXPECT_GE(samples[first_sample + 2] - samples[first_sample + 1], minimum_interval_us);
    if (target_standby) {
        EXPECT_EQ(ordinary->pressed_calls.load(), ordinary_calls);
    } else {
        EXPECT_GT(ordinary->pressed_calls.load(), ordinary_calls);
    }
    power->down.store(true);
    ASSERT_TRUE(events.wait_for(1));
    power->down.store(false);
    ASSERT_TRUE(events.wait_for(3));

    // Restoring memory alone must not require restart or another mode request.
    wisp::port::sim::set_timer_failure(false);
    power->down.store(true);
    ASSERT_TRUE(events.wait_for(4));
    power->down.store(false);
    ASSERT_TRUE(events.wait_for(6));
    service.stop();
    ASSERT_TRUE(bus.wait_idle(1000));
    EXPECT_EQ(
        events.snapshot(),
        (std::vector<std::string>{"button.power.down", "button.power.up", "button.power.short",
                                  "button.power.down", "button.power.up", "button.power.short"}));
    bus.stop();
}

}  // namespace

TEST(ButtonService, QueueFallbackSurvivesTimerFailureEnteringStandby) {
    check_fallback_transition_with_timer_failure(false, true);
}

TEST(ButtonService, QueueFallbackSurvivesTimerFailureEnteringActive) {
    check_fallback_transition_with_timer_failure(false, false);
}

TEST(ButtonService, WorkerFallbackSurvivesTimerFailureEnteringStandby) {
    check_fallback_transition_with_timer_failure(true, true);
}

TEST(ButtonService, WorkerFallbackSurvivesTimerFailureEnteringActive) {
    check_fallback_transition_with_timer_failure(true, false);
}

TEST(ButtonService, QueueCreationFailureKeepsPowerKeyUsable) { check_resource_fallback(false); }

TEST(ButtonService, WorkerCreationFailureDetachesSinksAndKeepsPowerKeyUsable) {
    check_resource_fallback(true);
}

TEST(ButtonService, FallbackTimerFailureIsReported) {
    FailureReset failures;
    DeviceRegistry registry;
    auto power = std::make_shared<EdgeButton>("button.power");
    ASSERT_TRUE(registry.add(power));
    EventBus bus;
    ButtonService service(registry, bus);
    wisp::port::sim::set_alloc_failure(wisp::os::Mem::Fast, true);
    wisp::port::sim::set_timer_failure(true);
    auto started = service.start();
    ASSERT_FALSE(started);
    EXPECT_EQ(started.error().code, wisp::ErrorCode::NoMemory);
    EXPECT_FALSE(power->has_sink());
    service.stop();
    wisp::port::sim::set_alloc_failure(wisp::os::Mem::Fast, false);
    wisp::port::sim::set_timer_failure(false);
    ASSERT_TRUE(service.start());
    EXPECT_TRUE(power->has_sink());
    service.stop();
}

TEST(ButtonService, SinkRegistrationFailureUsesPerDevicePolling) {
    DeviceRegistry registry;
    auto power = std::make_shared<EdgeButton>("button.power");
    power->reject_sink = true;
    ASSERT_TRUE(registry.add(power));
    EventBus bus;
    ButtonService service(registry, bus);
    service.poll(0);
    EXPECT_FALSE(power->has_sink());
    const size_t active_calls = power->pressed_calls.load();
    service.poll(19);
    EXPECT_EQ(power->pressed_calls.load(), active_calls);
    service.poll(20);
    EXPECT_EQ(power->pressed_calls.load(), active_calls + 1);
    service.set_standby(true);
    service.poll(119);
    EXPECT_EQ(power->pressed_calls.load(), active_calls + 1);
    service.poll(120);
    EXPECT_EQ(power->pressed_calls.load(), active_calls + 2);
    service.set_standby(false);
    service.poll(140);
    EXPECT_EQ(power->pressed_calls.load(), active_calls + 3);
    service.stop();
}

TEST(ButtonService, DestructionWaitsForInFlightProducer) {
    DeviceRegistry registry;
    auto button = std::make_shared<EdgeButton>("button.power");
    ASSERT_TRUE(registry.add(button));
    EventBus bus;
    auto service = std::make_unique<ButtonService>(registry, bus);
    service->poll(0);
    wisp::os::Semaphore entered(1, 0);
    wisp::os::Semaphore resume(1, 0);
    std::thread producer([&] { button->emit(true, 1000, &entered, &resume); });
    const bool callback_entered = entered.take(1000);
    EXPECT_TRUE(callback_entered);
    std::atomic<bool> destroyed{false};
    std::thread destroyer([&] {
        service.reset();
        destroyed.store(true);
    });
    const bool detach_entered = button->detach_entered.take(1000);
    EXPECT_TRUE(detach_entered);
    EXPECT_FALSE(destroyed.load());
    resume.give();
    producer.join();
    destroyer.join();
    EXPECT_TRUE(destroyed.load());
    EXPECT_FALSE(button->emit(false, 50000));
}

TEST(ButtonService, RegistryRemovalRetainsDeviceUntilSinksAreDetached) {
    DeviceRegistry registry;
    auto button = std::make_shared<EdgeButton>("button.power");
    std::weak_ptr<EdgeButton> weak_button = button;
    ASSERT_TRUE(registry.add(button));
    EventBus bus;
    ButtonService service(registry, bus);
    ASSERT_TRUE(service.start());
    ASSERT_TRUE(registry.remove("button.power"));
    button.reset();
    EXPECT_FALSE(weak_button.expired());
    service.stop();
    EXPECT_TRUE(weak_button.expired());
}

TEST(ButtonService, RestartDoesNotTreatOldOverflowAsNewInput) {
    DeviceRegistry registry;
    auto button = std::make_shared<EdgeButton>("button.power");
    ASSERT_TRUE(registry.add(button));
    EventBus bus;
    ButtonService service(registry, bus);
    service.poll(0);
    for (uint64_t i = 0; i < 40; ++i) {
        button->emit(true, i * 1000);
    }
    button->down.store(false);
    service.poll(100);
    EXPECT_GT(button->pressed_calls.load(), 0u);
    service.stop();
    button->pressed_calls.store(0);
    service.poll(0);
    EXPECT_EQ(button->pressed_calls.load(), 0u);
    service.stop();
}

TEST(ButtonService, ConcurrentStandbyAndStopAreSafe) {
    DeviceRegistry registry;
    auto button = std::make_shared<EdgeButton>("button.power");
    ASSERT_TRUE(registry.add(button));
    EventBus bus;
    ButtonService service(registry, bus);
    ASSERT_TRUE(service.start());
    std::thread transitions([&] {
        for (int i = 0; i < 100; ++i) {
            service.set_standby(i % 2 != 0);
        }
    });
    service.stop();
    transitions.join();
    EXPECT_FALSE(button->has_sink());
    EXPECT_FALSE(button->emit(true, wisp::os::now_us()));
    ASSERT_TRUE(service.start());
    EXPECT_TRUE(button->has_sink());
    service.stop();
}

TEST(EdgeEvent, ShortPressQueuedBeforeWorkerRuns) {
    DeviceRegistry registry;
    auto btn = std::make_shared<EdgeButton>("button.power");
    ASSERT_TRUE(registry.add(btn));
    EventBus bus;
    ASSERT_TRUE(bus.start());
    EventLog events;
    bus.subscribe("", [&](const Event& event) { events.append(event); });
    ButtonService service(registry, bus);

    service.poll(0);  // Attach sinks without starting the worker.
    const uint64_t edge_start_us = wisp::os::now_us();
    btn->emit(true, edge_start_us);
    btn->emit(false, edge_start_us + 50'000);
    ASSERT_TRUE(service.start());  // Both records are already queued.
    ASSERT_TRUE(events.wait_for(3));
    service.stop();
    ASSERT_TRUE(bus.wait_idle(1000));
    EXPECT_EQ(events.snapshot(), (std::vector<std::string>{"button.power.down", "button.power.up",
                                                           "button.power.short"}));
    bus.stop();
}

TEST(EdgeEvent, LongPressFiresOnceWhileHeld) {
    DeviceRegistry registry;
    auto btn = std::make_shared<EdgeButton>("button.power");
    ASSERT_TRUE(registry.add(btn));
    EventLog events;
    EventBus bus;
    ASSERT_TRUE(bus.start());
    bus.subscribe("", [&](const Event& event) { events.append(event); });
    ButtonService service(registry, bus, 100);

    ASSERT_TRUE(service.start());
    const uint64_t pressed_at_us = wisp::os::now_us();
    btn->emit(true, pressed_at_us);
    ASSERT_TRUE(events.wait_for(2));  // Observe down and the actual long deadline.
    EXPECT_GE(wisp::os::now_us() - pressed_at_us, 120'000u);
    btn->emit(false, wisp::os::now_us());
    ASSERT_TRUE(events.wait_for(3));  // Wait for release processing, not bus idleness alone.
    service.stop();
    ASSERT_TRUE(bus.wait_idle(1000));
    EXPECT_EQ(events.snapshot(), (std::vector<std::string>{"button.power.down", "button.power.long",
                                                           "button.power.up"}));
    EXPECT_EQ(btn->pressed_calls.load(), 0u);
    bus.stop();
}

TEST(EdgeEvent, WakeMaintenanceIsDeferredUntilTaskDrain) {
    DeviceRegistry registry;
    auto button = std::make_shared<EdgeButton>("button.power");
    ASSERT_TRUE(registry.add(button));
    EventBus bus;
    ButtonService service(registry, bus);
    service.poll(0);
    ASSERT_TRUE(button->emit(true, 1000));
    EXPECT_EQ(button->maintenance_calls.load(), 0u);
    service.poll(1);
    EXPECT_EQ(button->maintenance_calls.load(), 1u);
    service.poll(100);
    EXPECT_EQ(button->maintenance_calls.load(), 1u);
    ASSERT_TRUE(button->emit(false, 101000));
    EXPECT_EQ(button->maintenance_calls.load(), 1u);
    service.poll(102);
    EXPECT_EQ(button->maintenance_calls.load(), 2u);
}

TEST(EdgeEvent, OverflowAlsoMaintainsWakeForDroppedRelease) {
    DeviceRegistry registry;
    auto button = std::make_shared<EdgeButton>("button.power");
    ASSERT_TRUE(registry.add(button));
    EventBus bus;
    ButtonService service(registry, bus);
    service.poll(0);
    for (unsigned i = 0; i < 32; ++i) ASSERT_TRUE(button->emit(true, 1000 + i));
    ASSERT_FALSE(button->emit(false, 2000));
    EXPECT_EQ(button->maintenance_calls.load(), 0u);
    service.poll(3);
    EXPECT_EQ(button->maintenance_calls.load(), 33u);
    EXPECT_FALSE(button->down.load());
}

TEST(EdgeEvent, DebounceAndDuplicateEdges) {
    DeviceRegistry registry;
    auto btn = std::make_shared<EdgeButton>("button.power");
    ASSERT_TRUE(registry.add(btn));
    EventBus bus;
    ASSERT_TRUE(bus.start());
    std::vector<std::string> topics;
    bus.subscribe("", [&](const Event& event) { topics.push_back(event.topic); });
    ButtonService service(registry, bus);

    service.poll(0);
    btn->emit(true, 0);
    btn->emit(true, 5000);
    btn->emit(false, 10000);
    btn->emit(true, 40000);
    btn->emit(true, 45000);
    btn->emit(false, 90000);
    service.poll(110);
    ASSERT_TRUE(bus.wait_idle(1000));
    EXPECT_EQ(topics, (std::vector<std::string>{"button.power.down", "button.power.up",
                                                "button.power.short"}));
    service.stop();
    bus.stop();
}

TEST(EdgeEvent, LongDeadlineBeforeDebouncedRelease) {
    DeviceRegistry registry;
    auto button = std::make_shared<EdgeButton>("button.power");
    ASSERT_TRUE(registry.add(button));
    EventBus bus;
    ASSERT_TRUE(bus.start());
    std::vector<std::string> topics;
    bus.subscribe("", [&](const Event& event) { topics.push_back(event.topic); });
    ButtonService service(registry, bus, 100);

    service.poll(0);
    button->emit(true, 0);
    service.poll(20);             // Long deadline is 120 ms, after the confirmed press.
    button->emit(false, 110000);  // Release is confirmed at 130 ms.
    service.poll(130);
    service.stop();
    ASSERT_TRUE(bus.wait_idle(1000));
    EXPECT_EQ(topics, (std::vector<std::string>{"button.power.down", "button.power.long",
                                                "button.power.up"}));
    bus.stop();
}

TEST(PollOnly, ModeSwitchUsesPerDevicePolicy) {
    DeviceRegistry registry;
    auto button = std::make_shared<PollButton>("button.vol_up", 0);
    ASSERT_TRUE(registry.add(button));
    EventBus bus;
    ButtonService service(registry, bus);

    service.poll(0);
    const size_t initial_calls = button->pressed_calls.load();
    service.poll(20);
    EXPECT_EQ(button->pressed_calls.load(), initial_calls + 1);
    service.set_standby(true);
    service.poll(200);
    EXPECT_EQ(button->pressed_calls.load(), initial_calls + 1);
    service.stop();
}

TEST(MixedInputs, EdgeButtonIsNeverPolled) {
    DeviceRegistry registry;
    auto edge = std::make_shared<EdgeButton>("button.power");
    auto polled = std::make_shared<PollButton>("button.vol_up", 0);
    ASSERT_TRUE(registry.add(edge));
    ASSERT_TRUE(registry.add(polled));
    EventBus bus;
    ASSERT_TRUE(bus.start());
    ButtonService service(registry, bus);

    service.poll(0);
    edge->emit(true, 0);
    service.poll(20);
    edge->emit(false, 50000);
    service.poll(70);
    EXPECT_EQ(edge->pressed_calls.load(), 0u);
    EXPECT_EQ(polled->pressed_calls.load(), 3u);
    service.stop();
    bus.stop();
}

TEST(PollOnly, ActivePollsAt20Ms) {
    DeviceRegistry registry;
    auto button = std::make_shared<PollButton>("button.vol_up", 0);
    ASSERT_TRUE(registry.add(button));
    EventBus bus;
    ButtonService service(registry, bus);

    service.poll(0);
    const size_t initial_calls = button->pressed_calls.load();
    service.poll(19);
    EXPECT_EQ(button->pressed_calls.load(), initial_calls);
    service.poll(20);
    EXPECT_EQ(button->pressed_calls.load(), initial_calls + 1);
    service.stop();
}

TEST(PollOnly, StandbyStopsOrdinaryButton) {
    DeviceRegistry registry;
    auto button = std::make_shared<PollButton>("button.vol_up", 0);
    ASSERT_TRUE(registry.add(button));
    EventBus bus;
    ButtonService service(registry, bus);

    service.poll(0);
    const size_t initial_calls = button->pressed_calls.load();
    service.set_standby(true);
    service.poll(20);
    service.poll(1000);
    EXPECT_EQ(button->pressed_calls.load(), initial_calls);
    service.stop();
}

TEST(PollOnly, PowerFallbackPollsAt100Ms) {
    DeviceRegistry registry;
    auto button = std::make_shared<PollButton>("button.power", 100);
    ASSERT_TRUE(registry.add(button));
    EventBus bus;
    ButtonService service(registry, bus);

    service.poll(0);
    const size_t active_calls = button->pressed_calls.load();
    service.set_standby(true);
    service.poll(99);
    EXPECT_EQ(button->pressed_calls.load(), active_calls);
    service.poll(100);
    EXPECT_EQ(button->pressed_calls.load(), active_calls + 1);
    service.set_standby(false);
    service.poll(119);
    EXPECT_EQ(button->pressed_calls.load(), active_calls + 1);
    service.poll(120);
    EXPECT_EQ(button->pressed_calls.load(), active_calls + 2);
    service.stop();
}

TEST(EdgeEvent, HeldAcrossWake) {
    DeviceRegistry registry;
    auto btn = std::make_shared<EdgeButton>("button.power");
    ASSERT_TRUE(registry.add(btn));
    EventBus bus;
    ASSERT_TRUE(bus.start());
    std::vector<std::string> topics;
    bus.subscribe("", [&](const Event& event) { topics.push_back(event.topic); });
    ButtonService service(registry, bus);

    service.poll(0);
    btn->emit(true, 0);
    service.poll(20);
    service.set_standby(true);
    service.poll(500);
    service.set_standby(false);
    service.poll(600);
    btn->emit(false, 700000);
    service.poll(720);
    ASSERT_TRUE(bus.wait_idle(1000));
    EXPECT_EQ(topics, (std::vector<std::string>{"button.power.down", "button.power.up",
                                                "button.power.short"}));
    service.stop();
    bus.stop();
}

TEST(ButtonService, ModeSwitchKeepsPendingLongPress) {
    DeviceRegistry registry;
    auto btn = std::make_shared<EdgeButton>("button.power");
    ASSERT_TRUE(registry.add(btn));
    EventBus bus;
    ASSERT_TRUE(bus.start());
    std::vector<std::string> topics;
    bus.subscribe("", [&](const Event& event) { topics.push_back(event.topic); });
    ButtonService service(registry, bus, 100);

    service.poll(0);
    btn->emit(true, 0);
    service.poll(20);
    service.set_standby(true);
    service.poll(120);
    service.set_standby(false);
    service.poll(150);
    btn->emit(false, 160000);
    service.poll(180);
    ASSERT_TRUE(bus.wait_idle(1000));
    EXPECT_EQ(topics, (std::vector<std::string>{"button.power.down", "button.power.long",
                                                "button.power.up"}));
    service.stop();
    bus.stop();
}

TEST(ButtonService, EdgeQueueOverflowRecovers) {
    DeviceRegistry registry;
    auto btn = std::make_shared<EdgeButton>("button.power");
    ASSERT_TRUE(registry.add(btn));
    EventBus bus;
    ASSERT_TRUE(bus.start());
    std::vector<std::string> topics;
    bus.subscribe("", [&](const Event& event) { topics.push_back(event.topic); });
    ButtonService service(registry, bus);

    service.poll(0);
    btn->emit(true, 0);
    service.poll(20);
    btn->emit(false, 30000);
    for (uint64_t i = 0; i < 70; ++i) {
        btn->emit(true, 40000 + i * 1000);
    }
    btn->down.store(false);
    service.poll(120);
    ASSERT_TRUE(bus.wait_idle(1000));
    EXPECT_EQ(topics, (std::vector<std::string>{"button.power.down", "button.power.up",
                                                "button.power.short"}));
    service.stop();
    bus.stop();
}

TEST(ButtonService, BaseButtonDefaultsToPollOnlyWithoutStandbyFallback) {
    FakeButton device("button.power");
    const wisp::hal::Button& button = device;
    EXPECT_EQ(button.input_mode(), wisp::hal::ButtonInputMode::PollOnly);
    EXPECT_EQ(button.standby_poll_ms(), 0u);
    wisp::hal::ButtonEdgeSink sink;
    EXPECT_EQ(sink.context, nullptr);
    EXPECT_EQ(sink.push, nullptr);
    auto result = device.set_edge_sink(sink);
    ASSERT_FALSE(result);
    EXPECT_EQ(result.error().code, wisp::ErrorCode::Unsupported);
}

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

TEST(ButtonService, StandbyWhileRunningPreservesPendingLongPress) {
    DeviceRegistry registry;
    auto btn = std::make_shared<PollButton>("button.power", 100);
    ASSERT_TRUE(registry.add(btn));
    EventLog events;
    EventBus bus;
    ASSERT_TRUE(bus.start());
    bus.subscribe("", [&](const Event& e) { events.append(e); });
    ButtonService svc(registry, bus, 200, 5);
    ASSERT_TRUE(svc.start());
    btn->down = true;
    ASSERT_TRUE(events.wait_for(1));
    svc.set_standby(true);
    ASSERT_TRUE(events.wait_for(2));
    btn->down = false;
    ASSERT_TRUE(events.wait_for(3));
    svc.stop();
    ASSERT_TRUE(bus.wait_idle(1000));
    EXPECT_EQ(events.snapshot(), (std::vector<std::string>{"button.power.down", "button.power.long",
                                                           "button.power.up"}));
    bus.stop();
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

// A second start() call while the Timer is
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

// A button already held when start() runs (e.g. the power key still
// down from the press that powered the board on) produces no events for
// that press; the next press after its release is reported normally.
TEST(ButtonService, ButtonHeldAtStartIsIgnoredUntilReleased) {
    DeviceRegistry registry;
    auto btn = std::make_shared<FakeButton>("button.power");
    registry.add(btn);
    EventBus bus;
    bus.start();
    std::vector<std::string> topics;
    bus.subscribe("", [&](const Event& e) { topics.push_back(e.topic); });

    ButtonService svc(registry, bus, /*long_press_ms=*/50, /*poll_ms=*/5);
    btn->down = true;
    svc.start();
    wisp::os::sleep_ms(100);  // well past long_press_ms while still held
    btn->down = false;
    wisp::os::sleep_ms(20);
    btn->down = true;
    wisp::os::sleep_ms(20);
    btn->down = false;
    wisp::os::sleep_ms(20);
    svc.stop();
    ASSERT_TRUE(bus.wait_idle(1000));

    EXPECT_EQ(topics, (std::vector<std::string>{"button.power.down", "button.power.up",
                                                "button.power.short"}));
}

TEST(ButtonService, ManualPollCannotBypassRunningProductionCadence) {
    DeviceRegistry registry;
    auto button = std::make_shared<PollButton>("button.power", 100);
    ASSERT_TRUE(registry.add(button));
    EventBus bus;
    ButtonService service(registry, bus, 1000, 1000);
    ASSERT_TRUE(service.start());
    ASSERT_TRUE(button->wait_for_samples(2));  // startup check + first scheduled poll
    const auto reads = button->pressed_calls.load();
    for (int i = 0; i < 100; ++i) service.poll(wisp::os::now_ms() + 100000);
    EXPECT_EQ(button->pressed_calls.load(), reads);
    service.stop();
}
