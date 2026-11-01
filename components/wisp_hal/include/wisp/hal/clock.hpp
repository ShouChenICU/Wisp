#pragma once

#include <cstdint>

#include "wisp/base/result.hpp"
#include "wisp/hal/device.hpp"

namespace wisp::hal {

class Clock : public Device {
public:
    static constexpr DeviceClass kClass = DeviceClass::Clock;

    virtual Result<int64_t> get_unix() = 0;
    virtual Result<void> set_unix(int64_t seconds) = 0;
};

}  // namespace wisp::hal
