#include "self_check_header.hpp"

#include <utility>

#include "self_check_layout.hpp"
#include "wisp/ui/strings.hpp"
#include "wisp/ui/ui_runtime.hpp"

namespace wisp::app {

namespace {

constexpr int32_t kChipPadHor = 10;
constexpr int32_t kChipPadVer = 4;
constexpr int32_t kChipGap = 8;
constexpr int32_t kInfoGap = 12;
constexpr uint32_t kChipTextLight = 0xFFFFFF;
constexpr uint32_t kChipTextDark = 0x212121;

// Montserrat 24 for the title, falling back to the UI font for anything
// it lacks (a board name is normally ASCII, but nothing guarantees it).
const lv_font_t* title_font() {
    static const lv_font_t font = [] {
        lv_font_t f = lv_font_montserrat_24;
        f.fallback = ui::default_font();
        return f;
    }();
    return &font;
}

lv_obj_t* make_chip(lv_obj_t* parent, lv_obj_t** label) {
    lv_obj_t* chip = lv_obj_create(parent);
    lv_obj_remove_style_all(chip);
    lv_obj_set_size(chip, LV_SIZE_CONTENT, LV_SIZE_CONTENT);
    lv_obj_set_style_bg_opa(chip, LV_OPA_COVER, 0);
    lv_obj_set_style_radius(chip, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_style_pad_hor(chip, kChipPadHor, 0);
    lv_obj_set_style_pad_ver(chip, kChipPadVer, 0);
    lv_obj_remove_flag(chip, LV_OBJ_FLAG_SCROLLABLE);
    *label = lv_label_create(chip);
    return chip;
}

// Colours `chip` for `level`: white text on the strong colours, dark text
// on yellow and on the neutral gray.
void paint_chip(lv_obj_t* chip, lv_obj_t* label, SummaryLevel level) {
    set_bg_color_if_changed(chip, level_color(level));
    bool light_text = level == SummaryLevel::Good || level == SummaryLevel::Error;
    set_text_color_if_changed(label, light_text ? kChipTextLight : kChipTextDark);
}

std::string fraction(int part, int whole) {
    return std::to_string(part) + "/" + std::to_string(whole);
}

}  // namespace

void SelfCheckHeader::build(lv_obj_t* col, const std::string& title, const std::string& version,
                            const std::string& target, std::function<void()> on_devices_tap) {
    on_devices_tap_ = std::move(on_devices_tap);

    lv_obj_t* title_label = make_wrap_label(col);
    lv_obj_set_style_text_font(title_label, title_font(), 0);
    lv_label_set_text(title_label, title.c_str());

    lv_obj_t* info = make_plain_row(col, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(info, kInfoGap, 0);
    lv_obj_set_style_text_color(info, lv_color_hex(kColorDimText), 0);
    // A content-sized label only wraps at a max width in pixels.
    lv_obj_update_layout(info);
    const int32_t max_w = lv_obj_get_content_width(info);
    auto add_item = [&](const std::string& text) {
        lv_obj_t* label = lv_label_create(info);
        lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
        lv_obj_set_style_max_width(label, max_w, 0);
        lv_label_set_text(label, text.c_str());
        return label;
    };
    add_item("v" + version);
    if (!target.empty()) {
        add_item(target);
    }
    boot_ = add_item("");

    lv_obj_t* chips = make_plain_row(col, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(chips, kChipGap, 0);
    lv_obj_set_style_pad_row(chips, kChipGap, 0);
    devices_chip_ = make_chip(chips, &devices_label_);
    lv_obj_add_event_cb(devices_chip_, &SelfCheckHeader::devices_chip_cb, LV_EVENT_CLICKED, this);
    tests_chip_ = make_chip(chips, &tests_label_);
}

void SelfCheckHeader::set_boot_ms(uint64_t boot_ms) {
    if (boot_ != nullptr) {
        std::string text =
            std::string(ui::tr(ui::StrId::BootTime)) + " " + std::to_string(boot_ms) + " ms";
        set_text_if_changed(boot_, text.c_str());
    }
}

void SelfCheckHeader::show_devices(const DeviceSummary& summary) {
    if (devices_label_ == nullptr) {
        return;
    }
    set_text_if_changed(
        devices_label_,
        ui::tr_fill(ui::StrId::SummaryDevices, fraction(summary.ok, summary.total)).c_str());
    paint_chip(devices_chip_, devices_label_, summary.level);
}

void SelfCheckHeader::show_tests(int passed) {
    if (tests_label_ == nullptr) {
        return;
    }
    set_text_if_changed(
        tests_label_,
        ui::tr_fill(ui::StrId::SummaryTests, fraction(passed, kInteractiveTestCount)).c_str());
    paint_chip(tests_chip_, tests_label_,
               passed == kInteractiveTestCount ? SummaryLevel::Good : SummaryLevel::Neutral);
}

void SelfCheckHeader::devices_chip_cb(lv_event_t* e) {
    auto* self = static_cast<SelfCheckHeader*>(lv_event_get_user_data(e));
    if (self->on_devices_tap_) {
        self->on_devices_tap_();
    }
}

}  // namespace wisp::app
