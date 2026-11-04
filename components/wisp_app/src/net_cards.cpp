#include "net_cards.hpp"

#include <algorithm>
#include <utility>

#include "wisp/core/log.hpp"
#include "wisp/ui/strings.hpp"

namespace wisp::app {

namespace {

constexpr int32_t kCardMaxWidth = 360;
constexpr int32_t kCardPad = 12;
constexpr int32_t kCardRowGap = 6;
constexpr int32_t kQrMaxSide = 200;
constexpr int32_t kQrSafePercent = 45;
// Digits in a 6-digit code are set apart this far, on top of the space in
// the middle, so they read one by one.
constexpr int32_t kCodeLetterSpace = 6;

int32_t width_of(const hal::Area& a) { return a.x2 - a.x1 + 1; }
int32_t height_of(const hal::Area& a) { return a.y2 - a.y1 + 1; }

// An empty card on the top layer, centred in `safe` and bounded by it.
lv_obj_t* make_card(const hal::Area& safe) {
    lv_obj_t* card = lv_obj_create(lv_layer_top());
    lv_obj_set_width(card, std::min(width_of(safe), kCardMaxWidth));
    lv_obj_set_height(card, LV_SIZE_CONTENT);
    lv_obj_set_style_max_height(card, height_of(safe), 0);
    lv_obj_set_style_pad_all(card, kCardPad, 0);
    lv_obj_set_style_pad_row(card, kCardRowGap, 0);
    lv_obj_set_style_border_width(card, 2, 0);
    lv_obj_set_style_border_color(card, lv_palette_main(LV_PALETTE_LIGHT_BLUE), 0);
    lv_obj_set_flex_flow(card, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(card, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER, LV_FLEX_ALIGN_CENTER);
    lv_obj_set_scroll_dir(card, LV_DIR_VER);
    // A bar along the right edge would sit in a round screen's curve.
    lv_obj_set_scrollbar_mode(card, LV_SCROLLBAR_MODE_OFF);
    // Centre on the safe area's centre, which on an asymmetric safe area is
    // not the screen's.
    const int32_t screen_w = lv_display_get_horizontal_resolution(nullptr);
    const int32_t screen_h = lv_display_get_vertical_resolution(nullptr);
    lv_obj_align(card, LV_ALIGN_CENTER, (safe.x1 + safe.x2 + 1) / 2 - screen_w / 2,
                 (safe.y1 + safe.y2 + 1) / 2 - screen_h / 2);
    return card;
}

lv_obj_t* add_text(lv_obj_t* card, const std::string& text) {
    lv_obj_t* label = lv_label_create(card);
    lv_obj_set_width(label, LV_PCT(100));
    lv_label_set_long_mode(label, LV_LABEL_LONG_MODE_WRAP);
    lv_obj_set_style_text_align(label, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(label, text.c_str());
    return label;
}

lv_obj_t* add_button(lv_obj_t* card, const char* text, lv_event_cb_t cb, void* user_data) {
    lv_obj_t* btn = lv_button_create(card);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user_data);
    lv_obj_t* label = lv_label_create(btn);
    lv_label_set_text(label, text);
    return btn;
}

}  // namespace

std::string wifi_qr_payload(std::string_view ssid, std::string_view password) {
    auto escape = [](std::string_view in) {
        std::string out;
        out.reserve(in.size());
        for (char c : in) {
            if (c == '\\' || c == ';' || c == ',' || c == ':' || c == '"') out += '\\';
            out += c;
        }
        return out;
    };
    return "WIFI:T:WPA;S:" + escape(ssid) + ";P:" + escape(password) + ";;";
}

std::string spaced_code(std::string_view code) {
    if (code.size() != 6) return std::string(code);
    return std::string(code.substr(0, 3)) + " " + std::string(code.substr(3));
}

bool flatten_qr(lv_obj_t* qr, lv_color_format_t cf) {
    if (cf != LV_COLOR_FORMAT_RGB565 && cf != LV_COLOR_FORMAT_RGB565_SWAPPED) return false;
    lv_draw_buf_t* indexed = lv_canvas_get_draw_buf(qr);
    if (indexed == nullptr || indexed->header.cf != LV_COLOR_FORMAT_I1) return false;
    const uint32_t w = indexed->header.w;
    const uint32_t h = indexed->header.h;
    lv_draw_buf_t* flat = lv_draw_buf_create(w, h, cf, LV_STRIDE_AUTO);
    if (flat == nullptr) return false;
    // I1 keeps its two-entry palette ahead of the pixels; a set bit
    // (most significant first) is a dark module. Black and white read the
    // same in either byte order.
    const uint8_t* bits = indexed->data + 2 * sizeof(lv_color32_t);
    for (uint32_t y = 0; y < h; ++y) {
        const uint8_t* src = bits + y * indexed->header.stride;
        auto* dst = reinterpret_cast<uint16_t*>(flat->data + y * flat->header.stride);
        for (uint32_t x = 0; x < w; ++x) {
            const bool dark = (src[x / 8] >> (7 - x % 8)) & 1;
            dst[x] = dark ? 0x0000 : 0xFFFF;
        }
    }
    // lv_canvas_set_draw_buf() drops the old buffer from the image cache,
    // so it can be freed here; the widget's destructor frees the new one.
    lv_canvas_set_draw_buf(qr, flat);
    lv_draw_buf_destroy(indexed);
    return true;
}

// ---------------------------------------------------------------------------
// ProvisioningCard

ProvisioningCard::ProvisioningCard(std::function<void()> on_stop) : on_stop_(std::move(on_stop)) {}

void ProvisioningCard::open(const net::ApSession& session, const hal::Area& safe) {
    close();
    session_ = session;
    card_ = make_card(safe);

    add_text(card_, ui::tr(ui::StrId::SetupTitle));

    lv_obj_t* qr = lv_qrcode_create(card_);
    lv_qrcode_set_size(qr, std::min(width_of(safe) * kQrSafePercent / 100, kQrMaxSide));
    lv_qrcode_set_dark_color(qr, lv_color_black());
    lv_qrcode_set_light_color(qr, lv_color_white());
    // The quiet zone (a white margin of four modules) is part of the QR
    // format; some phone cameras miss codes drawn without one.
    lv_qrcode_set_quiet_zone(qr, true);
    const std::string payload = wifi_qr_payload(session.ssid, session.password);
    if (lv_qrcode_update(qr, payload.data(), static_cast<uint32_t>(payload.size())) !=
        LV_RESULT_OK) {
        WLOG_W("net_ui", "QR code could not be drawn; hotspot details are shown as text");
    } else if (!flatten_qr(qr, lv_display_get_color_format(lv_obj_get_display(qr)))) {
        // Still correct, only slower to draw.
        WLOG_W("net_ui", "QR code kept as an indexed image");
    }

    add_text(card_, ui::tr_fill(ui::StrId::Hotspot, session.ssid));
    add_text(card_, ui::tr_fill(ui::StrId::Password, session.password));
    add_text(card_, ui::tr_fill(ui::StrId::OpenAddress, "http://" + session.ip));
    add_button(card_, ui::tr(ui::StrId::StopSetup), &ProvisioningCard::stop_cb, this);
}

void ProvisioningCard::close() {
    if (card_ == nullptr) return;
    lv_obj_delete(card_);
    card_ = nullptr;
    // The password is only kept while it is on screen.
    session_ = {};
}

void ProvisioningCard::raise() {
    if (card_ != nullptr) lv_obj_move_foreground(card_);
}

void ProvisioningCard::stop_cb(lv_event_t* e) {
    auto* self = static_cast<ProvisioningCard*>(lv_event_get_user_data(e));
    if (self->on_stop_) self->on_stop_();
}

// ---------------------------------------------------------------------------
// PairingCard

PairingCard::PairingCard(std::function<void()> on_reject) : on_reject_(std::move(on_reject)) {}

int64_t PairingCard::seconds_left(uint64_t expires_ms, uint64_t now_ms) {
    if (now_ms >= expires_ms) return 0;
    return static_cast<int64_t>((expires_ms - now_ms + 999) / 1000);
}

void PairingCard::open(std::string code, uint64_t expires_ms, uint64_t now_ms,
                       const hal::Area& safe) {
    close();
    code_ = std::move(code);
    expires_ms_ = expires_ms;
    card_ = make_card(safe);

    add_text(card_, ui::tr(ui::StrId::PairTitle));

    lv_obj_t* digits = add_text(card_, spaced_code(code_));
    // Digits only, so the larger Latin font needs no CJK fallback.
    lv_obj_set_style_text_font(digits, &lv_font_montserrat_24, 0);
    lv_obj_set_style_text_letter_space(digits, kCodeLetterSpace, 0);

    countdown_ = add_text(card_, "");
    shown_seconds_ = -1;
    add_button(card_, ui::tr(ui::StrId::Reject), &PairingCard::reject_cb, this);
    tick(now_ms);
}

void PairingCard::tick(uint64_t now_ms) {
    if (card_ == nullptr) return;
    const int64_t left = seconds_left(expires_ms_, now_ms);
    if (left <= 0) {
        close();
        return;
    }
    if (left == shown_seconds_) return;
    shown_seconds_ = left;
    lv_label_set_text(countdown_,
                      ui::tr_fill(ui::StrId::PairRemaining, std::to_string(left)).c_str());
}

void PairingCard::close() {
    if (card_ == nullptr) return;
    lv_obj_delete(card_);
    card_ = nullptr;
    countdown_ = nullptr;
    code_.clear();
    expires_ms_ = 0;
}

void PairingCard::raise() {
    if (card_ != nullptr) lv_obj_move_foreground(card_);
}

void PairingCard::reject_cb(lv_event_t* e) {
    auto* self = static_cast<PairingCard*>(lv_event_get_user_data(e));
    if (self->on_reject_) self->on_reject_();
}

}  // namespace wisp::app
