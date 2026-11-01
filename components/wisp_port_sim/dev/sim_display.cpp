#include <algorithm>
#include <cstring>
#include <utility>

#include "wisp/port/sim/sim_board.hpp"

namespace wisp::port::sim {

SimDisplay::SimDisplay(std::string id, int32_t width, int32_t height, std::string shape,
                       int32_t corner_radius)
    : width_(width),
      height_(height),
      framebuffer_(static_cast<size_t>(width) * static_cast<size_t>(height), 0) {
    info_.id = std::move(id);
    info_.cls = kClass;
    info_.model = "sdl-display";
    info_.caps = wisp::Properties{
        {"width", width},
        {"height", height},
        {"shape", wisp::Value(std::move(shape))},
        {"corner_radius", corner_radius},
        {"panel", wisp::Value("amoled")},
        {"brightness", true},
        {"area_align", 2},
    };
}

wisp::Result<void> SimDisplay::flush(const wisp::hal::Area& area, const uint16_t* rgb565) {
    std::lock_guard<std::mutex> lock(mutex_);
    int32_t x1 = std::max<int32_t>(0, area.x1);
    int32_t x2 = std::min<int32_t>(width_ - 1, area.x2);
    int32_t y1 = std::max<int32_t>(0, area.y1);
    int32_t y2 = std::min<int32_t>(height_ - 1, area.y2);
    int32_t area_w = area.x2 - area.x1 + 1;
    for (int32_t y = y1; y <= y2; ++y) {
        const uint16_t* src_row = rgb565 +
                                  static_cast<size_t>(y - area.y1) * static_cast<size_t>(area_w) +
                                  static_cast<size_t>(x1 - area.x1);
        uint16_t* dst_row = framebuffer_.data() +
                            static_cast<size_t>(y) * static_cast<size_t>(width_) +
                            static_cast<size_t>(x1);
        std::memcpy(dst_row, src_row, static_cast<size_t>(x2 - x1 + 1) * sizeof(uint16_t));
    }
    return wisp::Result<void>::ok();
}

wisp::Result<void> SimDisplay::set_brightness(uint8_t level) {
    brightness_.store(level);
    return wisp::Result<void>::ok();
}

wisp::Result<void> SimDisplay::set_power(bool on) {
    powered_.store(on);
    return wisp::Result<void>::ok();
}

bool SimDisplay::powered() const { return powered_.load(); }

void SimDisplay::copy_framebuffer(uint16_t* out) const {
    std::lock_guard<std::mutex> lock(mutex_);
    std::memcpy(out, framebuffer_.data(), framebuffer_.size() * sizeof(uint16_t));
}

}  // namespace wisp::port::sim
