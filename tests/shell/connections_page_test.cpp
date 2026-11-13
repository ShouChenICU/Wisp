#include "wisp/shell/connections_page.hpp"

#include <gtest/gtest.h>

#include "shell/page_fixture.hpp"

TEST(ConnectionsPage, MissingServicesAndBackNavigation) {
    wisp::test::PageHarness h("circle", 360, 360);
    ASSERT_TRUE(h.started());
    h.boot();
    h.show(wisp::shell::Page::Settings);
    h.on_ui([&] {
        h.shell().open_connections(wisp::shell::ConnectionKind::Wifi);
        ASSERT_NE(h.shell().connections_page(), nullptr);
        EXPECT_NE(wisp::test::find_label_containing(h.shell().choice_screen(), "暂不可用"),
                  nullptr);
    });
    h.drag({60, 180}, {260, 180});
    ASSERT_TRUE(h.ui_until([&] { return h.shell().connections_page() == nullptr; }));
    EXPECT_EQ(h.shell().page(), wisp::shell::Page::Settings);
}

#include <cstdlib>
#include <deque>
#include <fstream>
#include <mutex>

#include "support/fake_wifi.hpp"
#include "support/temp_dir.hpp"
#include "wisp/core/device_registry.hpp"
#include "wisp/core/storage_manager.hpp"
#include "wisp/core/storage_worker.hpp"
#include "wisp/ui/symbols.hpp"
#include "wisp/ui/theme.hpp"

namespace {
class ConnectionsTest : public ::testing::Test {
protected:
    void boot(const std::string& shape = "rect", int size = 480,
              wisp::ui::Lang lang = wisp::ui::Lang::Zh) {
        registry.add(std::make_shared<wisp::test::FakeStorage>("storage.int", dir, false));
        ASSERT_TRUE(storage.init());
        bus.start();
        ASSERT_TRUE(models.init());
        wifi_device = std::make_shared<wisp::test::FakeWifi>("net.wifi");
        wifi_device->networks = {{"Open", -30, false}, {"Locked", -50, true}};
        wifi_device->networks[1].details.bssid = "02:11:22:33:44:55";
        wifi_device->networks[1].details.channel = 6;
        wifi_device->networks[1].details.security = "WPA2-Personal";
        wifi = std::make_unique<wisp::net::WifiService>(wifi_device, storage, bus, random);
        ASSERT_TRUE(wifi->init(0));
        h = std::make_unique<wisp::test::PageHarness>(shape, size, size);
        ASSERT_TRUE(h->started());
        h->deps().wifi = wifi.get();
        h->deps().models = &models;
        h->deps().start_wifi_setup = [] {};
        h->deps().admin_url = [] { return "http://192.168.100.200:8900"; };
        h->deps().submit_network = [this](std::function<void()> job) {
            std::lock_guard lock(mutex);
            if (reject) return false;
            jobs.push_back(std::move(job));
            return true;
        };
        h->on_ui([&] { wisp::ui::set_lang(lang); });
        h->boot();
        h->show(wisp::shell::Page::Settings);
    }
    void TearDown() override {
        h.reset();
        bus.stop();
    }
    void open(wisp::shell::ConnectionKind kind) {
        h->on_ui([&] { h->shell().open_connections(kind); });
    }
    auto* page() { return h->shell().connections_page(); }
    size_t queued() {
        std::lock_guard lock(mutex);
        return jobs.size();
    }
    void finish() {
        std::function<void()> job;
        {
            std::lock_guard lock(mutex);
            ASSERT_FALSE(jobs.empty());
            job = std::move(jobs.front());
            jobs.pop_front();
        }
        job();
        h->on_ui([] {});
    }
    std::string make_model(wisp::core::ModelKind kind, const std::string& name,
                           const std::string& model = "test-model") {
        auto r = models.create({.kind = kind, .name = name, .preset = "openai", .model = model});
        EXPECT_TRUE(r);
        return r ? r.value().id : "";
    }
    void click(const std::string& text) {
        h->on_ui([&] {
            auto* root = page()->view();
            auto* label = wisp::test::find_label_containing(root, text.c_str());
            ASSERT_NE(label, nullptr);
            lv_obj_send_event(lv_obj_get_parent(label), LV_EVENT_CLICKED, nullptr);
        });
        h->on_ui([] {});
    }
    void screenshot(const std::string& name, int size) {
        const char* dir = std::getenv("WISP_CONNECTIONS_SCREENSHOTS");
        if (!dir) return;
        h->on_ui([] { lv_obj_update_layout(lv_screen_active()); });
        auto shot = h->ui().snapshot_rgb888();
        ASSERT_TRUE(shot);
        std::ofstream file(std::string(dir) + "/" + name + ".ppm", std::ios::binary);
        ASSERT_TRUE(file);
        file << "P6\n" << size << " " << size << "\n255\n";
        file.write(reinterpret_cast<const char*>(shot.value().data()), shot.value().size());
    }
    static void random(uint8_t* out, size_t n) {
        static uint8_t seed = 0;
        for (size_t i = 0; i < n; ++i) out[i] = ++seed;
    }
    std::string dir = wisp::test::temp_dir().string();
    wisp::core::EventBus bus;
    wisp::core::DeviceRegistry registry;
    wisp::core::StorageManager storage{registry, bus};
    wisp::core::ModelProfileService models{storage, bus, random};
    std::shared_ptr<wisp::test::FakeWifi> wifi_device;
    std::unique_ptr<wisp::net::WifiService> wifi;
    std::unique_ptr<wisp::test::PageHarness> h;
    std::mutex mutex;
    std::deque<std::function<void()>> jobs;
    bool reject = false;
};
}  // namespace

