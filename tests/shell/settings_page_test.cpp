// The settings page (wisp/shell/settings_page.hpp, mvp spec 6.4) in the
// shell: what each control writes, and where; Wi-Fi and About.

#include "wisp/shell/settings_page.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "shell/page_fixture.hpp"
#include "wisp/core/storage_worker.hpp"

using wisp::Value;
using wisp::shell::Page;
using wisp::shell::SettingsPage;
using wisp::test::text_of;

namespace {

class SettingsPageTest : public ::testing::Test {
protected:
    void boot(const std::string& shape = "rect", int32_t w = 480, int32_t h = 480) {
        ASSERT_TRUE(worker.start());
        harness = std::make_unique<wisp::test::PageHarness>(shape, w, h);
        ASSERT_TRUE(harness->started());
        auto& d = harness->deps();
        d.settings = [] {
            wisp::core::SystemSettingsValues v{};
            v.brightness = 200;
            v.volume = 60;
            v.screen_off_s = 60;
            v.tts_mute = false;
            return v;
        };
        d.set_settings = [this](std::map<std::string, Value> m) {
            std::lock_guard<std::mutex> lock(mu);
            writes.push_back(std::move(m));
            all_on_worker = all_on_worker && worker.on_worker();
        };
        d.submit_io = [this](std::function<void()> job) { return worker.submit(std::move(job)); };
        d.wifi_status = [] { return std::string("已连接 WispLab  10.0.0.7"); };
        d.admin_url = [] { return std::string("http://10.0.0.7:8900"); };
        d.firmware_version = [] { return std::string("0.4.2"); };
        d.start_wifi_setup = [this] { ++wifi_setups; };
        d.open_diagnostics = [this] {
            ++diag_opens;
            diag = lv_obj_create(nullptr);
            lv_screen_load(diag);
        };
        d.close_diagnostics = [this] {
            lv_obj_delete(diag);
            diag = nullptr;
        };
        harness->boot();
        harness->show(Page::Settings);
        ASSERT_NE(page(), nullptr);
    }

    void TearDown() override {
        harness.reset();
        worker.stop();
    }

    SettingsPage* page() { return harness->shell().settings_page(); }
    std::vector<std::map<std::string, Value>> written() {
        (void)worker.drain(2000);
        std::lock_guard<std::mutex> lock(mu);
        return writes;
    }

    wisp::core::StorageWorker worker;
    std::unique_ptr<wisp::test::PageHarness> harness;
    std::mutex mu;
    std::vector<std::map<std::string, Value>> writes;
    bool all_on_worker = true;
    std::atomic<int> wifi_setups{0};
    std::atomic<int> diag_opens{0};
    lv_obj_t* diag = nullptr;
};

}  // namespace

