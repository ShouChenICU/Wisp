#pragma once

// Thread-safe simulated input state (spec 8.1). simulator/main.cpp's SDL
// event loop -- the only place in this project that touches SDL input --
// writes mouse/keyboard/tilt/SD-toggle state here; the simulated HAL
// devices in components/wisp_port_sim/dev/ read it back from whatever
// thread calls them (e.g. the button worker, or the UI thread's
// touch input driver). Kept SDL-free (implemented entirely inline, with
// standard synchronization and portable HAL/OS interfaces) so it can be included both by this
// component's public headers and by simulator/main.cpp, which does the
// actual SDL-to-SimInput translation.

#include <algorithm>
#include <array>
#include <cmath>
#include <condition_variable>
#include <cstdint>
#include <deque>
#include <memory>
#include <mutex>
#include <optional>
#include <utility>
#include <vector>

#include "wisp/hal/button.hpp"
#include "wisp/os/os.hpp"
#include "wisp/port/sim/imu_script.hpp"

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

enum class SimButtonKind { Power, VolUp, VolDown };

class SimInput {
public:
    explicit SimInput(bool sd_present_initial = true) : sd_present_(sd_present_initial) {}

    void mouse(int x, int y, bool down) {
        std::lock_guard<std::mutex> lock(mutex_);
        touch_.down = down;
        touch_.x = static_cast<int16_t>(x);
        touch_.y = static_cast<int16_t>(y);
    }

    void key_power(bool down) { key(SimButtonKind::Power, down); }
    void key_vol_up(bool down) { key(SimButtonKind::VolUp, down); }
    void key_vol_down(bool down) { key(SimButtonKind::VolDown, down); }
    void key_power(bool down, uint64_t at_us) { key(SimButtonKind::Power, down, at_us); }
    void key_vol_up(bool down, uint64_t at_us) { key(SimButtonKind::VolUp, down, at_us); }
    void key_vol_down(bool down, uint64_t at_us) { key(SimButtonKind::VolDown, down, at_us); }

    Result<void> set_button_sink(SimButtonKind kind, const void* owner, hal::ButtonEdgeSink sink) {
        auto& channel = buttons_[static_cast<size_t>(kind)];
        std::unique_lock<std::mutex> lock(channel.mutex);
        if (sink.push != nullptr) {
            if (channel.owner != nullptr) return Result<void>::err(ErrorCode::Busy);
            channel.owner = owner;
            channel.sink = sink;
        } else if (channel.owner == owner) {
            // Disable new delivery before waiting for the callback outside the lock.
            channel.sink = {};
            channel.pending.clear();
            channel.idle.wait(lock, [&] { return !channel.delivering; });
            channel.owner = nullptr;
        }
        return Result<void>::ok();
    }

    // Nudges the simulated tilt by (dx, dy) * 0.1g (spec 8.1's arrow-key
    // mapping): ax/ay each move by 0.1 per unit of dx/dy and are clamped
    // to [-1, 1]; tilt_state() derives az from them.
    void tilt(int dx, int dy) {
        std::lock_guard<std::mutex> lock(mutex_);
        ax_ = std::clamp(ax_ + static_cast<float>(dx) * 0.1f, -1.0f, 1.0f);
        ay_ = std::clamp(ay_ + static_cast<float>(dy) * 0.1f, -1.0f, 1.0f);
    }

    // --imu-script: SimImu plays these steps (imu_script.hpp) from the
    // first one's time on, instead of the arrow keys' tilt.
    void set_imu_script(std::vector<ImuScriptStep> steps) {
        auto shared = std::make_shared<const std::vector<ImuScriptStep>>(std::move(steps));
        std::lock_guard<std::mutex> lock(mutex_);
        imu_script_ = std::move(shared);
    }

    // Null without a script.
    std::shared_ptr<const std::vector<ImuScriptStep>> imu_script() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return imu_script_;
    }

    void toggle_sd() {
        std::lock_guard<std::mutex> lock(mutex_);
        sd_present_ = !sd_present_;
    }

    SimTouchState touch() const {
        std::lock_guard<std::mutex> lock(mutex_);
        return touch_;
    }

    bool power_pressed() const { return button_pressed(SimButtonKind::Power); }
    bool vol_up_pressed() const { return button_pressed(SimButtonKind::VolUp); }
    bool vol_down_pressed() const { return button_pressed(SimButtonKind::VolDown); }

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
    struct Edge {
        bool down;
        uint64_t at_us;
    };
    struct ButtonChannel {
        mutable std::mutex mutex;
        std::condition_variable idle;
        bool down = false;
        bool delivering = false;
        const void* owner = nullptr;
        hal::ButtonEdgeSink sink{};
        std::deque<Edge> pending;
    };

    bool button_pressed(SimButtonKind kind) const {
        const auto& channel = buttons_[static_cast<size_t>(kind)];
        std::lock_guard<std::mutex> lock(channel.mutex);
        return channel.down;
    }

    void key(SimButtonKind kind, bool down, std::optional<uint64_t> at_us = {}) {
        auto& channel = buttons_[static_cast<size_t>(kind)];
        std::unique_lock<std::mutex> lock(channel.mutex);
        if (channel.down == down) return;
        channel.down = down;
        if (channel.sink.push == nullptr) return;
        channel.pending.push_back({down, at_us.value_or(os::now_us())});
        if (channel.delivering) return;
        channel.delivering = true;
        // One drainer preserves edge order, including reentrant injection. No
        // input lock is held while the sink runs, so it may inspect SimInput.
        while (!channel.pending.empty()) {
            const auto edge = channel.pending.front();
            channel.pending.pop_front();
            const auto sink = channel.sink;
            lock.unlock();
            sink.push(sink.context, edge.down, edge.at_us);
            lock.lock();
        }
        channel.delivering = false;
        channel.idle.notify_all();
    }

    std::array<ButtonChannel, 3> buttons_;
    mutable std::mutex mutex_;
    SimTouchState touch_;
    float ax_ = 0.0f;
    float ay_ = 0.0f;
    bool sd_present_ = true;
    std::shared_ptr<const std::vector<ImuScriptStep>> imu_script_;
};

}  // namespace wisp::port::sim