TEST_F(ConnectionsTest, ModelsFilterProbeAndActivateIndependently) {
    boot();
    const auto a = make_model(wisp::core::ModelKind::Llm, "Chat A");
    const auto b = make_model(wisp::core::ModelKind::Llm, "Chat B");
    const auto voice = make_model(wisp::core::ModelKind::Asr, "Speech only");
    ASSERT_TRUE(models.activate(a));
    ASSERT_TRUE(models.activate(voice));
    int probes = 0;
    models.set_prober([&](const auto&) {
        ++probes;
        wisp::core::ModelProbe p;
        p.ok = true;
        p.latency_ms = 123;
        return p;
    });
    open(wisp::shell::ConnectionKind::Llm);
    h->on_ui([&] {
        EXPECT_NE(wisp::test::find_label_containing(page()->list(), "Chat A"), nullptr);
        EXPECT_EQ(wisp::test::find_label_containing(page()->list(), "Speech only"), nullptr);
        page()->test_model(b);
        page()->activate_model(b);  // Busy: a test never activates a profile.
        EXPECT_TRUE(page()->busy());
    });
    EXPECT_EQ(probes, 0);
    EXPECT_EQ(queued(), 1u);
    finish();
    EXPECT_EQ(probes, 1);
    EXPECT_EQ(models.get().active_for(wisp::core::ModelKind::Llm), a);
    h->on_ui([&] {
        EXPECT_EQ(wisp::test::text_of(page()->result_label()), "连接正常 · 123 ms");
        page()->activate_model(b);
    });
    finish();
    EXPECT_EQ(models.get().active_for(wisp::core::ModelKind::Llm), b);
    EXPECT_EQ(models.get().active_for(wisp::core::ModelKind::Asr), voice);
    // Web deletion and additions appear from the shared service snapshot.
    ASSERT_TRUE(models.remove(a));
    make_model(wisp::core::ModelKind::Llm, "Chat C");
    h->on_ui([&] {
        page()->refresh();
        EXPECT_EQ(wisp::test::find_label_containing(page()->list(), "Chat A"), nullptr);
        EXPECT_NE(wisp::test::find_label_containing(page()->list(), "Chat C"), nullptr);
    });
}

TEST_F(ConnectionsTest, ProbeErrorIsLocalisedAndLeavingDuringProbeIsSafe) {
    boot();
    auto id = make_model(wisp::core::ModelKind::Tts, "Voice");
    models.set_prober([&](const auto&) {
        // Close and reopen the page while its probe is in progress.
        h->on_ui([&] { h->shell().show(wisp::shell::Page::Home); });
        wisp::core::ModelProbe p;
        p.error = "server-secret-should-not-appear";
        return p;
    });
    open(wisp::shell::ConnectionKind::Tts);
    h->on_ui([&] { page()->test_model(id); });
    finish();
    EXPECT_EQ(h->shell().connections_page(), nullptr);
    h->show(wisp::shell::Page::Settings);
    open(wisp::shell::ConnectionKind::Tts);
    models.set_prober([](const auto&) {
        wisp::core::ModelProbe p;
        p.error = "http";
        p.http_status = 401;
        return p;
    });
    h->on_ui([&] { page()->test_model(id); });
    finish();
    h->on_ui([&] {
        EXPECT_EQ(wisp::test::text_of(page()->result_label()), "认证失败，请在管理后台检查密钥");
    });
    reject = true;
    h->on_ui([&] {
        page()->activate_model(id);
        EXPECT_FALSE(page()->busy());
        EXPECT_EQ(wisp::test::text_of(page()->result_label()), "暂不可用");
    });
}

TEST_F(ConnectionsTest, WifiPasswordCancelOpenNetworkAndOffDuringScan) {
    boot("circle", 360);
    open(wisp::shell::ConnectionKind::Wifi);
    ASSERT_EQ(queued(), 1u);
    finish();
    h->on_ui([&] {
        page()->select_network("Locked", true, false, false);
        ASSERT_NE(page()->password(), nullptr);
        EXPECT_TRUE(lv_textarea_get_password_mode(page()->password()));
        EXPECT_EQ(lv_textarea_get_password_show_time(page()->password()), 0u);
        lv_textarea_set_text(page()->password(), "private-pass");
    });
    click(LV_SYMBOL_CLOSE);
    EXPECT_TRUE(wifi->saved_networks().empty());
    h->on_ui([&] {
        EXPECT_EQ(page()->password(), nullptr);
        page()->select_network("Open", false, false, false);
    });
    finish();
    ASSERT_EQ(wifi->saved_networks().size(), 1u);
    EXPECT_EQ(wifi->saved_networks()[0].ssid, "Open");
    h->on_ui([&] {
        page()->scan();
        lv_obj_remove_state(page()->wifi_switch(), LV_STATE_CHECKED);
        lv_obj_send_event(page()->wifi_switch(), LV_EVENT_VALUE_CHANGED, nullptr);
    });
    h->on_ui([] {});
    ASSERT_EQ(queued(), 2u);
    finish();  // Scan result cannot override the queued user choice.
    finish();
    EXPECT_FALSE(wifi->enabled());
    h->on_ui([&] {
        page()->select_network("Locked", true, false, false);
        EXPECT_EQ(page()->password(), nullptr);
    });
    EXPECT_EQ(queued(), 0u);
}

TEST_F(ConnectionsTest, WifiPasswordSavesOnlyOnConnectAndSavedSelectionUsesCredentials) {
    boot();
    open(wisp::shell::ConnectionKind::Wifi);
    finish();
    h->on_ui([&] {
        page()->select_network("Locked", true, false, false);
        lv_textarea_set_text(page()->password(), "short");
    });
    click(LV_SYMBOL_OK);
    EXPECT_TRUE(wifi->saved_networks().empty());
    h->on_ui([&] {
        ASSERT_NE(page()->password(), nullptr);
        lv_textarea_set_text(page()->password(), "password123");
    });
    click(LV_SYMBOL_OK);
    finish();
    ASSERT_EQ(wifi->saved_networks().size(), 1u);
    h->on_ui([&] {
        page()->select_network("Locked", true, true, false);
        EXPECT_EQ(page()->password(), nullptr);
    });
    finish();
    h->on_ui([&] {
        page()->select_network("Locked", true, true, true);
        EXPECT_NE(page()->password(), nullptr);
    });
    h->show(wisp::shell::Page::Home);
    h->on_ui([&] { EXPECT_EQ(h->layer().keyboard(), nullptr); });
}

