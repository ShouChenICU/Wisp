// Firmware upload on the device side (connectivity spec 6.5): the
// confirmation dialog, what "yes" and "no" do, and when a freshly booted
// image is marked valid.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "app/ui_net_harness.hpp"
#include "support/fake_firmware.hpp"
#include "support/fake_http.hpp"
#include "support/lvgl_probe.hpp"
#include "support/wait.hpp"
#include "wisp/app/power_manager.hpp"
#include "wisp/core/system_settings.hpp"
#include "wisp/hal/display_geometry.hpp"
#include "wisp/net/pairing_service.hpp"
#include "wisp/os/os.hpp"
#include "wisp/ui/strings.hpp"

using wisp::net::Method;
using wisp::test::all_label_glyphs_resolvable;
using wisp::test::count_obj_of_class;
using wisp::test::find_button_with_label;
using wisp::test::find_label_text_containing;
using wisp::test::find_obj_of_class;
using wisp::test::ImageSpec;
using wisp::test::make_firmware_image;
using wisp::test::make_req;
using wisp::test::run_on_ui_blocking;
using wisp::test::run_on_ui_thread;
using wisp::test::UiNetHarness;
using wisp::test::UiNetOptions;
using wisp::test::wait_until;
using wisp::test::with_csrf;
using wisp::test::with_token;
using wisp::ui::Lang;
using wisp::ui::StrId;

namespace {

std::string pair_browser(UiNetHarness& h) {
    wisp::net::PairingService* pairing = h.app->pairing();
    EXPECT_TRUE(pairing->request_code(wisp::os::now_ms()));
    auto code = pairing->active_code(wisp::os::now_ms()).value_or("");
    auto token = pairing->confirm(code, wisp::net::ClientInfo{}, wisp::os::now_ms());
    EXPECT_TRUE(token);
    return token ? token.value() : std::string();
}

// Uploads `image` through the admin router's streamed route, as the HTTP
// server would.
int upload(UiNetHarness& h, const std::string& token, const std::vector<uint8_t>& image) {
    wisp::net::HttpRequest head =
        with_csrf(with_token(make_req(Method::Post, "/api/v1/system/firmware"), token));
    head.content_length = image.size();
    auto stream = h.server->stream_handler();
    if (!stream) return 0;
    auto start = stream(head);
    if (!start) return 0;
    if (!start->sink) return start->response.status;
    std::span<const uint8_t> rest(image);
    while (!rest.empty()) {
        const size_t n = std::min(rest.size(), wisp::net::kBodyChunkBytes);
        if (auto early = start->sink->on_data(rest.first(n))) return early->status;
        rest = rest.subspan(n);
    }
    return start->sink->on_end().status;
}

std::vector<uint8_t> image_of(const std::string& version) {
    ImageSpec spec;
    spec.version = version;
    return make_firmware_image(spec);
}

int msgboxes(UiNetHarness& h) {
    return std::stoi(run_on_ui_thread(h.app->ui(), [] {
        return std::to_string(count_obj_of_class(lv_layer_top(), &lv_msgbox_class));
    }));
}

bool dialog_says(UiNetHarness& h, const std::string& text) {
    return !run_on_ui_thread(h.app->ui(), [&] {
                return find_label_text_containing(lv_layer_top(), text.c_str());
            }).empty();
}

bool click(UiNetHarness& h, const std::string& text) {
    return run_on_ui_thread(h.app->ui(), [&] {
               lv_obj_t* btn = find_button_with_label(lv_layer_top(), text.c_str());
               if (btn == nullptr) return "";
               lv_obj_send_event(btn, LV_EVENT_CLICKED, nullptr);
               return "clicked";
           }) == "clicked";
}

std::vector<std::string> states(UiNetHarness& h) {
    std::vector<std::string> out;
    for (const auto& e : h.topic("system.firmware")) {
        out.push_back(e.payload.at("state").as_string());
    }
    return out;
}

UiNetOptions firmware_opts() {
    UiNetOptions opts;
    opts.with_server = true;
    opts.with_firmware = true;
    return opts;
}

}  // namespace

TEST(FirmwareConfirm, MarkValidOnlyAfterStart) {
    UiNetHarness h(firmware_opts());
    std::atomic<bool> report_first{false};
    h.firmware->on_mark_valid = [&] { report_first.store(h.emitted("WISP_BOOT_REPORT ")); };
    EXPECT_EQ(h.firmware->mark_valid_calls, 0);
    ASSERT_TRUE(h.start("system."));
    EXPECT_EQ(h.firmware->mark_valid_calls, 1);
    EXPECT_TRUE(report_first.load());
}

TEST(FirmwareConfirm, YesInstallsAndRestartsOffTheUiThread) {
    UiNetHarness h(firmware_opts());
    ASSERT_TRUE(h.start("system."));
    const std::string token = pair_browser(h);

    EXPECT_EQ(upload(h, token, image_of("0.3.0")), 202);
    ASSERT_TRUE(wait_until([&] { return dialog_says(h, "固件更新"); }, 3000));
    EXPECT_TRUE(dialog_says(h, "0.2.0\n→ 0.3.0"));
    EXPECT_FALSE(dialog_says(h, wisp::ui::tr(StrId::FirmwareDowngrade, Lang::Zh)));
    ASSERT_TRUE(click(h, "是"));
    ASSERT_TRUE(wait_until([&] { return h.reboots.load() == 1; }, 5000));
    EXPECT_FALSE(h.rebooted_on_ui_thread.load());
    EXPECT_EQ(h.firmware->activations, 1);
    EXPECT_EQ(states(h), (std::vector<std::string>{"received", "confirmed"}));
    EXPECT_EQ(msgboxes(h), 0);
}

