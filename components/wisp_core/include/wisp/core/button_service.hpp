#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <vector>

#include "wisp/base/result.hpp"
#include "wisp/core/device_registry.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/hal/button.hpp"
#include "wisp/os/isr_queue.hpp"
#include "wisp/os/os.hpp"

namespace wisp::core {

struct ButtonEdgeEvent {
    uint16_t button_index = 0;
    bool pressed = false;
    uint64_t timestamp_us = 0;
};

class ButtonService {
public:
    ButtonService(DeviceRegistry& registry, EventBus& bus, uint32_t long_press_ms = 1000,
                  uint32_t poll_ms = 20);
    ~ButtonService();

    ButtonService(const ButtonService&) = delete;
    ButtonService& operator=(const ButtonService&) = delete;

    Result<void> start();
    void stop();
    void set_standby(bool standby);
    // Deterministic deadline processing without starting a worker. Ignored
    // while running; production input is driven only by its worker or fallback.
    void poll(uint64_t now_ms);

private:
    struct ButtonState {
        std::shared_ptr<hal::Button> button;
        std::string id;
        hal::ButtonInputMode mode = hal::ButtonInputMode::PollOnly;
        bool edge_attached = false;
        bool pressed = false;
        bool ignore_until_release = false;
        bool long_sent = false;
        uint64_t down_at_us = 0;
        bool candidate_valid = false;
        bool candidate_pressed = false;
        uint64_t candidate_at_us = 0;
        uint64_t next_poll_ms = 0;
    };

    void initialize_locked();
    void refresh_buttons();
    void attach_edge_sinks();
    void detach_edge_sinks();
    void run_worker();
    void poll_internal(uint64_t now_ms);
    uint32_t next_wait_ms(uint64_t now_us) const;
    uint32_t poll_interval_ms(const ButtonState& state) const;
    void process_edge(const ButtonEdgeEvent& event);
    void process_deadlines(uint64_t now_us);
    void commit_edge(ButtonState& state, bool pressed, uint64_t at_us);
    void sample_poll_only(ButtonState& state, bool pressed, uint64_t at_us);
    void recover_overflow(uint64_t now_us);
    void publish_event(const ButtonState& state, const char* suffix);
    bool has_fallback_polling() const;
    Result<void> start_fallback_timer();
    void stop_fallback_timer();
    Result<void> use_legacy_fallback();

    DeviceRegistry& registry_;
    EventBus& bus_;
    uint32_t long_press_ms_;
    const uint32_t poll_ms_;

    mutable os::Mutex state_mutex_;
    os::Mutex queue_consumer_mutex_;
    std::vector<ButtonState> buttons_;
    std::vector<std::unique_ptr<os::IsrQueue::EdgeBinding>> bindings_;
    std::atomic<bool> initialized_{false};
    bool initial_level_checked_ = false;
    bool standby_ = false;
    bool have_poll_time_ = false;
    bool overflow_warned_ = false;
    uint64_t last_poll_ms_ = 0;
    uint32_t last_dropped_ = 0;
    uint32_t warned_sink_failures_ = 0;
    std::unique_ptr<os::IsrQueue> edge_queue_;
    std::atomic<os::IsrQueue*> edge_queue_raw_{nullptr};
    std::atomic<bool> worker_running_{false};
    os::Mutex lifecycle_mutex_;
    std::unique_ptr<os::Task> worker_;
    std::unique_ptr<os::Timer> fallback_timer_;
    std::atomic<bool> stopping_{false};
    std::atomic<bool> fallback_mode_{false};
};

}  // namespace wisp::core