namespace {
class ConnectionsShapes
    : public ConnectionsTest,
      public ::testing::WithParamInterface<std::tuple<const char*, wisp::ui::Lang>> {};
}  // namespace
TEST_P(ConnectionsShapes, LocalisedControlsStayInsideScreen) {
    const auto [shape, lang] = GetParam();
    boot(shape, 360, lang);
    make_model(wisp::core::ModelKind::Llm, "First provider");
    make_model(wisp::core::ModelKind::Llm, "A long provider name wraps lines");
    open(wisp::shell::ConnectionKind::Llm);
    auto check_button = [&](wisp::ui::StrId id) {
        h->on_ui([&] {
            auto find_control = [&](auto&& find, lv_obj_t* obj) -> lv_obj_t* {
                if (lv_obj_check_type(obj, &lv_label_class) &&
                    std::string(lv_label_get_text(obj)) == wisp::ui::tr(id) &&
                    lv_obj_get_parent(obj) != page()->list())
                    return obj;
                for (uint32_t i = 0; i < lv_obj_get_child_count(obj); ++i)
                    if (auto* found = find(find, lv_obj_get_child(obj, i))) return found;
                return nullptr;
            };
            auto* label = find_control(find_control, page()->list());
            ASSERT_NE(label, nullptr);
            auto* row = lv_obj_get_parent(label);
            lv_obj_update_layout(page()->list());
            auto bounds = wisp::test::coords_of(row);
            lv_obj_scroll_to_y(
                page()->list(),
                lv_obj_get_scroll_y(page()->list()) + (bounds.y1 + bounds.y2) / 2 - 180,
                LV_ANIM_OFF);
            lv_obj_update_layout(page()->list());
            const auto visible = wisp::test::coords_of(row);
            EXPECT_TRUE(wisp::test::box_visible(h->shape(), visible))
                << wisp::ui::tr(id) << " " << visible.x1 << "," << visible.y1 << " " << visible.x2
                << "," << visible.y2;
        });
    };
    click("A long");
    check_button(wisp::ui::StrId::ModelTest);
    check_button(wisp::ui::StrId::ModelActivate);
    h->show(wisp::shell::Page::Home);
    h->show(wisp::shell::Page::Settings);
    open(wisp::shell::ConnectionKind::Wifi);
    finish();
    h->on_ui([&] {
        EXPECT_NE(wisp::test::find_label_containing(page()->list(), LV_SYMBOL_REFRESH), nullptr);
    });
    // Network rows connect directly; connect is a password-view action.
    h->on_ui([&] {
        page()->select_network("Locked", true, false, false);
        lv_obj_update_layout(lv_screen_active());
        page()->refresh();
        const auto input = wisp::test::coords_of(page()->password());
        const auto keyboard = wisp::test::coords_of(h->layer().keyboard());
        EXPECT_TRUE(wisp::test::box_visible(h->shape(), input));
        const auto reveal =
            wisp::test::coords_of(lv_obj_get_child(lv_obj_get_parent(page()->password()), 1));
        EXPECT_LT(reveal.y2, keyboard.y1);
        EXPECT_LT(input.y2, keyboard.y1)
            << input.y1 << "," << input.y2 << " keyboard " << keyboard.y1;
        for (const char* icon : {LV_SYMBOL_CLOSE, LV_SYMBOL_OK, LV_SYMBOL_EYE_OPEN}) {
            auto* label = wisp::test::find_label_containing(page()->view(), icon);
            ASSERT_NE(label, nullptr);
            const auto glyph = wisp::test::coords_of(label);
            const auto button = wisp::test::coords_of(lv_obj_get_parent(label));
            EXPECT_GE(button.x2 - button.x1 + 1, 48);
            EXPECT_GE(button.y2 - button.y1 + 1, 48);
            EXPECT_LE(std::abs((glyph.x1 + glyph.x2) - (button.x1 + button.x2)), 2);
            EXPECT_LE(std::abs((glyph.y1 + glyph.y2) - (button.y1 + button.y2)), 2);
            EXPECT_TRUE(wisp::test::box_visible(h->shape(), button));
        }
    });
    click(LV_SYMBOL_CLOSE);
    h->on_ui([&] {
        EXPECT_EQ(page()->password(), nullptr);
        EXPECT_EQ(h->layer().keyboard(), nullptr);
    });
}
TEST_P(ConnectionsShapes, DetailActionsHaveGapsAndTrailingIconsAreCentred) {
    const auto [shape, lang] = GetParam();
    boot(shape, 360, lang);
    make_model(wisp::core::ModelKind::Llm, "Provider", "model-name");
    open(wisp::shell::ConnectionKind::Llm);
    auto centred = [](lv_obj_t* item, lv_obj_t* row) {
        const auto a = wisp::test::coords_of(item);
        const auto b = wisp::test::coords_of(row);
        EXPECT_LE(std::abs((a.y1 + a.y2) - (b.y1 + b.y2)), 2);
    };
    h->on_ui([&] {
        lv_obj_update_layout(page()->list());
        auto* arrow = wisp::test::find_label_containing(page()->list(), LV_SYMBOL_DOWN);
        ASSERT_NE(arrow, nullptr);
        centred(arrow, lv_obj_get_parent(arrow));
    });
    click("Provider");
    h->on_ui([&] {
        lv_obj_update_layout(page()->list());
        auto* arrow = wisp::test::find_label_containing(page()->list(), LV_SYMBOL_UP);
        ASSERT_NE(arrow, nullptr);
        centred(arrow, lv_obj_get_parent(arrow));
    });
    h->show(wisp::shell::Page::Home);
    h->show(wisp::shell::Page::Settings);
    ASSERT_TRUE(wifi->add_network("Locked", "password123", 0));
    wifi->tick(0);
    wifi->tick(1);
    open(wisp::shell::ConnectionKind::Wifi);
    finish();
    h->on_ui([&] {
        lv_obj_update_layout(page()->list());
        auto* name = wisp::test::find_label_containing(page()->list(), "Locked");
        ASSERT_NE(name, nullptr);
        auto* row = lv_obj_get_parent(lv_obj_get_parent(name));
        for (const char* symbol : {WISP_SYMBOL_LOCK, LV_SYMBOL_WIFI, WISP_SYMBOL_INFO}) {
            auto* icon = wisp::test::find_label_containing(row, symbol);
            ASSERT_NE(icon, nullptr);
            centred(icon, row);
        }
    });
    click("Locked");
    h->on_ui([&] {
        lv_obj_update_layout(page()->view());
        auto* connect = wisp::test::find_label_containing(
            page()->view(), wisp::ui::tr(wisp::ui::StrId::WifiDisconnect));
        auto* change = wisp::test::find_label_containing(
            page()->view(), wisp::ui::tr(wisp::ui::StrId::WifiChangePassword));
        ASSERT_NE(connect, nullptr);
        ASSERT_NE(change, nullptr);
        auto a = wisp::test::coords_of(lv_obj_get_parent(connect));
        auto b = wisp::test::coords_of(lv_obj_get_parent(change));
        EXPECT_GE(b.y1 - a.y2 - 1, wisp::ui::theme::kGapSm);
        lv_obj_scroll_to_y(page()->view(), LV_COORD_MAX, LV_ANIM_OFF);
        lv_obj_update_layout(page()->view());
        EXPECT_TRUE(
            wisp::test::box_visible(h->shape(), wisp::test::coords_of(lv_obj_get_parent(change))));
    });
}

