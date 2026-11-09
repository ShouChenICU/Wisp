#include "wisp/ui/drag.hpp"

#include <algorithm>
#include <cstdlib>

namespace wisp::ui {

void FlingTrack::add(int32_t along, uint64_t now) {
    samples_[next_] = {now, along};
    next_ = (next_ + 1) % samples_.size();
    count_ = std::min(count_ + 1, samples_.size());
}

bool FlingTrack::fling() const {
    if (count_ == 0) return false;
    // The speed from the oldest point inside the window to the latest; a
    // window with no time in it (every point in one millisecond) counts
    // as one millisecond.
    const size_t n = samples_.size();
    const Sample& last = samples_[(next_ + n - 1) % n];
    Sample ref = last;
    for (size_t i = 0; i < count_; ++i) {
        const Sample& s = samples_[(next_ + n - count_ + i) % n];
        if (last.ms - s.ms <= kFlingWindowMs) {
            ref = s;
            break;
        }
    }
    const int64_t dt = std::max<int64_t>(static_cast<int64_t>(last.ms - ref.ms), 1);
    const int64_t moved = last.along - ref.along;
    return moved > 0 && moved * 1000 >= int64_t{kFlingPxPerS} * dt;
}

uint32_t slide_ms(int32_t left, int32_t extent) {
    if (extent <= 0) return 1;
    const int64_t ms = int64_t{kSlideMs} * std::abs(left) / extent;
    return static_cast<uint32_t>(std::clamp<int64_t>(ms, 1, kSlideMs));
}

}  // namespace wisp::ui