TEST(FirmwareConfirm, NoKeepsTheRunningImage) {
    UiNetHarness h(firmware_opts());
    ASSERT_TRUE(h.start("system."));
    const std::string token = pair_browser(h);

    EXPECT_EQ(upload(h, token, image_of("0.3.0")), 202);
    ASSERT_TRUE(wait_until([&] { return dialog_says(h, "固件更新"); }, 3000));
    ASSERT_TRUE(click(h, "否"));
    ASSERT_TRUE(wait_until([&] { return states(h).size() == 2; }, 3000));
    EXPECT_EQ(states(h), (std::vector<std::string>{"received", "rejected"}));
    EXPECT_EQ(h.firmware->activations, 0);
    EXPECT_EQ(h.reboots.load(), 0);
    EXPECT_EQ(msgboxes(h), 0);

    // The slot is free again, and the dialog comes back for the next one.
    EXPECT_EQ(upload(h, token, image_of("0.3.1")), 202);
    ASSERT_TRUE(wait_until([&] { return dialog_says(h, "0.2.0\n→ 0.3.1"); }, 3000));
    EXPECT_EQ(msgboxes(h), 1);
}

TEST(FirmwareConfirm, DowngradeIsPointedOut) {
    UiNetOptions opts = firmware_opts();
    opts.firmware_version = "0.4.0-3-gabcdef0";
    UiNetHarness h(opts);
    ASSERT_TRUE(h.start("system."));
    const std::string token = pair_browser(h);
    EXPECT_EQ(upload(h, token, image_of("0.3.0")), 202);
    ASSERT_TRUE(wait_until([&] { return dialog_says(h, "固件更新"); }, 3000));
    EXPECT_TRUE(dialog_says(h, wisp::ui::tr(StrId::FirmwareDowngrade, Lang::Zh)));
}

TEST(FirmwareConfirm, HeadlessDeviceRefusesUploads) {
    UiNetOptions opts = firmware_opts();
    opts.with_display = false;
    UiNetHarness h(opts);
    ASSERT_TRUE(h.start("system."));
    const std::string token = pair_browser(h);
    EXPECT_EQ(upload(h, token, image_of("0.3.0")), 503);
    EXPECT_EQ(h.firmware->begins, 0);
}

TEST(FirmwareConfirm, DialogFitsEveryShapeAndLanguage) {
    for (const char* shape : {"rounded_rect", "circle"}) {
        SCOPED_TRACE(shape);
        UiNetOptions opts = firmware_opts();
        opts.shape = shape;
        opts.firmware_version = "0.9.0";
        UiNetHarness h(opts);
        ASSERT_TRUE(h.start("system."));
        const std::string token = pair_browser(h);
        wisp::app::PowerManager* pm = h.app->power_manager();
        run_on_ui_blocking(h.app->ui(), [&] { pm->enter_standby(); });

        EXPECT_EQ(upload(h, token, image_of("0.3.0")), 202);
        ASSERT_TRUE(wait_until([&] { return dialog_says(h, "固件更新"); }, 3000));
        EXPECT_FALSE(pm->standby());

        auto shape_caps = wisp::hal::screen_shape_from_caps(h.display->info().caps);
        ASSERT_TRUE(shape_caps);
        const wisp::hal::Area safe = wisp::hal::safe_rect(shape_caps.value());
        for (Lang lang : {Lang::Zh, Lang::Ja, Lang::En}) {
            SCOPED_TRACE(wisp::ui::lang_code(lang));
            ASSERT_TRUE(h.app->system_settings().set({{"language", wisp::ui::lang_code(lang)}}));
            const std::string title = wisp::ui::tr(StrId::FirmwareTitle, lang);
            ASSERT_TRUE(wait_until([&] { return dialog_says(h, title); }, 3000));
            EXPECT_TRUE(dialog_says(h, wisp::ui::tr(StrId::FirmwareDowngrade, lang)));
            EXPECT_EQ(msgboxes(h), 1);
            std::string problem = run_on_ui_thread(h.app->ui(), [&] {
                lv_obj_update_layout(lv_layer_top());
                lv_obj_t* box = find_obj_of_class(lv_layer_top(), &lv_msgbox_class);
                lv_area_t a;
                lv_obj_get_coords(box, &a);
                if (a.x1 < safe.x1 || a.y1 < safe.y1 || a.x2 > safe.x2 || a.y2 > safe.y2) {
                    return std::string("dialog leaves the safe area");
                }
                return all_label_glyphs_resolvable(lv_layer_top()) ? std::string()
                                                                   : std::string("missing glyph");
            });
            EXPECT_EQ(problem, "");
        }
        ASSERT_TRUE(click(h, wisp::ui::tr(StrId::No, Lang::En)));
        ASSERT_TRUE(wait_until([&] { return states(h).size() == 2; }, 3000));
    }
}