TEST_F(SettingsPageTest, ControlsWriteSettings) {
    boot();
    harness->on_ui([&] {
        EXPECT_EQ(lv_slider_get_value(page()->brightness()), 200);
        EXPECT_EQ(lv_slider_get_value(page()->volume()), 60);
        EXPECT_FALSE(lv_obj_has_state(page()->mute(), LV_STATE_CHECKED));
    });
    // Dragging the slider writes nothing until the finger lets go, and the
    // drag is no swipe back home.
    lv_point_t start{}, end{};
    harness->on_ui([&] {
        lv_obj_t* s = page()->brightness();
        lv_obj_update_layout(s);
        const lv_area_t a = wisp::test::coords_of(s);
        const int32_t y = (a.y1 + a.y2) / 2;
        const int32_t w = a.x2 - a.x1;
        // 120 of 10..255.
        const int32_t x = a.x1 + w * (120 - 10) / (255 - 10);
        start = {a.x2 - 4, y};
        end = {x, y};
    });
    harness->feed({{static_cast<int16_t>(start.x), static_cast<int16_t>(start.y), 0}});
    for (int i = 1; i <= 8; ++i) {
        const int32_t x = start.x + (end.x - start.x) * i / 8;
        harness->feed({{static_cast<int16_t>(x), static_cast<int16_t>(start.y), 0}});
    }
    EXPECT_TRUE(written().empty());
    harness->feed({});
    EXPECT_EQ(harness->shell().page(), Page::Settings);
    auto w = written();
    ASSERT_EQ(w.size(), 1u);
    ASSERT_EQ(w[0].count("brightness"), 1u);
    EXPECT_NEAR(static_cast<double>(w[0]["brightness"].as_int()), 120.0, 3.0);
    // Pinned to exactly 120 for the record: a release writes the value.
    harness->on_ui([&] {
        lv_slider_set_value(page()->brightness(), 120, LV_ANIM_OFF);
        lv_obj_send_event(page()->brightness(), LV_EVENT_RELEASED, nullptr);
    });
    w = written();
    ASSERT_EQ(w.size(), 2u);
    EXPECT_EQ(w[1]["brightness"].as_int(), 120);

    harness->on_ui([&] {
        lv_obj_add_state(page()->mute(), LV_STATE_CHECKED);
        lv_obj_send_event(page()->mute(), LV_EVENT_VALUE_CHANGED, nullptr);
        lv_obj_t* never = wisp::test::find_button_with_label(page()->list(), "从不");
        ASSERT_NE(never, nullptr);
        EXPECT_EQ(never, page()->screen_off(5));
        lv_obj_send_event(never, LV_EVENT_CLICKED, nullptr);
    });
    w = written();
    ASSERT_EQ(w.size(), 4u);
    EXPECT_TRUE(w[2]["tts_mute"].as_bool());
    EXPECT_EQ(w[3]["screen_off_s"].as_int(), 0);
    harness->on_ui([&] {
        // The six choices, the current one marked.
        EXPECT_NE(wisp::test::find_button_with_label(page()->list(), "15 秒"), nullptr);
        EXPECT_NE(wisp::test::find_button_with_label(page()->list(), "10 分钟"), nullptr);
        // Three to a row, two rows.
        lv_obj_update_layout(page()->list());
        const auto y = [&](size_t i) { return wisp::test::coords_of(page()->screen_off(i)).y1; };
        EXPECT_EQ(y(0), y(2));
        EXPECT_EQ(y(3), y(5));
        EXPECT_GT(y(3), y(2));
    });
    std::lock_guard<std::mutex> lock(mu);
    EXPECT_TRUE(all_on_worker);
}

TEST_F(SettingsPageTest, WifiAndAbout) {
    boot();
    harness->on_ui([&] {
        EXPECT_EQ(text_of(page()->wifi_label()), "已连接 WispLab  10.0.0.7");
        EXPECT_EQ(text_of(page()->version_label()), "固件版本  0.4.2");
        EXPECT_EQ(text_of(page()->address_label()), "管理后台  http://10.0.0.7:8900");
        EXPECT_NE(wisp::test::find_label_containing(
                      page()->list(), "在浏览器打开管理后台地址，按提示输入设备上显示的配对码"),
                  nullptr);
        EXPECT_NE(wisp::test::find_button_with_label(page()->list(), "硬件诊断"), nullptr);
    });
    harness->on_ui([&] { lv_obj_scroll_to_view(page()->wifi_row(), LV_ANIM_OFF); });
    harness->tap(harness->centre_of(page()->wifi_row()));
    EXPECT_EQ(wifi_setups.load(), 1);

    // About -> hardware diagnostics, over the settings page. (Its back
    // swipe goes home, as from the serial `diag`.)
    harness->on_ui([&] {
        lv_obj_t* list = page()->list();
        lv_obj_scroll_to_y(list, LV_COORD_MAX, LV_ANIM_OFF);
    });
    harness->tap(harness->centre_of(page()->diagnostics()));
    ASSERT_TRUE(harness->ui_until([&] { return harness->shell().diagnostics_open(); }));
    EXPECT_EQ(diag_opens.load(), 1);
    EXPECT_EQ(harness->active(), diag);
}
