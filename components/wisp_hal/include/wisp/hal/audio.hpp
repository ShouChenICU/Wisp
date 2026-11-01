#pragma once

#include <cstddef>
#include <cstdint>

#include "wisp/base/result.hpp"
#include "wisp/hal/device.hpp"

namespace wisp::hal {

struct AudioFormat {
    uint32_t sample_rate = 16000;
    uint8_t channels = 1;
};

class AudioOut : public Device {
public:
    static constexpr DeviceClass kClass = DeviceClass::AudioOut;

    virtual Result<void> open(const AudioFormat& format) = 0;
    virtual Result<size_t> write(const int16_t* frames, size_t count) = 0;
    virtual void close() = 0;
    virtual Result<void> set_volume(uint8_t percent) = 0;
};

class AudioIn : public Device {
public:
    static constexpr DeviceClass kClass = DeviceClass::AudioIn;

    virtual Result<void> open(const AudioFormat& format) = 0;
    virtual Result<size_t> read(int16_t* frames, size_t count) = 0;
    virtual void close() = 0;
    virtual Result<void> set_gain(float db) = 0;
};

}  // namespace wisp::hal
