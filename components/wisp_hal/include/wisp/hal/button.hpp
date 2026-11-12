#pragma once

#include <cstdint>

#include "wisp/hal/device.hpp"

namespace wisp::hal {

enum class ButtonInputMode : uint8_t { PollOnly, EdgeEvent };

struct ButtonEdgeSink {
    void* context = nullptr;
    bool (*push)(void* context, bool pressed, uint64_t timestamp_us) noexcept = nullptr;
};

// A physical button. `info().id` follows the "button.<name>" convention,
// e.g. "button.power", "button.vol_up", "button.vol_down". This interface
// exposes only the raw level; short-press/long-press gesture detection is
// built on top by the portable ButtonService (see wisp_core), not here.
// caps keys: "standby" (string, optional: "wake_source" for a button that
// must keep working at every power level; see StandbyPolicy in
// wisp/hal/device.hpp); "role" (string, optional: "talk" for the button
// held to talk, mvp spec 6.3 -- the board picks it, the shell finds it
// with shell::find_talk_button()).
class Button : public Device {
public:
    static constexpr DeviceClass kClass = DeviceClass::Button;

    virtual bool pressed() = 0;
    virtual ButtonInputMode input_mode() const { return ButtonInputMode::PollOnly; }

    // A failed attach must not retain the sink. For an installed sink,
    // set_edge_sink({}) must succeed and be quiescent: disable new callbacks
    // and wait for all in-flight callbacks before returning, so the caller
    // may immediately destroy the sink context. Call from task context only.
    virtual Result<void> set_edge_sink(ButtonEdgeSink) {
        return Result<void>::err(ErrorCode::Unsupported);
    }
    // Task-context maintenance after an edge or overflow recovery. Platforms
    // may update level-wake/sleep inhibition here, never in the ISR callback.
    virtual void service_input() {}
    virtual uint32_t standby_poll_ms() const { return 0; }
};

}  // namespace wisp::hal
