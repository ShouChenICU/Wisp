#pragma once

#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <string>
#include <string_view>

namespace wisp::agent {

// The token estimate's multiplier (agent-core spec 3.3): the model's reported prompt
// tokens over the bytes / 3 estimate of the last request it answered, kept across turns
// for one (profile id, model) pair. Keyed by the profile as well as the model because
// two providers serving a model of the same name need not count alike. wisp.agent only.
class Calibration {
public:
    static constexpr uint32_t kMinPct = 50;
    static constexpr uint32_t kMaxPct = 300;
    static constexpr uint32_t kOverflowPct = 150;

    // A turn on another profile or model starts again from 100.
    void begin_turn(std::string_view profile_id, std::string_view model) {
        if (profile_id == profile_id_ && model == model_) return;
        pct_ = 100;
        set_pair(profile_id, model);
    }

    // pct = clamp(prompt_tokens * 100 / max(1, est_tokens), kMinPct, kMaxPct); the pair
    // becomes this one.
    void observe(std::string_view profile_id, std::string_view model, uint32_t prompt_tokens,
                 size_t est_tokens) {
        const uint64_t pct = uint64_t{prompt_tokens} * 100 / std::max<size_t>(1, est_tokens);
        pct_ = static_cast<uint32_t>(std::clamp<uint64_t>(pct, kMinPct, kMaxPct));
        set_pair(profile_id, model);
    }

    // A context_length refusal: our estimate was short of the model's count, so trust
    // it less from now on. pct = max(pct, kOverflowPct); the pair becomes this one.
    void overflow(std::string_view profile_id, std::string_view model) {
        pct_ = std::max(pct_, kOverflowPct);
        set_pair(profile_id, model);
    }

    size_t scaled(size_t est_tokens) const { return est_tokens * pct_ / 100; }

    // The window in bytes / 3 tokens, for code that counts unscaled.
    uint32_t unscaled_window(uint32_t window) const {
        return static_cast<uint32_t>(uint64_t{window} * 100 / pct_);
    }

    uint32_t pct() const { return pct_; }

private:
    void set_pair(std::string_view profile_id, std::string_view model) {
        if (profile_id != profile_id_) profile_id_.assign(profile_id);
        if (model != model_) model_.assign(model);
    }

    uint32_t pct_ = 100;
    std::string profile_id_, model_;
};

}  // namespace wisp::agent
