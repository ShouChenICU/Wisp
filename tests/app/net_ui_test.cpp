// The self-check screen's network section and the provisioning and
// pairing-code cards (connectivity spec 8.1, 8.3), driven through real
// WifiService/PairingService instances over a FakeWifi.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "app/ui_net_harness.hpp"
#include "net_cards.hpp"
#include "net_section.hpp"
#include "support/lvgl_probe.hpp"
#include "support/wait.hpp"
#include "wisp/app/power_manager.hpp"
#include "wisp/core/system_settings.hpp"
#include "wisp/hal/display_geometry.hpp"
#include "wisp/net/pairing_service.hpp"
#include "wisp/net/wifi_service.hpp"
#include "wisp/os/os.hpp"
#include "wisp/ui/dialog.hpp"
#include "wisp/ui/fonts.hpp"
#include "wisp/ui/strings.hpp"

using wisp::test::all_label_glyphs_resolvable;
using wisp::test::check_action_row;
using wisp::test::collect_labels;
using wisp::test::count_obj_of_class;
using wisp::test::find_button_with_label;
using wisp::test::find_label_containing;
using wisp::test::find_label_text_containing;
using wisp::test::find_obj_of_class;
using wisp::test::run_on_ui_blocking;
using wisp::test::run_on_ui_thread;
using wisp::test::select_page_lang;
using wisp::test::UiNetHarness;
using wisp::test::UiNetOptions;
using wisp::test::wait_until;
using wisp::ui::Lang;
using wisp::ui::StrId;

namespace {

constexpr Lang kLangs[] = {Lang::Zh, Lang::Ja, Lang::En};
const char* const kShapes[] = {"rounded_rect", "circle"};

std::string text_of(StrId id, Lang lang) { return wisp::ui::tr(id, lang); }

bool on_screen(UiNetHarness& h, const std::string& needle) {
    return !run_on_ui_thread(h.app->ui(), [&] {
                return find_label_text_containing(lv_screen_active(), needle.c_str());
            }).empty();
}

bool on_top_layer(UiNetHarness& h, const std::string& needle) {
    return !run_on_ui_thread(h.app->ui(), [&] {
                return find_label_text_containing(lv_layer_top(), needle.c_str());
            }).empty();
}

bool has_button(UiNetHarness& h, lv_obj_t* (*root)(), const std::string& text) {
    return run_on_ui_thread(h.app->ui(), [&] {
               return find_button_with_label(root(), text.c_str()) != nullptr ? "yes" : "";
           }) == "yes";
}

lv_obj_t* screen() { return lv_screen_active(); }
lv_obj_t* top() { return lv_layer_top(); }

// Clicks the button labelled `text` under root(); false if there is none.
bool click(UiNetHarness& h, lv_obj_t* (*root)(), const std::string& text) {
    return run_on_ui_thread(h.app->ui(), [&] {
               lv_obj_t* btn = find_button_with_label(root(), text.c_str());
               if (btn == nullptr) return "";
               lv_obj_send_event(btn, LV_EVENT_CLICKED, nullptr);
               return "clicked";
           }) == "clicked";
}

bool qr_shown(UiNetHarness& h) {
    return run_on_ui_thread(h.app->ui(), [] {
               return find_obj_of_class(lv_layer_top(), &lv_qrcode_class) != nullptr ? "yes" : "";
           }) == "yes";
}

wisp::hal::Area safe_area(UiNetHarness& h) {
    auto shape = wisp::hal::screen_shape_from_caps(h.display->info().caps);
    EXPECT_TRUE(shape);
    return wisp::hal::safe_rect(shape.value());
}

bool inside(const lv_area_t& a, const wisp::hal::Area& r) {
    return a.x1 >= r.x1 && a.y1 >= r.y1 && a.x2 <= r.x2 && a.y2 <= r.y2;
}

bool inside(const lv_area_t& a, const lv_area_t& r) {
    return a.x1 >= r.x1 && a.y1 >= r.y1 && a.x2 <= r.x2 && a.y2 <= r.y2;
}

std::string fmt(const lv_area_t& a) {
    return std::to_string(a.x1) + "," + std::to_string(a.y1) + "-" + std::to_string(a.x2) + "," +
           std::to_string(a.y2);
}

// Checks that `card` (a direct child of the top layer) lies inside the
// safe area and that every label, button and QR code in it lies inside the
// card at both ends of its scroll range. Returns "" or a description of the
// first problem.
std::string check_card_fits(lv_obj_t* card, const wisp::hal::Area& safe) {
    lv_obj_update_layout(lv_layer_top());
    lv_area_t box;
    lv_obj_get_coords(card, &box);
    if (!inside(box, safe)) return "card " + fmt(box) + " leaves the safe area";
    for (bool bottom : {false, true}) {
        lv_obj_scroll_to_y(card, bottom ? LV_COORD_MAX : 0, LV_ANIM_OFF);
        lv_obj_update_layout(card);
        std::vector<std::pair<std::string, lv_area_t>> labels;
        collect_labels(card, labels);
        for (const auto& [text, a] : labels) {
            if (a.x1 < box.x1 || a.x2 > box.x2) {
                return "label '" + text + "' " + fmt(a) + " leaves the card " + fmt(box);
            }
        }
        // At either end of the scroll range, the first and last child rest
        // inside the card.
        uint32_t n = lv_obj_get_child_count(card);
        lv_obj_t* edge = lv_obj_get_child(card, bottom ? static_cast<int32_t>(n) - 1 : 0);
        lv_area_t e;
        lv_obj_get_coords(edge, &e);
        if (!inside(e, box)) {
            return std::string(bottom ? "last" : "first") + " child " + fmt(e) +
                   " outside the card " + fmt(box);
        }
    }
    lv_obj_scroll_to_y(card, 0, LV_ANIM_OFF);
    return "";
}

// The card holding the label that contains `needle`: the top layer's
// direct child it lives in.
lv_obj_t* card_of(const char* needle) {
    lv_obj_t* obj = find_label_containing(lv_layer_top(), needle);
    while (obj != nullptr && lv_obj_get_parent(obj) != lv_layer_top()) {
        obj = lv_obj_get_parent(obj);
    }
    return obj;
}

}  // namespace