INSTANTIATE_TEST_SUITE_P(ShapesAndLanguages, ConnectionsShapes,
                         ::testing::Combine(::testing::Values("rect", "rounded_rect", "circle"),
                                            ::testing::Values(wisp::ui::Lang::Zh,
                                                              wisp::ui::Lang::En,
                                                              wisp::ui::Lang::Ja)));

TEST_F(ConnectionsTest, AcceptedWifiSwitchPersistsAfterLeavingPage) {
    boot();
    open(wisp::shell::ConnectionKind::Wifi);
    finish();
    h->on_ui([&] {
        lv_obj_remove_state(page()->wifi_switch(), LV_STATE_CHECKED);
        lv_obj_send_event(page()->wifi_switch(), LV_EVENT_VALUE_CHANGED, nullptr);
    });
    h->on_ui([] {});
    ASSERT_EQ(queued(), 1u);
    h->show(wisp::shell::Page::Home);
    finish();
    EXPECT_FALSE(wifi->enabled());
}

TEST_F(ConnectionsTest, ModelRowsExpandOnlyOneAtATime) {
    boot();
    make_model(wisp::core::ModelKind::Llm, "Provider A");
    make_model(wisp::core::ModelKind::Llm, "Provider B");
    open(wisp::shell::ConnectionKind::Llm);
    h->on_ui(
        [&] { EXPECT_EQ(wisp::test::find_label_containing(page()->list(), "测试连接"), nullptr); });
    click("Provider A");
    h->on_ui(
        [&] { EXPECT_NE(wisp::test::find_label_containing(page()->list(), "测试连接"), nullptr); });
    click("Provider B");
    click("Provider B");
    h->on_ui(
        [&] { EXPECT_EQ(wisp::test::find_label_containing(page()->list(), "测试连接"), nullptr); });
}

TEST_F(ConnectionsTest, PendingSwitchDoesNotJumpBackOnRefresh) {
    boot();
    ASSERT_TRUE(wifi->add_network("Locked", "password123", 0));
    wifi->tick(0);
    wifi->tick(1);
    open(wisp::shell::ConnectionKind::Wifi);
    finish();
    h->on_ui([&] {
        lv_obj_remove_state(page()->wifi_switch(), LV_STATE_CHECKED);
        lv_obj_send_event(page()->wifi_switch(), LV_EVENT_VALUE_CHANGED, nullptr);
    });
    h->on_ui([&] {
        page()->refresh();
        EXPECT_FALSE(lv_obj_has_state(page()->wifi_switch(), LV_STATE_CHECKED));
        EXPECT_TRUE(lv_obj_has_state(page()->wifi_switch(), LV_STATE_DISABLED));
        EXPECT_EQ(wisp::test::find_label_containing(page()->list(), "Locked"), nullptr);
        EXPECT_EQ(wisp::test::find_label_containing(page()->list(), "Open"), nullptr);
        auto* pending = wisp::test::find_label_containing(page()->list(), "正在处理");
        ASSERT_NE(pending, nullptr);
        EXPECT_TRUE(lv_obj_is_visible(pending));
    });
    finish();
    h->on_ui([&] {
        EXPECT_EQ(wisp::test::find_label_containing(page()->list(), "Locked"), nullptr);
        EXPECT_EQ(wisp::test::find_label_containing(page()->list(), "Open"), nullptr);
        auto* off = wisp::test::find_label_containing(page()->list(), "已关闭");
        ASSERT_NE(off, nullptr);
        EXPECT_TRUE(lv_obj_is_visible(off));
    });
}

TEST_F(ConnectionsTest, PasswordRemainsAboveKeyboardAfterLiveRefresh) {
    boot("circle", 466);
    open(wisp::shell::ConnectionKind::Wifi);
    finish();
    lv_point_t target{};
    h->on_ui([&] {
        auto* name = wisp::test::find_label_containing(page()->list(), "Locked");
        ASSERT_NE(name, nullptr);
        lv_obj_update_layout(page()->list());
        const auto box = wisp::test::coords_of(name);
        target = {(box.x1 + box.x2) / 2, (box.y1 + box.y2) / 2};
    });
    h->tap(target);
    h->on_ui([&] {
        ASSERT_NE(page()->password(), nullptr);
        lv_textarea_set_text(page()->password(), "keep-typing");
        for (int i = 0; i < 3; ++i) page()->refresh();
        lv_obj_update_layout(lv_screen_active());
        const auto input = wisp::test::coords_of(page()->password());
        const auto kb = wisp::test::coords_of(h->layer().keyboard());
        EXPECT_TRUE(lv_obj_is_visible(page()->password()));
        EXPECT_TRUE(wisp::test::box_visible(h->shape(), input));
        EXPECT_LT(input.y2, kb.y1);
        EXPECT_EQ(std::string(lv_textarea_get_text(page()->password())), "keep-typing");
    });
}

