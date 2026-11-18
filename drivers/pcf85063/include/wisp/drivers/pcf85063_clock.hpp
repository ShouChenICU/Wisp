#pragma once

// Wisp adapter over the PCF85063 C driver (pcf85063.h), on the board's own
// bmgr i2c_master bus handle. The board names the id and role; the model
// string defaults to this chip. get_unix()/set_unix() both work in UTC --
// see pcf85063.h; converting to a local display time, if ever needed, is
// the caller's job, not this class's.

#include <cstdint>
#include <memory>
#include <string>

#include "driver/i2c_master.h"
#include "pcf85063.h"
#include "wisp/base/result.hpp"
#include "wisp/hal/clock.hpp"

namespace wisp::drivers {

struct Pcf85063ClockConfig {
    std::string id;
    // Becomes caps "role" when not empty.
    std::string role;
    std::string model = "PCF85063";
};

class Pcf85063Clock : public wisp::hal::Clock {
public:
    // Calls pcf85063_init(bus) and wraps the resulting handle. Fails with
    // DeviceFailure if that returns anything other than ESP_OK.
    static Result<std::shared_ptr<Pcf85063Clock>> create(i2c_master_bus_handle_t bus,
                                                         Pcf85063ClockConfig config);

    ~Pcf85063Clock() override;

    Pcf85063Clock(const Pcf85063Clock&) = delete;
    Pcf85063Clock& operator=(const Pcf85063Clock&) = delete;

    const wisp::hal::DeviceInfo& info() const override { return info_; }

    // pcf85063_get_unix(). Fails with DeviceFailure if the chip's
    // oscillator-stop flag is set or the decoded time is implausible
    // (see pcf85063.h) -- callers must not settimeofday() from this
    // result on failure.
    Result<int64_t> get_unix() override;
    // pcf85063_set_unix().
    Result<void> set_unix(int64_t seconds) override;

private:
    Pcf85063Clock(pcf85063_handle_t handle, Pcf85063ClockConfig config);

    wisp::hal::DeviceInfo info_;
    pcf85063_handle_t handle_;
};

}  // namespace wisp::drivers