TEST(NetUi, WifiQrPayloadEscapesSpecialCharacters) {
    EXPECT_EQ(wisp::app::wifi_qr_payload("Wisp-3F2A", "aB3dE5gH"),
              "WIFI:T:WPA;S:Wisp-3F2A;P:aB3dE5gH;;");
    // \ ; , : " are escaped with a backslash in both fields.
    EXPECT_EQ(wisp::app::wifi_qr_payload(R"(a\b;c,d:e"f)", R"(p;w:"\)"),
              R"(WIFI:T:WPA;S:a\\b\;c\,d\:e\"f;P:p\;w\:\"\\;;)");
}

TEST(NetUi, PairingCodeIsSpacedForReading) {
    EXPECT_EQ(wisp::app::spaced_code("012345"), "012 345");
    EXPECT_EQ(wisp::app::spaced_code("12"), "12");
}

TEST(NetUi, SectionShowsStateAndButton) {
    UiNetHarness h;
    ASSERT_TRUE(h.start());
    ASSERT_TRUE(wait_until([&] { return on_screen(h, "Not set up"); }, 3000));
    EXPECT_TRUE(on_screen(h, "Wi-Fi"));
    EXPECT_TRUE(has_button(h, screen, "Set up Wi-Fi"));
    EXPECT_FALSE(has_button(h, screen, "Stop setup"));
}

// The power & clock card's time sync row: "Not synced" until the station
// gets an IP and a server answers, then the local time of that sync, in
// the page's language and the stored time zone.
TEST(NetUi, PowerClockCardShowsTheTimeSync) {
    UiNetOptions opts;
    opts.with_sntp = true;
    UiNetHarness h(opts);
    ASSERT_TRUE(h.start("system.time"));
    ASSERT_TRUE(wait_until([&] { return on_screen(h, "Not synced"); }, 3000));
    EXPECT_TRUE(on_screen(h, "Time sync"));
    {
        std::lock_guard<std::mutex> lock(h.wall_clock_mutex);
        EXPECT_TRUE(h.wall_clock_sets.empty());
    }

    ASSERT_TRUE(h.app->wifi()->add_network("Home", "password1", wisp::os::now_ms()));
    // 00:00 UTC is 08:00 at the default UTC+8.
    ASSERT_TRUE(wait_until([&] { return on_screen(h, "Synced 08:00"); }, 5000));
    {
        std::lock_guard<std::mutex> lock(h.wall_clock_mutex);
        EXPECT_EQ(h.wall_clock_sets, (std::vector<int64_t>{wisp::test::kSntpTime}));
    }
    ASSERT_EQ(h.topic("system.time").size(), 1u);
    EXPECT_EQ(h.app->time_sync()->status().server, "pool.ntp.org");

    ASSERT_TRUE(select_page_lang(h.app->ui(), Lang::Zh));
    ASSERT_TRUE(wait_until([&] { return on_screen(h, "已同步 08:00"); }, 3000));
    EXPECT_TRUE(on_screen(h, text_of(StrId::TimeSync, Lang::Zh)));

    ASSERT_TRUE(h.app->system_settings().set({{"utc_offset_min", -210}}));
    ASSERT_TRUE(wait_until([&] { return on_screen(h, "已同步 20:30"); }, 3000));
}

// The sync goes through the admin route too, and is refused without an IP.
TEST(NetUi, TimeSyncWithoutNetworkStaysNotSynced) {
    UiNetOptions opts;
    opts.with_sntp = true;
    UiNetHarness h(opts);
    ASSERT_TRUE(h.start());
    ASSERT_NE(h.app->time_sync(), nullptr);
    EXPECT_FALSE(h.app->time_sync()->sync_now());
    wisp::os::sleep_ms(600);
    EXPECT_TRUE(h.sntp->take_calls().empty());
    EXPECT_TRUE(on_screen(h, "Not synced"));
}

// No Sntp port: no service, and the row still says so.
TEST(NetUi, NoSntpPortMeansNoTimeSync) {
    UiNetHarness h;
    ASSERT_TRUE(h.start());
    EXPECT_EQ(h.app->time_sync(), nullptr);
    ASSERT_TRUE(wait_until([&] { return on_screen(h, "Not synced"); }, 3000));
}

TEST(NetUi, SectionShowsTheConnectedNetwork) {
    UiNetOptions opts;
    opts.saved = {{"Home", "password1"}};
    UiNetHarness h(opts);
    ASSERT_TRUE(h.start());
    ASSERT_TRUE(wait_until([&] { return on_screen(h, "Connected Home"); }, 5000));
    std::string text = run_on_ui_thread(
        h.app->ui(), [] { return find_label_text_containing(lv_screen_active(), "Connected"); });
    EXPECT_EQ(text, "Wi-Fi: Connected Home  192.168.1.50");
    EXPECT_TRUE(on_screen(h, "-50 dBm"));
}