TEST_F(ConnectionsTest, InfoTouchIsIndependentAndPasswordActionFollowsInput) {
    boot("circle", 466);
    open(wisp::shell::ConnectionKind::Wifi);
    finish();
    lv_obj_t* info = nullptr;
    h->on_ui([&] {
        auto* label = wisp::test::find_label_containing(page()->list(), WISP_SYMBOL_INFO);
        ASSERT_NE(label, nullptr);
        info = lv_obj_get_parent(label);
    });
    h->tap(h->centre_of(info));
    h->on_ui([&] {
        EXPECT_NE(page()->view(), page()->list());
        EXPECT_EQ(page()->password(), nullptr);
        EXPECT_TRUE(page()->back());
    });
    EXPECT_EQ(queued(), 0u);
    EXPECT_TRUE(wifi->saved_networks().empty());
    click("Locked");
    h->on_ui([&] {
        auto* label = wisp::test::find_label_containing(page()->view(), LV_SYMBOL_OK);
        ASSERT_NE(label, nullptr);
        auto* join = lv_obj_get_parent(label);
        EXPECT_TRUE(lv_obj_has_state(join, LV_STATE_DISABLED));
        lv_textarea_set_text(page()->password(), "short");
        EXPECT_TRUE(lv_obj_has_state(join, LV_STATE_DISABLED));
        lv_textarea_set_text(page()->password(), "password123");
        EXPECT_FALSE(lv_obj_has_state(join, LV_STATE_DISABLED));
    });
    click(LV_SYMBOL_CLOSE);
    EXPECT_TRUE(wifi->saved_networks().empty());
}

TEST_F(ConnectionsTest, ScanKeepsRefreshIconAndPhoneSetupHasGroupSpacing) {
    boot();
    open(wisp::shell::ConnectionKind::Wifi);
    lv_obj_t* icon = nullptr;
    lv_area_t button_bounds{};
    h->on_ui([&] {
        icon = wisp::test::find_label_containing(page()->list(), LV_SYMBOL_REFRESH);
        ASSERT_NE(icon, nullptr);
        button_bounds = wisp::test::coords_of(lv_obj_get_parent(icon));
        EXPECT_TRUE(lv_obj_is_visible(icon));
        EXPECT_NE(lv_anim_get(icon, nullptr), nullptr);
        EXPECT_EQ(lv_color_to_u32(lv_obj_get_style_text_color(icon, LV_PART_MAIN)),
                  lv_color_to_u32(lv_color_hex(wisp::ui::theme::kAccent)));
    });
    ASSERT_TRUE(
        h->ui_until([&] { return lv_obj_get_style_transform_rotation(icon, LV_PART_MAIN) > 0; }));
    h->on_ui([&] {
        const auto bounds = wisp::test::coords_of(lv_obj_get_parent(icon));
        EXPECT_EQ(bounds.x1, button_bounds.x1);
        EXPECT_EQ(bounds.x2, button_bounds.x2);
        EXPECT_EQ(bounds.y1, button_bounds.y1);
        EXPECT_EQ(bounds.y2, button_bounds.y2);
    });
    finish();
    h->on_ui([&] {
        EXPECT_TRUE(lv_obj_is_visible(icon));
        EXPECT_EQ(lv_anim_get(icon, nullptr), nullptr);
        EXPECT_EQ(lv_obj_get_style_transform_rotation(icon, LV_PART_MAIN), 0);
        lv_obj_update_layout(page()->list());
        auto* phone = wisp::test::find_label_containing(
            page()->list(), wisp::ui::tr(wisp::ui::StrId::WifiSetupPhone));
        ASSERT_NE(phone, nullptr);
        auto* card = lv_obj_get_parent(lv_obj_get_parent(phone));
        auto* networks = lv_obj_get_child(lv_obj_get_parent(card), lv_obj_get_index(card) - 1);
        EXPECT_GE(wisp::test::coords_of(card).y1 - wisp::test::coords_of(networks).y2 - 1,
                  wisp::ui::theme::kGapLg);
    });
    reject = true;
    h->on_ui([&] {
        page()->scan();
        EXPECT_TRUE(lv_obj_is_visible(icon));
        EXPECT_EQ(lv_anim_get(icon, nullptr), nullptr);
        EXPECT_EQ(lv_obj_get_style_transform_rotation(icon, LV_PART_MAIN), 0);
    });
}

TEST_F(ConnectionsTest, ModelProfilesShareAGroupWithCurrentCheckAndActionRows) {
    boot();
    auto current = make_model(wisp::core::ModelKind::Llm, "Current provider");
    make_model(wisp::core::ModelKind::Llm, "Other provider");
    ASSERT_TRUE(models.activate(current));
    open(wisp::shell::ConnectionKind::Llm);
    h->on_ui([&] {
        auto* a = wisp::test::find_label_containing(page()->list(), "Current provider");
        auto* b = wisp::test::find_label_containing(page()->list(), "Other provider");
        ASSERT_NE(a, nullptr);
        ASSERT_NE(b, nullptr);
        auto* group = lv_obj_get_parent(lv_obj_get_parent(lv_obj_get_parent(a)));
        EXPECT_EQ(group, lv_obj_get_parent(lv_obj_get_parent(lv_obj_get_parent(b))));
        EXPECT_NE(wisp::test::find_label_containing(group, LV_SYMBOL_OK), nullptr);
    });
    click("Other provider");
    h->on_ui([&] {
        auto* test = wisp::test::find_label_containing(page()->list(), "测试连接");
        ASSERT_NE(test, nullptr);
        EXPECT_NE(wisp::test::find_label_containing(lv_obj_get_parent(test), LV_SYMBOL_REFRESH),
                  nullptr);
    });
}

