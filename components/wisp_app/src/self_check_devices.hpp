#pragma once

// The self-check screen's device card: one row per boot-report entry, a
// status dot (green OK, yellow degraded, red failed, gray missing), the id
// and the driver name, dimmer. Rows that are not OK come first and carry
// their state and the boot report's reason on a second line.
//
// A row's state can change after boot (the SD card going in or out, see
// set_state()); the row then moves to its new place.
//
// UI thread only.

#include <string>
#include <vector>

#include "lvgl.h"
#include "self_check_model.hpp"
#include "wisp/core/boot_report.hpp"

namespace wisp::app {

struct DeviceRowInfo {
    std::string id;
    std::string model;  // "-" when the device is not in the registry
    core::DeviceState state = core::DeviceState::Ok;
    std::string reason;
};

class DeviceSection {
public:
    // Appends the card to `col` (which owns the widgets), with `devices`
    // in boot-report order; call again after the column was rebuilt.
    void build(lv_obj_t* col, std::vector<DeviceRowInfo> devices);

    // Shows the device `id` in `state` from now on. Returns true if that
    // changed its state (and so, maybe, the summary).
    bool set_state(const std::string& id, core::DeviceState state);

    DeviceSummary summary() const;

    // Scrolls the page (animated) until the first row that is not OK is
    // in view. Returns false, doing nothing, when every device is OK.
    bool scroll_to_first_problem();

private:
    struct Row {
        DeviceRowInfo boot;  // as the boot report had it
        core::DeviceState state;
        lv_obj_t* row = nullptr;
        lv_obj_t* dot = nullptr;
        lv_obj_t* reason = nullptr;
    };

    void build_row(Row& row, int32_t text_width);
    // Dot colour and second line for the row's current state.
    void render(Row& row);
    // Puts the rows into display order (device_display_order()).
    void reorder();
    std::vector<core::DeviceState> states() const;

    lv_obj_t* card_ = nullptr;
    // In boot-report order; the widgets are in display order.
    std::vector<Row> rows_;
};

}  // namespace wisp::app
