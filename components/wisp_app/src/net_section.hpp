#pragma once

// The self-check screen's network block (connectivity spec 8.1): one
// status line ("Wi-Fi: <state>", plus the signal strength while
// connected) and a start/stop button for the setup hotspot. Without a WifiService (no "net.wifi"
// device, or networking disabled) the line says there is no Wi-Fi hardware and there is no button.
//
// Threading: UI thread only. refresh() reads the service's state (short
// lock-protected copies); tick() only re-renders the backoff countdown
// from what refresh() last read. The button calls start_provisioning() /
// stop_provisioning() directly: both only flip the radio into or out of
// AP mode under the service's own lock, never a scan. The resulting
// net.ap.state event is what changes the button text.

#include <cstdint>
#include <string>

#include "lvgl.h"
#include "wisp/net/wifi_service.hpp"

namespace wisp::app {

// The value half of the status line: not set up / not connected /
// connecting / retrying in N s / <ssid> rejected the password /
// connected <ssid> <ip>. The signal strength is shown next to it on a
// label of its own (see NetSection). `has_saved` tells "never set up"
// apart from "set up but off" (e.g. after standby dropped the link);
// `all_rejected` (every saved network is auth_failed, connectivity spec
// 4.2) turns the backoff countdown into the ask to set Wi-Fi up again.
std::string wifi_status_text(const net::WifiStatus& status, bool has_saved, bool all_rejected,
                             uint64_t now_ms);

class NetSection {
public:
    // `wifi` may be null; it must outlive this object.
    explicit NetSection(net::WifiService* wifi);

    NetSection(const NetSection&) = delete;
    NetSection& operator=(const NetSection&) = delete;

    // Appends the status line (and the button) to `col`, which owns them.
    // Call again after the column was rebuilt.
    void build(lv_obj_t* col, uint64_t now_ms);
    // Re-reads the service and re-renders.
    void refresh(uint64_t now_ms);
    // Re-renders the backoff countdown when its second changes.
    void tick(uint64_t now_ms);

private:
    void render(uint64_t now_ms);
    static void button_cb(lv_event_t* e);

    net::WifiService* wifi_;
    net::WifiStatus status_;
    bool has_saved_ = false;
    bool all_rejected_ = false;
    bool ap_active_ = false;
    // What the widgets show, so render() only touches them on a change.
    std::string shown_;
    int shown_button_ = -1;  // 0 start, 1 stop, -1 not rendered yet

    lv_obj_t* label_ = nullptr;
    // "-45 dBm" as one unit: on its own label in a wrapping row, so a
    // narrow line moves it down whole instead of breaking it at the "-" or
    // the space (both LV_TXT_BREAK_CHARS).
    lv_obj_t* rssi_label_ = nullptr;
    std::string shown_rssi_;
    lv_obj_t* button_ = nullptr;
    lv_obj_t* button_label_ = nullptr;
};

}  // namespace wisp::app
