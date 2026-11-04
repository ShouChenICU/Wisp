#pragma once

// The self-check screen's sensor card: the IMU's accelerometer and
// gyroscope, one line each, with every number in a fixed-width,
// right-aligned cell so the columns hold still while the values change,
// and the tilt ball to their right (below them when the card is too
// narrow). Without an IMU the card says so; a failed read shows above the
// numbers.
//
// UI thread only. update() only touches a widget whose value changed.

#include <cstdint>

#include "lvgl.h"
#include "self_check_sampler.hpp"

namespace wisp::app {

class SensorSection {
public:
    // Appends the card to `col` (which owns the widgets); call again after
    // the column was rebuilt.
    void build(lv_obj_t* col, bool has_imu);
    void update(const SampleSnapshot& snap);

private:
    bool has_imu_ = false;
    lv_obj_t* status_ = nullptr;
    // ax, ay, az, gx, gy, gz.
    lv_obj_t* values_[6] = {};
    lv_obj_t* ball_ = nullptr;
    int32_t ball_x_ = 0;
    int32_t ball_y_ = 0;
    // The grid's column widths; LVGL keeps a pointer to them.
    int32_t columns_[5] = {};
};

}  // namespace wisp::app
