#include "wisp/core/gesture.hpp"

#include <cmath>

namespace wisp::core {

bool ShakeDetector::feed(float x, float y, float z, uint64_t t_ms) {
    const bool above = std::sqrt(x * x + y * y + z * z) > kThreshold;
    const bool rising = above && !above_;
    above_ = above;
    // Edges inside the refractory period are not kept: they belong to the
    // shake that already fired.
    if (!rising || t_ms < quiet_until_) return false;
    if (edges_n_ == edges_.size()) {
        edges_[0] = edges_[1];
        edges_[1] = edges_[2];
        --edges_n_;
    }
    edges_[edges_n_++] = t_ms;
    if (edges_n_ < edges_.size() || t_ms - edges_[0] > kWindowMs) return false;
    edges_n_ = 0;
    quiet_until_ = t_ms + kRefractoryMs;
    return true;
}

bool FlipDetector::feed(float z, uint64_t t_ms) {
    const int face = z > kFaceThreshold ? 1 : (z < -kFaceThreshold ? -1 : 0);
    if (face == 0) {
        // Mid-turn or on edge: the hold starts again from the next reading
        // that has a face.
        candidate_ = 0;
        return false;
    }
    if (settled_ == 0) {
        // The face it starts on is where it lies, not a flip.
        settled_ = face;
        return false;
    }
    if (face == settled_) {
        candidate_ = 0;
        return false;
    }
    if (candidate_ != face) {
        candidate_ = face;
        candidate_since_ = t_ms;
    }
    if (t_ms - candidate_since_ < kStableMs) return false;
    settled_ = face;
    candidate_ = 0;
    return true;
}

}  // namespace wisp::core