// The signal strength is one unit: it may move to the next line, but is
// never split at its "-" or its space, in any language or screen shape.
TEST(NetUi, SignalStrengthStaysOnOneLine) {
    for (Lang lang : kLangs) {
        for (const char* shape : kShapes) {
            SCOPED_TRACE(std::string(wisp::ui::lang_code(lang)) + " " + shape);
            UiNetOptions opts;
            opts.shape = shape;
            opts.saved = {{"Home", "password1"}};
            UiNetHarness h(opts);
            ASSERT_TRUE(h.start());
            ASSERT_TRUE(select_page_lang(h.app->ui(), lang));
            ASSERT_TRUE(wait_until([&] { return on_screen(h, "-50 dBm"); }, 5000));
            std::string problem = run_on_ui_thread(h.app->ui(), [] {
                lv_obj_t* rssi = find_label_containing(lv_screen_active(), "-50 dBm");
                lv_obj_t* status = lv_obj_get_child(lv_obj_get_parent(rssi), 0);
                lv_obj_update_layout(lv_screen_active());
                lv_area_t r, row, st;
                lv_obj_get_coords(rssi, &r);
                lv_obj_get_coords(lv_obj_get_parent(rssi), &row);
                lv_obj_get_coords(status, &st);
                const int32_t line =
                    lv_font_get_line_height(lv_obj_get_style_text_font(rssi, LV_PART_MAIN));
                if (lv_area_get_height(&r) != line) return "dBm label " + fmt(r) + " wraps";
                if (!inside(r, row)) return "dBm label " + fmt(r) + " outside " + fmt(row);
                if (!inside(st, row)) return "status " + fmt(st) + " outside " + fmt(row);
                return std::string();
            });
            EXPECT_EQ(problem, "");
        }
    }
}

TEST(NetUi, SectionCountsDownToTheNextAttempt) {
    UiNetOptions opts;
    opts.saved = {{"Home", "password1"}};
    opts.wifi_reachable = false;
    UiNetHarness h(opts);
    ASSERT_TRUE(h.start());
    ASSERT_TRUE(wait_until([&] { return on_screen(h, "Retrying in "); }, 5000));
    auto seconds = [&] {
        std::string text = run_on_ui_thread(h.app->ui(), [] {
            return find_label_text_containing(lv_screen_active(), "Retrying in ");
        });
        size_t at = text.find("Retrying in ");
        return at == std::string::npos ? -1 : std::atoi(text.c_str() + at + 12);
    };
    int first = seconds();
    EXPECT_GE(first, 1);
    EXPECT_LE(first, 5);
    // Counts down (before the next attempt restarts it at 10 s).
    ASSERT_TRUE(wait_until([&] { return seconds() < first || seconds() > 5; }, 2500));
}

// Every saved network has had its password rejected three times in a row:
// nothing will be retried until the user sets Wi-Fi up again, so the line
// says that instead of counting down to a retry.
TEST(NetUi, SectionSaysWhenEveryPasswordWasRejected) {
    const std::pair<Lang, const char*> expected[] = {
        {Lang::Zh, "Wi-Fi：Home 的密码被拒绝，请重新配网"},
        {Lang::Ja, "Wi-Fi：Home のパスワードが拒否されました。再設定してください"},
        {Lang::En, "Wi-Fi: Home rejected the password. Set up Wi-Fi again"},
    };
    for (const auto& [lang, line] : expected) {
        SCOPED_TRACE(wisp::ui::lang_code(lang));
        UiNetOptions opts;
        opts.saved = {{"Home", "wrongpass"}};
        opts.saved_auth_failed = true;
        UiNetHarness h(opts);
        ASSERT_TRUE(h.start());
        ASSERT_TRUE(select_page_lang(h.app->ui(), lang));
        ASSERT_TRUE(wait_until([&] { return on_screen(h, line); }, 5000));
        const std::string retrying = text_of(StrId::NetRetrying, lang);
        EXPECT_FALSE(on_screen(h, retrying.substr(0, retrying.find("{}"))));
        EXPECT_TRUE(run_on_ui_thread(h.app->ui(), [] {
                        return all_label_glyphs_resolvable(lv_screen_active()) ? "yes" : "";
                    }) == "yes");
    }
}

// Every network parked but no name in the status (e.g. a backoff that is
// not about one network): the line still asks to set Wi-Fi up again,
// without a blank where the name would go.
TEST(NetUi, RejectedLineWithoutAName) {
    wisp::net::WifiStatus st;
    st.state = wisp::net::WifiState::Backoff;
    st.reason = "auth_failed";
    st.retry_at_ms = 5000;
    for (Lang lang : kLangs) {
        SCOPED_TRACE(wisp::ui::lang_code(lang));
        EXPECT_EQ(wisp::app::wifi_status_text(st, true, true, 0, lang),
                  text_of(StrId::NetRejectedNoName, lang));
        st.ssid = "Home";
        EXPECT_EQ(wisp::app::wifi_status_text(st, true, true, 0, lang),
                  wisp::ui::tr_fill(StrId::NetRejected, lang, "Home"));
        st.ssid.clear();
    }
}

// Bytes that are not UTF-8 (a GBK router) show as "?", not as garbage.
TEST(NetUi, NonUtf8SsidShowsQuestionMarks) {
    const std::string gbk = "\xc4\xe3\xba\xc3";
    UiNetOptions opts;
    opts.saved = {{gbk, ""}};
    UiNetHarness h(opts);
    h.wifi->networks.push_back({gbk, -40, false});
    ASSERT_TRUE(h.start());
    ASSERT_TRUE(wait_until([&] { return on_screen(h, "Connected ????  "); }, 5000));
}

TEST(NetUi, NoWifiDeviceShowsNoButton) {
    UiNetOptions opts;
    opts.with_wifi = false;
    UiNetHarness h(opts);
    ASSERT_TRUE(h.start());
    ASSERT_TRUE(wait_until([&] { return on_screen(h, "No Wi-Fi hardware"); }, 3000));
    EXPECT_FALSE(has_button(h, screen, "Set up Wi-Fi"));
    EXPECT_FALSE(has_button(h, screen, "Stop setup"));
}

