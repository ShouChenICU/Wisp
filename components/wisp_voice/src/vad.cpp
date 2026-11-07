#include "wisp/voice/vad.hpp"

#include <algorithm>
#include <cmath>

#include "wisp/voice/limits.hpp"

namespace wisp::voice {

namespace {

// Full scale for dBFS: a square wave at 32768 has RMS 32768.
constexpr double kFullScaleSquared = 32768.0 * 32768.0;
constexpr double kLevelFloorDb = -60.0;

}  // namespace

uint8_t level_of(double mean_square) {
    const double db =
        mean_square > 0 ? 10.0 * std::log10(mean_square / kFullScaleSquared) : kLevelFloorDb;
    const double pct = std::clamp((db - kLevelFloorDb) / -kLevelFloorDb, 0.0, 1.0) * 100.0;
    return static_cast<uint8_t>(std::lround(pct));
}

VadVerdict Vad::verdict_after_speech() const {
    return speech_ms_ < kMinSpeechMs ? VadVerdict::TooShort : VadVerdict::Done;
}

VadVerdict Vad::push(std::span<const int16_t> frame) {
    double e = 0;
    for (int16_t s : frame) e += static_cast<double>(s) * s;
    if (!frame.empty()) e /= static_cast<double>(frame.size());

    level_ = level_of(e);

    if (final_ != VadVerdict::Continue) return final_;

    const uint32_t t = elapsed_ms_;
    elapsed_ms_ += kFrameMs;
    if (t < kNoiseEstimateMs) {
        if (head_count_ < kHeadFrames) head_[head_count_++] = e;
        return VadVerdict::Continue;
    }
    if (head_count_ > 0) {
        // The mean of the quietest half: a transient at the start (a codec
        // pop, the tone's tail) falls in the loud half and is left out.
        std::sort(head_.begin(), head_.begin() + head_count_);
        const size_t n = (head_count_ + 1) / 2;
        double sum = 0;
        for (size_t i = 0; i < n; ++i) sum += head_[i];
        noise_ = sum / static_cast<double>(n);
        head_count_ = 0;
    }

    const bool speech = e > std::max(noise_ * kSpeechRatio, static_cast<double>(kAbsFloor));
    // Down fast, up slowly: quiet speech under the threshold must not drag
    // the floor up to its own level, while a background that keeps rising
    // is still followed (see AdaptsToRisingNoise in tests/voice).
    if (!speech) noise_ += (e - noise_) / (e < noise_ ? kFallDiv : kRiseDiv);

    if (!started_) {
        run_ = speech ? run_ + 1 : 0;
        if (run_ >= kStartFrames) {
            started_ = true;
            speech_ms_ = kStartFrames * kFrameMs;
        } else if (elapsed_ms_ >= kStartTimeoutMs) {
            final_ = VadVerdict::NoSpeech;
        }
        return final_;
    }

    if (speech) {
        speech_ms_ += kFrameMs;
        silence_ms_ = 0;
    } else {
        silence_ms_ += kFrameMs;
    }
    if (silence_ms_ >= kEndSilenceMs) final_ = verdict_after_speech();
    return final_;
}

VadVerdict Vad::finish() {
    if (final_ == VadVerdict::Continue) final_ = verdict_after_speech();
    return final_;
}

}  // namespace wisp::voice
