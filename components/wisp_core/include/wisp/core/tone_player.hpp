#pragma once

// TonePlayer (scene-device spec 5.4): the notification sounds, synthesized
// at run time and played through a hal::AudioOut on a task of its own
// ("wisp.tone"), so nothing that asks for a sound waits for it.
//
//   tone   one 1 kHz beep of kToneMs, with 5 ms fades in and out.
//   alarm  segments of kAlarmSegmentMs (880 Hz for 200 ms, 100 ms of
//          silence, 880 Hz for 200 ms, 500 ms of silence), repeated until
//          silence(), another play(), or kAlarmMaxMs of sound.
//
// The volume is the caller's (the notify_vol setting): the samples are
// scaled by it, the device's own volume is left alone. A cpu wake lock
// ("system.tone") is held while a sound plays, so automatic light sleep
// cannot cut it short. Without an output, or when open() fails, a sound is
// silently skipped: notifications work without a speaker.
//
// The task touches no flash (task_config(): flash_access = false): it
// synthesizes into a buffer it owns and writes to the output; it never
// reads Settings.
//
// Thread-safe: play(), silence() and playing() may be called from any
// thread, and never wait.

#include <array>
#include <atomic>
#include <cstddef>
#include <cstdint>
#include <memory>
#include <optional>
#include <span>

#include "wisp/base/result.hpp"
#include "wisp/core/wakelock.hpp"
#include "wisp/hal/audio.hpp"
#include "wisp/os/os.hpp"

namespace wisp::core {

enum class ToneKind : uint8_t { Tone, Alarm };

struct TonePlayerConfig {
    size_t stack_bytes = 4096;
    int priority = 3;
    uint32_t sample_rate = 16000;
};

// Fills `out` with the frames of `kind` from frame `offset` on and returns
// how many it wrote: for a tone, up to its end (0 past it); an alarm
// loops, so it always fills `out`.
size_t synth_tone(ToneKind kind, uint32_t sample_rate, uint32_t offset, std::span<int16_t> out);

class TonePlayer {
public:
    static constexpr uint32_t kToneMs = 150;
    static constexpr uint32_t kAlarmSegmentMs = 1000;
    static constexpr uint32_t kAlarmMaxMs = 30000;
    // Frames synthesized and written at a time; a silence() or a new
    // play() takes effect between two of them.
    static constexpr size_t kChunkFrames = 256;

    // `out` and `locks` may be null.
    TonePlayer(std::shared_ptr<hal::AudioOut> out, WakeLockRegistry* locks,
               TonePlayerConfig config = {});
    ~TonePlayer();  // stop()

    TonePlayer(const TonePlayer&) = delete;
    TonePlayer& operator=(const TonePlayer&) = delete;

    // "wisp.tone", flash_access = false (see the file header), so the port
    // may put its stack in PSRAM.
    static os::TaskConfig task_config(const TonePlayerConfig& c);

    // Spawns the task; without an output there is nothing to play and no
    // task (Ok). Busy if started before, NoMemory if the task cannot be
    // made.
    Result<void> start();
    // Stops the sound and the task. Idempotent.
    void stop();

    // Starts `kind` at `volume_percent` (0-100; 0 is silence()), replacing
    // whatever is playing. Never waits; does nothing before start().
    void play(ToneKind kind, uint8_t volume_percent);
    // Stops what is playing within one chunk. Cheap when nothing plays.
    void silence();
    // A sound was asked for and has not ended yet.
    bool playing() const;

private:
    struct Request {
        ToneKind kind;
        uint8_t volume;
    };

    void run();
    // Plays one request until it ends or `generation_` moves on from `gen`.
    void play_one(const Request& r, uint32_t gen);
    // Writes `n` frames of buf_; false if the output failed.
    bool write_all(size_t n);

    std::shared_ptr<hal::AudioOut> out_;
    WakeLockRegistry* locks_;
    TonePlayerConfig config_;
    std::unique_ptr<os::Task> task_;
    os::Semaphore wake_{1, 0};

    // Guards pending_ and the moves of playing_ to false.
    mutable os::Mutex mu_;
    std::optional<Request> pending_;
    // Moves on with every play() and silence(): the sound in progress
    // stops when it is no longer the one it started with.
    std::atomic<uint32_t> generation_{0};
    std::atomic<bool> playing_{false};
    std::atomic<bool> quit_{false};
    bool open_warned_ = false;  // task only

    std::array<int16_t, kChunkFrames> buf_{};  // task only
};

}  // namespace wisp::core