namespace {
class ConnectionsPreview : public ConnectionsTest, public ::testing::WithParamInterface<bool> {};
}  // namespace
TEST_P(ConnectionsPreview, ListFeedbackAndPasswordNavigation) {
    const bool circle = GetParam();
    const int size = circle ? 466 : 480;
    const std::string prefix = circle ? "circle-" : "square-";
    boot(circle ? "circle" : "rect", size);
    auto current = make_model(wisp::core::ModelKind::Llm, "OpenAI", "gpt-4.1");
    auto other = make_model(wisp::core::ModelKind::Llm, "DeepSeek", "deepseek-chat");
    make_model(wisp::core::ModelKind::Llm, "Qwen", "qwen-plus");
    ASSERT_TRUE(models.activate(current));
    models.set_prober([](const auto&) {
        wisp::core::ModelProbe result;
        result.ok = true;
        result.latency_ms = 328;
        return result;
    });
    open(wisp::shell::ConnectionKind::Llm);
    screenshot(prefix + "models", size);
    click("DeepSeek");
    click("测试连接");
    finish();
    screenshot(prefix + "model-test", size);
    EXPECT_EQ(models.get().active_for(wisp::core::ModelKind::Llm), current);
    h->show(wisp::shell::Page::Home);
    h->show(wisp::shell::Page::Settings);
    wifi_device->networks = {{"Home", -35, true}, {"Studio", -58, true}, {"Guest", -74, false}};
    wifi_device->networks[0].details.bssid = "02:11:22:33:44:55";
    wifi_device->networks[0].details.channel = 6;
    wifi_device->networks[0].details.security = "WPA2-Personal";
    open(wisp::shell::ConnectionKind::Wifi);
    finish();
    screenshot(prefix + "wifi", size);
    h->on_ui([&] { lv_obj_scroll_to_y(page()->list(), LV_COORD_MAX, LV_ANIM_OFF); });
    screenshot(prefix + "wifi-bottom", size);
    h->on_ui([&] { lv_obj_scroll_to_y(page()->list(), 0, LV_ANIM_OFF); });
    click("Home");
    h->on_ui([&] {
        ASSERT_NE(page()->password(), nullptr);
        lv_textarea_set_text(page()->password(), "password123");
        lv_obj_update_layout(page()->list());
        auto input = wisp::test::coords_of(page()->password());
        auto keyboard = wisp::test::coords_of(h->layer().keyboard());
        EXPECT_LT(input.y2, keyboard.y1);
    });
    screenshot(prefix + "password", size);
    // A page-local back restores the list without leaving Wi-Fi settings.
    h->on_ui([&] {
        EXPECT_TRUE(page()->back());
        EXPECT_EQ(page()->password(), nullptr);
        EXPECT_EQ(h->layer().keyboard(), nullptr);
        page()->select_network("Home", true, false, false);
        lv_textarea_set_text(page()->password(), "password123");
        auto* kb = h->layer().keyboard();
        uint32_t key = 0;
        const char* const* map = lv_buttonmatrix_get_map(kb);
        bool found = false;
        for (size_t i = 0; map[i] && *map[i]; ++i) {
            if (std::string(map[i]) == "\n") continue;
            if (std::string(map[i]) == LV_SYMBOL_OK) {
                found = true;
                break;
            }
            ++key;
        }
        ASSERT_TRUE(found);
        lv_buttonmatrix_set_selected_button(kb, key);
        lv_obj_send_event(kb, LV_EVENT_VALUE_CHANGED, nullptr);
    });
    h->on_ui([] {});
    ASSERT_EQ(queued(), 1u);
    finish();
    EXPECT_EQ(wifi->saved_networks().size(), 1u);
    wifi->tick(wisp::os::now_ms());
    auto link = wifi_device->link();
    link.details.gateway = "192.168.1.1";
    link.details.netmask = "255.255.255.0";
    link.details.dns = "192.168.1.2";
    link.details.mac = "02:AA:BB:CC:DD:EE";
    wifi_device->set_link(link);
    wifi->tick(wisp::os::now_ms() + 1);
    ASSERT_EQ(wifi->status().state, wisp::net::WifiState::Connected);
    h->on_ui([&] { page()->refresh(); });
    click("Home");
    screenshot(prefix + "wifi-details", size);
    h->on_ui([&] { lv_obj_scroll_to_y(page()->view(), LV_COORD_MAX, LV_ANIM_OFF); });
    screenshot(prefix + "wifi-details-bottom", size);
}
INSTANTIATE_TEST_SUITE_P(ScreenShapes, ConnectionsPreview, ::testing::Bool());

TEST_F(ConnectionsTest, PasswordModalValidationAndCancelRestoreList) {
    boot("circle", 360);
    open(wisp::shell::ConnectionKind::Wifi);
    finish();
    click("Locked");
    click(LV_SYMBOL_EYE_OPEN);
    h->on_ui([&] {
        EXPECT_FALSE(lv_textarea_get_password_mode(page()->password()));
        lv_textarea_set_text(page()->password(), "short");
    });
    click(LV_SYMBOL_OK);
    h->on_ui([&] {
        lv_obj_update_layout(page()->list());
        const auto keyboard = wisp::test::coords_of(h->layer().keyboard());
        auto* error = wisp::test::find_label_containing(page()->view(), "需8-63位");
        ASSERT_NE(error, nullptr);
        EXPECT_LT(wisp::test::coords_of(error).y2, keyboard.y1);
        EXPECT_LT(wisp::test::coords_of(page()->password()).y2, keyboard.y1);
        EXPECT_EQ(std::string(lv_textarea_get_text(page()->password())), "short");
    });
    // A modal blocks page navigation; cancellation keeps the original list.
    h->drag({150, 85}, {285, 85});
    h->on_ui([&] { EXPECT_NE(page()->password(), nullptr); });
    click(LV_SYMBOL_CLOSE);
    ASSERT_TRUE(h->ui_until([&] { return page() && !page()->password(); }));
    EXPECT_TRUE(wifi->saved_networks().empty());
    h->drag({60, 180}, {280, 180});
    ASSERT_TRUE(h->ui_until([&] { return page() == nullptr; }));
}

