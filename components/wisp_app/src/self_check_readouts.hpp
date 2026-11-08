#pragma once

// The self-check screen's plain readout cards: power and clock (battery,
// time, the last network time sync), storage (internal flash, SD card)
// and performance (FPS, CPU, flush time; a long press toggles the LVGL
// performance overlay). The power and storage values sit in aligned
// "label  value" rows; a storage value is two items (online state,
// capacity) that wrap as a whole, so a narrow cell never splits "5.9 MB".
//
// UI thread only. update() only touches a label whose text changed.

#include <cstdint>
#include <functional>
#include <optional>

#include "lvgl.h"
#include "self_check_layout.hpp"
#include "self_check_sampler.hpp"
#include "wisp/core/storage_manager.hpp"
#include "wisp/ui/ui_runtime.hpp"

namespace wisp::app {

class PowerClockSection {
public:
    // Appends the card to `col`. `text` must outlive the widgets.
    void build(lv_obj_t* col, PageText& text, bool has_power, bool has_clock);
    void update(const SampleSnapshot& snap);
    // The time sync row: "Synced HH:MM" for the local time of the last
    // sync, `local_unix` (UTC seconds plus the time zone), in the Settings
    // time format (`h24`; ui::format_clock()), or "Not synced" for
    // nullopt. In the page language; the caller re-shows it after a
    // switch.
    void show_sync(std::optional<int64_t> local_unix, bool h24 = true);

private:
    const PageText* text_ = nullptr;
    bool has_power_ = false;
    bool has_clock_ = false;
    lv_obj_t* battery_ = nullptr;
    lv_obj_t* time_ = nullptr;
    lv_obj_t* sync_ = nullptr;
};

class StorageSection {
public:
    void build(lv_obj_t* col, PageText& text);
    void update(const core::StorageStatus& status);

private:
    struct Medium {
        lv_obj_t* state = nullptr;
        lv_obj_t* capacity = nullptr;
    };
    Medium add_medium(lv_obj_t* table, int row, ui::StrId label, PageText& text);
    void show(const Medium& medium, const core::MediaStatus* status) const;

    const PageText* text_ = nullptr;

    Medium internal_;
    Medium sd_;
};

class PerfSection {
public:
    // `on_long_press` runs on the UI thread.
    void build(lv_obj_t* col, PageText& text, std::function<void()> on_long_press);
    void update(const ui::UiPerf& perf);
    // Internal RAM (`free`/`largest`, -1 if unknown) and PSRAM (`psram_free`/
    // `psram_total`, bytes). `psram_total` <= 0 shows "-": the platform
    // either has no PSRAM or doesn't report one. Same refresh cadence as
    // update() (SelfCheck::refresh_ui()).
    void update_ram(int64_t free, int64_t largest, int64_t total, int64_t psram_free,
                    int64_t psram_total);

private:
    static void long_press_cb(lv_event_t* e);

    const PageText* text_ = nullptr;
    lv_obj_t* label_ = nullptr;
    lv_obj_t* ram_ = nullptr;
    lv_obj_t* psram_ = nullptr;
    std::function<void()> on_long_press_;
};

}  // namespace wisp::app
