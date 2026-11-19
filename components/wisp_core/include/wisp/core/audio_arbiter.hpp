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
// Music (spec 5.2) is the lowest kind and never holds the others back:
//   - try_acquire_music() fails while any other lease is held (or being
//     acquired); there is one Music lease at a time.
//   - Any other acquire, once it is past its own rules, preempts the
//     Music lease: it calls the lease's preempt callback (once per lease)
//     and waits up to preempt_grant_ms for the lease to be released, then
//     is granted anyway. From the preemption on, the Music lease is no
//     longer valid(), so its owner does not open the output again; past
//     the grant it is revoked, and releasing it later is harmless.
//   - After a preemption or a refused try_acquire_music(), once no other
//     lease has been held for resume_delay_ms (each release of the last
//     one restarts the delay), the resume listener is called once. The
//     owner decides whether to resume (a user pause is never resumed;
//     that rule is the player's) and re-acquires through
//     try_acquire_music(). The delay is driven by tick(): the arbiter's
//     own os::Timer ("wisp.audio", every tick_ms, running only while a
//     resume waits) between start() and stop(), or a test calling it with
//     its own clock.
//
// Threads: every method may be called from any thread. acquire() and
// acquire_input() may wait (see above); try_acquire_music(), held(),
// tick() and the Lease methods do not, except that releasing a Music
// lease waits for a call of its preempt callback in progress. The
// callbacks (the voice stop, a preempt, the resume listener) run outside
// the arbiter's state lock, one at a time: the voice stop and a preempt on
// the thread that acquires, the listener on the timer's thread (on the
// device the shared esp_timer task). They must be short and must not wait
// for anything a thread releasing a Music lease may hold (set an atomic,
// post work); from inside one a Music lease may be released and
// try_acquire_music() called, but no setter, acquire() or acquire_input(). A setter waits for a
// call of the previous callback in progress, so its target may be
// destroyed once it has been replaced; likewise once a Music lease has
// been released its preempt is no longer running and never runs again.
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
        uint32_t preempt_grant_ms = 200;
        uint32_t resume_delay_ms = 800;
        uint32_t alarm_wait_ms = 500;
        uint32_t tone_wait_ms = 300;
        // The period of the timer that calls tick().
        uint32_t tick_ms = 100;
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

        // Granted and not yet released; a Music lease also stops being
        // valid when it is preempted.
        bool valid() const;
        AudioKind kind() const { return kind_; }
        // Releases now; a no-op on an invalid lease.
        void release();

    private:
        friend class AudioArbiter;
        Lease(AudioArbiter* a, AudioKind kind, bool input, uint32_t id = 0)
            : arbiter_(a), kind_(kind), input_(input), id_(id) {}

        AudioArbiter* arbiter_ = nullptr;
        AudioKind kind_ = AudioKind::Music;
        bool input_ = false;
        uint32_t id_ = 0;  // Music only
    };

    explicit AudioArbiter(Config config, std::function<uint64_t()> now_ms = os::now_ms);
    // Config{} (a default argument cannot be, as Config is nested).
    AudioArbiter();
    ~AudioArbiter();  // stop()

    AudioArbiter(const AudioArbiter&) = delete;
    AudioArbiter& operator=(const AudioArbiter&) = delete;

    // Tone, Voice (an output lease) or Alarm, by the rules in the file
    // header; an invalid Lease when refused, and for Music. `stopped`
    // (optional) is polled while waiting for the speech, a tone or an
    // alarm: true gives up with an invalid Lease.
    Lease acquire(AudioKind kind, const std::function<bool()>& stopped = {});
    // A Voice lease for recording (see the file header); never refused.
    Lease acquire_input();
    // Music, by the rules in the file header; `preempt` asks the owner to
    // close the output and release the lease (see Threads).
    Lease try_acquire_music(std::function<void()> preempt);

    // Called by acquire(Alarm), see the file header. Null removes it.
    void set_voice_stop(std::function<void()> stop);
    // See the file header. Null removes it.
    void set_resume_listener(std::function<void()> listener);

    // Calls the resume listener when it is due (see the file header).
    void tick();
    // Starts / stops the timer that calls tick(). Idempotent; leases may
    // be held across them. Not concurrent with each other (the owner
    // calls them), and stop() not from inside a callback. Without a timer
    // (the port could not make one) music is never resumed by the
    // arbiter.
    void start();
    void stop();

    // A lease of `kind` is held (Voice: output or input; Music: valid).
    bool held(AudioKind kind) const;

private:
    void release(const Lease& l);
    bool music_valid(uint32_t id) const;
    // Any lease but Music held or being acquired (mu_ held).
    bool others_busy() const;
    // One fewer lease but Music: starts the resume delay when it was the
    // last (mu_ held).
    void others_dropped(uint32_t& count);
    // Waits until `done` (checked under mu_), `stopped` or `timeout_ms`;
    // returns done's last answer.
    bool wait_until(const std::function<bool()>& done, const std::function<bool()>& stopped,
                    uint32_t timeout_ms);
    // Asks the Music lease to go and waits for it up to preempt_grant_ms
    // (see the file header). The caller's own lease is counted already.
    void preempt_music();
    // Starts the timer when a resume is armed (no lock held).
    void arm();
    // The timer's callback: tick(), or the timer stops when nothing is
    // armed.
    void on_timer();
    // Runs the callback `cb` names, if set, under call_mu_ (see the file
    // header).
    void call(std::function<void()> AudioArbiter::* cb);

    Config config_;
    std::function<uint64_t()> now_ms_;

    // Guards the state below up to the callbacks.
    mutable os::Mutex mu_;
    uint32_t tone_ = 0;
    uint32_t voice_out_ = 0;
    uint32_t voice_in_ = 0;
    uint32_t alarm_ = 0;
    // acquire(Alarm) calls in progress: they hold speech and tones back
    // already, as the alarm they will become does.
    uint32_t alarm_pending_ = 0;
    // The Music lease (0: none; ids never repeat before the counter wraps).
    uint32_t music_id_ = 0;
    uint32_t next_music_id_ = 0;
    bool music_preempted_ = false;
    std::function<void()> music_preempt_;
    // A preemption or a refused music acquire waits for its resume.
    bool resume_armed_ = false;
    // When the last lease but Music was released.
    uint64_t quiet_since_ms_ = 0;

    // Held while a callback runs, while one is replaced and while a Music
    // lease is released, so a replaced callback, or a released lease's
    // preempt, is no longer running afterwards. Taken before mu_, never
    // after.
    os::Mutex call_mu_;
    std::function<void()> voice_stop_;
    std::function<void()> resume_listener_;

    // Made by the first start(); runs only while running_ and a resume
    // is armed, so an idle arbiter wakes nothing up.
    std::unique_ptr<os::Timer> timer_;
    bool running_ = false;  // mu_
};

}  // namespace wisp::core