TEST(NetUi, ProvisioningCardShowsQrAndFitsSafeArea) {
    for (Lang lang : kLangs) {
        for (const char* shape : kShapes) {
            SCOPED_TRACE(std::string(wisp::ui::lang_code(lang)) + " " + shape);
            UiNetOptions opts;
            opts.shape = shape;
            // The cards follow the system language, the button the page's.
            opts.lang = wisp::ui::lang_code(lang);
            UiNetHarness h(opts);
            ASSERT_TRUE(h.start());
            ASSERT_TRUE(select_page_lang(h.app->ui(), lang));
            ASSERT_TRUE(wait_until(
                [&] { return has_button(h, screen, text_of(StrId::StartSetup, lang)); }, 3000));
            ASSERT_TRUE(click(h, screen, text_of(StrId::StartSetup, lang)));
            ASSERT_TRUE(wait_until([&] { return qr_shown(h); }, 3000));
            EXPECT_TRUE(h.wifi->ap_active.load());

            const wisp::hal::Area safe = safe_area(h);
            const int32_t safe_w = safe.x2 - safe.x1 + 1;
            const std::string title = text_of(StrId::SetupTitle, lang);
            std::string problem = run_on_ui_thread(h.app->ui(), [&] {
                lv_obj_t* qr = find_obj_of_class(lv_layer_top(), &lv_qrcode_class);
                lv_obj_t* card = card_of(title.c_str());
                if (card == nullptr) return std::string("no card titled ") + title;
                if (lv_obj_get_parent(qr) != card) return std::string("QR code not in the card");
                std::string fit = check_card_fits(card, safe);
                if (!fit.empty()) return fit;
                lv_area_t q;
                lv_obj_get_coords(qr, &q);
                const int32_t want = std::min<int32_t>(safe_w * 45 / 100, 200);
                if (lv_area_get_width(&q) != want || lv_area_get_height(&q) != want) {
                    return "QR code " + fmt(q) + ", want side " + std::to_string(want);
                }
                std::string row = check_action_row(
                    find_button_with_label(card, text_of(StrId::StopSetup, lang).c_str()),
                    wisp::ui::kDialogButtonMinHeight, wisp::ui::kDialogButtonGap);
                if (!row.empty()) return "stop button: " + row;
                return all_label_glyphs_resolvable(lv_layer_top()) ? std::string()
                                                                   : std::string("missing glyph");
            });
            EXPECT_EQ(problem, "");

            EXPECT_TRUE(on_top_layer(h, "Wisp-3F2A"));
            EXPECT_TRUE(on_top_layer(h, h.wifi->ap_password()));
            EXPECT_TRUE(on_top_layer(h, "http://192.168.4.1"));
            EXPECT_FALSE(on_top_layer(h, ":8900"));
            // The section's button now stops it too.
            ASSERT_TRUE(wait_until(
                [&] { return has_button(h, screen, text_of(StrId::StopSetup, lang)); }, 3000));

            // The card's own button closes the hotspot, and the card with it.
            ASSERT_TRUE(click(h, top, text_of(StrId::StopSetup, lang)));
            ASSERT_TRUE(wait_until([&] { return !qr_shown(h); }, 3000));
            EXPECT_FALSE(h.wifi->ap_active.load());
            EXPECT_FALSE(on_top_layer(h, title));
            ASSERT_TRUE(wait_until(
                [&] { return has_button(h, screen, text_of(StrId::StartSetup, lang)); }, 3000));
        }
    }
}

// The QR code's modules as the widget draws them: one bool per pixel,
// true for dark (an I1 draw buf, palette first, bit set = dark).
std::vector<bool> qr_modules(lv_obj_t* qr) {
    const lv_draw_buf_t* buf = lv_canvas_get_draw_buf(qr);
    std::vector<bool> dark;
    if (buf == nullptr || buf->header.cf != LV_COLOR_FORMAT_I1) return dark;
    const uint8_t* bits = buf->data + 8;
    for (uint32_t y = 0; y < buf->header.h; ++y) {
        for (uint32_t x = 0; x < buf->header.w; ++x) {
            dark.push_back((bits[y * buf->header.stride + x / 8] >> (7 - x % 8)) & 1);
        }
    }
    return dark;
}

// Describes the first pixel of `qr`'s 16-bit draw buf (in `cf`) that is
// not black where `dark` says so, or white elsewhere; "" if all match.
std::string check_flat_qr(lv_obj_t* qr, const std::vector<bool>& dark, lv_color_format_t cf) {
    const lv_draw_buf_t* buf = lv_canvas_get_draw_buf(qr);
    if (buf == nullptr) return "no draw buf";
    if (buf->header.cf != cf) return "format " + std::to_string(buf->header.cf);
    const uint32_t w = buf->header.w;
    if (dark.size() != static_cast<size_t>(w) * buf->header.h) return "size changed";
    for (uint32_t y = 0; y < buf->header.h; ++y) {
        const auto* row = reinterpret_cast<const uint16_t*>(buf->data + y * buf->header.stride);
        for (uint32_t x = 0; x < w; ++x) {
            const uint16_t want = dark[y * w + x] ? 0x0000 : 0xFFFF;
            if (row[x] != want) {
                return "pixel " + std::to_string(x) + "," + std::to_string(y) + " is " +
                       std::to_string(row[x]);
            }
        }
    }
    return "";
}

