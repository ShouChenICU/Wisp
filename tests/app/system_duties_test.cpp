// SystemDuties (components/wisp_app/src/system_duties.hpp) over a booted
// App that never opens the hardware diagnostics: the power key, the
// confirm dialogs, the network cards and WISP_PERF all work over the home
// screen the device boots into (mvp spec 6.6).

#include <gtest/gtest.h>

#include <algorithm>
#include <mutex>
#include <set>
#include <span>
#include <string>
#include <vector>

#include "app/ui_net_harness.hpp"
#include "factory_reset.hpp"
#include "net_cards.hpp"
#include "support/fake_firmware.hpp"
#include "support/fake_http.hpp"
#include "support/lvgl_probe.hpp"
#include "support/wait.hpp"
#include "wisp/app/power_manager.hpp"
#include "wisp/base/json.hpp"
#include "wisp/net/pairing_service.hpp"
#include "wisp/net/wifi_service.hpp"
#include "wisp/os/os.hpp"
#include "wisp/shell/shell.hpp"
#include "wisp/ui/system_layer.hpp"

using wisp::net::Method;
using wisp::test::count_obj_of_class;
using wisp::test::find_button_with_label;
using wisp::test::find_label_text_containing;
using wisp::test::run_on_ui_blocking;
using wisp::test::run_on_ui_thread;
using wisp::test::UiNetHarness;
using wisp::test::UiNetOptions;
using wisp::test::wait_until;

namespace {

class SystemDutiesTest : public ::testing::Test {
protected:
    void boot(UiNetOptions opts = {}) {
        h = std::make_unique<UiNetHarness>(opts);
        ASSERT_TRUE(h->start("system."));
        ASSERT_NE(h->app->shell(), nullptr);
        on_home();
    }

    // The home screen is in front and the diagnostics were never built.
    void on_home() {
        bool home = false;
        bool diag = true;
        run_on_ui_blocking(h->app->ui(), [&] {
            home = lv_screen_active() == h->app->shell()->home_screen();
            diag = h->app->shell()->diagnostics_open();
        });
        EXPECT_TRUE(home);
        EXPECT_FALSE(diag);
    }

    int msgboxes() {
        return std::stoi(run_on_ui_thread(h->app->ui(), [] {
            return std::to_string(count_obj_of_class(lv_layer_top(), &lv_msgbox_class));
        }));
    }

    bool top_says(const std::string& text) {
        return !run_on_ui_thread(h->app->ui(), [&] {
                    return find_label_text_containing(lv_layer_top(), text.c_str());
                }).empty();
    }

    bool click(const std::string& text) {
        return run_on_ui_thread(h->app->ui(), [&] {
                   lv_obj_t* btn = find_button_with_label(lv_layer_top(), text.c_str());
                   if (btn == nullptr) return "";
                   lv_obj_send_event(btn, LV_EVENT_CLICKED, nullptr);
                   return "clicked";
               }) == "clicked";
    }

    void press(const char* topic) {
        h->app->bus().publish(wisp::core::Event{topic, "button.power"});
    }

    std::string pair_browser() {
        wisp::net::PairingService* pairing = h->app->pairing();
        EXPECT_TRUE(pairing->request_code(wisp::os::now_ms()));
        auto code = pairing->active_code(wisp::os::now_ms()).value_or("");
        auto token = pairing->confirm(code, wisp::net::ClientInfo{}, wisp::os::now_ms());
        EXPECT_TRUE(token);
        return token ? token.value() : std::string();
    }

