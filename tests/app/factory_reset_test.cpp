// Factory reset (connectivity spec 6.1): what it erases and keeps, the order
// of its steps, and the on-device confirmation the admin route asks for.

#include "factory_reset.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <filesystem>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <string>
#include <vector>

#include "app/ui_net_harness.hpp"
#include "support/fake_http.hpp"
#include "support/lvgl_probe.hpp"
#include "support/wait.hpp"
#include "wisp/app/power_manager.hpp"
#include "wisp/base/json.hpp"
#include "wisp/core/llm_config.hpp"
#include "wisp/core/settings.hpp"
#include "wisp/core/storage_manager.hpp"
#include "wisp/core/system_settings.hpp"
#include "wisp/hal/display_geometry.hpp"
#include "wisp/net/pairing_service.hpp"
#include "wisp/net/wifi_service.hpp"
#include "wisp/os/os.hpp"
#include "wisp/port/sim/settings_json_file.hpp"
#include "wisp/ui/strings.hpp"

using wisp::net::Method;
using wisp::test::all_label_glyphs_resolvable;
using wisp::test::count_obj_of_class;
using wisp::test::find_button_with_label;
using wisp::test::find_label_text_containing;
using wisp::test::find_obj_of_class;
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

// Pairs a browser through the service, as the code flow would.
std::string pair_browser(UiNetHarness& h) {
    wisp::net::PairingService* pairing = h.app->pairing();
    EXPECT_TRUE(pairing->request_code(wisp::os::now_ms()));
    auto code = pairing->active_code(wisp::os::now_ms()).value_or("");
    auto token = pairing->confirm(code, wisp::net::ClientInfo{}, wisp::os::now_ms());
    EXPECT_TRUE(token);
    return token ? token.value() : std::string();
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

wisp::net::HttpResponse post_reset(UiNetHarness& h, const std::string& token) {
    return h.server->handle(
        with_csrf(with_token(make_req(Method::Post, "/api/v1/system/factory-reset"), token)));
}

std::vector<std::string> reset_states(UiNetHarness& h) {
    std::vector<std::string> out;
    for (const auto& e : h.topic("system.factory_reset")) {
        out.push_back(e.payload.at("state").as_string());
    }
    return out;
}

}  // namespace

TEST(FactoryReset, ClearsEverythingButLibraryAndReboots) {
    UiNetOptions opts;
    opts.saved = {{"Home", "password1"}};
    UiNetHarness h(opts);
    ASSERT_TRUE(h.start("system."));
    wisp::app::App& app = *h.app;

    ASSERT_TRUE(app.system_settings().set({{"language", "en"}}));
    ASSERT_TRUE(wisp::core::Settings(*app.platform().settings, "power").set("screen_off_s", 30));
    ASSERT_EQ(app.wifi()->saved_ssids().size(), 1u);
    pair_browser(h);
    ASSERT_EQ(app.pairing()->clients().size(), 1u);
    ASSERT_TRUE(app.llm().set(
        {.base_url = "https://api.example.com/v1", .model = "m", .api_key = "sk-test"}));
    const std::string system = app.storage().write_root(wisp::core::Area::System).value();
    ASSERT_TRUE(std::filesystem::exists(system + "/secrets/llm.json"));
    const std::string library =
        app.storage().write_root(wisp::core::Area::Library).value() + "/scene.json";
    const std::string kept = R"({"keep":true})";
    ASSERT_TRUE(app.storage().write_atomic(
        library,
        std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(kept.data()), kept.size())));

    wisp::app::factory_reset(app);

    EXPECT_EQ(h.reboots.load(), 1);
    EXPECT_EQ(reset_states(h), std::vector<std::string>{"done"});
    // Settings: every key gone, on disk too.
    {
        wisp::port::sim::JsonFileSettingsBackend fresh(h.settings_path());
        EXPECT_EQ(wisp::core::Settings(fresh, "sys").get_string("lang", "none"), "none");
        EXPECT_EQ(wisp::core::Settings(fresh, "power").get_int("screen_off_s", -1), -1);
    }
    // Wi-Fi, pairings and the LLM config, in memory and on disk.
    EXPECT_TRUE(app.wifi()->saved_ssids().empty());
    auto wifi_json = app.storage().read_file(system + "/net/wifi.json");
    ASSERT_TRUE(wifi_json);
    EXPECT_EQ(std::string(wifi_json.value().begin(), wifi_json.value().end()).find("Home"),
              std::string::npos);
    EXPECT_TRUE(app.pairing()->clients().empty());
    auto clients_json = app.storage().read_file(system + "/net/clients.json");
    if (clients_json) {
        EXPECT_EQ(
            std::string(clients_json.value().begin(), clients_json.value().end()).find("hash"),
            std::string::npos);
    }
    EXPECT_FALSE(app.llm().get().has_key);
    EXPECT_TRUE(app.llm().get().base_url.empty());
    EXPECT_FALSE(std::filesystem::exists(system + "/secrets/llm.json"));
    // Library data stays.
    auto scene = app.storage().read_file(library);
    ASSERT_TRUE(scene);
    EXPECT_EQ(std::string(scene.value().begin(), scene.value().end()), kept);
}