// The QR code never changes while the card is open, so it is stored in
// the display's own 16-bit format: drawing it each frame is then a copy
// rather than an indexed-colour decode (which halved the frame rate while
// scrolling under the card).
TEST(NetUi, QrCodeIsFlattenedToTheDisplayFormat) {
    UiNetHarness h;
    ASSERT_TRUE(h.start());
    for (lv_color_format_t cf : {LV_COLOR_FORMAT_RGB565, LV_COLOR_FORMAT_RGB565_SWAPPED}) {
        SCOPED_TRACE(std::to_string(cf));
        std::string problem = run_on_ui_thread(h.app->ui(), [&] {
            lv_obj_t* qr = lv_qrcode_create(lv_layer_top());
            lv_qrcode_set_size(qr, 87);
            lv_qrcode_set_quiet_zone(qr, true);
            const std::string payload = wisp::app::wifi_qr_payload("Wisp-3F2A", "secret12");
            lv_qrcode_update(qr, payload.data(), static_cast<uint32_t>(payload.size()));
            const std::vector<bool> dark = qr_modules(qr);
            std::string result;
            if (dark.empty())
                result = "QR code is not I1";
            else if (!wisp::app::flatten_qr(qr, cf))
                result = "flatten_qr failed";
            else
                result = check_flat_qr(qr, dark, cf);
            lv_obj_delete(qr);
            return result;
        });
        EXPECT_EQ(problem, "");
    }
}

TEST(NetUi, ProvisioningCardQrUsesTheDisplayFormat) {
    UiNetHarness h;
    ASSERT_TRUE(h.start());
    ASSERT_TRUE(h.app->wifi()->start_provisioning(wisp::os::now_ms()));
    ASSERT_TRUE(wait_until([&] { return qr_shown(h); }, 3000));
    std::string problem = run_on_ui_thread(h.app->ui(), [] {
        lv_obj_t* qr = find_obj_of_class(lv_layer_top(), &lv_qrcode_class);
        const lv_draw_buf_t* buf = lv_canvas_get_draw_buf(qr);
        const lv_color_format_t want = lv_display_get_color_format(lv_obj_get_display(qr));
        if (buf == nullptr) return std::string("no draw buf");
        if (buf->header.cf != want) {
            return "QR in format " + std::to_string(buf->header.cf) + ", display " +
                   std::to_string(want);
        }
        return std::string();
    });
    EXPECT_EQ(problem, "");
}

// Scrolling the self-check screen under the open card redraws only what
// shows around it (wisp/ui/overlay.hpp).
TEST(NetUi, ScrollingUnderTheProvisioningCardLeavesTheCardAlone) {
    UiNetHarness h;
    ASSERT_TRUE(h.start());
    ASSERT_TRUE(h.app->wifi()->start_provisioning(wisp::os::now_ms()));
    ASSERT_TRUE(wait_until([&] { return qr_shown(h); }, 3000));
    // One UI job, so nothing else is drawn between the two frames.
    lv_area_t core{};
    size_t before = 0;
    bool scrolled = false;
    run_on_ui_blocking(h.app->ui(), [&] {
        lv_obj_t* card = lv_obj_get_parent(find_obj_of_class(lv_layer_top(), &lv_qrcode_class));
        lv_obj_get_coords(card, &core);
        const int32_t r = lv_obj_get_style_radius(card, LV_PART_MAIN);
        core.y1 += r;
        core.y2 -= r;
        lv_refr_now(nullptr);
        before = h.display->areas().size();
        lv_obj_t* scr = lv_screen_active();
        for (uint32_t i = 0; i < lv_obj_get_child_count(scr) && !scrolled; ++i) {
            lv_obj_t* c = lv_obj_get_child(scr, static_cast<int32_t>(i));
            if (lv_obj_get_scroll_bottom(c) > 0) {
                lv_obj_scroll_by(c, 0, -40, LV_ANIM_OFF);
                lv_refr_now(nullptr);
                scrolled = true;
            }
        }
    });
    ASSERT_TRUE(scrolled);
    const std::vector<wisp::hal::Area> areas = h.display->areas();
    ASSERT_GT(areas.size(), before);
    for (size_t i = before; i < areas.size(); ++i) {
        const auto& f = areas[i];
        EXPECT_FALSE(f.x1 <= core.x2 && f.x2 >= core.x1 && f.y1 <= core.y2 && f.y2 >= core.y1)
            << f.x1 << "," << f.y1 << "-" << f.x2 << "," << f.y2;
    }
}

TEST(NetUi, ProvisioningCardClosesWhenTheHotspotStopsElsewhere) {
    UiNetHarness h;
    ASSERT_TRUE(h.start());
    ASSERT_TRUE(h.app->wifi()->start_provisioning(wisp::os::now_ms()));
    ASSERT_TRUE(wait_until([&] { return qr_shown(h); }, 3000));
    ASSERT_TRUE(h.app->wifi()->stop_provisioning());
    ASSERT_TRUE(wait_until([&] { return !qr_shown(h); }, 3000));
}

// The success view's OK label up to its "{}" (the seconds left).
std::string ok_prefix(Lang lang) {
    const std::string full = text_of(StrId::SetupDoneOk, lang);
    return full.substr(0, full.find("{}"));
}

std::string done_title(Lang lang) { return wisp::ui::tr_fill(StrId::SetupDoneTitle, lang, "Home"); }

// Starts the hotspot from the screen and joins "Home" (in the harness's
// range) the way the setup page does. `clients` phones stay on the hotspot.
void provision_home(UiNetHarness& h, int clients) {
    ASSERT_TRUE(h.app->wifi()->start_provisioning(wisp::os::now_ms()));
    ASSERT_TRUE(wait_until([&] { return qr_shown(h); }, 3000));
    h.wifi->ap_client_count = clients;
    ASSERT_TRUE(h.app->wifi()->add_network("Home", "password1", wisp::os::now_ms()));
}

// Clicks the button whose label starts with `prefix`; false if none.
bool click_prefixed(UiNetHarness& h, const std::string& prefix) {
    return run_on_ui_thread(h.app->ui(), [&] {
               lv_obj_t* label = find_label_containing(lv_layer_top(), prefix.c_str());
               if (label == nullptr) return "";
               lv_obj_send_event(lv_obj_get_parent(label), LV_EVENT_CLICKED, nullptr);
               return "clicked";
           }) == "clicked";
}