    // Uploads a firmware image through the admin router's streamed route,
    // as the HTTP server would.
    int upload(const std::string& token, const std::string& version) {
        wisp::test::ImageSpec spec;
        spec.version = version;
        const std::vector<uint8_t> image = wisp::test::make_firmware_image(spec);
        wisp::net::HttpRequest head = wisp::test::with_csrf(wisp::test::with_token(
            wisp::test::make_req(Method::Post, "/api/v1/system/firmware"), token));
        head.content_length = image.size();
        auto stream = h->server->stream_handler();
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

    std::unique_ptr<UiNetHarness> h;
};

}  // namespace

// Spec 8's power key, with no self-check screen anywhere: a short press
// toggles standby, a long press asks to power off.
TEST_F(SystemDutiesTest, PowerKeyWithoutSelfCheck) {
    boot();
    wisp::app::PowerManager* pm = h->app->power_manager();
    ASSERT_NE(pm, nullptr);

    press("button.power.short");
    ASSERT_TRUE(wait_until([&] { return pm->standby(); }, 3000));
    press("button.power.short");
    ASSERT_TRUE(wait_until([&] { return !pm->standby(); }, 3000));

    press("button.power.long");
    ASSERT_TRUE(wait_until([&] { return top_says("关机"); }, 3000));
    EXPECT_EQ(msgboxes(), 1);
    on_home();
    // A short press while the question is up cancels it.
    press("button.power.short");
    ASSERT_TRUE(wait_until([&] { return msgboxes() == 0; }, 3000));
    EXPECT_FALSE(pm->standby());
}

// A factory reset and a firmware update both ask over the home screen.
TEST_F(SystemDutiesTest, ResetAndFirmwareDialogs) {
    UiNetOptions opts;
    opts.with_server = true;
    opts.with_firmware = true;
    boot(opts);

    wisp::app::request_factory_reset(*h->app);
    ASSERT_TRUE(wait_until([&] { return top_says("恢复出厂设置"); }, 3000));
    EXPECT_EQ(msgboxes(), 1);
    on_home();
    ASSERT_TRUE(click("否"));
    ASSERT_TRUE(wait_until([&] { return msgboxes() == 0; }, 3000));

    const std::string token = pair_browser();
    EXPECT_EQ(upload(token, "0.3.0"), 202);
    ASSERT_TRUE(wait_until([&] { return top_says("固件更新"); }, 3000));
    EXPECT_TRUE(top_says("0.2.0\n→ 0.3.0"));
    EXPECT_EQ(msgboxes(), 1);
    on_home();
    ASSERT_TRUE(click("否"));
    ASSERT_TRUE(wait_until([&] { return msgboxes() == 0; }, 3000));
    EXPECT_FALSE(wisp::app::take_factory_reset_request(*h->app));
}

// Connectivity spec 8.1's cards over the home screen.
TEST_F(SystemDutiesTest, PairingAndProvisioningCards) {
    boot();
    auto ap = h->app->wifi()->start_provisioning(wisp::os::now_ms());
    ASSERT_TRUE(ap);
    const std::string ssid = ap.value().ssid;
    ASSERT_TRUE(wait_until([&] { return top_says(ssid); }, 3000));

    wisp::net::PairingService* pairing = h->app->pairing();
    ASSERT_TRUE(pairing->request_code(wisp::os::now_ms()));
    const std::string code = pairing->active_code(wisp::os::now_ms()).value_or("");
    ASSERT_FALSE(code.empty());
    ASSERT_TRUE(wait_until([&] { return top_says(wisp::app::spaced_code(code)); }, 3000));
    on_home();

    pairing->cancel_code();
    ASSERT_TRUE(wait_until([&] { return !top_says(wisp::app::spaced_code(code)); }, 3000));
    ASSERT_TRUE(h->app->wifi()->stop_provisioning());
    ASSERT_TRUE(wait_until([&] { return !top_says(ssid); }, 3000));
}

// WISP_PERF every 5 s via Platform::emit_line, with exactly the fields
// docs/architecture.md's "机器可读输出" section documents. The home screen
// shows the wisp, so its frame rate is not 0.
TEST_F(SystemDutiesTest, PerfLineEveryFiveSeconds) {
    boot();
    auto perf_lines = [&] {
        std::vector<std::string> out;
        std::lock_guard<std::mutex> lock(h->lines_mutex);
        for (const auto& line : h->lines) {
            if (line.rfind("WISP_PERF ", 0) == 0) out.push_back(line);
        }
        return out;
    };
    ASSERT_TRUE(wait_until([&] { return !perf_lines().empty(); }, 6000));

    const std::string line = perf_lines().front();
    auto parsed = wisp::from_json(line.substr(std::string("WISP_PERF ").size()));
    ASSERT_TRUE(parsed);
    const wisp::Value& v = parsed.value();
    ASSERT_EQ(v.type(), wisp::Value::Type::Map);
    std::set<std::string> keys;
    for (const auto& [k, _] : v.as_map()) keys.insert(k);
    EXPECT_EQ(keys, (std::set<std::string>{"fps", "cpu", "flush_ms", "frames", "heap_internal_free",
                                           "heap_large_free", "wisp_fps", "wisp_ms"}));

    // fps/flush_ms/wisp_* are computed as doubles, but a momentarily
    // whole-number value comes back from JSON as Int.
    auto is_number = [](const wisp::Value* value) {
        return value != nullptr && (value->type() == wisp::Value::Type::Int ||
                                    value->type() == wisp::Value::Type::Double);
    };
    for (const char* k : {"fps", "flush_ms", "wisp_fps", "wisp_ms"}) {
        EXPECT_TRUE(is_number(v.find(k))) << k;
    }
    for (const char* k : {"cpu", "frames", "heap_internal_free", "heap_large_free"}) {
        ASSERT_NE(v.find(k), nullptr) << k;
        EXPECT_EQ(v.find(k)->type(), wisp::Value::Type::Int) << k;
    }
    EXPECT_GT(v.find("wisp_fps")->as_double(), 0);
}
