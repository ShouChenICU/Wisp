#pragma once

#include <cstdint>

namespace wisp::port::esp {

constexpr uint32_t kButtonStandbyPollMs = 100;
constexpr bool button_pressed(bool high, bool active_low) { return high != active_low; }
constexpr bool power_button_use_gpio(bool validated, bool configured, bool edge_ready) {
    return validated && configured && edge_ready;
}

// Task-context wake policy. Holding the sleep lock before disabling a
// level wake source closes the held-key immediate sleep/wake loop.
class ButtonWakeState {
public:
    bool armed() const { return armed_; }
    template <class Ops>
    bool update(bool held, Ops& ops) {
        if (held) {
            if (!locked_) {
                if (!ops.lock_sleep()) return false;
                locked_ = true;
            }
            if (armed_) {
                if (!ops.disable_wake()) return false;
                armed_ = false;
            }
        } else {
            if (!armed_) {
                if (!ops.enable_wake()) return false;
                armed_ = true;
            }
            if (locked_) {
                ops.unlock_sleep();
                locked_ = false;
            }
        }
        return true;
    }
    template <class Ops>
    bool reset(Ops& ops) {
        if (armed_) {
            if (!ops.disable_wake()) return false;
            armed_ = false;
        }
        if (locked_) {
            ops.unlock_sleep();
            locked_ = false;
        }
        return true;
    }

private:
    bool armed_ = false;
    bool locked_ = false;
};

}  // namespace wisp::port::esp