TEST_F(ConnectionsTest, CurrentNetworkAppearsOnceAndDetailsDoNotReconnect) {
    boot();
    ASSERT_TRUE(wifi->add_network("Locked", "password123", 0));
    wifi->tick(0);
    wifi->tick(1);
    ASSERT_EQ(wifi->status().state, wisp::net::WifiState::Connected);
    open(wisp::shell::ConnectionKind::Wifi);
    finish();
    h->on_ui([&] {
        size_t count = 0;
        auto visit = [&](auto&& visit, lv_obj_t* obj) -> void {
            if (lv_obj_check_type(obj, &lv_label_class) &&
                std::string(lv_label_get_text(obj)) == "Locked")
                ++count;
            for (uint32_t i = 0; i < lv_obj_get_child_count(obj); ++i)
                visit(visit, lv_obj_get_child(obj, i));
        };
        visit(visit, page()->list());
        EXPECT_EQ(count, 1u);
    });
    const auto calls = wifi_device->connect_calls.load();
    click("Locked");
    EXPECT_EQ(queued(), 0u);
    h->on_ui([&] {
        EXPECT_NE(wisp::test::find_label_containing(page()->view(), "192.168.1.50"), nullptr);
        EXPECT_NE(wisp::test::find_label_containing(page()->view(), "dBm"), nullptr);
        EXPECT_NE(wisp::test::find_label_containing(page()->view(), "安全性"), nullptr);
        EXPECT_NE(wisp::test::find_label_containing(page()->view(), "已保存"), nullptr);
        EXPECT_NE(wisp::test::find_label_containing(page()->view(), "断开连接"), nullptr);
        EXPECT_EQ(wisp::test::find_button_with_label(page()->view(), "连接"), nullptr);
        EXPECT_TRUE(page()->back());
    });
    EXPECT_EQ(wifi_device->connect_calls.load(), calls);
}

TEST_F(ConnectionsTest, ActivationFailureKeepsCurrentProfileAndExpandedFeedback) {
    boot();
    auto current = make_model(wisp::core::ModelKind::Llm, "Current");
    auto other = make_model(wisp::core::ModelKind::Llm, "Other");
    ASSERT_TRUE(models.activate(current));
    open(wisp::shell::ConnectionKind::Llm);
    h->on_ui([&] { page()->activate_model(other); });
    // A web edit may invalidate the selected profile before the queued activation runs.
    ASSERT_TRUE(models.remove(other));
    finish();
    EXPECT_EQ(models.get().active_for(wisp::core::ModelKind::Llm), current);
    h->on_ui([&] { EXPECT_FALSE(page()->busy()); });
}

TEST_F(ConnectionsTest, ConnectedDetailsDisconnectWithoutForgettingAndCanReconnect) {
    boot();
    ASSERT_TRUE(wifi->add_network("Locked", "password123", 0));
    wifi->tick(0);
    wifi->tick(1);
    open(wisp::shell::ConnectionKind::Wifi);
    finish();
    click("Locked");
    click("断开连接");
    finish();
    EXPECT_TRUE(wifi->enabled());
    EXPECT_EQ(wifi->status().state, wisp::net::WifiState::Off);
    EXPECT_EQ(wifi->saved_networks().size(), 1u);
    wifi->tick(10000);
    EXPECT_EQ(wifi->status().state, wisp::net::WifiState::Off);
    click("Locked");
    finish();
    wifi->tick(10001);
    wifi->tick(10002);
    EXPECT_EQ(wifi->status().state, wisp::net::WifiState::Connected);
}

TEST_P(ConnectionsShapes, DetailsScrollFromTextAndFollowFingerBackToNetworkList) {
    const auto [shape, lang] = GetParam();
    boot(shape, 360, lang);
    ASSERT_TRUE(wifi->add_network("Locked", "password123", 0));
    wifi->tick(0);
    auto link = wifi_device->link();
    link.details.gateway = "192.168.1.1";
    link.details.netmask = "255.255.255.0";
    link.details.dns = "192.168.1.2";
    link.details.mac = "02:AA:BB:CC:DD:EE";
    wifi_device->set_link(link);
    wifi->tick(1);
    open(wisp::shell::ConnectionKind::Wifi);
    finish();
    click("Locked");
    lv_point_t start{};
    h->on_ui([&] {
        for (const char* value : {"192.168.1.1", "255.255.255.0", "192.168.1.2",
                                  "02:AA:BB:CC:DD:EE", "02:11:22:33:44:55", "WPA2-Personal"})
            EXPECT_NE(wisp::test::find_label_containing(page()->view(), value), nullptr);
        EXPECT_EQ(wisp::test::find_label_containing(page()->view(), LV_SYMBOL_LEFT), nullptr);
        auto* name = wisp::test::find_label_containing(page()->view(), "Locked");
        ASSERT_NE(name, nullptr);
        lv_obj_update_layout(page()->view());
        const auto bounds = wisp::test::coords_of(name);
        start = {(bounds.x1 + bounds.x2) / 2, (bounds.y1 + bounds.y2) / 2};
    });
    h->drag(start, {start.x, start.y - 85});
    h->on_ui([&] { EXPECT_GT(lv_obj_get_scroll_y(page()->view()), 20); });
    ASSERT_TRUE(h->ui_until([&] { return !lv_obj_is_scrolling(page()->view()); }));
    h->feed({{80, 180, 0}});
    h->feed({{145, 180, 0}});
    h->on_ui([&] {
        ASSERT_NE(page()->detail_root(), nullptr);
        lv_obj_update_layout(lv_screen_active());
        EXPECT_GT(lv_obj_get_x(page()->detail_root()), 0);
        EXPECT_LT(lv_obj_get_x(page()->detail_root()), 360);
    });
    h->feed({{82, 180, 0}});
    h->feed({});
    ASSERT_TRUE(h->ui_until([&] {
        return page() && page()->detail_root() && lv_obj_get_x(page()->detail_root()) == 0;
    }));
    h->drag({70, 180}, {310, 180});
    ASSERT_TRUE(h->ui_until([&] { return page() && !page()->detail_root(); }));
    h->on_ui([&] { EXPECT_EQ(page()->view(), page()->list()); });
    EXPECT_EQ(h->shell().page(), wisp::shell::Page::Settings);
}

