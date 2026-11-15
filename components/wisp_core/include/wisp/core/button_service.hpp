#pragma once

#include <atomic>
#include <cstdint>
#include <memory>
#include <string>
#include <string_view>
#include <vector>

#include "wisp/base/fixed_string.hpp"
#include "wisp/base/result.hpp"
#include "wisp/core/device_registry.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/hal/button.hpp"
#include "wisp/os/isr_queue.hpp"
#include "wisp/os/large.hpp"
#include "wisp/os/os.hpp"

namespace wisp::core {

// A raw press/release record from a driver's edge sink, before debounce;
// internal to ButtonService. Consumers see ButtonEdge below.
struct ButtonEdgeEvent {
    uint16_t button_index = 0;
    bool pressed = false;
    uint64_t timestamp_us = 0;
};

// A confirmed gesture edge, as ButtonService's listeners receive it.
struct ButtonEdge {
    enum class Kind : uint8_t { Down, Up, Short, Long };
    // The button's device id, e.g. "button.power". 32 bytes: the longest
    // id DeviceRegistry::add() accepts for a button.
    FixedString<32> id;
    Kind kind = Kind::Down;
    // os::now_us() base: the confirmed edge (Down, Up, Short; Short has
    // its Up's time) or the long-press deadline, Down + the long-press
    // threshold (Long), however late the service noticed it. A PollOnly
    // key's edges come from the poll time, so they have millisecond
    // resolution.
    uint64_t at_us = 0;
};
using ButtonListener = os::LargeFunction<void(const ButtonEdge&)>;

// Turns button input into gesture edges. For every down/up/short/long it
// first calls its listeners, then publishes the event
// "<button id>.<edge>" (source: the button id) on the bus, an observable
// copy for the WebSocket hub and diagnostics that may be dropped. A
// consumer that must not miss an edge listens instead.
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

    // Listeners. Each one is called for every edge of every button, in
    // registration order, with the edge's own time, before the bus event
    // of that edge. They run on whichever thread found the edge: the
    // wisp.button worker, the fallback polling timer's task (the esp_timer
    // task on the device), the thread calling poll() (tests), or once the
    // start() caller when the worker cannot be created -- never in an ISR,
    // whose sink only queues a record for the worker. Delivery is
    // serialised: every path holds the state lock, so listener calls
    // never overlap, even across those threads. A listener runs
    // under ButtonService's state lock and its listener lock, so it may
    // only push into an EdgeInbox, store an atomic, update a state cell,
    // schedule a dispatch slot or make a non-blocking post (one that fails
    // rather than waits when the queue is full): it must not block, touch
    // flash, or call
    // ButtonService (add_listener()/remove_listener() from inside a
    // listener deadlocks on the listener lock).
    //
    // Lock order: lifecycle -> state -> listeners -> whatever the listener
    // takes (an inbox's lock, a cell's lock, a dispatcher's ready list).
    // add_listener()/remove_listener() take only the listener lock, so a
    // follower may call them from any thread while buttons are pressed
    // and the service runs, provided it holds no lock its own listener
    // takes. Every listener must be removed before ButtonService is
    // destroyed; the destructor reports one left as misuse
    // (dispatch_misuse(): an abort on the host, an error log on the
    // device).
    //
    // Any thread; allocates (Mem::Large). Ids start at 1 and are never 0.
    uint32_t add_listener(ButtonListener fn);
    // Any thread. When it returns, the listener is not running on any
    // thread and is never called again: a call in progress elsewhere is
    // waited for. The function is destroyed after the listener lock is
    // released. Unknown or already removed ids are ignored.
    void remove_listener(uint32_t id);
    // Runs the path of a real edge -- listeners, then the bus event
    // "<id>.<edge>" with source `id` -- for a button that need not exist,
    // without start(). Ordered with real edges (takes the state lock).
    void emit_for_test(std::string_view id, ButtonEdge::Kind kind, uint64_t at_us);

private:
    struct ButtonState {
        std::shared_ptr<hal::Button> button;
        std::string id;
        // Role power (hal/roles.hpp): the one key still polled in standby.
        bool power_key = false;
        hal::ButtonInputMode mode = hal::ButtonInputMode::PollOnly;
        bool edge_attached = false;
        bool saw_edge = false;
        bool recovery_level = false;
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
    void drain_edges(uint32_t timeout_ms);
    void poll_internal(uint64_t now_ms);
    uint32_t next_wait_ms(uint64_t now_us) const;
    uint32_t poll_interval_ms(const ButtonState& state) const;
    void process_edge(const ButtonEdgeEvent& event);
    void process_deadlines(uint64_t now_us);
    void commit_edge(ButtonState& state, bool pressed, uint64_t at_us);
    void sample_poll_only(ButtonState& state, bool pressed, uint64_t at_us);
    void recover_overflow(uint64_t now_us);
    // Caller holds state_mutex_. Listeners first, then the bus event.
    void emit(std::string_view id, ButtonEdge::Kind kind, uint64_t at_us);
    uint64_t long_deadline_us(const ButtonState& state) const;
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
    bool recovering_ = false;
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

    struct Listener {
        uint32_t id = 0;
        ButtonListener fn;
    };
    // Leaf lock under state_mutex_, held across the calls so that
    // remove_listener() returns only after a call in progress.
    os::Mutex listeners_mutex_;
    os::LargeVector<Listener> listeners_;
    uint32_t next_listener_id_ = 1;
};

}  // namespace wisp::core
