#include "wisp/core/audio_arbiter.hpp"

#include <mutex>
#include <utility>

#include "wisp/core/log.hpp"

namespace wisp::core {

namespace {

constexpr const char* kTag = "audio";

// The arbiter whose callback this thread is running: it holds that
// arbiter's call_mu_ already, so a Music lease released from inside the
// callback must not take it again.
thread_local const AudioArbiter* t_in_callback = nullptr;

}  // namespace

const char* to_string(AudioKind kind) {
    switch (kind) {
        case AudioKind::Music:
            return "music";
        case AudioKind::Tone:
            return "tone";
        case AudioKind::Voice:
            return "voice";
        case AudioKind::Alarm:
            return "alarm";
    }
    return "music";
}

AudioArbiter::Lease::Lease(Lease&& o) noexcept
    : arbiter_(std::exchange(o.arbiter_, nullptr)), kind_(o.kind_), input_(o.input_), id_(o.id_) {}

AudioArbiter::Lease& AudioArbiter::Lease::operator=(Lease&& o) noexcept {
    if (this != &o) {
        release();
        arbiter_ = std::exchange(o.arbiter_, nullptr);
        kind_ = o.kind_;
        input_ = o.input_;
        id_ = o.id_;
    }
    return *this;
}

bool AudioArbiter::Lease::valid() const {
    if (arbiter_ == nullptr) return false;
    return kind_ != AudioKind::Music || arbiter_->music_valid(id_);
}

void AudioArbiter::Lease::release() {
    if (arbiter_ == nullptr) return;
    std::exchange(arbiter_, nullptr)->release(*this);
}

AudioArbiter::AudioArbiter(Config config, std::function<uint64_t()> now_ms)
    : config_(config), now_ms_(std::move(now_ms)) {}

AudioArbiter::AudioArbiter() : AudioArbiter(Config{}) {}

AudioArbiter::~AudioArbiter() { stop(); }

AudioArbiter::Lease AudioArbiter::acquire(AudioKind kind, const std::function<bool()>& stopped) {
    Lease lease;
    switch (kind) {
        case AudioKind::Tone: {
            std::lock_guard<os::Mutex> lock(mu_);
            if (voice_out_ > 0 || alarm_ > 0 || alarm_pending_ > 0) return {};
            ++tone_;
            lease = Lease(this, AudioKind::Tone, false);
            break;
        }
        case AudioKind::Voice: {
            const bool granted = wait_until(
                [this] {
                    if (tone_ > 0 || alarm_ > 0 || alarm_pending_ > 0) return false;
                    ++voice_out_;
                    return true;
                },
                stopped, config_.tone_wait_ms);
            if (!granted) return {};
            lease = Lease(this, AudioKind::Voice, false);
            break;
        }
        case AudioKind::Alarm: {
            {
                std::lock_guard<os::Mutex> lock(mu_);
                ++alarm_pending_;
            }
            // Always, as the speech may be on its way to the output
            // (synthesizing its first sentence) without a lease yet.
            call(&AudioArbiter::voice_stop_);
            preempt_music();
            wait_until([this] { return voice_out_ == 0; }, stopped, config_.alarm_wait_ms);
            std::lock_guard<os::Mutex> lock(mu_);
            if (stopped && stopped()) {
                others_dropped(alarm_pending_);
                return {};
            }
            --alarm_pending_;
            ++alarm_;
            return Lease(this, AudioKind::Alarm, false);
        }
        case AudioKind::Music:
            return {};
    }
    preempt_music();
    return lease;
}

AudioArbiter::Lease AudioArbiter::acquire_input() {
    Lease lease;
    {
        std::lock_guard<os::Mutex> lock(mu_);
        ++voice_in_;
        lease = Lease(this, AudioKind::Voice, true);
    }
    preempt_music();
    return lease;
}

AudioArbiter::Lease AudioArbiter::try_acquire_music(std::function<void()> preempt) {
    std::unique_lock<os::Mutex> lock(mu_);
    if (others_busy()) {
        resume_armed_ = true;
        lock.unlock();
        arm();
        return {};
    }
    if (music_id_ != 0) return {};
    if (++next_music_id_ == 0) ++next_music_id_;
    music_id_ = next_music_id_;
    music_preempted_ = false;
    music_preempt_ = std::move(preempt);
    resume_armed_ = false;
    return Lease(this, AudioKind::Music, false, music_id_);
}

void AudioArbiter::set_voice_stop(std::function<void()> stop) {
    std::lock_guard<os::Mutex> calls(call_mu_);
    voice_stop_ = std::move(stop);
}

void AudioArbiter::set_resume_listener(std::function<void()> listener) {
    std::lock_guard<os::Mutex> calls(call_mu_);
    resume_listener_ = std::move(listener);
}

void AudioArbiter::tick() {
    {
        std::lock_guard<os::Mutex> lock(mu_);
        if (!resume_armed_ || others_busy() || music_id_ != 0) return;
        if (now_ms_() - quiet_since_ms_ < config_.resume_delay_ms) return;
        resume_armed_ = false;
    }
    call(&AudioArbiter::resume_listener_);
}

void AudioArbiter::start() {
    if (!timer_) {
        timer_ = os::Timer::create("wisp.audio", config_.tick_ms, true, [this] { on_timer(); });
        if (!timer_) {
            WLOG_W(kTag, "no timer: music is not resumed after speech");
            return;
        }
    }
    {
        std::lock_guard<os::Mutex> lock(mu_);
        running_ = true;
    }
    arm();
}

void AudioArbiter::stop() {
    {
        std::lock_guard<os::Mutex> lock(mu_);
        running_ = false;
    }
    if (timer_) timer_->stop();
}

void AudioArbiter::arm() {
    {
        std::lock_guard<os::Mutex> lock(mu_);
        if (!running_ || !resume_armed_) return;
    }
    // Outside mu_: start() may wait for a call of on_timer() that stopped
    // the timer to return, and that call takes mu_.
    timer_->start();
}

void AudioArbiter::on_timer() {
    {
        // Under mu_, so an arm() that set resume_armed_ either is seen
        // here or starts the timer again after this stop.
        std::lock_guard<os::Mutex> lock(mu_);
        if (!resume_armed_ || !running_) {
            timer_->stop();  // from its own callback: returns at once
            return;
        }
    }
    tick();
}

bool AudioArbiter::held(AudioKind kind) const {
    std::lock_guard<os::Mutex> lock(mu_);
    switch (kind) {
        case AudioKind::Music:
            return music_id_ != 0 && !music_preempted_;
        case AudioKind::Tone:
            return tone_ > 0;
        case AudioKind::Voice:
            return voice_out_ > 0 || voice_in_ > 0;
        case AudioKind::Alarm:
            return alarm_ > 0;
    }
    return false;
}

void AudioArbiter::release(const Lease& l) {
    if (l.kind_ == AudioKind::Music) {
        // Waits for its preempt, if one is running on another thread, so
        // the owner may go once this returns.
        std::unique_lock<os::Mutex> calls(call_mu_, std::defer_lock);
        if (t_in_callback != this) calls.lock();
        std::function<void()> gone;  // destroyed outside mu_
        std::lock_guard<os::Mutex> lock(mu_);
        if (music_id_ != l.id_) return;  // revoked
        music_id_ = 0;
        gone = std::move(music_preempt_);
        music_preempt_ = nullptr;
        return;
    }
    std::lock_guard<os::Mutex> lock(mu_);
    switch (l.kind_) {
        case AudioKind::Tone:
            others_dropped(tone_);
            break;
        case AudioKind::Voice:
            others_dropped(l.input_ ? voice_in_ : voice_out_);
            break;
        case AudioKind::Alarm:
            others_dropped(alarm_);
            break;
        case AudioKind::Music:
            break;
    }
}

bool AudioArbiter::music_valid(uint32_t id) const {
    std::lock_guard<os::Mutex> lock(mu_);
    return music_id_ == id && !music_preempted_;
}

bool AudioArbiter::others_busy() const {
    return tone_ > 0 || voice_out_ > 0 || voice_in_ > 0 || alarm_ > 0 || alarm_pending_ > 0;
}

void AudioArbiter::others_dropped(uint32_t& count) {
    --count;
    if (!others_busy()) quiet_since_ms_ = now_ms_();
}

bool AudioArbiter::wait_until(const std::function<bool()>& done,
                              const std::function<bool()>& stopped, uint32_t timeout_ms) {
    const uint64_t deadline = now_ms_() + timeout_ms;
    for (;;) {
        {
            std::lock_guard<os::Mutex> lock(mu_);
            if (done()) return true;
        }
        if ((stopped && stopped()) || now_ms_() >= deadline) return false;
        os::sleep_ms(1);
    }
}

void AudioArbiter::preempt_music() {
    uint32_t id = 0;
    {
        // call_mu_ before the lease is looked at: it cannot be released
        // (and its owner go) between the look and the call.
        std::lock_guard<os::Mutex> calls(call_mu_);
        std::function<void()> preempt;
        {
            std::lock_guard<os::Mutex> lock(mu_);
            if (music_id_ == 0) return;
            id = music_id_;
            if (!music_preempted_) {
                music_preempted_ = true;
                resume_armed_ = true;
                // A copy: the callback may release the lease, which
                // clears music_preempt_ while it runs.
                preempt = music_preempt_;
            }
        }
        if (preempt) {
            const AudioArbiter* outer = std::exchange(t_in_callback, this);
            preempt();
            t_in_callback = outer;
        }
    }
    arm();
    if (wait_until([this, id] { return music_id_ != id; }, {}, config_.preempt_grant_ms)) return;
    std::function<void()> gone;  // destroyed outside mu_
    std::lock_guard<os::Mutex> lock(mu_);
    if (music_id_ != id) return;
    music_id_ = 0;
    gone = std::move(music_preempt_);
    music_preempt_ = nullptr;
    WLOG_W(kTag, "music did not stop within %u ms: revoked",
           static_cast<unsigned>(config_.preempt_grant_ms));
}

void AudioArbiter::call(std::function<void()> AudioArbiter::* cb) {
    // Called in place under call_mu_, which its setter takes too.
    std::lock_guard<os::Mutex> calls(call_mu_);
    if (!(this->*cb)) return;
    const AudioArbiter* outer = std::exchange(t_in_callback, this);
    (this->*cb)();
    t_in_callback = outer;
}

}  // namespace wisp::core
