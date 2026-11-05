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
    // Reports no touch while powered off.
    wisp::Result<std::vector<wisp::hal::TouchPoint>> read() override;
    wisp::Result<void> set_power(wisp::hal::PowerLevel level) override;
    bool powered() const { return powered_.load(); }

private:
    wisp::hal::DeviceInfo info_;
    std::shared_ptr<SimInput> input_;
    std::atomic<bool> powered_{true};
};

class SimImu : public wisp::hal::Imu {
public:
    SimImu(std::string id, std::shared_ptr<SimInput> input);
    const wisp::hal::DeviceInfo& info() const override { return info_; }
    // Fails while powered off, like the real sensors.
    wisp::Result<wisp::hal::ImuSample> read() override;
    wisp::Result<void> set_rate(uint32_t hz) override;
    wisp::Result<void> set_power(wisp::hal::PowerLevel level) override;
    bool powered() const { return powered_.load(); }

private:
    wisp::hal::DeviceInfo info_;
    std::shared_ptr<SimInput> input_;
    std::atomic<uint32_t> rate_hz_{100};
    std::atomic<bool> powered_{true};
};

// Fixed status (80%, 3900 mV, not charging, USB present).
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
// ("保存相对系统时间的偏移").
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
    // A wake source: stays on at every level.
    wisp::Result<void> set_power(wisp::hal::PowerLevel level) override;

private:
    wisp::hal::DeviceInfo info_;
    Kind kind_;
    std::shared_ptr<SimInput> input_;
};

// Backed by a real SDL playback device: open() calls
// SDL_OpenAudioDevice(nullptr, 0, ...) and fails with
// ErrorCode::AlreadyExists if already open; write() applies
// `volume_`, splits the buffer into ~20ms chunks, and queues each one
// via SDL_QueueAudio only once the queue has drained below ~200ms --
// queuing a whole clip instantly lets close() drop almost all of it
// by closing the device before playback has caught up. close()
// drains the remaining queue, then waits out the device's own
// internal buffer (`buffer_frames_`) too -- a drained queue doesn't
// mean the device's hardware/callback buffer is empty -- before
// actually closing the device, so unplayed audio isn't discarded.
// write()'s wait loop uses a progress-based stall deadline, reset
// whenever a chunk is actually queued, rather than one fixed
// deadline for the whole call. If the device stops playing or the
// stall deadline passes, write() returns whatever was written so far
// (ok, with a short count) once at least one chunk was queued, and
// only reports ErrorCode::Timeout if nothing was written at all. See
// dev/sim_audio.cpp.
// `device_` is the SDL_AudioDeviceID of the open device (0 if not open)
// stored as a plain uint32_t -- SDL_AudioDeviceID is just a typedef for
// Uint32 -- so this shared header never has to include <SDL.h>; only
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
    // Only tracks the level (the host's audio device has no power state
    // to change); powered() lets tests check it.
    wisp::Result<void> set_power(wisp::hal::PowerLevel level) override;
    bool powered() const { return powered_.load(); }

private:
    wisp::hal::DeviceInfo info_;
    uint32_t device_ = 0;
    int channels_ = 1;
    int sample_rate_ = 16000;
    // SDL's `have.samples` from open(): the device's own internal
    // buffer size in frames, used by close() to wait out whatever
    // that buffer may still hold after the queue itself has drained
    // to zero.
    uint16_t buffer_frames_ = 0;
    std::atomic<uint8_t> volume_{100};
    std::atomic<bool> powered_{true};
};

// Backed by a real SDL capture device: open() calls
// SDL_OpenAudioDevice(nullptr, 1, ...) and fails with
// ErrorCode::AlreadyExists if already open; read() polls
// SDL_DequeueAudio until `count` frames have arrived, using a
// progress-based stall deadline, reset whenever data is actually
// dequeued, rather than one fixed deadline for the whole call. If
// the device stops playing or the stall deadline
// passes, read() returns whatever was captured so far (ok, with a short
// count) when at least one frame was captured, and only reports
// ErrorCode::Timeout if nothing was captured at all; close() closes the
// device. SimBoard::init() only ever constructs this when SDL audio is
// up and SDL_GetNumAudioDevices(1) says a capture device actually
// exists (see sim_board.cpp), but open() still fails cleanly with
// ErrorCode::DeviceFailure if called without one, rather than hanging
// or crashing.
class SimAudioIn : public wisp::hal::AudioIn {
public:
    explicit SimAudioIn(std::string id);
    ~SimAudioIn() override;
    const wisp::hal::DeviceInfo& info() const override { return info_; }
    wisp::Result<void> open(const wisp::hal::AudioFormat& format) override;
    wisp::Result<size_t> read(int16_t* frames, size_t count) override;
    void close() override;
    wisp::Result<void> set_gain(float db) override;
    // Same as SimAudioOut::set_power().
    wisp::Result<void> set_power(wisp::hal::PowerLevel level) override;
    bool powered() const { return powered_.load(); }

private:
    wisp::hal::DeviceInfo info_;
    uint32_t device_ = 0;
    int channels_ = 1;
    std::atomic<bool> powered_{true};
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
