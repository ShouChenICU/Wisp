#include "net_section.hpp"

#include "wisp/core/log.hpp"
#include "wisp/os/os.hpp"
#include "wisp/ui/strings.hpp"

namespace wisp::app {

namespace {

constexpr const char* kTag = "net_ui";
// Between the status text and the signal strength, about two spaces.
constexpr int32_t kRssiGap = 10;

}  // namespace

std::string wifi_status_text(const net::WifiStatus& status, bool has_saved, uint64_t now_ms) {
    switch (status.state) {
        case net::WifiState::Off:
            return ui::tr(has_saved ? ui::StrId::NetOff : ui::StrId::NetNotSetUp);
        case net::WifiState::Connecting:
            return ui::tr(ui::StrId::NetConnecting);
        case net::WifiState::Backoff: {
            // Rounded up, so it reads 1 (not 0) in the last second.
            uint64_t left_ms = status.retry_at_ms > now_ms ? status.retry_at_ms - now_ms : 0;
            return ui::tr_fill(ui::StrId::NetRetrying, std::to_string((left_ms + 999) / 1000));
        }
        case net::WifiState::Connected:
            return std::string(ui::tr(ui::StrId::NetConnected)) + " " + status.ssid + "  " +
                   status.ip;
    }
    return {};
}

NetSection::NetSection(net::WifiService* wifi) : wifi_(wifi) {}

void NetSection::build(lv_obj_t* col, uint64_t now_ms) {
    lv_obj_t* row = lv_obj_create(col);
    lv_obj_remove_style_all(row);
    lv_obj_set_size(row, LV_PCT(100), LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW_WRAP);
    lv_obj_set_style_pad_column(row, kRssiGap, 0);
    lv_obj_remove_flag(row, LV_OBJ_FLAG_SCROLLABLE);
    // A content-sized label only wraps at its max width, which has to be
    // in pixels (a percentage is not applied while measuring the text).
    lv_obj_update_layout(row);
    const int32_t max_w = lv_obj_get_content_width(row);

    label_ = lv_label_create(row);
    lv_obj_set_style_max_width(label_, max_w, 0);
    lv_label_set_long_mode(label_, LV_LABEL_LONG_MODE_WRAP);
    rssi_label_ = lv_label_create(row);
    lv_obj_add_flag(rssi_label_, LV_OBJ_FLAG_HIDDEN);
    shown_.clear();
    shown_rssi_.clear();
    shown_button_ = -1;
    button_ = nullptr;
    button_label_ = nullptr;
    if (wifi_ != nullptr) {
        button_ = lv_button_create(col);
        lv_obj_add_event_cb(button_, &NetSection::button_cb, LV_EVENT_CLICKED, this);
        button_label_ = lv_label_create(button_);
    }
    refresh(now_ms);
}

void NetSection::refresh(uint64_t now_ms) {
    if (wifi_ != nullptr) {
        status_ = wifi_->status();
        has_saved_ = !wifi_->saved_ssids().empty();
        ap_active_ = wifi_->ap_status().active;
    }
    render(now_ms);
}

void NetSection::tick(uint64_t now_ms) {
    if (status_.state == net::WifiState::Backoff) render(now_ms);
}

void NetSection::render(uint64_t now_ms) {
    if (label_ == nullptr) return;
    std::string text =
        wifi_ == nullptr
            ? std::string(ui::tr(ui::StrId::NetNoHardware))
            : ui::tr_fill(ui::StrId::NetLine, wifi_status_text(status_, has_saved_, now_ms));
    if (text != shown_) {
        shown_ = text;
        lv_label_set_text(label_, text.c_str());
    }
    std::string rssi = wifi_ != nullptr && status_.state == net::WifiState::Connected
                           ? std::to_string(status_.rssi) + " dBm"
                           : std::string();
    if (rssi != shown_rssi_) {
        shown_rssi_ = rssi;
        lv_label_set_text(rssi_label_, rssi.c_str());
        if (rssi.empty()) {
            lv_obj_add_flag(rssi_label_, LV_OBJ_FLAG_HIDDEN);
        } else {
            lv_obj_remove_flag(rssi_label_, LV_OBJ_FLAG_HIDDEN);
        }
    }
    const int button_state = ap_active_ ? 1 : 0;
    if (button_label_ != nullptr && button_state != shown_button_) {
        shown_button_ = button_state;
        lv_label_set_text(button_label_,
                          ui::tr(ap_active_ ? ui::StrId::StopSetup : ui::StrId::StartSetup));
    }
}

void NetSection::button_cb(lv_event_t* e) {
    auto* self = static_cast<NetSection*>(lv_event_get_user_data(e));
    if (self->wifi_ == nullptr) return;
    if (self->ap_active_) {
        if (auto r = self->wifi_->stop_provisioning(); !r) {
            WLOG_W(kTag, "stopping the setup hotspot failed: %s", r.error().message.c_str());
        }
    } else if (auto r = self->wifi_->start_provisioning(os::now_ms()); !r) {
        WLOG_W(kTag, "starting the setup hotspot failed: %s", r.error().message.c_str());
    }
}

}  // namespace wisp::app
