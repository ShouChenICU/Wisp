#pragma once

// ESP-IDF wisp::hal::Clock backed by drivers/pcf85063 (the board's own
// bmgr i2c_master bus handle -- see boards/waveshare_amoled_216/
// board.cpp). The chip model string is passed in by the board (AGENTS.md
// "板子无关" rule). get_unix()/set_unix() both work in UTC -- see
// pcf85063.h; converting to a local display time, if ever needed, is the
// caller's job, not this class's.

#include <cstdint>
#include <memory>
#include <string>

#include "driver/i2c_master.h"
#include "pcf85063.h"
#include "wisp/base/result.hpp"
#include "wisp/hal/clock.hpp"

namespace wisp::port::esp {

class EspClock : public wisp::hal::Clock {
public:
    // Calls pcf85063_init(bus) and wraps the resulting handle. Fails with
    // DeviceFailure if that returns anything other than ESP_OK.
    static Result<std::shared_ptr<EspClock>> create(i2c_master_bus_handle_t bus, std::string model);

    ~EspClock() override;

    EspClock(const EspClock&) = delete;
    EspClock& operator=(const EspClock&) = delete;

    const wisp::hal::DeviceInfo& info() const override { return info_; }

    // pcf85063_get_unix(). Fails with DeviceFailure if the chip's
    // oscillator-stop flag is set or the decoded time is implausible
    // (see pcf85063.h) -- callers must not settimeofday() from this
    // result on failure.
    Result<int64_t> get_unix() override;
    // pcf85063_set_unix().
    Result<void> set_unix(int64_t seconds) override;

private:
    EspClock(pcf85063_handle_t handle, std::string model);

    wisp::hal::DeviceInfo info_;
    pcf85063_handle_t handle_;
};

}  // namespace wisp::port::esp
