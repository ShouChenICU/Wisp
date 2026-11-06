#pragma once

// Gesture detectors over accelerometer samples (scene-device spec 6.1; the
// thresholds are the plan's ruling on it). Portable and clock-free: each
// feed() takes the sample's own time, so the host tests drive them with
// made-up timelines. Units: m/s² and milliseconds.
//
//   shake  every rising edge of the acceleration's magnitude across
//          kThreshold (about 2 g) counts; three within kWindowMs fire
//          once, and then nothing fires for kRefractoryMs.
//   flip   z above +kFaceThreshold is face up, below -kFaceThreshold face
//          down. Once the device has settled on one face, the other face
//          held for kStableMs fires once and becomes the settled face.
//          Readings in between (on edge, mid-turn) belong to neither and
//          restart the hold.
//
// Not thread-safe: one detector is fed from one thread.

#include <array>
#include <cstddef>
#include <cstdint>

namespace wisp::core {

class ShakeDetector {
public:
    static constexpr float kThreshold = 20.0f;
    static constexpr uint64_t kWindowMs = 500, kRefractoryMs = 1000;

    // True once per shake.
    bool feed(float x, float y, float z, uint64_t t_ms);

private:
    bool above_ = false;
    // The last rising edges, oldest first; `edges_n_` of them are valid.
    std::array<uint64_t, 3> edges_{};
    size_t edges_n_ = 0;
    // Nothing fires before this time (the refractory period).
    uint64_t quiet_until_ = 0;
};

class FlipDetector {
public:
    static constexpr float kFaceThreshold = 7.0f;
    static constexpr uint64_t kStableMs = 300;

    // True once per flip.
    bool feed(float z, uint64_t t_ms);

private:
    // +1 face up, -1 face down, 0 not known yet / none.
    int settled_ = 0;
    int candidate_ = 0;
    uint64_t candidate_since_ = 0;
};

}  // namespace wisp::core