TEST_F(ConnectionsTest, UnconnectedDetailsUseScanMetadataAndPasswordModalPreservesDetails) {
    boot("circle", 360);
    open(wisp::shell::ConnectionKind::Wifi);
    finish();
    h->on_ui([&] {
        auto* name = wisp::test::find_label_containing(page()->list(), "Locked");
        ASSERT_NE(name, nullptr);
        auto* row = lv_obj_get_parent(lv_obj_get_parent(name));
        auto* info = wisp::test::find_label_containing(row, WISP_SYMBOL_INFO);
        ASSERT_NE(info, nullptr);
        lv_obj_send_event(lv_obj_get_parent(info), LV_EVENT_CLICKED, nullptr);
    });
    lv_obj_t* details = nullptr;
    int32_t scroll = 0;
    h->on_ui([&] {
        details = page()->detail_root();
        ASSERT_NE(details, nullptr);
        EXPECT_NE(wisp::test::find_label_containing(page()->view(), "WPA2-Personal"), nullptr);
        EXPECT_NE(wisp::test::find_label_containing(page()->view(), "02:11:22:33:44:55"), nullptr);
        EXPECT_EQ(wisp::test::find_label_containing(page()->view(), "192.168.1.50"), nullptr);
        EXPECT_NE(wisp::test::find_button_with_label(page()->view(),
                                                     wisp::ui::tr(wisp::ui::StrId::WifiConnect)),
                  nullptr);
        lv_obj_scroll_to_y(page()->view(), LV_COORD_MAX, LV_ANIM_OFF);
        lv_obj_update_layout(page()->view());
        scroll = lv_obj_get_scroll_y(page()->view());
    });
    h->on_ui([&] {
        auto* connect = wisp::test::find_button_with_label(
            page()->view(), wisp::ui::tr(wisp::ui::StrId::WifiConnect));
        ASSERT_NE(connect, nullptr);
        lv_obj_send_event(connect, LV_EVENT_CLICKED, nullptr);
    });
    h->on_ui([&] {
        ASSERT_NE(page()->password(), nullptr);
        EXPECT_EQ(page()->detail_root(), details);
        EXPECT_TRUE(h->layer().confirm_open());
        EXPECT_FALSE(lv_obj_has_flag(page()->list(), LV_OBJ_FLAG_HIDDEN));
        lv_textarea_set_text(page()->password(), "private-pass");
    });
    click(LV_SYMBOL_CLOSE);
    h->on_ui([&] {
        EXPECT_FALSE(h->layer().confirm_open());
        EXPECT_EQ(page()->detail_root(), details);
        EXPECT_EQ(lv_obj_get_scroll_y(page()->view()), scroll);
    });
    EXPECT_TRUE(wifi->saved_networks().empty());
}

TEST_F(ConnectionsTest, TurningWifiOffCancelsDetailDragAndPasswordModal) {
    boot();
    ASSERT_TRUE(wifi->add_network("Locked", "password123", 0));
    wifi->tick(0);
    wifi->tick(1);
    open(wisp::shell::ConnectionKind::Wifi);
    finish();
    click("Locked");
    h->feed({{80, 180, 0}});
    h->feed({{145, 180, 0}});
    ASSERT_TRUE(wifi->set_enabled(false, 2));
    h->on_ui([&] {
        page()->refresh();
        EXPECT_EQ(page()->detail_root(), nullptr);
        EXPECT_EQ(page()->view(), page()->list());
        EXPECT_EQ(wisp::test::find_label_containing(page()->list(), "Locked"), nullptr);
    });
    h->feed({});
    ASSERT_TRUE(wifi->set_enabled(true, 3));
    h->on_ui([&] {
        page()->refresh();
        page()->select_network("Other", true, false, false);
        ASSERT_NE(page()->password(), nullptr);
    });
    ASSERT_TRUE(wifi->set_enabled(false, 4));
    h->on_ui([&] {
        page()->refresh();
        EXPECT_EQ(page()->password(), nullptr);
        EXPECT_FALSE(h->layer().confirm_open());
        EXPECT_EQ(h->layer().keyboard(), nullptr);
    });
}

TEST_F(ConnectionsTest, KeyboardCancelDismissesPasswordModalWithoutSaving) {
    boot("circle", 360);
    open(wisp::shell::ConnectionKind::Wifi);
    finish();
    click("Locked");
    h->on_ui([&] {
        lv_textarea_set_text(page()->password(), "private-pass");
        auto* keyboard = h->layer().keyboard();
        uint32_t key = 0;
        const char* const* map = lv_buttonmatrix_get_map(keyboard);
        bool found = false;
        for (size_t i = 0; map[i] && *map[i]; ++i) {
            if (std::string(map[i]) == "\n") continue;
            if (std::string(map[i]) == LV_SYMBOL_KEYBOARD) {
                found = true;
                break;
            }
            ++key;
        }
        ASSERT_TRUE(found);
        lv_buttonmatrix_set_selected_button(keyboard, key);
        lv_obj_send_event(keyboard, LV_EVENT_VALUE_CHANGED, nullptr);
    });
    ASSERT_TRUE(h->ui_until([&] { return page() && !page()->password(); }));
    h->on_ui([&] {
        EXPECT_FALSE(h->layer().confirm_open());
        EXPECT_EQ(page()->view(), page()->list());
    });
    EXPECT_TRUE(wifi->saved_networks().empty());
}
