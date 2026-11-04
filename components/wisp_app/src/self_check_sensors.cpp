#include "self_check_sensors.hpp"

#include <algorithm>
#include <cstdio>
#include <string>

#include "self_check_layout.hpp"
#include "wisp/ui/strings.hpp"

namespace wisp::app {

namespace {

constexpr int32_t kBallTravelPx = 20;
constexpr int32_t kBallSize = 14;
constexpr int32_t kCellGap = 8;
constexpr int32_t kBallGap = 12;
constexpr uint32_t kBallColor = 0x4FC3F7;

const int32_t kGridRows[] = {LV_GRID_CONTENT, LV_GRID_CONTENT, LV_GRID_TEMPLATE_LAST};

// The widest value each column is sized for: accelerations to two
// decimals (in g), rotation rates to one (in °/s).
constexpr const char* kWidestValues[] = {"-00.00", "-000.0"};
constexpr const char* kFormats[] = {"%+.2f", "%+.1f"};

int32_t text_width(const char* text, const lv_font_t* font) {
    lv_point_t size;
    lv_text_get_size(&size, text, font, 0, 0, LV_COORD_MAX, LV_TEXT_FLAG_NONE);
    return size.x;
}

std::string imu_status(ui::StrId id) { return std::string("IMU: ") + ui::tr(id); }

}  // namespace

void SensorSection::build(lv_obj_t* col, bool has_imu) {
    has_imu_ = has_imu;
    lv_obj_t* card = make_card(col, ui::StrId::SectionSensors);

    // Empty and hidden while the IMU reads fine.
    status_ = make_wrap_label(card);
    if (!has_imu) {
        lv_label_set_text(status_, imu_status(ui::StrId::StateMissing).c_str());
        ball_ = nullptr;
        std::fill(std::begin(values_), std::end(values_), nullptr);
        return;
    }
    lv_label_set_text(status_, "");
    lv_obj_add_flag(status_, LV_OBJ_FLAG_HIDDEN);

    lv_obj_t* row = make_plain_row(card, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, kBallGap, 0);
    lv_obj_set_style_pad_row(row, kBallGap, 0);

    lv_obj_t* grid = lv_obj_create(row);
    lv_obj_remove_style_all(grid);
    lv_obj_set_size(grid, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_flex_grow(grid, 1);
    lv_obj_remove_flag(grid, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_set_style_pad_column(grid, kCellGap, 0);
    lv_obj_set_style_pad_row(grid, 4, 0);

    const lv_font_t* font = lv_obj_get_style_text_font(card, LV_PART_MAIN);
    int32_t cell = 0;
    for (const char* widest : kWidestValues) {
        cell = std::max(cell, text_width(widest, font));
    }
    columns_[0] = LV_GRID_CONTENT;
    columns_[1] = columns_[2] = columns_[3] = cell;
    columns_[4] = LV_GRID_TEMPLATE_LAST;
    lv_obj_set_grid_dsc_array(grid, columns_, kGridRows);

    const ui::StrId keys[] = {ui::StrId::Accel, ui::StrId::Gyro};
    for (int line = 0; line < 2; ++line) {
        lv_obj_t* key = lv_label_create(grid);
        lv_label_set_text(key, ui::tr(keys[line]));
        lv_obj_set_style_text_color(key, lv_color_hex(kColorDimText), 0);
        lv_obj_set_grid_cell(key, LV_GRID_ALIGN_START, 0, 1, LV_GRID_ALIGN_CENTER, line, 1);
        for (int axis = 0; axis < 3; ++axis) {
            lv_obj_t* value = lv_label_create(grid);
            lv_label_set_long_mode(value, LV_LABEL_LONG_MODE_CLIP);
            lv_obj_set_style_text_align(value, LV_TEXT_ALIGN_RIGHT, 0);
            lv_obj_set_grid_cell(value, LV_GRID_ALIGN_STRETCH, axis + 1, 1, LV_GRID_ALIGN_CENTER,
                                 line, 1);
            lv_label_set_text(value, "--");
            values_[line * 3 + axis] = value;
        }
    }

    lv_obj_t* frame = lv_obj_create(row);
    lv_obj_set_size(frame, 2 * kBallTravelPx + 20, 2 * kBallTravelPx + 20);
    lv_obj_set_style_radius(frame, LV_RADIUS_CIRCLE, 0);
    lv_obj_remove_flag(frame, LV_OBJ_FLAG_SCROLLABLE);

    ball_ = lv_obj_create(frame);
    lv_obj_set_size(ball_, kBallSize, kBallSize);
    lv_obj_set_style_radius(ball_, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_bg_color(ball_, lv_color_hex(kBallColor), 0);
    lv_obj_remove_flag(ball_, LV_OBJ_FLAG_SCROLLABLE);
    ball_x_ = 0;
    ball_y_ = 0;
    lv_obj_align(ball_, LV_ALIGN_CENTER, 0, 0);
}

void SensorSection::update(const SampleSnapshot& snap) {
    if (!has_imu_ || ball_ == nullptr) {
        return;
    }
    if (snap.imu_ok) {
        if (!lv_obj_has_flag(status_, LV_OBJ_FLAG_HIDDEN)) {
            lv_label_set_text(status_, "");
            lv_obj_add_flag(status_, LV_OBJ_FLAG_HIDDEN);
        }
        const float readings[] = {snap.imu.ax, snap.imu.ay, snap.imu.az,
                                  snap.imu.gx, snap.imu.gy, snap.imu.gz};
        for (int i = 0; i < 6; ++i) {
            char buf[16];
            std::snprintf(buf, sizeof(buf), kFormats[i / 3], static_cast<double>(readings[i]));
            set_text_if_changed(values_[i], buf);
        }
    } else {
        set_text_if_changed(status_, imu_status(ui::StrId::ReadFailed).c_str());
        lv_obj_remove_flag(status_, LV_OBJ_FLAG_HIDDEN);
        for (lv_obj_t* value : values_) {
            set_text_if_changed(value, "--");
        }
    }

    float ox = std::clamp(snap.imu.ax, -1.0f, 1.0f) * static_cast<float>(kBallTravelPx);
    float oy = std::clamp(-snap.imu.ay, -1.0f, 1.0f) * static_cast<float>(kBallTravelPx);
    auto x = static_cast<int32_t>(ox);
    auto y = static_cast<int32_t>(oy);
    if (x != ball_x_ || y != ball_y_) {
        ball_x_ = x;
        ball_y_ = y;
        lv_obj_align(ball_, LV_ALIGN_CENTER, x, y);
    }
}

}  // namespace wisp::app
