#include "self_check_readouts.hpp"

#include <cstdio>
#include <string>
#include <utility>

#include "self_check_layout.hpp"
#include "wisp/app/format.hpp"
#include "wisp/ui/strings.hpp"

namespace wisp::app {

namespace {

const char* missing_or_pending(bool present) {
    return present ? "--" : ui::tr(ui::StrId::StateMissing);
}

std::string battery_text(const SampleSnapshot& snap) {
    if (!snap.power_ok) {
        return ui::tr(ui::StrId::ReadFailed);
    }
    std::string value;
    if (snap.power.battery_percent < 0) {
        value = ui::tr(ui::StrId::LevelUnknown);
    } else {
        value = std::to_string(snap.power.battery_percent) + "%";
    }
    value += "  ";
    value += ui::tr(snap.power.charging ? ui::StrId::Charging : ui::StrId::NotCharging);
    return value;
}

constexpr int32_t kItemGap = 8;

}  // namespace

void PowerClockSection::build(lv_obj_t* col, bool has_power, bool has_clock) {
    has_power_ = has_power;
    has_clock_ = has_clock;
    lv_obj_t* card = make_card(col, ui::StrId::SectionPowerClock);
    lv_obj_t* table = make_table(card, 2);
    battery_ = add_table_row(table, 0, ui::tr(ui::StrId::Battery));
    lv_label_set_text(battery_, missing_or_pending(has_power));
    time_ = add_table_row(table, 1, ui::tr(ui::StrId::Time));
    lv_label_set_text(time_, missing_or_pending(has_clock));
}

void PowerClockSection::update(const SampleSnapshot& snap) {
    if (has_power_ && battery_ != nullptr) {
        set_text_if_changed(battery_, battery_text(snap).c_str());
    }
    if (has_clock_ && time_ != nullptr) {
        std::string value =
            snap.rtc_ok ? format_time(snap.rtc_unix) : std::string(ui::tr(ui::StrId::ReadFailed));
        set_text_if_changed(time_, value.c_str());
    }
}

void StorageSection::build(lv_obj_t* col) {
    lv_obj_t* card = make_card(col, ui::StrId::SectionStorage);
    lv_obj_t* table = make_table(card, 2);
    internal_ = add_medium(table, 0, ui::tr(ui::StrId::InternalFlash));
    sd_ = add_medium(table, 1, ui::tr(ui::StrId::SdCard));
}

StorageSection::Medium StorageSection::add_medium(lv_obj_t* table, int row, const char* label) {
    add_table_key(table, row, label);
    lv_obj_t* items = make_plain_row(table, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(items, kItemGap, 0);
    lv_obj_set_grid_cell(items, LV_GRID_ALIGN_STRETCH, 1, 1, LV_GRID_ALIGN_START, row, 1);
    lv_obj_t* state = lv_label_create(items);
    lv_label_set_text(state, "--");
    lv_obj_t* capacity = lv_label_create(items);
    lv_label_set_text(capacity, "");
    return Medium{state, capacity};
}

void StorageSection::show(const Medium& medium, const core::MediaStatus* status) {
    if (status == nullptr) {
        set_text_if_changed(medium.state, ui::tr(ui::StrId::StateMissing));
        set_text_if_changed(medium.capacity, "");
        return;
    }
    uint64_t used =
        status->total_bytes > status->free_bytes ? status->total_bytes - status->free_bytes : 0;
    set_text_if_changed(medium.state,
                        ui::tr(status->online ? ui::StrId::Online : ui::StrId::Offline));
    set_text_if_changed(medium.capacity,
                        (format_bytes(used) + " / " + format_bytes(status->total_bytes)).c_str());
}

void StorageSection::update(const core::StorageStatus& status) {
    if (internal_.state == nullptr) {
        return;
    }
    const core::MediaStatus* internal = nullptr;
    const core::MediaStatus* sd = nullptr;
    for (const auto& media : status.media) {
        if (media.id == "storage.int") {
            internal = &media;
        } else if (media.id == "storage.sd") {
            sd = &media;
        }
    }
    show(internal_, internal);
    show(sd_, sd);
}

void PerfSection::build(lv_obj_t* col, std::function<void()> on_long_press) {
    on_long_press_ = std::move(on_long_press);
    lv_obj_t* card = make_card(col, ui::StrId::SectionPerf);
    label_ = make_wrap_label(card);
    lv_label_set_text(label_, "FPS 0 | CPU 0% | flush 0.0ms");
    lv_obj_add_flag(label_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(label_, &PerfSection::long_press_cb, LV_EVENT_LONG_PRESSED, this);
}

void PerfSection::update(const ui::UiPerf& perf) {
    if (label_ == nullptr) {
        return;
    }
    char buf[96];
    std::snprintf(buf, sizeof(buf), "FPS %.0f | CPU %u%% | flush %.1fms",
                  static_cast<double>(perf.fps), static_cast<unsigned>(perf.cpu_percent),
                  static_cast<double>(perf.avg_flush_ms));
    set_text_if_changed(label_, buf);
}

void PerfSection::long_press_cb(lv_event_t* e) {
    auto* self = static_cast<PerfSection*>(lv_event_get_user_data(e));
    if (self->on_long_press_) {
        self->on_long_press_();
    }
}

}  // namespace wisp::app
