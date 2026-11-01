#pragma once

// Thread-safe simulated input state (spec 8.1). simulator/main.cpp's SDL
// event loop -- the only place in this project that touches SDL input --
// writes mouse/keyboard/tilt/SD-toggle state here; the simulated HAL
// devices in components/wisp_port_sim/dev/ read it back from whatever
// thread calls them (e.g. ButtonService's poll timer, or the UI thread's
// touch input driver). Kept SDL-free (implemented entirely inline, with
// only <mutex>/<cmath>/<algorithm>) so it can be included both by this
// component's public headers and by simulator/main.cpp, which does the
// actual SDL-to-SimInput translation.

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <mutex>

namespace wisp::port::sim {

struct SimTouchState {
    bool down = false;
    int16_t x = 0;
    int16_t y = 0;
};

// ax/ay/az in g, matching wisp::hal::ImuSample's units.
struct SimTiltState {
    float ax = 0;
    float ay = 0;
    float az = 1;
};

class SimInput {
public:
    explicit SimInput(bool sd_present_initial = true) : sd_present_(sd_present_initial) {}

    void mouse(int x, int y, bool down) {
        std::lock_guard<std::mutex> lock(mutex_);
        touch_.down = down;
        touch_.x = static_cast<int16_t>(x);
        touch_.y = static_cast<int16_t>(y);
    }

    void key_power(bool down) {
        std::lock_guard<std::mutex> lock(mutex_);
        power_down_ = down;
    }

    void key_vol_up(bool down) {
        std::lock_guard<std::mutex> lock(mutex_);
        vol_up_down_ = down;
    }

    void key_vol_down(bool down) {
        std::lock_guard<std::mutex> lock(mutex_);
        vol_down_down_ = down;
    }

    // Nudges the simulated tilt by (dx, dy) * 0.1g (spec 8.1's arrow-key
    // mapping): ax/ay each move by 0.1 per unit of dx/dy and are clamped
    // to [-1, 1]; tilt_state() derives az from them.
    void tilt(int dx, int dy) {
        std::lock_guard<std::mutex> lock(mutex_);
        ax_ = std::clamp(ax_ + static_cast<float>(dx) * 0.1f, -1.0f, 1.0f);
        ay_ = std::clamp(ay_ + static_cast<float>(dy) * 0.1f, -1.0f, 1.0f);
    }

    void toggle_sd() {
        std::lock_guard<std::mutex> lock(mutex_);
        sd_present_ = !sd_present_;
    }

    SimTouchState touch() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return touch_;
    }

    bool power_pressed() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return power_down_;
    }

    bool vol_up_pressed() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return vol_up_down_;
    }

    bool vol_down_pressed() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return vol_down_down_;
    }

    // az = sqrt(max(0, 1 - ax^2 - ay^2)), so the reported vector never
    // implies more than 1g of tilt (spec 8.1).
    SimTiltState tilt_state() const {
        std::lock_guard<std::mutex> lock(mutex_);
        SimTiltState s;
        s.ax = ax_;
        s.ay = ay_;
        s.az = std::sqrt(std::max(0.0f, 1.0f - ax_ * ax_ - ay_ * ay_));
        return s;
    }

    bool sd_present() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return sd_present_;
    }

private:
    mutable std::mutex mutex_;
    SimTouchState touch_;
    bool power_down_ = false;
    bool vol_up_down_ = false;
    bool vol_down_down_ = false;
    float ax_ = 0.0f;
    float ay_ = 0.0f;
    bool sd_present_ = true;
};

}  // namespace wisp::port::sim