// The browser that asked must still hear "done": the event goes out (and
// the admin socket gets a chance to send it) before the tokens -- and
// with them that socket -- are revoked.
TEST(FactoryReset, AnnouncesDoneBeforeRevokingTokens) {
    UiNetHarness h;
    ASSERT_TRUE(h.start("system."));
    pair_browser(h);
    std::atomic<bool> done_seen_at_revoke{false};
    std::atomic<int> revokes{0};
    int listener = h.app->pairing()->add_revoke_listener([&](const auto& id) {
        if (id) return;
        ++revokes;
        std::lock_guard<std::mutex> lock(h.events_mutex);
        for (const auto& e : h.events) {
            if (e.topic == "system.factory_reset") done_seen_at_revoke.store(true);
        }
    });

    wisp::app::factory_reset(*h.app);
    h.app->pairing()->remove_revoke_listener(listener);
    EXPECT_EQ(revokes.load(), 1);
    EXPECT_TRUE(done_seen_at_revoke.load());
    EXPECT_EQ(h.reboots.load(), 1);
}

TEST(FactoryReset, CancelPublishesCancelled) {
    UiNetOptions opts;
    opts.saved = {{"Home", "password1"}};
    opts.with_server = true;
    UiNetHarness h(opts);
    ASSERT_TRUE(h.start("system."));
    const std::string token = pair_browser(h);

    auto res = post_reset(h, token);
    EXPECT_EQ(res.status, 202);
    ASSERT_TRUE(wait_until([&] { return dialog_says(h, "恢复出厂设置"); }, 3000));
    EXPECT_TRUE(dialog_says(h, "将清除 Wi-Fi、配对和模型配置并重启，场景数据保留。"));

    ASSERT_TRUE(click(h, "否"));
    ASSERT_TRUE(wait_until([&] { return reset_states(h).size() == 2; }, 3000));
    EXPECT_EQ(reset_states(h), (std::vector<std::string>{"requested", "cancelled"}));
    EXPECT_EQ(msgboxes(h), 0);
    wisp::os::sleep_ms(100);
    EXPECT_EQ(h.reboots.load(), 0);
    EXPECT_EQ(h.app->wifi()->saved_ssids().size(), 1u);
    EXPECT_EQ(h.app->pairing()->clients().size(), 1u);
}

TEST(FactoryReset, ConfirmOnDeviceResetsOffTheUiThread) {
    UiNetOptions opts;
    opts.saved = {{"Home", "password1"}};
    opts.with_server = true;
    UiNetHarness h(opts);
    ASSERT_TRUE(h.start("system."));
    const std::string token = pair_browser(h);

    EXPECT_EQ(post_reset(h, token).status, 202);
    ASSERT_TRUE(wait_until([&] { return dialog_says(h, "恢复出厂设置"); }, 3000));
    ASSERT_TRUE(click(h, "是"));
    ASSERT_TRUE(wait_until([&] { return h.reboots.load() == 1; }, 5000));
    EXPECT_FALSE(h.rebooted_on_ui_thread.load());
    EXPECT_EQ(reset_states(h), (std::vector<std::string>{"requested", "done"}));
    EXPECT_TRUE(h.app->wifi()->saved_ssids().empty());
    EXPECT_TRUE(h.app->pairing()->clients().empty());
    EXPECT_EQ(msgboxes(h), 0);
    // The revoked token no longer works.
    EXPECT_EQ(post_reset(h, token).status, 401);
}

