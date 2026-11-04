#pragma once

// The top of the self-check screen: the board name as a large title (it
// wraps, never clips), a dimmer line with the firmware version, the
// target and the boot time, and the two summary chips, "设备 n/m 正常"
// and "交互 k/3 已测". Tapping the device chip asks the owner to show the
// first device that is not OK.
//
// The info line is three labels in a wrapping row rather than one text, so
// a narrow screen moves a whole item ("启动 20 ms") to the next line
// instead of breaking it, or leaving a separator at a line's end.
//
// UI thread only. The chips only touch LVGL when their text or colour
// changes.

#include <cstdint>
#include <functional>
#include <string>

#include "lvgl.h"
#include "self_check_model.hpp"

namespace wisp::app {

class SelfCheckHeader {
public:
    // Appends the header to `col` (which owns the widgets); call again
    // after the column was rebuilt. `target` may be empty (not shown).
    // `on_devices_tap` runs on the UI thread when the device chip is
    // tapped.
    void build(lv_obj_t* col, const std::string& title, const std::string& version,
               const std::string& target, std::function<void()> on_devices_tap);

    void set_boot_ms(uint64_t boot_ms);
    void show_devices(const DeviceSummary& summary);
    void show_tests(int passed);

private:
    static void devices_chip_cb(lv_event_t* e);

    lv_obj_t* boot_ = nullptr;
    lv_obj_t* devices_chip_ = nullptr;
    lv_obj_t* devices_label_ = nullptr;
    lv_obj_t* tests_chip_ = nullptr;
    lv_obj_t* tests_label_ = nullptr;
    std::function<void()> on_devices_tap_;
};

}  // namespace wisp::app
