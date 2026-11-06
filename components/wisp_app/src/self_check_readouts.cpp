#include "self_check_readouts.hpp"

#include <cstdio>
#include <string>
#include <utility>

#include "self_check_layout.hpp"
#include "wisp/app/format.hpp"
#include "wisp/ui/strings.hpp"

namespace wisp::app {

namespace {

// "--" until the first sample when the device is there, "missing" (for
// good) when it is not.
void init_value(lv_obj_t* label, bool present, PageText& text) {
    if (present) {
        lv_label_set_text(label, "--");
    } else {
        text.bind(label, ui::StrId::StateMissing);
    }
}

std::string battery_text(const SampleSnapshot& snap, const PageText& text) {
    if (!snap.power_ok) {
        return text.tr(ui::StrId::ReadFailed);
    }
    std::string value;
    if (snap.power.battery_percent < 0) {
        value = text.tr(ui::StrId::LevelUnknown);
    } else {
        value = std::to_string(snap.power.battery_percent) + "%";
    }
    value += "  ";
    value += text.tr(snap.power.charging ? ui::StrId::Charging : ui::StrId::NotCharging);
    return value;
}

constexpr int32_t kItemGap = 8;

// Whole kilobytes, rounded: a diagnostic row, not a precise accounting
// (matches the granularity of the `mem` console command's figures).
std::string format_kb(int64_t bytes) {
    int64_t kb = bytes > 0 ? (bytes + 512) / 1024 : 0;
    return std::to_string(kb) + " KB";
}

// Megabytes with one decimal place, PSRAM being large enough that whole
// KB would be noise.
std::string format_mb(int64_t bytes) {
    double mb = bytes > 0 ? static_cast<double>(bytes) / (1024.0 * 1024.0) : 0.0;
    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f", mb);
    return buf;
}

}  // namespace

void PowerClockSection::build(lv_obj_t* col, PageText& text, bool has_power, bool has_clock) {
    text_ = &text;
    has_power_ = has_power;
    has_clock_ = has_clock;
    lv_obj_t* card = make_card(col, ui::StrId::SectionPowerClock, text);
    lv_obj_t* table = make_table(card, 3);
    battery_ = add_table_row(table, 0, ui::StrId::Battery, text);
    init_value(battery_, has_power, text);
    time_ = add_table_row(table, 1, ui::StrId::Time, text);
    init_value(time_, has_clock, text);
    sync_ = add_table_row(table, 2, ui::StrId::TimeSync, text);
    show_sync(std::nullopt);
}

void PowerClockSection::show_sync(std::optional<int64_t> local_unix) {
    if (sync_ == nullptr) return;
    if (!local_unix) {
        set_text_if_changed(sync_, text_->tr(ui::StrId::TimeNotSynced));
        return;
    }
    set_text_if_changed(sync_,
                        text_->tr_fill(ui::StrId::TimeSynced, format_hhmm(*local_unix)).c_str());
}

void PowerClockSection::update(const SampleSnapshot& snap) {
    if (has_power_ && battery_ != nullptr) {
        set_text_if_changed(battery_, battery_text(snap, *text_).c_str());
    }
    if (has_clock_ && time_ != nullptr) {
        std::string value = snap.rtc_ok ? format_time(snap.rtc_unix)
                                        : std::string(text_->tr(ui::StrId::ReadFailed));
        set_text_if_changed(time_, value.c_str());
    }
}

void StorageSection::build(lv_obj_t* col, PageText& text) {
    text_ = &text;
    lv_obj_t* card = make_card(col, ui::StrId::SectionStorage, text);
    lv_obj_t* table = make_table(card, 2);
    internal_ = add_medium(table, 0, ui::StrId::InternalFlash, text);
    sd_ = add_medium(table, 1, ui::StrId::SdCard, text);
}

StorageSection::Medium StorageSection::add_medium(lv_obj_t* table, int row, ui::StrId label,
                                                  PageText& text) {
    add_table_key(table, row, label, text);
    lv_obj_t* items = make_plain_row(table, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(items, kItemGap, 0);
    lv_obj_set_grid_cell(items, LV_GRID_ALIGN_STRETCH, 1, 1, LV_GRID_ALIGN_START, row, 1);
    lv_obj_t* state = lv_label_create(items);
    lv_label_set_text(state, "--");
    lv_obj_t* capacity = lv_label_create(items);
    lv_label_set_text(capacity, "");
    return Medium{state, capacity};
}

void StorageSection::show(const Medium& medium, const core::MediaStatus* status) const {
    if (status == nullptr) {
        set_text_if_changed(medium.state, text_->tr(ui::StrId::StateMissing));
        set_text_if_changed(medium.capacity, "");
        return;
    }
    uint64_t used =
        status->total_bytes > status->free_bytes ? status->total_bytes - status->free_bytes : 0;
    set_text_if_changed(medium.state,
                        text_->tr(status->online ? ui::StrId::Online : ui::StrId::Offline));
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

void PerfSection::build(lv_obj_t* col, PageText& text, std::function<void()> on_long_press) {
    text_ = &text;
    on_long_press_ = std::move(on_long_press);
    lv_obj_t* card = make_card(col, ui::StrId::SectionPerf, text);
    label_ = make_wrap_label(card);
    lv_label_set_text(label_, "FPS 0 | CPU 0% | flush 0.0ms");
    lv_obj_add_flag(label_, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_add_event_cb(label_, &PerfSection::long_press_cb, LV_EVENT_LONG_PRESSED, this);

    lv_obj_t* table = make_table(card, 2);
    ram_ = add_table_row(table, 0, ui::StrId::PerfInternalRam, text);
    lv_label_set_text(ram_, "--");
    psram_ = add_table_row(table, 1, ui::StrId::PerfPsram, text);
    lv_label_set_text(psram_, "--");
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

void PerfSection::update_ram(int64_t free, int64_t largest, int64_t psram_free,
                             int64_t psram_total) {
    if (ram_ != nullptr) {
        std::string value;
        if (free < 0) {
            // No heap_info at all (the simulator doesn't report this
            // today) -- same bare "-" as a boot-report row with nothing
            // to show (self_check_screen.cpp's build_devices()).
            value = "-";
        } else {
            value = std::string(text_->tr(ui::StrId::PerfFree)) + " " + format_kb(free);
            if (largest >= 0) {
                value += " | ";
                value += text_->tr(ui::StrId::PerfLargestBlock);
                value += " " + format_kb(largest);
            }
        }
        set_text_if_changed(ram_, value.c_str());
    }
    if (psram_ != nullptr) {
        std::string value;
        if (psram_total <= 0) {
            value = "-";
        } else {
            // Each figure follows its own label so neither reads as the
            // other ("free 3.9 MB / total 8.0 MB").
            value = std::string(text_->tr(ui::StrId::PerfFree)) + " " + format_mb(psram_free) +
                    " MB / " + text_->tr(ui::StrId::PerfTotal) + " " + format_mb(psram_total) +
                    " MB";
        }
        set_text_if_changed(psram_, value.c_str());
    }
}

void PerfSection::long_press_cb(lv_event_t* e) {
    auto* self = static_cast<PerfSection*>(lv_event_get_user_data(e));
    if (self->on_long_press_) {
        self->on_long_press_();
    }
}

}  // namespace wisp::app