TEST(FactoryReset, RepeatedRequestsShowOneDialog) {
    UiNetOptions opts;
    opts.with_server = true;
    UiNetHarness h(opts);
    ASSERT_TRUE(h.start("system."));
    const std::string token = pair_browser(h);

    EXPECT_EQ(post_reset(h, token).status, 202);
    ASSERT_TRUE(wait_until([&] { return dialog_says(h, "恢复出厂设置"); }, 3000));
    EXPECT_EQ(post_reset(h, token).status, 202);
    ASSERT_TRUE(wait_until([&] { return reset_states(h).size() == 2; }, 3000));
    h.app->bus().wait_idle(2000);
    run_on_ui_blocking(h.app->ui(), [] {});
    EXPECT_EQ(msgboxes(h), 1);

    ASSERT_TRUE(click(h, "否"));
    ASSERT_TRUE(wait_until([&] { return reset_states(h).size() == 3; }, 3000));
    EXPECT_EQ(reset_states(h).back(), "cancelled");
    EXPECT_EQ(msgboxes(h), 0);
}

// If the reset task cannot start, nothing is erased, the browser hears
// "cancelled", and the next request asks again.
TEST(FactoryReset, FailedTaskStartCancelsAndAllowsAnotherRequest) {
    UiNetOptions opts;
    opts.saved = {{"Home", "password1"}};
    opts.with_server = true;
    UiNetHarness h(opts);
    ASSERT_TRUE(h.start("system."));
    const std::string token = pair_browser(h);
    std::atomic<int> spawns{0};
    wisp::app::set_reset_spawner(
        *h.app,
        [&](const wisp::os::TaskConfig&, std::function<void()>) -> std::unique_ptr<wisp::os::Task> {
            spawns.fetch_add(1);
            return nullptr;
        });

    EXPECT_EQ(post_reset(h, token).status, 202);
    ASSERT_TRUE(wait_until([&] { return dialog_says(h, "恢复出厂设置"); }, 3000));
    ASSERT_TRUE(click(h, "是"));
    ASSERT_TRUE(wait_until([&] { return reset_states(h).size() == 2; }, 3000));
    EXPECT_EQ(reset_states(h), (std::vector<std::string>{"requested", "cancelled"}));
    EXPECT_EQ(spawns.load(), 1);
    wisp::os::sleep_ms(100);
    EXPECT_EQ(h.reboots.load(), 0);
    EXPECT_EQ(h.app->wifi()->saved_ssids().size(), 1u);
    EXPECT_EQ(h.app->pairing()->clients().size(), 1u);

    // Not stuck in "confirmed": a new request shows the dialog again, and
    // with the task able to start this time the reset goes through.
    wisp::app::set_reset_spawner(*h.app, nullptr);
    EXPECT_EQ(post_reset(h, token).status, 202);
    ASSERT_TRUE(wait_until([&] { return msgboxes(h) == 1; }, 3000));
    ASSERT_TRUE(click(h, "是"));
    ASSERT_TRUE(wait_until([&] { return h.reboots.load() == 1; }, 5000));
    EXPECT_EQ(reset_states(h),
              (std::vector<std::string>{"requested", "cancelled", "requested", "done"}));
}

// One confirm dialog at a time: with the power-off question up, a reset
// request is answered "cancelled" and no second dialog appears.
TEST(FactoryReset, RequestDuringPowerOffDialogIsCancelled) {
    UiNetOptions opts;
    opts.with_server = true;
    UiNetHarness h(opts);
    ASSERT_TRUE(h.start("system."));
    const std::string token = pair_browser(h);

    h.app->bus().publish(wisp::core::Event{"button.power.long", "button.power"});
    ASSERT_TRUE(wait_until([&] { return dialog_says(h, "关机"); }, 3000));
    ASSERT_EQ(msgboxes(h), 1);

    EXPECT_EQ(post_reset(h, token).status, 202);
    ASSERT_TRUE(wait_until([&] { return reset_states(h).size() == 2; }, 3000));
    EXPECT_EQ(reset_states(h), (std::vector<std::string>{"requested", "cancelled"}));
    run_on_ui_blocking(h.app->ui(), [] {});
    EXPECT_EQ(msgboxes(h), 1);
    EXPECT_FALSE(dialog_says(h, "恢复出厂设置"));
    EXPECT_TRUE(dialog_says(h, "关机"));

    // Once the power-off question is answered, a reset can be asked.
    ASSERT_TRUE(click(h, "否"));
    ASSERT_TRUE(wait_until([&] { return msgboxes(h) == 0; }, 3000));
    EXPECT_EQ(post_reset(h, token).status, 202);
    ASSERT_TRUE(wait_until([&] { return dialog_says(h, "恢复出厂设置"); }, 3000));
}

