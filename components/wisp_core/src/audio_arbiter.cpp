#include "wisp/core/audio_arbiter.hpp"

#include <mutex>
#include <utility>

namespace wisp::core {

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
    : arbiter_(std::exchange(o.arbiter_, nullptr)), kind_(o.kind_), input_(o.input_) {}

AudioArbiter::Lease& AudioArbiter::Lease::operator=(Lease&& o) noexcept {
    if (this != &o) {
        release();
        arbiter_ = std::exchange(o.arbiter_, nullptr);
        kind_ = o.kind_;
        input_ = o.input_;
    }
    return *this;
}

void AudioArbiter::Lease::release() {
    if (arbiter_ == nullptr) return;
    arbiter_->release(*this);
    arbiter_ = nullptr;
}

AudioArbiter::AudioArbiter(Config config, std::function<uint64_t()> now_ms)
    : config_(config), now_ms_(std::move(now_ms)) {}

AudioArbiter::AudioArbiter() : AudioArbiter(Config{}) {}

AudioArbiter::Lease AudioArbiter::acquire(AudioKind kind, const std::function<bool()>& stopped) {
    switch (kind) {
        case AudioKind::Tone: {
            std::lock_guard<os::Mutex> lock(mu_);
            if (voice_out_ > 0 || alarm_ > 0 || alarm_pending_ > 0) return {};
            ++tone_;
            return Lease(this, AudioKind::Tone, false);
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
            return Lease(this, AudioKind::Voice, false);
        }
        case AudioKind::Alarm: {
            {
                std::lock_guard<os::Mutex> lock(mu_);
                ++alarm_pending_;
            }
            // Always, as the speech may be on its way to the output
            // (synthesizing its first sentence) without a lease yet.
            call_voice_stop();
            wait_until([this] { return voice_out_ == 0; }, stopped, config_.alarm_wait_ms);
            std::lock_guard<os::Mutex> lock(mu_);
            --alarm_pending_;
            if (stopped && stopped()) return {};
            ++alarm_;
            return Lease(this, AudioKind::Alarm, false);
        }
        case AudioKind::Music:
            break;
    }
    return {};
}

AudioArbiter::Lease AudioArbiter::acquire_input() {
    std::lock_guard<os::Mutex> lock(mu_);
    ++voice_in_;
    return Lease(this, AudioKind::Voice, true);
}

void AudioArbiter::set_voice_stop(std::function<void()> stop) {
    std::lock_guard<os::Mutex> calls(call_mu_);
    voice_stop_ = std::move(stop);
}

bool AudioArbiter::held(AudioKind kind) const {
    std::lock_guard<os::Mutex> lock(mu_);
    switch (kind) {
        case AudioKind::Music:
            return false;
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
    std::lock_guard<os::Mutex> lock(mu_);
    switch (l.kind_) {
        case AudioKind::Tone:
            --tone_;
            break;
        case AudioKind::Voice:
            --(l.input_ ? voice_in_ : voice_out_);
            break;
        case AudioKind::Alarm:
            --alarm_;
            break;
        case AudioKind::Music:
            break;
    }
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

void AudioArbiter::call_voice_stop() {
    std::lock_guard<os::Mutex> calls(call_mu_);
    if (voice_stop_) voice_stop_();
}

}  // namespace wisp::core
