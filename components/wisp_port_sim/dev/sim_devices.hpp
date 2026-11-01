#pragma once

// Private device declarations shared by components/wisp_port_sim/dev/
// *.cpp and sim_board.cpp (which constructs them). Not installed under
// include/ -- nothing outside this component names these types
// directly. simulator/main.cpp only ever sees wisp::hal::* interfaces
// plus SimBoard::display() (the one simulated device whose concrete type
// main.cpp's SDL render loop needs -- see wisp/port/sim/sim_board.hpp's
// file header, where SimDisplay itself is declared for that reason).

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>

#include "wisp/hal/audio.hpp"
#include "wisp/hal/button.hpp"
#include "wisp/hal/clock.hpp"
#include "wisp/hal/imu.hpp"
#include "wisp/hal/power.hpp"
#include "wisp/hal/storage.hpp"
#include "wisp/hal/touch.hpp"
#include "wisp/port/sim/sim_input.hpp"

namespace wisp::port::sim {

class SimTouch : public wisp::hal::Touch {
public:
    SimTouch(std::string id, std::shared_ptr<SimInput> input);
    const wisp::hal::DeviceInfo& info() const override { return info_; }
    wisp::Result<std::vector<wisp::hal::TouchPoint>> read() override;

private:
    wisp::hal::DeviceInfo info_;
    std::shared_ptr<SimInput> input_;
};

class SimImu : public wisp::hal::Imu {
public:
    SimImu(std::string id, std::shared_ptr<SimInput> input);
    const wisp::hal::DeviceInfo& info() const override { return info_; }
    wisp::Result<wisp::hal::ImuSample> read() override;
    wisp::Result<void> set_rate(uint32_t hz) override;

private:
    wisp::hal::DeviceInfo info_;
    std::shared_ptr<SimInput> input_;
    std::atomic<uint32_t> rate_hz_{100};
};

// Fixed status (80%, 3900 mV, not charging, USB present -- task-13-brief.md).
class SimPower : public wisp::hal::Power {
public:
    SimPower(std::string id, std::function<void()> on_power_off);
    const wisp::hal::DeviceInfo& info() const override { return info_; }
    wisp::Result<wisp::hal::PowerStatus> status() override;
    wisp::Result<void> power_off() override;

private:
    wisp::hal::DeviceInfo info_;
    std::function<void()> on_power_off_;
};

// get_unix() tracks the host's wall-clock time plus an offset;
// set_unix(seconds) recomputes that offset so later get_unix() calls
// return `seconds` plus however much wall-clock time has since elapsed
// ("保存相对系统时间的偏移" -- task-13-brief.md).
class SimClock : public wisp::hal::Clock {
public:
    explicit SimClock(std::string id);
    const wisp::hal::DeviceInfo& info() const override { return info_; }
    wisp::Result<int64_t> get_unix() override;
    wisp::Result<void> set_unix(int64_t seconds) override;

private:
    wisp::hal::DeviceInfo info_;
    std::atomic<int64_t> offset_seconds_{0};
};

class SimButton : public wisp::hal::Button {
public:
    enum class Kind { Power, VolUp, VolDown };

    SimButton(std::string id, Kind kind, std::shared_ptr<SimInput> input);
    const wisp::hal::DeviceInfo& info() const override { return info_; }
    bool pressed() override;

private:
    wisp::hal::DeviceInfo info_;
    Kind kind_;
    std::shared_ptr<SimInput> input_;
};

// Backed by a real SDL playback device (fix round 1, item 1): open()
// calls SDL_OpenAudioDevice(nullptr, 0, ...), write() applies `volume_`
// and queues via SDL_QueueAudio (blocking while more than ~200ms is
// already queued, so playback runs at roughly real-time pace), close()
// closes the device. See dev/sim_audio.cpp. `device_` is the
// SDL_AudioDeviceID of the open device (0 if not open) stored as a
// plain uint32_t -- SDL_AudioDeviceID is just a typedef for Uint32 --
// so this shared header never has to include <SDL.h>; only
// dev/sim_audio.cpp does.
class SimAudioOut : public wisp::hal::AudioOut {
public:
    explicit SimAudioOut(std::string id);
    ~SimAudioOut() override;
    const wisp::hal::DeviceInfo& info() const override { return info_; }
    wisp::Result<void> open(const wisp::hal::AudioFormat& format) override;
    wisp::Result<size_t> write(const int16_t* frames, size_t count) override;
    void close() override;
    wisp::Result<void> set_volume(uint8_t percent) override;

private:
    wisp::hal::DeviceInfo info_;
    uint32_t device_ = 0;
    int channels_ = 1;
    int sample_rate_ = 16000;
    std::atomic<uint8_t> volume_{100};
};

// Backed by a real SDL capture device (fix round 1, item 1): open()
// calls SDL_OpenAudioDevice(nullptr, 1, ...), read() polls
// SDL_DequeueAudio (with a bounded timeout, so a capture device that
// never produces data can't hang the caller forever) until `count`
// frames have arrived, close() closes the device. SimBoard::init()
// only ever constructs this when SDL_GetNumAudioDevices(1) says a
// capture device actually exists (see sim_board.cpp) -- the dummy audio
// driver the simulator selects in --headless mode has none -- but
// open() still fails cleanly with ErrorCode::DeviceFailure if called
// without one, rather than hanging or crashing.
class SimAudioIn : public wisp::hal::AudioIn {
public:
    explicit SimAudioIn(std::string id);
    ~SimAudioIn() override;
    const wisp::hal::DeviceInfo& info() const override { return info_; }
    wisp::Result<void> open(const wisp::hal::AudioFormat& format) override;
    wisp::Result<size_t> read(int16_t* frames, size_t count) override;
    void close() override;
    wisp::Result<void> set_gain(float db) override;

private:
    wisp::hal::DeviceInfo info_;
    uint32_t device_ = 0;
    int channels_ = 1;
};

// mount_point() is a plain host directory; capacity() reports a fixed
// `total_bytes` and derives free_bytes from how many bytes are actually
// used under that directory right now. online() defers to `online_fn`
// (nullptr means "always online", used for storage.int).
class SimStorage : public wisp::hal::Storage {
public:
    SimStorage(std::string id, std::string mount_point, bool removable, std::string fs,
               uint64_t total_bytes, std::function<bool()> online_fn);
    const wisp::hal::DeviceInfo& info() const override { return info_; }
    std::string mount_point() const override { return mount_point_; }
    bool online() override { return online_fn_ ? online_fn_() : true; }
    wisp::Result<wisp::hal::Capacity> capacity() override;

private:
    wisp::hal::DeviceInfo info_;
    std::string mount_point_;
    uint64_t total_bytes_;
    std::function<bool()> online_fn_;
};

}  // namespace wisp::port::sim
