#pragma once

// AudioArbiter (media-library spec 5): who may have the main speaker.
// Every sound source takes a Lease of its kind before it opens the
// AudioOut and drops it once the output is closed again; the arbiter
// decides who gets one. Priority, highest first:
//
//   Alarm  TonePlayer's alarm.
//   Voice  speech (TTS) through PcmPlayer; also recording, which holds an
//          input lease (acquire_input()) although it does not use the
//          speaker, so the microphone does not pick up music.
//   Tone   TonePlayer's notification beep.
//   Music  the media player.
//
// The rules between speech and the sounds (spec 5.1, unchanged from when
// VoiceService made them itself):
//   - An Alarm always wins: acquire(Alarm) calls the voice stop (see
//     set_voice_stop()), then waits up to alarm_wait_ms for the Voice
//     output leases to be released, and is granted either way.
//   - A Tone while a Voice output lease or an Alarm is held (or an Alarm
//     is being acquired) is refused at once.
//   - A Voice output lease waits up to tone_wait_ms while a Tone or an
//     Alarm is held (or an Alarm is being acquired), then is refused.
//   - An input lease neither waits for nor holds back tones and alarms: a
//     notification beep or an alarm still sounds while the mic records,
//     as it did before, and the start tone a capture plays with the mic
//     open is let through.
//
// Threads: every method may be called from any thread. acquire() may
// wait (see above); try_acquire_music(), held() and the Lease methods do
// not. The voice stop is called outside the arbiter's state lock, on the
// thread that acquires the Alarm; it must not wait for anything that
// thread's caller holds, acquire a lease or call set_voice_stop().
// set_voice_stop() waits for a call of the previous callback still in
// progress, so its target may be destroyed once it has been replaced.
//
// Lifetime: every Lease is released (destroyed or release()d) before the
// arbiter is destroyed.

#include <cstdint>
#include <functional>
#include <memory>

#include "wisp/os/os.hpp"

namespace wisp::core {

// Ascending priority.
enum class AudioKind : uint8_t { Music, Tone, Voice, Alarm };

// "music" "tone" "voice" "alarm"
const char* to_string(AudioKind kind);

class AudioArbiter {
public:
    struct Config {
        uint32_t alarm_wait_ms = 500;
        uint32_t tone_wait_ms = 300;
    };

    // A granted share of the output; move-only, released on destruction.
    // A default or refused Lease is invalid. One Lease object is not
    // shared between threads (like a unique_ptr), but it may be moved to
    // and released on another thread than the one that acquired it.
    class Lease {
    public:
        Lease() = default;
        ~Lease() { release(); }
        Lease(Lease&& o) noexcept;
        Lease& operator=(Lease&& o) noexcept;
        Lease(const Lease&) = delete;
        Lease& operator=(const Lease&) = delete;

        bool valid() const { return arbiter_ != nullptr; }
        AudioKind kind() const { return kind_; }
        // Releases now; a no-op on an invalid lease.
        void release();

    private:
        friend class AudioArbiter;
        Lease(AudioArbiter* a, AudioKind kind, bool input)
            : arbiter_(a), kind_(kind), input_(input) {}

        AudioArbiter* arbiter_ = nullptr;
        AudioKind kind_ = AudioKind::Music;
        bool input_ = false;
    };

    explicit AudioArbiter(Config config, std::function<uint64_t()> now_ms = os::now_ms);
    // Config{} (a default argument cannot be, as Config is nested).
    AudioArbiter();
    ~AudioArbiter() = default;

    AudioArbiter(const AudioArbiter&) = delete;
    AudioArbiter& operator=(const AudioArbiter&) = delete;

    // Tone, Voice (an output lease) or Alarm, by the rules in the file
    // header; an invalid Lease when refused. `stopped` (optional) is
    // polled while waiting: true gives up with an invalid Lease.
    Lease acquire(AudioKind kind, const std::function<bool()>& stopped = {});
    // A Voice lease for recording (see the file header); never refused.
    Lease acquire_input();

    // Called by acquire(Alarm), see the file header. Null removes it.
    void set_voice_stop(std::function<void()> stop);

    // A lease of `kind` is held (Voice: output or input).
    bool held(AudioKind kind) const;

private:
    void release(const Lease& l);
    // Waits until `done` (checked under mu_), `stopped` or `timeout_ms`;
    // returns done's last answer.
    bool wait_until(const std::function<bool()>& done, const std::function<bool()>& stopped,
                    uint32_t timeout_ms);
    void call_voice_stop();

    Config config_;
    std::function<uint64_t()> now_ms_;

    // Guards the counts below.
    mutable os::Mutex mu_;
    uint32_t tone_ = 0;
    uint32_t voice_out_ = 0;
    uint32_t voice_in_ = 0;
    uint32_t alarm_ = 0;
    // acquire(Alarm) calls in progress: they hold speech and tones back
    // already, as the alarm they will become does.
    uint32_t alarm_pending_ = 0;

    // Held while a callback runs and while one is replaced, so a replaced
    // callback is no longer running once its setter returns. Taken before
    // mu_, never after.
    os::Mutex call_mu_;
    std::function<void()> voice_stop_;
};

}  // namespace wisp::core
