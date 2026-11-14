// Legacy scene API geometry; see scene-device spec 5.5.

#include "wisp/ui/system_layer.hpp"

namespace wisp::ui {

hal::Area SystemLayer::status_area(const hal::ScreenShape& s) {
    const hal::Area safe = hal::safe_rect(s);
    const int32_t x1 = (s.width - kCapsuleW) / 2;
    const int32_t y1 = safe.y1 + kCapsuleTopGap;
    return {x1, y1, x1 + kCapsuleW - 1, y1 + kCapsuleH - 1};
}

}  // namespace wisp::ui
