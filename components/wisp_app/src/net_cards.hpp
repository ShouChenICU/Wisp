#pragma once

// The provisioning (setup hotspot) and pairing-code cards of the
// self-check screen (connectivity spec 8.1).
//
// Both are children of lv_layer_top(), so they float above the scrolling
// screen. Each is centred in the display's safe area (spec 5.6), never
// wider or taller than it; if the content is taller (a small round
// screen, a long translation) it scrolls inside the card, which keeps its
// first and last rows inside the safe area at either end. All text comes
// from ui::tr() in the current language; a language change is handled by
// the owner closing and reopening the card.
//
// Threading: every member function must be called on the UI thread. The
// buttons only call the owner's callback, which asks a service to act;
// the card itself closes when the resulting event comes back, so no card
// ever deletes itself from inside its own event handler.
//
// Lifetime: close() deletes the LVGL objects. The destructor makes no
// LVGL call (an owner may be destroyed off the UI thread), so the owner
// must close() an open card on the UI thread first.

#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include "lvgl.h"
#include "wisp/hal/display.hpp"
#include "wisp/net/wifi_service.hpp"

namespace wisp::app {

// "WIFI:T:WPA;S:<ssid>;P:<password>;;" -- the Wi-Fi QR format phone
// cameras join directly -- with \ ; , : and " backslash-escaped in both
// fields, as that format requires.
std::string wifi_qr_payload(std::string_view ssid, std::string_view password);

// "123456" -> "123 456": easier to read off the screen and type. Any
// other length comes back unchanged.
std::string spaced_code(std::string_view code);

// Replaces the drawn QR code `qr` (an lv_qrcode after a successful
// lv_qrcode_update(); its draw buf is I1) with the same pixels in `cf`,
// which must be LV_COLOR_FORMAT_RGB565 or LV_COLOR_FORMAT_RGB565_SWAPPED:
// dark modules black, light ones white, whatever colours were set. The
// widget then draws by copying rather than decoding an indexed image on
// every frame. Must run on the UI thread; false (and `qr` unchanged) if
// the buffer cannot be allocated or `qr` holds no I1 image.
bool flatten_qr(lv_obj_t* qr, lv_color_format_t cf);

class ProvisioningCard {
public:
    // `on_stop` runs (on the UI thread) when the card's stop button is
    // clicked.
    explicit ProvisioningCard(std::function<void()> on_stop);
    ~ProvisioningCard() = default;

    ProvisioningCard(const ProvisioningCard&) = delete;
    ProvisioningCard& operator=(const ProvisioningCard&) = delete;

    // Shows `session` (replacing a card already open): title, the QR code
    // (side min(45% of the safe width, 200 px)), hotspot name, password,
    // "then open http://<ip>" and a stop button. The address is the bare
    // hotspot IP: phones open port 80 by default, and the device's
    // captive redirect there forwards to the setup page.
    void open(const net::ApSession& session, const hal::Area& safe);
    void close();
    bool is_open() const { return card_ != nullptr; }
    // The session shown; meaningful only while open.
    const net::ApSession& session() const { return session_; }
    // Brings the card to the front of the top layer.
    void raise();

private:
    static void stop_cb(lv_event_t* e);

    std::function<void()> on_stop_;
    net::ApSession session_;
    lv_obj_t* card_ = nullptr;
};

class PairingCard {
public:
    // `on_reject` runs (on the UI thread) when the reject button is
    // clicked.
    explicit PairingCard(std::function<void()> on_reject);
    ~PairingCard() = default;

    PairingCard(const PairingCard&) = delete;
    PairingCard& operator=(const PairingCard&) = delete;

    // Shows `code` (replacing a card already open) with a countdown to
    // `expires_ms` (os::now_ms() timebase).
    void open(std::string code, uint64_t expires_ms, uint64_t now_ms, const hal::Area& safe);
    // Updates the "N seconds left" line; closes the card once the code has
    // expired (the service announces that too, at its own tick).
    void tick(uint64_t now_ms);
    void close();
    bool is_open() const { return card_ != nullptr; }
    const std::string& code() const { return code_; }
    uint64_t expires_ms() const { return expires_ms_; }
    void raise();

private:
    static void reject_cb(lv_event_t* e);
    // Whole seconds left, rounded up, so the line reads 120 right after
    // the code is issued and never shows 0 while the code still works.
    static int64_t seconds_left(uint64_t expires_ms, uint64_t now_ms);

    std::function<void()> on_reject_;
    std::string code_;
    uint64_t expires_ms_ = 0;
    int64_t shown_seconds_ = -1;
    lv_obj_t* card_ = nullptr;
    lv_obj_t* countdown_ = nullptr;
};

}  // namespace wisp::app