TEST(NetUi, ProvisioningSuccessViewShowsTheNewAddressInsideSafeArea) {
    for (Lang lang : kLangs) {
        for (const char* shape : {"rect", "rounded_rect", "circle"}) {
            SCOPED_TRACE(std::string(wisp::ui::lang_code(lang)) + " " + shape);
            UiNetOptions opts;
            opts.shape = shape;
            opts.lang = wisp::ui::lang_code(lang);
            UiNetHarness h(opts);
            ASSERT_TRUE(h.start());
            provision_home(h, 1);
            ASSERT_TRUE(wait_until([&] { return on_top_layer(h, done_title(lang)); }, 5000));
            EXPECT_FALSE(qr_shown(h));
            EXPECT_TRUE(on_top_layer(h, "http://192.168.1.50:8900"));
            EXPECT_FALSE(on_top_layer(h, h.wifi->ap_password()));
            EXPECT_TRUE(h.wifi->ap_active.load());  // the phone is still on it

            const wisp::hal::Area safe = safe_area(h);
            const std::string title = done_title(lang);
            const std::string ok = ok_prefix(lang);
            std::string problem = run_on_ui_thread(h.app->ui(), [&] {
                lv_obj_t* card = card_of(title.c_str());
                if (card == nullptr) return std::string("no card titled ") + title;
                std::string fit = check_card_fits(card, safe);
                if (!fit.empty()) return fit;
                lv_obj_t* label = find_label_containing(card, ok.c_str());
                if (label == nullptr) return std::string("no OK button");
                std::string row =
                    check_action_row(lv_obj_get_parent(label), wisp::ui::kDialogButtonMinHeight,
                                     wisp::ui::kDialogButtonGap);
                if (!row.empty()) return "OK button: " + row;
                return all_label_glyphs_resolvable(lv_layer_top()) ? std::string()
                                                                   : std::string("missing glyph");
            });
            EXPECT_EQ(problem, "");

            // OK closes the card and the hotspot that was still waiting.
            ASSERT_TRUE(click_prefixed(h, ok));
            ASSERT_TRUE(wait_until([&] { return !on_top_layer(h, title); }, 3000));
            ASSERT_TRUE(wait_until([&] { return !h.wifi->ap_active.load(); }, 3000));
        }
    }
}

// The hotspot closing (here at once: nobody is left on it) leaves the
// success view up, so the address stays readable.
TEST(NetUi, ProvisioningSuccessViewOutlivesTheHotspot) {
    UiNetHarness h;
    ASSERT_TRUE(h.start());
    provision_home(h, 0);
    ASSERT_TRUE(wait_until([&] { return on_top_layer(h, done_title(Lang::Zh)); }, 5000));
    ASSERT_TRUE(wait_until([&] { return !h.wifi->ap_active.load(); }, 3000));
    // Past the screen's one-second resync of the services.
    wisp::os::sleep_ms(1500);
    EXPECT_TRUE(on_top_layer(h, done_title(Lang::Zh)));
    EXPECT_TRUE(on_top_layer(h, "http://192.168.1.50:8900"));
    ASSERT_TRUE(click_prefixed(h, ok_prefix(Lang::Zh)));
    ASSERT_TRUE(wait_until([&] { return !on_top_layer(h, done_title(Lang::Zh)); }, 3000));
    // Closed for good: the result that is still kept does not bring it back.
    wisp::os::sleep_ms(1500);
    EXPECT_FALSE(on_top_layer(h, done_title(Lang::Zh)));
}

// The countdown on OK, driven with explicit timestamps.
TEST(NetUi, ProvisioningSuccessViewCountsDownAndCloses) {
    UiNetHarness h;
    ASSERT_TRUE(h.start());
    const wisp::hal::Area safe = safe_area(h);
    int stops = 0;
    std::string seen = run_on_ui_thread(h.app->ui(), [&] {
        wisp::app::ProvisioningCard card([&] { ++stops; });
        card.show_success("Home", "http://192.168.1.50:8900", 1000, safe, Lang::Zh);
        std::string out;
        auto label = [&] {
            std::string t = find_label_text_containing(lv_layer_top(), "确定（");
            return t.empty() ? std::string("closed") : t;
        };
        out += label() + "|";
        for (uint64_t t : {1999u, 2000u, 15000u, 15999u}) {
            card.tick(t);
            out += label() + "|";
        }
        card.tick(16000);
        out += std::string(card.is_open() ? "open" : "closed") + "|" + label();
        // A language change rebuilds the view and keeps its deadline.
        card.show_success("Home", "http://192.168.1.50:8900", 20000, safe, Lang::Zh);
        card.reopen(safe, Lang::En, 27000);
        out += "|" + find_label_text_containing(lv_layer_top(), "OK (") + "|" +
               std::string(card.showing_success() ? "success" : "qr");
        card.close();
        return out;
    });
    EXPECT_EQ(seen,
              "确定（15）|确定（15）|确定（14）|确定（1）|确定（1）|closed|closed|OK (8)|success");
    EXPECT_EQ(stops, 0);  // the timer only closes the card
}