// The other order: with the reset question up, a long press on the power
// key is ignored, and the key's next short press turns the screen off as
// it would with no dialog of its own pending.
TEST(FactoryReset, PowerKeyDuringResetDialogIsIgnored) {
    UiNetOptions opts;
    opts.with_server = true;
    UiNetHarness h(opts);
    ASSERT_TRUE(h.start("system."));
    const std::string token = pair_browser(h);

    EXPECT_EQ(post_reset(h, token).status, 202);
    ASSERT_TRUE(wait_until([&] { return dialog_says(h, "恢复出厂设置"); }, 3000));

    h.app->bus().publish(wisp::core::Event{"button.power.long", "button.power"});
    h.app->bus().wait_idle(2000);
    run_on_ui_blocking(h.app->ui(), [] {});
    EXPECT_EQ(msgboxes(h), 1);
    EXPECT_TRUE(dialog_says(h, "恢复出厂设置"));
    EXPECT_FALSE(dialog_says(h, "关机"));

    wisp::app::PowerManager* pm = h.app->power_manager();
    ASSERT_NE(pm, nullptr);
    h.app->bus().publish(wisp::core::Event{"button.power.short", "button.power"});
    ASSERT_TRUE(wait_until([&] { return pm->standby(); }, 3000));
    // The reset question is still waiting for an answer.
    EXPECT_EQ(msgboxes(h), 1);
    EXPECT_EQ(reset_states(h), (std::vector<std::string>{"requested"}));
}

// Without a screen nobody could confirm, so the route says so up front.
TEST(FactoryReset, HeadlessDeviceRefusesTheRequest) {
    UiNetOptions opts;
    opts.with_display = false;
    opts.with_server = true;
    UiNetHarness h(opts);
    ASSERT_TRUE(h.start("system."));
    const std::string token = pair_browser(h);
    EXPECT_EQ(post_reset(h, token).status, 503);
}

// The request wakes a sleeping screen so the dialog is seen; the dialog
// fits the safe area and follows a language change while open.
TEST(FactoryReset, DialogFitsEveryShapeAndLanguage) {
    for (const char* shape : {"rounded_rect", "circle"}) {
        SCOPED_TRACE(shape);
        UiNetOptions opts;
        opts.shape = shape;
        opts.with_server = true;
        UiNetHarness h(opts);
        ASSERT_TRUE(h.start("system."));
        const std::string token = pair_browser(h);
        wisp::app::PowerManager* pm = h.app->power_manager();
        run_on_ui_blocking(h.app->ui(), [&] { pm->enter_standby(); });

        EXPECT_EQ(post_reset(h, token).status, 202);
        ASSERT_TRUE(wait_until([&] { return dialog_says(h, "恢复出厂设置"); }, 3000));
        EXPECT_FALSE(pm->standby());

        auto shape_caps = wisp::hal::screen_shape_from_caps(h.display->info().caps);
        ASSERT_TRUE(shape_caps);
        const wisp::hal::Area safe = wisp::hal::safe_rect(shape_caps.value());
        for (Lang lang : {Lang::Zh, Lang::Ja, Lang::En}) {
            SCOPED_TRACE(wisp::ui::lang_code(lang));
            ASSERT_TRUE(h.app->system_settings().set({{"language", wisp::ui::lang_code(lang)}}));
            const std::string title = wisp::ui::tr(StrId::ResetTitle, lang);
            ASSERT_TRUE(wait_until([&] { return dialog_says(h, title); }, 3000));
            EXPECT_EQ(msgboxes(h), 1);
            std::string problem = run_on_ui_thread(h.app->ui(), [&] {
                lv_obj_update_layout(lv_layer_top());
                lv_obj_t* box = find_obj_of_class(lv_layer_top(), &lv_msgbox_class);
                lv_area_t a;
                lv_obj_get_coords(box, &a);
                if (a.x1 < safe.x1 || a.y1 < safe.y1 || a.x2 > safe.x2 || a.y2 > safe.y2) {
                    return "dialog " + std::to_string(a.x1) + "," + std::to_string(a.y1) + "-" +
                           std::to_string(a.x2) + "," + std::to_string(a.y2) +
                           " leaves the safe area";
                }
                return all_label_glyphs_resolvable(lv_layer_top()) ? std::string()
                                                                   : std::string("missing glyph");
            });
            EXPECT_EQ(problem, "");
        }
        ASSERT_TRUE(click(h, wisp::ui::tr(StrId::No, Lang::En)));
        ASSERT_TRUE(wait_until([&] { return reset_states(h).size() == 2; }, 3000));
        EXPECT_EQ(h.reboots.load(), 0);
    }
}
