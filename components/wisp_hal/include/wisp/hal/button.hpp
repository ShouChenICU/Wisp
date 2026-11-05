#pragma once

#include "wisp/hal/device.hpp"

namespace wisp::hal {

// A physical button. `info().id` follows the "button.<name>" convention,
// e.g. "button.power", "button.vol_up", "button.vol_down". This interface
// exposes only the raw level; short-press/long-press gesture detection is
// built on top by the portable ButtonService (see wisp_core), not here.
// caps keys: "standby" (string, optional: "wake_source" for a button that
// must keep working at every power level; see StandbyPolicy in
// wisp/hal/device.hpp).
class Button : public Device {
public:
    static constexpr DeviceClass kClass = DeviceClass::Button;

    virtual bool pressed() = 0;
};

}  // namespace wisp::hal
