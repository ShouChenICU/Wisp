#include "wisp/core/button_service.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <functional>
#include <memory>
#include <mutex>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "support/fake_devices.hpp"
#include "support/wait.hpp"
#include "wisp/core/device_registry.hpp"
#include "wisp/core/dispatcher.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/hal/roles.hpp"
#include "wisp/os/os.hpp"
#include "wisp/port/sim/sim_os.hpp"

using wisp::core::ButtonService;
using wisp::core::DeviceRegistry;
using wisp::core::Event;
using wisp::core::EventBus;
using wisp::test::FakeButton;

namespace {

wisp::Properties role_caps(wisp::Value role) { return wisp::Properties{{"role", std::move(role)}}; }

// The power key, which ButtonService keeps polling in standby.
wisp::Properties power_caps() { return role_caps(wisp::Value(wisp::hal::role::kPower)); }

// registry has one FakeButton "button.power" (role power); bus is started and
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
        btn = std::make_shared<FakeButton>("button.power", power_caps());
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
    explicit EdgeButton(std::string id, wisp::Properties caps = {}) {
        info_.id = std::move(id);
        info_.cls = kClass;
        info_.model = "EDGE_FAKE";
        info_.caps = std::move(caps);
    }

    const wisp::hal::DeviceInfo& info() const override { return info_; }
    bool pressed() override {
        ++pressed_calls;
        const bool level = down.load();
        if (on_sample) on_sample();
        return level;
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
    std::function<void()> on_sample;
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
    PollButton(std::string id, uint32_t standby_poll_ms, wisp::Properties caps = {})
        : FakeButton(std::move(id), std::move(caps)), standby_poll_ms_(standby_poll_ms) {}

    bool pressed() override {
        ++pressed_calls;
        {
            std::lock_guard<std::mutex> lock(samples_mutex_);
            sample_times_us_.push_back(wisp::os::now_us());
            samples_changed_.notify_all();
        }
        if (on_sample) on_sample();
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

    std::function<void()> on_sample;
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
    auto power = std::make_shared<EdgeButton>("button.power", power_caps());
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
    auto power = std::make_shared<PollButton>("button.power", 100, power_caps());
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

TEST(ButtonService, OverflowDuringPollSampleDoesNotReplayOldPress) {
    DeviceRegistry registry;
    auto power = std::make_shared<EdgeButton>("button.power", power_caps());
    auto talk = std::make_shared<EdgeButton>("button.talk");
    auto polled = std::make_shared<PollButton>("button.pmu", 0);
    ASSERT_TRUE(registry.add(power));
    ASSERT_TRUE(registry.add(talk));
    ASSERT_TRUE(registry.add(polled));
    EventLog events;
    EventBus bus;
    ASSERT_TRUE(bus.start());
    bus.subscribe("", [&](const Event& event) { events.append(event); });
    ButtonService service(registry, bus, 100);
    service.poll(0);
    polled->on_sample = [&] {
        for (unsigned i = 0; i < 31; ++i) EXPECT_TRUE(talk->emit(i % 2 == 0, 21'000 + i));
        EXPECT_TRUE(power->emit(true, 22'000));
        EXPECT_FALSE(talk->emit(false, 23'000));
        EXPECT_FALSE(power->emit(false, 23'001));
    };
    service.poll(20);
    polled->on_sample = {};
    service.poll(50);
    service.poll(200);
    ASSERT_TRUE(bus.wait_idle(1000));
    EXPECT_TRUE(events.snapshot().empty());
    EXPECT_FALSE(power->down.load());
    EXPECT_FALSE(talk->down.load());
    ASSERT_TRUE(power->emit(true, 210'000));
    ASSERT_TRUE(power->emit(false, 260'000));
    service.poll(280);
    ASSERT_TRUE(bus.wait_idle(1000));
    EXPECT_EQ(events.snapshot(), (std::vector<std::string>{"button.power.down", "button.power.up",
                                                           "button.power.short"}));
    service.stop();
    bus.stop();
}

TEST(ButtonService, OverflowSnapshotRetriesWhenProducerContinuesDuringSampling) {
    DeviceRegistry registry;
    auto power = std::make_shared<EdgeButton>("button.power", power_caps());
    ASSERT_TRUE(registry.add(power));
    EventLog events;
    EventBus bus;
    ASSERT_TRUE(bus.start());
    bus.subscribe("", [&](const Event& event) { events.append(event); });
    ButtonService service(registry, bus, 100);
    service.poll(0);
    for (unsigned i = 0; i < 33; ++i) power->emit(true, 1000 + i);
    unsigned samples = 0;
    power->on_sample = [&] {
        ++samples;
        // Change the physical level after pressed() has captured its result.
        // Alternate enough attempts to require progress across worker turns.
        if (samples <= 4) {
            for (unsigned i = 0; i < 33; ++i) power->emit(i % 2 != 0, 2000 + samples * 100 + i);
            power->emit(false, 3000 + samples * 100);
        }
    };
    for (uint64_t now = 10; now <= 60; now += 10) service.poll(now);
    power->on_sample = {};
    service.poll(300);
    ASSERT_TRUE(bus.wait_idle(1000));
    EXPECT_GE(samples, 5u);
    EXPECT_TRUE(events.snapshot().empty());
    ASSERT_TRUE(power->emit(true, 310'000));
    ASSERT_TRUE(power->emit(false, 360'000));
    service.poll(380);
    ASSERT_TRUE(bus.wait_idle(1000));
    EXPECT_EQ(events.snapshot(), (std::vector<std::string>{"button.power.down", "button.power.up",
                                                           "button.power.short"}));
    service.stop();
    bus.stop();
}

void check_boot_held_fallback(unsigned failure) {
    FailureReset failures;
    DeviceRegistry registry;
    auto power = std::make_shared<EdgeButton>("button.power", power_caps());
    power->down.store(true);
    power->reject_sink = failure == 2;
    auto clock = std::make_shared<PollButton>("button.clock", 0);
    ASSERT_TRUE(registry.add(power));
    ASSERT_TRUE(registry.add(clock));
    EventLog events;
    EventBus bus;
    ASSERT_TRUE(bus.start());
    bus.subscribe("", [&](const Event& event) { events.append(event); });
    ButtonService service(registry, bus, 40, 5);
    wisp::port::sim::set_task_failure(failure == 0);
    wisp::port::sim::set_alloc_failure(wisp::os::Mem::Fast, failure == 1);
    ASSERT_TRUE(service.start());
    ASSERT_TRUE(clock->wait_for_samples(20));
    ASSERT_TRUE(bus.wait_idle(1000));
    EXPECT_TRUE(events.snapshot().empty());
    power->down.store(false);
    ASSERT_TRUE(clock->wait_for_samples(clock->sample_times().size() + 4));
    ASSERT_TRUE(bus.wait_idle(1000));
    EXPECT_TRUE(events.snapshot().empty());
    power->down.store(true);
    ASSERT_TRUE(events.wait_for(1));
    power->down.store(false);
    ASSERT_TRUE(events.wait_for(3));
    service.stop();
    ASSERT_TRUE(bus.wait_idle(1000));
    EXPECT_EQ(events.snapshot(), (std::vector<std::string>{"button.power.down", "button.power.up",
                                                           "button.power.short"}));
    bus.stop();
}

TEST(ButtonService, WorkerFailureSuppressesBootHeld) { check_boot_held_fallback(0); }
TEST(ButtonService, QueueFailureSuppressesBootHeld) { check_boot_held_fallback(1); }
TEST(ButtonService, AttachFailureSuppressesBootHeld) { check_boot_held_fallback(2); }

TEST(ButtonService, WorkerFailurePreservesQueuedGestureAfterAttach) {
    FailureReset failures;
    DeviceRegistry registry;
    auto power = std::make_shared<EdgeButton>("button.power", power_caps());
    ASSERT_TRUE(registry.add(power));
    EventLog events;
    EventBus bus;
    ASSERT_TRUE(bus.start());
    bus.subscribe("", [&](const Event& event) { events.append(event); });
    ButtonService service(registry, bus);
    service.poll(0);
    ASSERT_TRUE(power->emit(true, wisp::os::now_us()));
    wisp::os::sleep_ms(30);
    ASSERT_TRUE(power->emit(false, wisp::os::now_us()));
    wisp::os::sleep_ms(25);
    wisp::port::sim::set_task_failure(true);
    ASSERT_TRUE(service.start());
    ASSERT_TRUE(events.wait_for(3));
    service.stop();
    ASSERT_TRUE(bus.wait_idle(1000));
    EXPECT_EQ(events.snapshot(), (std::vector<std::string>{"button.power.down", "button.power.up",
                                                           "button.power.short"}));
    bus.stop();
}

TEST(ButtonService, WorkerFailurePreservesNewHeldPressAfterAttach) {
    FailureReset failures;
    DeviceRegistry registry;
    auto power = std::make_shared<EdgeButton>("button.power", power_caps());
    ASSERT_TRUE(registry.add(power));
    EventLog events;
    EventBus bus;
    ASSERT_TRUE(bus.start());
    bus.subscribe("", [&](const Event& event) { events.append(event); });
    ButtonService service(registry, bus, 50, 5);
    service.poll(0);
    ASSERT_TRUE(power->emit(true, wisp::os::now_us()));
    wisp::port::sim::set_task_failure(true);
    ASSERT_TRUE(service.start());
    ASSERT_TRUE(events.wait_for(2));
    power->down.store(false);
    ASSERT_TRUE(events.wait_for(3));
    service.stop();
    ASSERT_TRUE(bus.wait_idle(1000));
    EXPECT_EQ(events.snapshot(), (std::vector<std::string>{"button.power.down", "button.power.long",
                                                           "button.power.up"}));
    bus.stop();
}

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
    auto power = std::make_shared<EdgeButton>("button.power", power_caps());
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
    auto power = std::make_shared<EdgeButton>("button.power", power_caps());
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
    auto button = std::make_shared<EdgeButton>("button.power", power_caps());
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
    auto button = std::make_shared<EdgeButton>("button.power", power_caps());
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
    auto button = std::make_shared<EdgeButton>("button.power", power_caps());
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
    auto button = std::make_shared<EdgeButton>("button.power", power_caps());
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
    auto btn = std::make_shared<EdgeButton>("button.power", power_caps());
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
    auto btn = std::make_shared<EdgeButton>("button.power", power_caps());
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
    auto button = std::make_shared<EdgeButton>("button.power", power_caps());
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
    auto button = std::make_shared<EdgeButton>("button.power", power_caps());
    ASSERT_TRUE(registry.add(button));
    EventBus bus;
    ButtonService service(registry, bus);
    service.poll(0);
    for (unsigned i = 0; i < 32; ++i) ASSERT_TRUE(button->emit(true, 1000 + i));
    ASSERT_FALSE(button->emit(false, 2000));
    EXPECT_EQ(button->maintenance_calls.load(), 0u);
    service.poll(3);
    // Overflow discards the ambiguous records; the recovery sample still
    // performs deferred wake maintenance for the dropped release.
    EXPECT_EQ(button->maintenance_calls.load(), 1u);
    EXPECT_FALSE(button->down.load());
}

TEST(EdgeEvent, DebounceAndDuplicateEdges) {
    DeviceRegistry registry;
    auto btn = std::make_shared<EdgeButton>("button.power", power_caps());
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
    auto button = std::make_shared<EdgeButton>("button.power", power_caps());
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
    auto edge = std::make_shared<EdgeButton>("button.power", power_caps());
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
    auto button = std::make_shared<PollButton>("button.power", 100, power_caps());
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

TEST(ButtonService, StandbyPollsTheKeyWithThePowerRole) {
    DeviceRegistry registry;
    auto button = std::make_shared<PollButton>("button.side", 100, power_caps());
    ASSERT_TRUE(registry.add(button));
    EventBus bus;
    ASSERT_TRUE(bus.start());
    std::vector<std::string> topics;
    bus.subscribe("", [&](const Event& e) { topics.push_back(e.topic); });
    ButtonService service(registry, bus);

    service.poll(0);
    const size_t active_calls = button->pressed_calls.load();
    service.set_standby(true);
    button->down = true;
    service.poll(99);
    EXPECT_EQ(button->pressed_calls.load(), active_calls);
    service.poll(100);
    EXPECT_EQ(button->pressed_calls.load(), active_calls + 1);
    button->down = false;
    service.poll(200);
    EXPECT_EQ(button->pressed_calls.load(), active_calls + 2);
    service.stop();
    ASSERT_TRUE(bus.wait_idle(1000));
    EXPECT_EQ(topics, (std::vector<std::string>{"button.side.down", "button.side.up",
                                                "button.side.short"}));
    bus.stop();
}

// One key may be both the power and the talk key.
TEST(ButtonService, StandbyPollsAPowerKeyThatAlsoHasOtherRoles) {
    DeviceRegistry registry;
    auto button = std::make_shared<PollButton>(
        "button.side", 100,
        role_caps(wisp::Value(wisp::List{wisp::Value(wisp::hal::role::kTalk),
                                         wisp::Value(wisp::hal::role::kPower)})));
    ASSERT_TRUE(registry.add(button));
    EventBus bus;
    ButtonService service(registry, bus);

    service.poll(0);
    const size_t active_calls = button->pressed_calls.load();
    service.set_standby(true);
    service.poll(100);
    EXPECT_EQ(button->pressed_calls.load(), active_calls + 1);
    service.stop();
}

// The id alone does not make a key the power key: a board without a
// power-role key polls nothing in standby.
TEST(ButtonService, WithoutThePowerRoleNoKeyIsPolledInStandby) {
    DeviceRegistry registry;
    auto button = std::make_shared<PollButton>("button.power", 100);
    ASSERT_TRUE(registry.add(button));
    EventBus bus;
    ButtonService service(registry, bus);

    service.poll(0);
    const size_t active_calls = button->pressed_calls.load();
    service.set_standby(true);
    service.poll(100);
    service.poll(1000);
    EXPECT_EQ(button->pressed_calls.load(), active_calls);
    service.stop();
}

TEST(EdgeEvent, HeldAcrossWake) {
    DeviceRegistry registry;
    auto btn = std::make_shared<EdgeButton>("button.power", power_caps());
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
    auto btn = std::make_shared<EdgeButton>("button.power", power_caps());
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
    auto btn = std::make_shared<EdgeButton>("button.power", power_caps());
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
    FakeButton device("button.power", power_caps());
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
    auto btn = std::make_shared<PollButton>("button.power", 100, power_caps());
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
    auto btn = std::make_shared<FakeButton>("button.power", power_caps());
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
    auto btn = std::make_shared<FakeButton>("button.power", power_caps());
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
    auto button = std::make_shared<PollButton>("button.power", 100, power_caps());
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

namespace {

using wisp::core::ButtonEdge;
using Kind = wisp::core::ButtonEdge::Kind;

struct SeenEdge {
    std::string id;
    Kind kind;
    uint64_t at_us;
    bool operator==(const SeenEdge&) const = default;
};

// Removes its listener when the scope ends: declared after what the
// listener captures, it goes first, and the service never outlives a
// listener (its destructor reports one left).
struct ScopedListener {
    ScopedListener(ButtonService& service, wisp::core::ButtonListener fn)
        : service(service), id(service.add_listener(std::move(fn))) {}
    ~ScopedListener() { service.remove_listener(id); }
    ScopedListener(const ScopedListener&) = delete;
    ScopedListener& operator=(const ScopedListener&) = delete;
    ButtonService& service;
    const uint32_t id;
};

std::mutex g_misuse_mu;
std::vector<std::string> g_misuses;
void record_misuse(const char* what) {
    std::lock_guard<std::mutex> lock(g_misuse_mu);
    g_misuses.emplace_back(what);
}

}  // namespace

TEST(ButtonService, ListenerGetsEveryEdgeInOrderWithItsTime) {
    Fixture f;
    std::vector<SeenEdge> seen;  // poll() runs the listeners on this thread
    ScopedListener listener(f.svc, [&](const ButtonEdge& e) {
        seen.push_back({std::string(e.id.view()), e.kind, e.at_us});
    });

    // A press of 1.2 s. The poll at 1100 ms finds the long deadline past:
    // Long carries the deadline (Down + 1 s), not the time it was found.
    f.btn->down = true;
    f.svc.poll(0);
    f.svc.poll(1100);
    f.btn->down = false;
    f.svc.poll(1200);
    // A short press.
    f.btn->down = true;
    f.svc.poll(2000);
    f.btn->down = false;
    f.svc.poll(2300);

    EXPECT_EQ(seen, (std::vector<SeenEdge>{{"button.power", Kind::Down, 0},
                                           {"button.power", Kind::Long, 1'000'000},
                                           {"button.power", Kind::Up, 1'200'000},
                                           {"button.power", Kind::Down, 2'000'000},
                                           {"button.power", Kind::Up, 2'300'000},
                                           {"button.power", Kind::Short, 2'300'000}}));
}

TEST(EdgeEvent, ListenerTimesAreTheConfirmedEdges) {
    DeviceRegistry registry;
    auto button = std::make_shared<EdgeButton>("button.power", power_caps());
    ASSERT_TRUE(registry.add(button));
    EventBus bus;
    ButtonService service(registry, bus);
    std::vector<SeenEdge> seen;
    ScopedListener listener(service, [&](const ButtonEdge& e) {
        seen.push_back({std::string(e.id.view()), e.kind, e.at_us});
    });

    service.poll(0);
    ASSERT_TRUE(button->emit(true, 0));
    service.poll(30);  // Down is confirmed 20 ms after the press.
    // Released before the long deadline (1020 ms), confirmed after it:
    // the release commits the Long first, at its deadline.
    ASSERT_TRUE(button->emit(false, 1'010'000));
    service.poll(1100);
    service.stop();

    EXPECT_EQ(seen, (std::vector<SeenEdge>{{"button.power", Kind::Down, 20'000},
                                           {"button.power", Kind::Long, 1'020'000},
                                           {"button.power", Kind::Up, 1'030'000}}));
}

TEST(ButtonService, RemovedListenerIsNeverCalledAgain) {
    Fixture f;
    wisp::os::Semaphore entered{1, 0};
    wisp::os::Semaphore resume{1, 0};
    std::atomic<int> calls{0};
    std::atomic<bool> removed{false};
    std::atomic<int> calls_after_removal{0};
    const uint32_t id = f.svc.add_listener([&](const ButtonEdge&) {
        if (removed.load()) ++calls_after_removal;
        if (calls.fetch_add(1) == 0) {
            entered.give();
            resume.take(wisp::os::kForever);
        }
        if (removed.load()) ++calls_after_removal;
    });
    EXPECT_GE(id, 1u);
    // Whatever assertion fails below, the worker is let out of the
    // listener and the remover joined before the fixture's stop(), which
    // would otherwise wait for the parked worker forever; then the
    // listener is removed (again: a no-op once the remover ran).
    std::thread remover;
    wisp::os::Semaphore remover_started{1, 0};
    wisp::os::Semaphore remover_done{1, 0};
    std::atomic<bool> resumed{false};
    struct Release {
        std::function<void()> fn;
        ~Release() { fn(); }
    } release{[&] {
        if (!resumed.exchange(true)) resume.give();
        if (remover.joinable()) remover.join();
        f.svc.remove_listener(id);
    }};
    ASSERT_TRUE(f.svc.start());

    f.btn->down = true;
    ASSERT_TRUE(entered.take(2000));  // the worker is inside the listener, for Down
    remover = std::thread([&] {
        remover_started.give();
        f.svc.remove_listener(id);
        removed.store(true);
        remover_done.give();
    });
    ASSERT_TRUE(remover_started.take(2000));
    // The removal waits for the call in progress: this wait is expected
    // to time out.
    EXPECT_FALSE(remover_done.take(50));
    resumed.store(true);
    resume.give();
    ASSERT_TRUE(remover_done.take(2000));
    remover.join();

    // A listener added while the worker runs gets the rest of the press;
    // the removed one gets none of it.
    wisp::os::Semaphore short_seen{1, 0};
    ScopedListener late(f.svc, [&](const ButtonEdge& e) {
        if (e.kind == Kind::Short) short_seen.give();
    });
    f.btn->down = false;
    ASSERT_TRUE(short_seen.take(2000));
    EXPECT_EQ(calls.load(), 1);
    EXPECT_EQ(calls_after_removal.load(), 0);
}

TEST(ButtonService, BusEventsAreUnchanged) {
    DeviceRegistry registry;
    auto button = std::make_shared<FakeButton>("button.power", power_caps());
    ASSERT_TRUE(registry.add(button));
    EventBus bus;  // started only after the presses: events wait in its queue
    std::vector<std::pair<std::string, std::string>> events;
    bus.subscribe("", [&](const Event& e) { events.emplace_back(e.topic, e.source); });
    ButtonService service(registry, bus);
    std::vector<bool> bus_empty_at_call;
    std::vector<SeenEdge> seen;
    ScopedListener listener(service, [&](const ButtonEdge& e) {
        // Listeners run before the bus copy of the same edge is published.
        bus_empty_at_call.push_back(seen.empty() && bus.wait_idle(0));
        seen.push_back({std::string(e.id.view()), e.kind, e.at_us});
    });

    button->down = true;
    service.poll(0);
    service.poll(1000);
    button->down = false;
    service.poll(1100);
    button->down = true;
    service.poll(2000);
    button->down = false;
    service.poll(2100);
    ASSERT_TRUE(bus.start());
    ASSERT_TRUE(bus.wait_idle(1000));
    bus.stop();

    ASSERT_FALSE(bus_empty_at_call.empty());
    EXPECT_TRUE(bus_empty_at_call.front());
    EXPECT_EQ(seen.size(), 6u);
    const std::string id = "button.power";
    EXPECT_EQ(events, (std::vector<std::pair<std::string, std::string>>{{id + ".down", id},
                                                                        {id + ".long", id},
                                                                        {id + ".up", id},
                                                                        {id + ".down", id},
                                                                        {id + ".up", id},
                                                                        {id + ".short", id}}));
}

TEST(ButtonService, EmitForTestReachesListenersAndTheBus) {
    DeviceRegistry registry;  // no button device at all
    EventBus bus;
    std::vector<std::pair<std::string, std::string>> events;
    bus.subscribe("", [&](const Event& e) { events.emplace_back(e.topic, e.source); });
    ButtonService service(registry, bus);
    std::vector<SeenEdge> seen;
    std::vector<bool> bus_empty_at_call;
    ScopedListener listener(service, [&](const ButtonEdge& e) {
        bus_empty_at_call.push_back(bus.wait_idle(0));
        seen.push_back({std::string(e.id.view()), e.kind, e.at_us});
    });

    service.emit_for_test("button.side", Kind::Down, 5);
    service.emit_for_test("button.side", Kind::Long, 1'000'005);
    service.emit_for_test("button.side", Kind::Up, 1'200'000);
    service.emit_for_test("button.side", Kind::Short, 7);
    ASSERT_TRUE(bus.start());
    ASSERT_TRUE(bus.wait_idle(1000));
    bus.stop();

    EXPECT_EQ(seen, (std::vector<SeenEdge>{{"button.side", Kind::Down, 5},
                                           {"button.side", Kind::Long, 1'000'005},
                                           {"button.side", Kind::Up, 1'200'000},
                                           {"button.side", Kind::Short, 7}}));
    EXPECT_EQ(bus_empty_at_call, (std::vector<bool>{true, false, false, false}));
    const std::string id = "button.side";
    EXPECT_EQ(events,
              (std::vector<std::pair<std::string, std::string>>{
                  {id + ".down", id}, {id + ".long", id}, {id + ".up", id}, {id + ".short", id}}));
}

TEST(ButtonService, ListenersRunInRegistrationOrder) {
    DeviceRegistry registry;
    EventBus bus;
    ButtonService service(registry, bus);
    std::vector<int> order;
    const uint32_t first = service.add_listener([&](const ButtonEdge&) { order.push_back(1); });
    const uint32_t second = service.add_listener([&](const ButtonEdge&) { order.push_back(2); });
    const uint32_t third = service.add_listener([&](const ButtonEdge&) { order.push_back(3); });
    EXPECT_EQ(first, 1u);
    EXPECT_EQ(second, 2u);
    service.emit_for_test("button.side", Kind::Down, 0);
    service.remove_listener(second);
    service.remove_listener(second);  // a second removal is harmless
    service.emit_for_test("button.side", Kind::Up, 0);
    EXPECT_EQ(order, (std::vector<int>{1, 2, 3, 1, 3}));
    service.remove_listener(first);
    service.remove_listener(third);
}

// A removed listener's function, and what it captured, is destroyed
// outside the listener lock: here its last capture's destructor calls
// back into ButtonService, which would deadlock under that lock.
TEST(ButtonService, RemovedListenerIsDestroyedOutsideTheLock) {
    DeviceRegistry registry;
    EventBus bus;
    ButtonService service(registry, bus);
    struct Reenter {
        ButtonService& service;
        std::atomic<bool>& destroyed;
        ~Reenter() {
            service.remove_listener(service.add_listener([](const ButtonEdge&) {}));
            destroyed.store(true);
        }
    };
    std::atomic<bool> destroyed{false};
    auto probe = std::make_shared<Reenter>(service, destroyed);
    const uint32_t id = service.add_listener([probe](const ButtonEdge&) {});
    probe.reset();
    ASSERT_FALSE(destroyed.load());

    std::atomic<bool> done{false};
    std::thread remover([&] {
        service.remove_listener(id);
        done.store(true);
    });
    if (!wisp::test::wait_until([&] { return done.load(); }, 2000)) {
        // Deadlocked: no way to unwind the remover; fail without hanging.
        remover.detach();
        FAIL() << "remove_listener() destroyed the function under its lock";
    }
    remover.join();
    EXPECT_TRUE(destroyed.load());
}

TEST(ButtonService, DestroyedWithAListenerLeftIsReportedAsMisuse) {
    {
        std::lock_guard<std::mutex> lock(g_misuse_mu);
        g_misuses.clear();
    }
    wisp::core::set_misuse_handler(&record_misuse);
    {
        DeviceRegistry registry;
        EventBus bus;
        ButtonService service(registry, bus);
        service.add_listener([](const ButtonEdge&) {});
    }
    wisp::core::set_misuse_handler(nullptr);
    std::lock_guard<std::mutex> lock(g_misuse_mu);
    EXPECT_EQ(g_misuses, std::vector<std::string>{"ButtonService destroyed with 1 listener"});
}
