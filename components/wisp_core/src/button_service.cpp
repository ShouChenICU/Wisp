#include "wisp/core/button_service.hpp"

#include <utility>

namespace wisp::core {

ButtonService::ButtonService(DeviceRegistry& registry, EventBus& bus, uint32_t long_press_ms,
                             uint32_t poll_ms)
    : registry_(registry),
      bus_(bus),
      long_press_ms_(long_press_ms),
      poll_ms_(poll_ms),
      timer_("wisp.button", poll_ms, /*periodic=*/true, [this] { poll(os::now_ms()); }) {}

void ButtonService::start() {
    refresh_buttons();
    timer_.start();
}

void ButtonService::stop() { timer_.stop(); }

void ButtonService::refresh_buttons() {
    std::vector<ButtonState> fresh;
    for (auto& button : registry_.list<hal::Button>()) {
        ButtonState state;
        state.id = button->info().id;
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
