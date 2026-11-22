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

// caps keys (AudioOut and AudioIn): "standby" (string, optional: "off"
// when set_power() releases the audio hardware at Standby/DeepStandby;
// see StandbyPolicy in wisp/hal/device.hpp). Playing below Dim requires an
// "audio" wake lock (wisp/core/wakelock.hpp); recording below Dim is not
// supported.
// AudioIn also: "channels" (list of strings, optional: one role per
// physical capture channel, "mic" or "ref" for the speaker signal looped
// back; read() still delivers the mono mix of the "mic" channels).
class AudioOut : public Device {
public:
    static constexpr DeviceClass kClass = DeviceClass::AudioOut;
    static constexpr Interface kInterface = Interface::AudioOut;

    virtual Result<void> open(const AudioFormat& format) = 0;
    virtual Result<size_t> write(const int16_t* frames, size_t count) = 0;
    virtual void close() = 0;
    virtual Result<void> set_volume(uint8_t percent) = 0;

protected:
    AudioOut() : Device(kInterface) {}
};

class AudioIn : public Device {
public:
    static constexpr DeviceClass kClass = DeviceClass::AudioIn;
    static constexpr Interface kInterface = Interface::AudioIn;

    virtual Result<void> open(const AudioFormat& format) = 0;
    virtual Result<size_t> read(int16_t* frames, size_t count) = 0;
    virtual void close() = 0;
    virtual Result<void> set_gain(float db) = 0;

protected:
    AudioIn() : Device(kInterface) {}
};

}  // namespace wisp::hal
