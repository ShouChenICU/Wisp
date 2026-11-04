#pragma once

// The self-check screen's plain readout cards: power and clock (battery,
// time), storage (internal flash, SD card) and performance (FPS, CPU,
// flush time; a long press toggles the LVGL performance overlay). The
// power and storage values sit in aligned "label  value" rows; a storage
// value is two items (online state, capacity) that wrap as a whole, so a
// narrow cell never splits "5.9 MB".
//
// UI thread only. update() only touches a label whose text changed.

#include <functional>

#include "lvgl.h"
#include "self_check_sampler.hpp"
#include "wisp/core/storage_manager.hpp"
#include "wisp/ui/ui_runtime.hpp"

namespace wisp::app {

class PowerClockSection {
public:
    // Appends the card to `col`; call again after the column was rebuilt.
    void build(lv_obj_t* col, bool has_power, bool has_clock);
    void update(const SampleSnapshot& snap);

private:
    bool has_power_ = false;
    bool has_clock_ = false;
    lv_obj_t* battery_ = nullptr;
    lv_obj_t* time_ = nullptr;
};

class StorageSection {
public:
    void build(lv_obj_t* col);
    void update(const core::StorageStatus& status);

private:
    struct Medium {
        lv_obj_t* state = nullptr;
        lv_obj_t* capacity = nullptr;
    };
    Medium add_medium(lv_obj_t* table, int row, const char* label);
    static void show(const Medium& medium, const core::MediaStatus* status);

    Medium internal_;
    Medium sd_;
};

class PerfSection {
public:
    // `on_long_press` runs on the UI thread.
    void build(lv_obj_t* col, std::function<void()> on_long_press);
    void update(const ui::UiPerf& perf);

private:
    static void long_press_cb(lv_event_t* e);

    lv_obj_t* label_ = nullptr;
    std::function<void()> on_long_press_;
};

}  // namespace wisp::app
