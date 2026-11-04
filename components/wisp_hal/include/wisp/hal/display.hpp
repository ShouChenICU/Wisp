#pragma once

#include <cassert>
#include <cstdint>
#include <functional>
#include <utility>

#include "wisp/base/result.hpp"
#include "wisp/hal/device.hpp"

namespace wisp::hal {

// A closed pixel rectangle: [x1, x2] x [y1, y2], both ends inclusive.
struct Area {
    int32_t x1 = 0;
    int32_t y1 = 0;
    int32_t x2 = 0;
    int32_t y2 = 0;
};

// caps keys: "width" (int), "height" (int), "shape" (string, one of
// "rect"/"rounded_rect"/"circle"; see wisp/hal/display_geometry.hpp for
// how these -- plus "corner_radius" below for "rounded_rect" -- turn into
// a ScreenShape/safe area), "corner_radius" (int), "panel" (string),
// "brightness" (bool, true if set_brightness() is supported),
// "area_align" (int, flush()'s required alignment -- see
// wisp::ui::align_area()), "rgb565_be" (bool, optional, default false:
// true if flush() takes each RGB565 pixel high byte first in memory,
// i.e. already in the panel's wire order, so the caller renders in that
// order and nobody spends CPU swapping bytes).
//
// spec 5.3: every call on a Display must come from the UI thread (the
// one running UiRuntime's render loop) -- concurrent access from another
// thread is a protocol violation that, on real panel hardware, shows up
// as a torn/corrupted frame rather than a crash, so it's easy to miss
// without an explicit check. set_owner_thread_check() lets UiRuntime::
// start() install that check once it knows which thread owns the
// display; concrete implementations assert it (debug builds only) at the
// top of every method below via assert_owner_thread(). Before a check is
// installed -- no UiRuntime yet, or a display never attached to one, as
// in headless mode -- every call is allowed, since there is no UI thread
// to violate.
class Display : public Device {
public:
    static constexpr DeviceClass kClass = DeviceClass::Display;

    // Writes `rgb565` (area.x2 - area.x1 + 1) * (area.y2 - area.y1 + 1)
    // pixels, row-major, into `area`, in the byte order caps "rgb565_be"
    // declares. Blocks until the transfer has completed (the caller may
    // reuse/free the buffer as soon as this returns).
    virtual Result<void> flush(const Area& area, const uint16_t* rgb565) = 0;

    // Like flush(), but may return while the transfer is still running,
    // so the caller can render the next part of the frame meanwhile. The
    // caller must leave `rgb565` untouched and call wait_flush() before
    // reusing it and before the next flush()/flush_async(). The defaults
    // are for displays whose flush() is cheap or has nothing to wait for:
    // flush_async() is flush(), and wait_flush() has nothing to wait for.
    virtual Result<void> flush_async(const Area& area, const uint16_t* rgb565) {
        return flush(area, rgb565);
    }
    virtual Result<void> wait_flush() { return Result<void>::ok(); }

    virtual Result<void> set_brightness(uint8_t level) = 0;
    virtual Result<void> set_power(bool on) = 0;
    virtual bool powered() const = 0;

    void set_owner_thread_check(std::function<bool()> on_ui_thread) {
        owner_thread_check_ = std::move(on_ui_thread);
    }

protected:
    // Compiles out entirely when NDEBUG is defined. That is not the same
    // as "release" or "firmware": firmware/sdkconfig.defaults enables
    // CONFIG_COMPILER_OPTIMIZATION_ASSERTIONS_ENABLE, which does not
    // define NDEBUG, so this check stays compiled in (and active) on
    // real hardware too, catching a cross-thread call there with an
    // abort+reboot instead of a silently torn frame. Concrete
    // implementations call it at the top of flush()/set_brightness()/
    // set_power().
    void assert_owner_thread() const {
#ifndef NDEBUG
        assert(!owner_thread_check_ || owner_thread_check_());
#endif
    }

private:
    std::function<bool()> owner_thread_check_;
};

}  // namespace wisp::hal