TEST(NetUi, PairingCardCountsDownAndCloses) {
    UiNetOptions opts;
    opts.pairing_interval_ms = 0;  // a second code right after the first
    UiNetHarness h(opts);
    ASSERT_TRUE(h.start());
    wisp::net::PairingService* pairing = h.app->pairing();
    ASSERT_NE(pairing, nullptr);

    ASSERT_TRUE(pairing->request_code(wisp::os::now_ms()));
    const std::string code = pairing->active_code(wisp::os::now_ms()).value_or("");
    ASSERT_EQ(code.size(), 6u);
    ASSERT_TRUE(wait_until([&] { return on_top_layer(h, "配对请求"); }, 3000));
    EXPECT_TRUE(on_top_layer(h, wisp::app::spaced_code(code)));

    auto remaining = [&] {
        std::string text = run_on_ui_thread(
            h.app->ui(), [] { return find_label_text_containing(lv_layer_top(), "剩余 "); });
        return text.empty() ? -1 : std::atoi(text.c_str() + std::string("剩余 ").size());
    };
    int first = remaining();
    EXPECT_GE(first, 119);
    EXPECT_LE(first, 120);
    ASSERT_TRUE(wait_until([&] { return remaining() == first - 1; }, 2500));

    // "Reject" kills the code at once and closes the card.
    ASSERT_TRUE(click(h, top, "拒绝"));
    ASSERT_TRUE(wait_until([&] { return !on_top_layer(h, "配对请求"); }, 3000));
    EXPECT_FALSE(pairing->active_code(wisp::os::now_ms()).has_value());
    auto codes = h.topic("net.pairing.code");
    ASSERT_FALSE(codes.empty());
    EXPECT_FALSE(codes.back().payload.at("active").as_bool());

    // A successful pairing closes it as well.
    ASSERT_TRUE(pairing->request_code(wisp::os::now_ms()));
    ASSERT_TRUE(wait_until([&] { return on_top_layer(h, "配对请求"); }, 3000));
    const std::string next = pairing->active_code(wisp::os::now_ms()).value_or("");
    ASSERT_TRUE(pairing->confirm(next, wisp::net::ClientInfo{}, wisp::os::now_ms()));
    ASSERT_TRUE(wait_until([&] { return !on_top_layer(h, "配对请求"); }, 3000));
}

TEST(NetUi, PairingCodeUsesFont24) {
    UiNetHarness h;
    ASSERT_TRUE(h.start());
    wisp::net::PairingService* pairing = h.app->pairing();
    ASSERT_NE(pairing, nullptr);
    ASSERT_TRUE(pairing->request_code(wisp::os::now_ms()));
    const std::string code = pairing->active_code(wisp::os::now_ms()).value_or("");
    ASSERT_TRUE(wait_until([&] { return on_top_layer(h, "配对请求"); }, 3000));
    const std::string same = run_on_ui_thread(h.app->ui(), [&] {
        lv_obj_t* digits =
            find_label_containing(lv_layer_top(), wisp::app::spaced_code(code).c_str());
        if (digits == nullptr) return "no label";
        return lv_obj_get_style_text_font(digits, LV_PART_MAIN) == wisp::ui::font(24) ? "yes"
                                                                                      : "no";
    });
    EXPECT_EQ(same, "yes");
}

// With a short code lifetime: the card counts down to the service's
// deadline and closes at it, not before.
TEST(NetUi, PairingCardClosesAtExpiry) {
    UiNetOptions opts;
    opts.pairing_ttl_ms = 2500;
    UiNetHarness h(opts);
    ASSERT_TRUE(h.start());
    wisp::net::PairingService* pairing = h.app->pairing();
    ASSERT_TRUE(pairing->request_code(wisp::os::now_ms()));
    uint64_t expires = 0;
    ASSERT_TRUE(pairing->active_code(wisp::os::now_ms(), &expires).has_value());
    ASSERT_TRUE(wait_until([&] { return on_top_layer(h, "配对请求"); }, 2000));
    EXPECT_TRUE(on_top_layer(h, "剩余 3 秒") || on_top_layer(h, "剩余 2 秒"));

    ASSERT_TRUE(wait_until([&] { return !on_top_layer(h, "配对请求"); }, 4000));
    EXPECT_GE(wisp::os::now_ms(), expires);
    EXPECT_FALSE(pairing->active_code(wisp::os::now_ms()).has_value());
}

// The bus drops its oldest events when full. A lost net.ap.state or
// net.pairing.code must not leave the cards stale: the screen re-reads the
// services on its own.
TEST(NetUi, CardsFollowTheServicesWhenEventsAreLost) {
    UiNetHarness h;
    ASSERT_TRUE(h.start());
    ASSERT_TRUE(wait_until([&] { return on_screen(h, "Not set up"); }, 3000));

    wisp::os::Semaphore entered(1, 0);
    wisp::os::Semaphore release(1, 0);
    auto block_sub = h.app->bus().subscribe("test.block", [&](const wisp::core::Event&) {
        entered.give();
        release.take(wisp::os::kForever);
    });
    h.app->bus().publish(wisp::core::Event{"test.block", "test"});
    ASSERT_TRUE(entered.take(3000));
    auto flood = [&] {
        for (int i = 0; i < 256; ++i) {
            h.app->bus().publish(wisp::core::Event{"test.flood", "test"});
        }
    };

    wisp::net::WifiService* wifi = h.app->wifi();
    wisp::net::PairingService* pairing = h.app->pairing();
    ASSERT_TRUE(wifi->start_provisioning(wisp::os::now_ms()));
    flood();
    bool setup_shown = wait_until([&] { return qr_shown(h); }, 3000);
    bool button_flipped = wait_until([&] { return has_button(h, screen, "Stop setup"); }, 3000);

    ASSERT_TRUE(pairing->request_code(wisp::os::now_ms()));
    flood();
    bool pair_shown = wait_until([&] { return on_top_layer(h, "配对请求"); }, 3000);

    pairing->cancel_code();
    flood();
    bool pair_closed = wait_until([&] { return !on_top_layer(h, "配对请求"); }, 3000);

    ASSERT_TRUE(wifi->stop_provisioning());
    flood();
    bool setup_closed = wait_until([&] { return !qr_shown(h); }, 3000);

    release.give();
    h.app->bus().unsubscribe(block_sub);
    EXPECT_TRUE(setup_shown);
    EXPECT_TRUE(button_flipped);
    EXPECT_TRUE(pair_shown);
    EXPECT_TRUE(pair_closed);
    EXPECT_TRUE(setup_closed);
}

