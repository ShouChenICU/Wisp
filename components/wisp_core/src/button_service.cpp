#include "wisp/core/button_service.hpp"

#include <algorithm>
#include <cstring>
#include <limits>
#include <utility>

#include "wisp/core/log.hpp"

namespace wisp::core {

namespace {
constexpr const char* kTag = "button_service";
constexpr const char* kIdPrefix = "button.";
constexpr size_t kEdgeQueueCapacity = 32;
constexpr uint64_t kDebounceUs = 20'000;
constexpr uint32_t kFallbackStandbyMs = 100;
}  // namespace

ButtonService::ButtonService(DeviceRegistry& registry, EventBus& bus, uint32_t long_press_ms,
                             uint32_t poll_ms)
    : registry_(registry), bus_(bus), long_press_ms_(long_press_ms), poll_ms_(poll_ms) {}

ButtonService::~ButtonService() { stop(); }

Result<void> ButtonService::start() {
    std::lock_guard<os::Mutex> lifecycle_lock(lifecycle_mutex_);
    initialize_locked();
    if (worker_ != nullptr || (fallback_timer_ != nullptr && fallback_timer_->active())) {
        return Result<void>::ok();
    }

    {
        std::lock_guard<os::Mutex> state_lock(state_mutex_);
        if (!initial_level_checked_) {
            for (auto& state : buttons_) {
                if (state.mode == hal::ButtonInputMode::PollOnly && !state.pressed &&
                    !state.candidate_valid) {
                    state.ignore_until_release = state.button->pressed();
                }
            }
            initial_level_checked_ = true;
        }
    }

    if (fallback_mode_ || edge_queue_ == nullptr) {
        return use_legacy_fallback();
    }

    stopping_.store(false, std::memory_order_release);
    std::lock_guard<os::Mutex> queue_lock(queue_consumer_mutex_);
    worker_running_.store(true, std::memory_order_release);
    os::TaskConfig config;
    config.name = "wisp.button";
    config.stack_bytes = 4096;
    config.priority = 5;
    config.flash_access = false;
    config.hot_stack = false;
    worker_ = os::Task::spawn(config, [this] { run_worker(); });
    if (worker_ == nullptr) {
        worker_running_.store(false, std::memory_order_release);
        stopping_.store(true, std::memory_order_release);
        detach_edge_sinks();
        // Quiescent detach closes the input history. Consume valid queued
        // gestures before changing mode, rather than treating them as boot-held.
        drain_edges(0);
        {
            std::lock_guard<os::Mutex> state_lock(state_mutex_);
            recover_overflow(os::now_us());
            process_deadlines(os::now_us());
            for (auto& state : buttons_) {
                if (state.mode == hal::ButtonInputMode::EdgeEvent) {
                    if (!state.saw_edge && !state.pressed && !state.candidate_valid) {
                        state.ignore_until_release = state.button->pressed();
                    }
                    state.candidate_valid = false;
                    state.mode = hal::ButtonInputMode::PollOnly;
                }
            }
        }
        fallback_mode_ = true;
        WLOG_W(kTag, "worker creation failed; using timer polling fallback");
        return use_legacy_fallback();
    }

    fallback_mode_ = false;
    edge_queue_->wake();
    return Result<void>::ok();
}

void ButtonService::stop() {
    std::lock_guard<os::Mutex> lifecycle_lock(lifecycle_mutex_);
    stopping_.store(true, std::memory_order_release);
    detach_edge_sinks();
    if (edge_queue_ != nullptr) {
        edge_queue_->wake();
    }
    if (worker_ != nullptr) {
        worker_->join();
        worker_.reset();
    }
    worker_running_.store(false, std::memory_order_release);
    stop_fallback_timer();
    fallback_mode_ = false;
    stopping_.store(false, std::memory_order_release);
    {
        std::lock_guard<os::Mutex> queue_lock(queue_consumer_mutex_);
        edge_queue_raw_.store(nullptr, std::memory_order_release);
        edge_queue_.reset();
    }
    initialized_.store(false, std::memory_order_release);
    initial_level_checked_ = false;
    {
        std::lock_guard<os::Mutex> state_lock(state_mutex_);
        buttons_.clear();
        bindings_.clear();
        have_poll_time_ = false;
        last_poll_ms_ = 0;
        last_dropped_ = 0;
        overflow_warned_ = false;
        recovering_ = false;
    }
}

void ButtonService::set_standby(bool standby) {
    std::lock_guard<os::Mutex> lifecycle_lock(lifecycle_mutex_);
    {
        std::lock_guard<os::Mutex> state_lock(state_mutex_);
        if (standby_ == standby) {
            return;
        }
        standby_ = standby;
        const bool running = worker_ != nullptr || fallback_timer_ != nullptr;
        const uint64_t now_ms = !running && have_poll_time_ ? last_poll_ms_ : os::now_ms();
        for (auto& state : buttons_) {
            if (state.mode == hal::ButtonInputMode::PollOnly) {
                const uint32_t interval = poll_interval_ms(state);
                state.next_poll_ms =
                    interval == 0 ? std::numeric_limits<uint64_t>::max() : now_ms + interval;
            }
        }
    }

    if (edge_queue_ != nullptr) {
        edge_queue_->wake();
    }
    if (fallback_mode_) {
        auto result = start_fallback_timer();
        if (!result) {
            WLOG_E(kTag, "unable to update fallback polling timer");
        }
    }
}

void ButtonService::initialize_locked() {
    if (initialized_.load(std::memory_order_relaxed)) {
        return;
    }

    refresh_buttons();
    auto queue_result = os::IsrQueue::create(kEdgeQueueCapacity);
    if (!queue_result) {
        {
            std::lock_guard<os::Mutex> state_lock(state_mutex_);
            for (auto& state : buttons_) {
                if (state.mode == hal::ButtonInputMode::EdgeEvent) {
                    state.mode = hal::ButtonInputMode::PollOnly;
                }
            }
        }
        fallback_mode_ = true;
        WLOG_W(kTag, "edge queue creation failed; using polling fallback");
    } else {
        edge_queue_ = std::move(queue_result.value());
        edge_queue_raw_.store(edge_queue_.get(), std::memory_order_release);
        attach_edge_sinks();
    }
    initialized_.store(true, std::memory_order_release);
}

void ButtonService::refresh_buttons() {
    std::lock_guard<os::Mutex> state_lock(state_mutex_);
    buttons_.clear();
    bindings_.clear();

    auto devices = registry_.list<hal::Button>();
    buttons_.reserve(devices.size());
    bindings_.resize(
        std::min(devices.size(), static_cast<size_t>(std::numeric_limits<uint16_t>::max()) + 1));
    for (auto& button : devices) {
        const std::string& id = button->info().id;
        if (id.compare(0, std::strlen(kIdPrefix), kIdPrefix) != 0) {
            WLOG_W(kTag, "button device id '%s' does not start with '%s'; topic will be malformed",
                   id.c_str(), kIdPrefix);
        }
        ButtonState state;
        state.id = id;
        state.mode = button->input_mode();
        state.button = std::move(button);
        state.next_poll_ms = 0;
        buttons_.push_back(std::move(state));
    }
}

void ButtonService::attach_edge_sinks() {
    std::lock_guard<os::Mutex> state_lock(state_mutex_);
    for (size_t i = 0; i < buttons_.size() && i < bindings_.size(); ++i) {
        auto& state = buttons_[i];
        if (state.mode != hal::ButtonInputMode::EdgeEvent) {
            continue;
        }
        bindings_[i] = edge_queue_->bind_edge(static_cast<uint32_t>(i));
        auto result = bindings_[i] == nullptr
                          ? Result<void>::err(ErrorCode::NoMemory)
                          : state.button->set_edge_sink(hal::ButtonEdgeSink{
                                bindings_[i].get(), &os::IsrQueue::EdgeBinding::push_edge});
        if (result) {
            state.edge_attached = true;
        } else {
            state.mode = hal::ButtonInputMode::PollOnly;
            ++warned_sink_failures_;
            WLOG_W(kTag, "edge input unavailable for '%s'; using polling fallback",
                   state.id.c_str());
        }
    }
}

void ButtonService::detach_edge_sinks() {
    std::lock_guard<os::Mutex> state_lock(state_mutex_);
    for (auto& state : buttons_) {
        if (state.edge_attached) {
            auto result = state.button->set_edge_sink({});
            if (!result) {
                WLOG_W(kTag, "failed to detach edge input for '%s'", state.id.c_str());
            }
            state.edge_attached = false;
        }
    }
}

void ButtonService::run_worker() {
    while (!stopping_.load(std::memory_order_acquire)) {
        const uint64_t now_us = os::now_us();
        const uint32_t timeout_ms = next_wait_ms(now_us);
        {
            std::lock_guard<os::Mutex> queue_lock(queue_consumer_mutex_);
            drain_edges(timeout_ms);
        }
        if (!stopping_.load(std::memory_order_acquire)) {
            poll_internal(os::now_ms());
        }
    }
}

void ButtonService::drain_edges(uint32_t timeout_ms) {
    if (edge_queue_ == nullptr) return;
    for (;;) {
        {
            std::lock_guard<os::Mutex> state_lock(state_mutex_);
            if (recovering_ || edge_queue_->dropped() != last_dropped_) return;
        }
        os::IsrQueueRecord record;
        if (!edge_queue_->pop(record, timeout_ms)) return;
        {
            std::lock_guard<os::Mutex> state_lock(state_mutex_);
            if (edge_queue_->dropped() != last_dropped_) return;
        }
        process_edge(ButtonEdgeEvent{static_cast<uint16_t>(record.source), record.value != 0,
                                     record.timestamp_us});
        timeout_ms = 0;
    }
}

uint32_t ButtonService::next_wait_ms(uint64_t now_us) const {
    std::lock_guard<os::Mutex> state_lock(state_mutex_);
    if (recovering_) return 0;
    uint64_t deadline_us = std::numeric_limits<uint64_t>::max();
    for (const auto& state : buttons_) {
        if (state.mode == hal::ButtonInputMode::PollOnly &&
            state.next_poll_ms != std::numeric_limits<uint64_t>::max()) {
            deadline_us = std::min(deadline_us, state.next_poll_ms * 1000);
        }
        if (state.candidate_valid) {
            deadline_us = std::min(deadline_us, state.candidate_at_us + kDebounceUs);
        }
        if (state.pressed && !state.long_sent) {
            deadline_us = std::min(deadline_us,
                                   state.down_at_us + static_cast<uint64_t>(long_press_ms_) * 1000);
        }
    }
    if (deadline_us == std::numeric_limits<uint64_t>::max()) {
        return os::kForever;
    }
    if (deadline_us <= now_us) {
        return 0;
    }
    const uint64_t delta_ms = (deadline_us - now_us + 999) / 1000;
    return static_cast<uint32_t>(std::min<uint64_t>(delta_ms, os::kForever - 1));
}

uint32_t ButtonService::poll_interval_ms(const ButtonState& state) const {
    if (!standby_) {
        return poll_ms_;
    }
    if (state.id != "button.power") {
        return 0;
    }
    const uint32_t interval = state.button->standby_poll_ms();
    return interval != 0 || !fallback_mode_ ? interval : kFallbackStandbyMs;
}

void ButtonService::process_edge(const ButtonEdgeEvent& event) {
    std::lock_guard<os::Mutex> state_lock(state_mutex_);
    if (event.button_index >= buttons_.size()) {
        return;
    }
    auto& state = buttons_[event.button_index];
    if (state.mode != hal::ButtonInputMode::EdgeEvent) {
        return;
    }

    state.saw_edge = true;
    state.button->service_input();
    process_deadlines(event.timestamp_us);
    if (event.pressed == state.pressed) {
        if (state.candidate_valid && state.candidate_pressed != event.pressed) {
            state.candidate_valid = false;
        }
        return;
    }
    if (state.candidate_valid) {
        if (state.candidate_pressed == event.pressed) {
            return;
        }
        state.candidate_valid = false;
        if (event.pressed == state.pressed) {
            return;
        }
    }
    state.candidate_valid = true;
    state.candidate_pressed = event.pressed;
    state.candidate_at_us = event.timestamp_us;
}

void ButtonService::process_deadlines(uint64_t now_us) {
    for (auto& state : buttons_) {
        if (state.mode == hal::ButtonInputMode::EdgeEvent && state.candidate_valid &&
            now_us >= state.candidate_at_us + kDebounceUs) {
            const bool pressed = state.candidate_pressed;
            const uint64_t confirmed_at_us = state.candidate_at_us + kDebounceUs;
            state.candidate_valid = false;
            commit_edge(state, pressed, confirmed_at_us);
        }
        if (state.pressed && !state.long_sent &&
            now_us >= state.down_at_us + static_cast<uint64_t>(long_press_ms_) * 1000) {
            state.long_sent = true;
            publish_event(state, "long");
        }
    }
}

void ButtonService::commit_edge(ButtonState& state, bool pressed, uint64_t at_us) {
    if (state.ignore_until_release) {
        if (!pressed) {
            state.ignore_until_release = false;
        }
        return;
    }
    if (pressed == state.pressed) {
        return;
    }
    if (pressed) {
        state.pressed = true;
        state.down_at_us = at_us;
        state.long_sent = false;
        publish_event(state, "down");
        return;
    }
    // A delayed worker may confirm release after the long deadline. Preserve
    // chronological gesture order even when both deadlines are processed now.
    if (!state.long_sent &&
        at_us >= state.down_at_us + static_cast<uint64_t>(long_press_ms_) * 1000) {
        state.long_sent = true;
        publish_event(state, "long");
    }
    state.pressed = false;
    publish_event(state, "up");
    if (!state.long_sent) {
        publish_event(state, "short");
    }
}

void ButtonService::sample_poll_only(ButtonState& state, bool pressed, uint64_t at_us) {
    if (state.ignore_until_release) {
        if (!pressed) {
            state.ignore_until_release = false;
        }
        return;
    }
    if (pressed && !state.pressed) {
        state.pressed = true;
        state.down_at_us = at_us;
        state.long_sent = false;
        publish_event(state, "down");
    } else if (!pressed && state.pressed) {
        state.pressed = false;
        publish_event(state, "up");
        if (!state.long_sent) {
            publish_event(state, "short");
        }
    }
}

void ButtonService::recover_overflow(uint64_t now_us) {
    if (edge_queue_ == nullptr) {
        return;
    }
    if (!recovering_ && edge_queue_->dropped() == last_dropped_) {
        return;
    }
    recovering_ = true;
    if (!overflow_warned_) {
        WLOG_W(kTag, "edge queue overflow; resynchronizing button levels");
        overflow_warned_ = true;
    }

    // The queue lock excludes other consumers, not ISR producers. Discard at
    // an atomic boundary, sample outside the ISR critical section, then accept
    // only a window with no producer attempts (including dropped records).
    // One attempt per turn bounds recovery work under continuous input.
    const uint32_t dropped = edge_queue_->begin_resync();
    for (auto& state : buttons_) {
        if (state.mode != hal::ButtonInputMode::EdgeEvent) continue;
        state.button->service_input();
        state.recovery_level = state.button->pressed();
    }
    if (!edge_queue_->resync_unchanged()) return;

    last_dropped_ = dropped;
    recovering_ = false;
    for (auto& state : buttons_) {
        if (state.mode != hal::ButtonInputMode::EdgeEvent) continue;
        state.candidate_valid = false;
        commit_edge(state, state.recovery_level, now_us);
    }
}

void ButtonService::publish_event(const ButtonState& state, const char* suffix) {
    Event event;
    event.topic = state.id + "." + suffix;
    event.source = state.id;
    bus_.publish(std::move(event));
}

void ButtonService::poll(uint64_t now_ms) {
    std::lock_guard<os::Mutex> lifecycle_lock(lifecycle_mutex_);
    if (worker_ != nullptr || fallback_timer_ != nullptr) {
        return;
    }
    initialize_locked();
    poll_internal(now_ms);
}

void ButtonService::poll_internal(uint64_t now_ms) {
    const uint64_t now_us = now_ms * 1000;

    std::unique_lock<os::Mutex> queue_lock(queue_consumer_mutex_, std::try_to_lock);
    if (!worker_running_.load(std::memory_order_acquire) && queue_lock.owns_lock()) {
        drain_edges(0);
    }

    std::lock_guard<os::Mutex> state_lock(state_mutex_);
    have_poll_time_ = true;
    last_poll_ms_ = now_ms;
    for (auto& state : buttons_) {
        if (state.mode != hal::ButtonInputMode::PollOnly) {
            continue;
        }
        const uint32_t interval = poll_interval_ms(state);
        if (interval == 0) {
            state.next_poll_ms = std::numeric_limits<uint64_t>::max();
            continue;
        }
        if (state.next_poll_ms == std::numeric_limits<uint64_t>::max()) {
            state.next_poll_ms = now_ms + interval;
            continue;
        }
        if (now_ms >= state.next_poll_ms) {
            sample_poll_only(state, state.button->pressed(), now_us);
            state.next_poll_ms = now_ms + interval;
        }
    }
    recover_overflow(now_us);
    if (!recovering_) process_deadlines(now_us);
}

bool ButtonService::has_fallback_polling() const {
    std::lock_guard<os::Mutex> state_lock(state_mutex_);
    for (const auto& state : buttons_) {
        if (state.mode == hal::ButtonInputMode::PollOnly && poll_interval_ms(state) != 0) {
            return true;
        }
    }
    return false;
}

Result<void> ButtonService::start_fallback_timer() {
    // This is the only input channel after queue/worker failure. Keep its
    // lifetime independent of mode changes, which must not need fresh RAM.
    // The retained base tick also services the active cadence after wake;
    // per-device deadlines suppress standby reads between their 100 ms polls.
    if (fallback_timer_ != nullptr && fallback_timer_->active()) {
        return Result<void>::ok();
    }
    if (!has_fallback_polling()) {
        return Result<void>::ok();
    }
    const uint32_t period_ms = std::min(poll_ms_, kFallbackStandbyMs);
    fallback_timer_ =
        os::Timer::create("wisp.button", period_ms, true, [this] { poll_internal(os::now_ms()); });
    if (fallback_timer_ == nullptr) {
        return Result<void>::err(ErrorCode::NoMemory, "button fallback timer creation failed");
    }
    fallback_timer_->start();
    return Result<void>::ok();
}

void ButtonService::stop_fallback_timer() {
    if (fallback_timer_ != nullptr) {
        fallback_timer_->stop();
        fallback_timer_.reset();
    }
}

Result<void> ButtonService::use_legacy_fallback() {
    fallback_mode_ = true;
    return start_fallback_timer();
}

}  // namespace wisp::core
