#include "net_section.hpp"

#include "wisp/core/log.hpp"
#include "wisp/os/os.hpp"
#include "wisp/ui/strings.hpp"

namespace wisp::app {

namespace {

constexpr const char* kTag = "net_ui";

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
                   status.ip + "  " + std::to_string(status.rssi) + " dBm";
    }
    return {};
}

NetSection::NetSection(net::WifiService* wifi) : wifi_(wifi) {}

void NetSection::build(lv_obj_t* col, uint64_t now_ms) {
    label_ = lv_label_create(col);
    lv_obj_set_width(label_, LV_PCT(100));
    lv_label_set_long_mode(label_, LV_LABEL_LONG_MODE_WRAP);
    shown_.clear();
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
    std::string text = wifi_ == nullptr ? std::string(ui::tr(ui::StrId::NetNoHardware))
                                        : "Wi-Fi: " + wifi_status_text(status_, has_saved_, now_ms);
    if (text != shown_) {
        shown_ = text;
        lv_label_set_text(label_, text.c_str());
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