TEST(NetUi, PairingCardStacksAboveProvisioningCardInsideSafeArea) {
    for (Lang lang : kLangs) {
        for (const char* shape : kShapes) {
            SCOPED_TRACE(std::string(wisp::ui::lang_code(lang)) + " " + shape);
            UiNetOptions opts;
            opts.shape = shape;
            // The cards follow the system language, the button the page's.
            opts.lang = wisp::ui::lang_code(lang);
            UiNetHarness h(opts);
            ASSERT_TRUE(h.start());
            ASSERT_TRUE(select_page_lang(h.app->ui(), lang));
            ASSERT_TRUE(h.app->wifi()->start_provisioning(wisp::os::now_ms()));
            ASSERT_TRUE(wait_until([&] { return qr_shown(h); }, 3000));
            ASSERT_TRUE(h.app->pairing()->request_code(wisp::os::now_ms()));
            const std::string pair_title = text_of(StrId::PairTitle, lang);
            ASSERT_TRUE(wait_until([&] { return on_top_layer(h, pair_title); }, 3000));

            const wisp::hal::Area safe = safe_area(h);
            const std::string setup_title = text_of(StrId::SetupTitle, lang);
            std::string problem = run_on_ui_thread(h.app->ui(), [&] {
                lv_obj_t* setup = card_of(setup_title.c_str());
                lv_obj_t* pair = card_of(pair_title.c_str());
                if (setup == nullptr || pair == nullptr || setup == pair) {
                    return std::string("cards not found");
                }
                if (lv_obj_get_index(pair) < lv_obj_get_index(setup)) {
                    return std::string("pairing card is below the provisioning card");
                }
                std::string fit = check_card_fits(pair, safe);
                if (!fit.empty()) return "pairing: " + fit;
                std::string row = check_action_row(
                    find_button_with_label(pair, text_of(StrId::Reject, lang).c_str()),
                    wisp::ui::kDialogButtonMinHeight, wisp::ui::kDialogButtonGap);
                if (!row.empty()) return "reject button: " + row;
                fit = check_card_fits(setup, safe);
                if (!fit.empty()) return "provisioning: " + fit;
                return all_label_glyphs_resolvable(lv_layer_top()) ? std::string()
                                                                   : std::string("missing glyph");
            });
            EXPECT_EQ(problem, "");
        }
    }
}

// The cards are system UI: they follow the system language (spec 8.3),
// staying open when it changes, and ignore the self-check page's own
// language.
TEST(NetUi, OpenCardsFollowTheSystemLanguage) {
    UiNetHarness h;
    ASSERT_TRUE(h.start());
    ASSERT_TRUE(h.app->wifi()->start_provisioning(wisp::os::now_ms()));
    ASSERT_TRUE(h.app->pairing()->request_code(wisp::os::now_ms()));
    // The system language starts as Chinese; the page as English.
    ASSERT_TRUE(wait_until([&] { return on_top_layer(h, "配对请求") && qr_shown(h); }, 3000));
    EXPECT_TRUE(has_button(h, screen, "Stop setup"));
    const std::string code = h.app->pairing()->active_code(wisp::os::now_ms()).value_or("");

    const std::string pair_title = text_of(StrId::PairTitle, Lang::Ja);
    const std::string setup_title = text_of(StrId::SetupTitle, Lang::Ja);
    ASSERT_TRUE(h.app->system_settings().set({{"language", "ja"}}));
    ASSERT_TRUE(wait_until([&] { return on_top_layer(h, pair_title); }, 3000));
    EXPECT_TRUE(on_top_layer(h, setup_title));
    EXPECT_TRUE(on_top_layer(h, wisp::app::spaced_code(code)));
    const std::string remaining = text_of(StrId::PairRemaining, Lang::Ja);
    EXPECT_TRUE(on_top_layer(h, remaining.substr(0, remaining.find("{}"))));
    EXPECT_TRUE(has_button(h, top, text_of(StrId::StopSetup, Lang::Ja)));
    EXPECT_FALSE(on_top_layer(h, "配对请求"));
    // The page keeps its own language.
    EXPECT_TRUE(has_button(h, screen, "Stop setup"));
    EXPECT_EQ(
        run_on_ui_thread(
            h.app->ui(),
            [] { return std::to_string(count_obj_of_class(lv_layer_top(), &lv_qrcode_class)); }),
        "1");
    auto stacked = [&] {
        return run_on_ui_thread(h.app->ui(), [&] {
            lv_obj_t* setup = card_of(setup_title.c_str());
            lv_obj_t* pair = card_of(pair_title.c_str());
            return setup != nullptr && pair != nullptr &&
                           lv_obj_get_index(pair) > lv_obj_get_index(setup)
                       ? std::string("ok")
                       : std::string();
        });
    };
    EXPECT_EQ(stacked(), "ok");

    // A page language switch leaves the cards alone.
    ASSERT_TRUE(select_page_lang(h.app->ui(), Lang::Zh));
    EXPECT_TRUE(on_top_layer(h, pair_title));
    EXPECT_TRUE(on_top_layer(h, setup_title));
    EXPECT_FALSE(on_top_layer(h, "配对请求"));
    EXPECT_TRUE(has_button(h, screen, "停止配网"));
    EXPECT_EQ(stacked(), "ok");
}

// Someone is waiting at a browser for the code: a sleeping screen wakes.
TEST(NetUi, PairingCodeWakesTheScreen) {
    UiNetHarness h;
    ASSERT_TRUE(h.start());
    wisp::app::PowerManager* pm = h.app->power_manager();
    run_on_ui_blocking(h.app->ui(), [&] { pm->enter_standby(); });
    ASSERT_TRUE(pm->standby());
    ASSERT_TRUE(h.app->pairing()->request_code(wisp::os::now_ms()));
    ASSERT_TRUE(wait_until([&] { return !pm->standby(); }, 3000));
    EXPECT_TRUE(on_top_layer(h, "配对请求"));
}
