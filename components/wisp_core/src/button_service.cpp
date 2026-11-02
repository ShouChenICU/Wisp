#include "wisp/core/button_service.hpp"

#include <cstring>
#include <mutex>
#include <utility>

#include "wisp/core/log.hpp"

namespace wisp::core {

namespace {
constexpr const char* kTag = "button_service";
constexpr const char* kIdPrefix = "button.";
}  // namespace

ButtonService::ButtonService(DeviceRegistry& registry, EventBus& bus, uint32_t long_press_ms,
                             uint32_t poll_ms)
    : registry_(registry),
      bus_(bus),
      long_press_ms_(long_press_ms),
      poll_ms_(poll_ms),
      timer_(std::make_unique<os::Timer>("wisp.button", poll_ms_, /*periodic=*/true,
                                         [this] { poll(os::now_ms()); })) {}

void ButtonService::start() {
    std::lock_guard<os::Mutex> timer_lock(timer_mutex_);
    if (timer_->active()) {
        // Already running: a second start() must be a true no-op,
        // including leaving buttons_ untouched -- see the doc comment on
        // start() for why re-running refresh_buttons() here would be
        // unsafe.
        return;
    }
    {
        std::lock_guard<os::Mutex> lock(mutex_);
        refresh_buttons();
    }
    timer_->start();
}

void ButtonService::stop() {
    std::lock_guard<os::Mutex> timer_lock(timer_mutex_);
    timer_->stop();
}

void ButtonService::set_poll_interval_ms(uint32_t poll_ms) {
    std::lock_guard<os::Mutex> timer_lock(timer_mutex_);
    if (poll_ms_ == poll_ms) {
        return;
    }
    bool was_active = timer_->active();
    if (was_active) {
        timer_->stop();
    }
    poll_ms_ = poll_ms;
    timer_ = std::make_unique<os::Timer>("wisp.button", poll_ms_, /*periodic=*/true,
                                         [this] { poll(os::now_ms()); });
    if (was_active) {
        timer_->start();
    }
}

uint32_t ButtonService::poll_interval_ms() const {
    std::lock_guard<os::Mutex> timer_lock(timer_mutex_);
    return poll_ms_;
}

void ButtonService::refresh_buttons() {
    std::vector<ButtonState> fresh;
    for (auto& button : registry_.list<hal::Button>()) {
        const std::string& id = button->info().id;
        if (id.compare(0, std::strlen(kIdPrefix), kIdPrefix) != 0) {
            WLOG_W(kTag, "button device id '%s' does not start with '%s'; topic will be malformed",
                   id.c_str(), kIdPrefix);
        }
        ButtonState state;
        state.id = id;
        state.button = std::move(button);
        fresh.push_back(std::move(state));
    }
    buttons_ = std::move(fresh);
    buttons_loaded_ = true;
}

void ButtonService::publish_event(const ButtonState& state, const char* suffix) {
    Event e;
    e.topic = state.id + "." + suffix;
    e.source = state.id;
    bus_.publish(std::move(e));
}

void ButtonService::poll(uint64_t now_ms) {
    std::lock_guard<os::Mutex> lock(mutex_);

    if (!buttons_loaded_) {
        refresh_buttons();
    }

    for (auto& state : buttons_) {
        bool down = state.button->pressed();

        if (down && !state.pressed) {
            // Freshly pressed.
            state.pressed = true;
            state.down_at = now_ms;
            state.long_sent = false;
            publish_event(state, "down");
        } else if (down && state.pressed) {
            // Still held: fire `long` exactly once per press, once the
            // hold duration crosses the threshold.
            if (!state.long_sent && now_ms - state.down_at >= long_press_ms_) {
                state.long_sent = true;
                publish_event(state, "long");
            }
        } else if (!down && state.pressed) {
            // Released: `up`, plus `short` only if `long` never fired.
            state.pressed = false;
            publish_event(state, "up");
            if (!state.long_sent) {
                publish_event(state, "short");
            }
        }
        // !down && !state.pressed: idle, nothing to do.
    }
}

}  // namespace wisp::core
