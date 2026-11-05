#include "self_check_tests.hpp"

#include <cstdio>
#include <utility>

#include "self_check_layout.hpp"
#include "wisp/ui/strings.hpp"

namespace wisp::app {

namespace {

constexpr int32_t kTouchAreaHeight = 60;
constexpr int32_t kRowGap = 8;

// A row with `content` growing to fill it and a status label at its end,
// both centred vertically. Returns the status label.
lv_obj_t* add_status(lv_obj_t* row, lv_obj_t* content) {
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, kRowGap, 0);
    lv_obj_set_flex_grow(content, 1);
    lv_obj_t* status = lv_label_create(row);
    lv_label_set_text(status, "");
    return status;
}

}  // namespace

std::string TestSection::button_text() const {
    return std::string(text_->tr(ui::StrId::Button)) + ": " + button_topic_;
}

void TestSection::build(lv_obj_t* col, PageText& text, Callbacks callbacks,
                        const LoopbackStatus& loop_status) {
    text_ = &text;
    callbacks_ = std::move(callbacks);
    lv_obj_t* card = make_card(col, ui::StrId::SectionTests, text);

    // Touch: a name and status line, then the area itself.
    lv_obj_t* touch_row = make_plain_row(card, LV_FLEX_FLOW_ROW);
    lv_obj_t* touch_name = lv_label_create(touch_row);
    lv_label_set_long_mode(touch_name, LV_LABEL_LONG_MODE_WRAP);
    text.bind(touch_name, ui::StrId::TouchTest);
    status_[static_cast<int>(InteractiveTest::Touch)] = add_status(touch_row, touch_name);

    lv_obj_t* box = lv_obj_create(card);
    lv_obj_set_width(box, LV_PCT(100));
    lv_obj_set_height(box, kTouchAreaHeight);
    lv_obj_add_flag(box, LV_OBJ_FLAG_CLICKABLE);
    lv_obj_remove_flag(box, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_event_cb(box, &TestSection::touch_cb, LV_EVENT_PRESSED, this);
    lv_obj_add_event_cb(box, &TestSection::touch_cb, LV_EVENT_PRESSING, this);
    lv_obj_add_event_cb(box, &TestSection::touch_cb, LV_EVENT_RELEASED, this);
    touch_label_ = lv_label_create(box);
    lv_obj_center(touch_label_);
    text.bind(touch_label_, ui::StrId::TouchTestArea);

    // Buttons: the last event and the status on one line.
    lv_obj_t* button_row = make_plain_row(card, LV_FLEX_FLOW_ROW);
    button_label_ = lv_label_create(button_row);
    lv_label_set_long_mode(button_label_, LV_LABEL_LONG_MODE_WRAP);
    lv_label_set_text(button_label_, button_text().c_str());
    status_[static_cast<int>(InteractiveTest::Button)] = add_status(button_row, button_label_);

    // Loopback: the button, its progress and the status.
    lv_obj_t* loop_row = make_plain_row(card, LV_FLEX_FLOW_ROW);
    lv_obj_t* btn = lv_button_create(loop_row);
    lv_obj_add_event_cb(btn, &TestSection::loopback_cb, LV_EVENT_CLICKED, this);
    text.bind(lv_label_create(btn), ui::StrId::LoopbackButton);
    loop_label_ = lv_label_create(loop_row);
    lv_label_set_long_mode(loop_label_, LV_LABEL_LONG_MODE_WRAP);
    lv_label_set_text(loop_label_, loopback_status_text(loop_status, text.lang()).c_str());
    status_[static_cast<int>(InteractiveTest::Loopback)] = add_status(loop_row, loop_label_);
}

void TestSection::show(const InteractiveTests& tests) {
    const InteractiveTest all[] = {InteractiveTest::Touch, InteractiveTest::Button,
                                   InteractiveTest::Loopback};
    for (InteractiveTest test : all) {
        lv_obj_t* status = status_[static_cast<int>(test)];
        if (status == nullptr) {
            continue;
        }
        if (tests.passed(test)) {
            std::string text = std::string(LV_SYMBOL_OK " ") + text_->tr(ui::StrId::TestPassed);
            set_text_if_changed(status, text.c_str());
            set_text_color_if_changed(status, kColorGood);
        } else {
            set_text_if_changed(status, text_->tr(ui::StrId::TestUntested));
            set_text_color_if_changed(status, kColorDimText);
        }
    }
}

void TestSection::set_button_event(const std::string& topic) {
    button_topic_ = topic;
    retext();
}

void TestSection::retext() {
    if (button_label_ != nullptr) {
        set_text_if_changed(button_label_, button_text().c_str());
    }
}

void TestSection::set_loopback(const LoopbackStatus& status) {
    if (loop_label_ != nullptr) {
        set_text_if_changed(loop_label_, loopback_status_text(status, text_->lang()).c_str());
    }
}

void TestSection::touch_cb(lv_event_t* e) {
    auto* self = static_cast<TestSection*>(lv_event_get_user_data(e));
    if (lv_event_get_code(e) == LV_EVENT_RELEASED) {
        lv_label_set_text(self->touch_label_, self->text_->tr(ui::StrId::TouchTestArea));
        return;
    }
    lv_indev_t* indev = lv_indev_active();
    lv_point_t p{};
    if (indev != nullptr) {
        lv_indev_get_point(indev, &p);
    }
    char buf[64];
    std::snprintf(buf, sizeof(buf), "%s: (%d, %d)", self->text_->tr(ui::StrId::Touch),
                  static_cast<int>(p.x), static_cast<int>(p.y));
    lv_label_set_text(self->touch_label_, buf);
    if (lv_event_get_code(e) == LV_EVENT_PRESSED && self->callbacks_.on_touch) {
        self->callbacks_.on_touch();
    }
}

void TestSection::loopback_cb(lv_event_t* e) {
    auto* self = static_cast<TestSection*>(lv_event_get_user_data(e));
    if (self->callbacks_.on_loopback) {
        self->callbacks_.on_loopback();
    }
}

}  // namespace wisp::app
