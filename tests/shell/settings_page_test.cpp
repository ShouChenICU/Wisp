// The settings page (wisp/shell/settings_page.hpp, mvp spec 6.4) in the
// shell: its groups and rows, what each control writes and where, the
// choice pages its value rows open, Wi-Fi, the system group and a
// language change.

#include "wisp/shell/settings_page.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <fstream>
#include <future>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "shell/page_fixture.hpp"
#include "support/cell_barrier.hpp"
#include "wisp/core/storage_worker.hpp"
#include "wisp/shell/choice_page.hpp"
#include "wisp/shell/home.hpp"

using wisp::Value;
using wisp::shell::Page;
using wisp::shell::SettingsPage;
using wisp::test::coords_of;
using wisp::test::text_of;
using Row = wisp::shell::SettingsPage::Row;

namespace {

class SettingsPageTest : public ::testing::Test {
protected:
    void boot(const std::string& shape = "rect", int32_t w = 480, int32_t h = 480,
              bool can_power = true) {
        ASSERT_TRUE(worker.start());
        harness = std::make_unique<wisp::test::PageHarness>(shape, w, h);
        ASSERT_TRUE(harness->started());
        auto& d = harness->deps();
        cell().set(stored);
        d.settings = &cell();
        // As SystemSettings::set(): a stored change reaches the cell. With
        // `refuse` the write is recorded and not stored.
        d.set_settings = [this](std::map<std::string, Value> m) {
            std::lock_guard<std::mutex> lock(mu);
            all_on_worker = all_on_worker && worker.on_worker();
            if (!refuse) {
                wisp::core::SettingsSnapshot v = cell().get();
                for (const auto& [key, value] : m) {
                    const wisp::core::SettingSpec* spec = wisp::core::find_setting(key);
                    ASSERT_NE(spec, nullptr) << key;
                    ASSERT_TRUE(
                        wisp::core::assign_setting(v, wisp::core::setting_id_of(*spec), value))
                        << key;
                }
                cell().set(v);
            }
            writes.push_back(std::move(m));
        };
        d.submit_io = [this](std::function<void()> job) {
            return !queue_full && worker.submit(std::move(job));
        };
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
        if (can_power) {
            d.restart = [this] { ++restarts; };
            d.power_off = [this] { ++power_offs; };
        }
        harness->boot();
        harness->show(Page::Settings);
        ASSERT_NE(page(), nullptr);
    }

    void TearDown() override {
        // A write still queued sets the harness's cell.
        (void)worker.drain(2000);
        harness.reset();
        worker.stop();
    }

    static wisp::core::SettingsSnapshot defaults() {
        wisp::core::SettingsSnapshot v{};
        v.brightness = 200;
        v.volume = 60;
        v.notify_vol = 40;
        v.screen_off_s = 60;
        v.deep_after_s = 300;
        v.language = wisp::core::LangCode::Zh;
        v.utc_offset_min = 480;
        v.time_24h = true;
        v.tts_mute = false;
        return v;
    }

    wisp::core::StateCell<wisp::core::SettingsSnapshot>& cell() { return harness->settings(); }
    // Every delivery of the cell so far has run on the UI thread.
    void settle() {
        ASSERT_TRUE(wisp::test::cell_barrier(cell(), &harness->ui()));
        harness->on_ui([] {});
    }
    SettingsPage* page() { return harness->shell().settings_page(); }
    wisp::shell::ChoicePage* choice() { return harness->shell().choice_page(); }
    std::vector<std::map<std::string, Value>> written() {
        (void)worker.drain(2000);
        std::lock_guard<std::mutex> lock(mu);
        return writes;
    }
    std::string value_of(Row r) {
        std::string s;
        harness->on_ui([&] { s = text_of(page()->value(r)); });
        return s;
    }
    // Scrolls `r` into view and taps it.
    void tap_row(Row r) {
        harness->on_ui([&] { lv_obj_scroll_to_view_recursive(page()->row(r), LV_ANIM_OFF); });
        harness->tap(harness->centre_of(page()->row(r)));
    }
    void tap_choice(size_t i) {
        harness->on_ui([&] { lv_obj_scroll_to_view_recursive(choice()->row(i), LV_ANIM_OFF); });
        harness->tap(harness->centre_of(choice()->row(i)));
    }
    bool back_on_settings() {
        return harness->ui_until([&] {
            return choice() == nullptr && lv_screen_active() == harness->shell().page_screen() &&
                   !harness->shell().sliding();
        });
    }
    // The choice rows' labels.
    std::vector<std::string> choice_labels() {
        std::vector<std::string> out;
        harness->on_ui([&] {
            for (size_t i = 0; i < choice()->count(); ++i)
                out.push_back(text_of(lv_obj_get_child(choice()->row(i), 0)));
        });
        return out;
    }

    wisp::core::StorageWorker worker;
    std::unique_ptr<wisp::test::PageHarness> harness;
    wisp::core::SettingsSnapshot stored = defaults();
    std::mutex mu;
    std::vector<std::map<std::string, Value>> writes;
    bool all_on_worker = true;
    std::atomic<bool> refuse{false};
    // The storage queue takes no job.
    std::atomic<bool> queue_full{false};
    std::atomic<int> wifi_setups{0};
    std::atomic<int> diag_opens{0};
    std::atomic<int> restarts{0}, power_offs{0};
    lv_obj_t* diag = nullptr;
};

}  // namespace

// The hand-written layout list (settings-schema spec 3.8) against the table:
// every key the page may write has exactly one row, and no other key has one.
TEST(SettingsPageLayout, EveryDevicePageKeyHasOneRow) {
    std::map<wisp::core::SettingId, int> rows;
    for (const wisp::shell::SettingRow& r : SettingsPage::setting_rows()) ++rows[r.id];
    for (const wisp::core::SettingSpec& spec : wisp::core::kSettingsSchema) {
        const wisp::core::SettingId id = wisp::core::setting_id_of(spec);
        const bool shown = (spec.flags & wisp::core::kSettingDevicePage) != 0;
        EXPECT_EQ(rows.count(id) != 0 ? rows[id] : 0, shown ? 1 : 0) << spec.name;
    }
}

TEST(SettingsPageLayout, EveryTimeZoneChoiceHasACity) {
    for (int32_t offset : wisp::core::kUtcOffsetChoices) {
        const auto& zones = SettingsPage::time_zones();
        EXPECT_EQ(std::count_if(zones.begin(), zones.end(),
                                [&](const auto& z) { return z.utc_offset_min == offset; }),
                  1)
            << offset;
    }
}

TEST(SettingsPageLayout, LanguageOrderIsAPermutationOfTheCodes) {
    const auto& order = SettingsPage::kLanguageOrder;
    const auto codes = wisp::core::setting_spec(wisp::core::SettingId::Language).enum_values;
    ASSERT_EQ(order.size(), codes.size());
    for (std::string_view code : codes) {
        EXPECT_EQ(
            std::count_if(order.begin(), order.end(),
                          [&](wisp::core::LangCode c) { return wisp::core::lang_code(c) == code; }),
            1)
            << code;
    }
}

TEST(SettingsPageLayout, PercentFollowsTheRange) {
    const auto& brightness = wisp::core::setting_spec(wisp::core::SettingId::Brightness);
    const auto& volume = wisp::core::setting_spec(wisp::core::SettingId::Volume);
    EXPECT_EQ(SettingsPage::percent(10, brightness), 0);
    EXPECT_EQ(SettingsPage::percent(255, brightness), 100);
    EXPECT_EQ(SettingsPage::percent(200, brightness), 78);
    EXPECT_EQ(SettingsPage::percent(60, volume), 60);
}

TEST_F(SettingsPageTest, GroupsAndRowsInOrder) {
    boot();
    harness->on_ui([&] {
        lv_obj_update_layout(page()->list());
        const char* headings[] = {"显示与声音", "屏幕与电源", "网络", "模型", "语言与时间", "系统"};
        ASSERT_EQ(page()->groups(), 6u);
        for (size_t i = 0; i < 6; ++i) {
            EXPECT_EQ(text_of(page()->heading(i)), headings[i]);
            // Each heading sits right above its card.
            EXPECT_LT(coords_of(page()->heading(i)).y2, coords_of(page()->card(i)).y1);
            if (i > 0)
                EXPECT_GT(coords_of(page()->heading(i)).y1, coords_of(page()->card(i - 1)).y2);
        }
        // The rows top to bottom.
        lv_obj_t* in_order[] = {page()->brightness(),          page()->volume(),
                                page()->notify_volume(),       page()->speak(),
                                page()->row(Row::ScreenOff),   page()->row(Row::DeepStandby),
                                page()->row(Row::Wifi),        page()->row(Row::Language),
                                page()->row(Row::TimeZone),    page()->row(Row::TimeFormat),
                                page()->row(Row::Version),     page()->row(Row::Address),
                                page()->row(Row::Diagnostics), page()->row(Row::Restart),
                                page()->row(Row::PowerOff)};
        for (size_t i = 1; i < std::size(in_order); ++i)
            EXPECT_GT(coords_of(in_order[i]).y1, coords_of(in_order[i - 1]).y1) << i;
        // Every row is a thumb's height.
        for (Row r : {Row::ScreenOff, Row::DeepStandby, Row::Wifi, Row::Language, Row::TimeZone,
                      Row::TimeFormat, Row::Version, Row::Address, Row::Diagnostics, Row::Restart,
                      Row::PowerOff})
            EXPECT_GE(lv_obj_get_height(page()->row(r)), 48);

        // The values.
        EXPECT_EQ(text_of(page()->brightness_value()), "78%");
        EXPECT_EQ(text_of(page()->volume_value()), "60%");
        EXPECT_EQ(text_of(page()->notify_value()), "40%");
        EXPECT_TRUE(lv_obj_has_state(page()->speak(), LV_STATE_CHECKED));
        EXPECT_EQ(text_of(page()->value(Row::ScreenOff)), "1 分钟");
        EXPECT_EQ(text_of(page()->value(Row::DeepStandby)), "5 分钟");
        EXPECT_EQ(text_of(page()->value(Row::Wifi)), "已连接 WispLab  10.0.0.7");
        EXPECT_EQ(text_of(page()->value(Row::Language)), "简体中文");
        EXPECT_EQ(text_of(page()->value(Row::TimeZone)), "UTC+08:00 北京");
        EXPECT_EQ(text_of(page()->value(Row::TimeFormat)), "24 小时制");
        EXPECT_EQ(text_of(page()->value(Row::Version)), "0.4.2");
        EXPECT_EQ(text_of(page()->value(Row::Address)), "http://10.0.0.7:8900");
        EXPECT_EQ(wisp::test::find_label_containing(page()->list(), "深度待机时让芯片浅睡省电"),
                  nullptr);
        EXPECT_NE(wisp::test::find_label_containing(
                      page()->list(), "在浏览器打开管理后台地址，按提示输入设备上显示的配对码"),
                  nullptr);
        EXPECT_NE(wisp::test::find_label_containing(page()->list(), "硬件诊断"), nullptr);
    });
}

// A long value is cut short on its row rather than wrapped past it.
TEST_F(SettingsPageTest, LongValuesStayOnTheirRow) {
    boot("circle", 360, 360);
    harness->on_ui([&] {
        lv_label_set_text(page()->value(Row::Wifi),
                          "已连接 一个非常非常长的无线网络名称用来测试截断  192.168.100.200");
        lv_obj_update_layout(page()->list());
        const lv_area_t row = coords_of(page()->row(Row::Wifi));
        const lv_area_t v = coords_of(page()->value(Row::Wifi));
        EXPECT_LE(v.x2, row.x2);
        EXPECT_GE(v.y1, row.y1);
        EXPECT_LE(v.y2, row.y2);
        EXPECT_LE(lv_obj_get_height(page()->row(Row::Wifi)), 48 + 1);
    });
}

// The admin address is what the user types into a browser, so it is never
// cut off: it gets its own line under the row's name, and wraps if even
// that is too narrow.
class SettingsAddressTest : public SettingsPageTest {
protected:
    void check(const std::string& shape, int32_t size) {
        boot(shape, size, size);
        harness->on_ui([&] {
            lv_obj_t* v = page()->value(Row::Address);
            lv_label_set_text(v, "http://192.168.100.200:8900");
            lv_obj_update_layout(page()->list());
            const lv_area_t row = coords_of(page()->row(Row::Address));
            const lv_area_t va = coords_of(v);
            EXPECT_EQ(lv_label_get_long_mode(v), LV_LABEL_LONG_WRAP);
            EXPECT_GE(va.y1, row.y2);
            EXPECT_GE(va.x1, row.x1);
            EXPECT_LE(va.x2, row.x2);
            lv_point_t full;
            lv_text_get_size(&full, lv_label_get_text(v),
                             lv_obj_get_style_text_font(v, LV_PART_MAIN), 0, 0, LV_COORD_MAX,
                             LV_TEXT_FLAG_NONE);
            const int32_t width = std::max<int32_t>(1, lv_obj_get_content_width(v));
            const int32_t lines = (full.x + width - 1) / width;
            EXPECT_GE(lv_obj_get_height(v), lines * full.y);
            lv_obj_t* hint =
                wisp::test::find_label_containing(page()->list(), "在浏览器打开管理后台地址");
            ASSERT_NE(hint, nullptr);
            EXPECT_GE(coords_of(hint).y1, va.y2);
        });
    }
};

TEST_F(SettingsAddressTest, Circle466) { check("circle", 466); }
TEST_F(SettingsAddressTest, Circle360) { check("circle", 360); }
TEST_F(SettingsAddressTest, RoundedRect480) { check("rounded_rect", 480); }

TEST_F(SettingsPageTest, ControlsWriteSettings) {
    boot();
    harness->on_ui([&] {
        EXPECT_EQ(lv_slider_get_value(page()->brightness()), 200);
        EXPECT_EQ(lv_slider_get_value(page()->volume()), 60);
        EXPECT_EQ(lv_slider_get_value(page()->notify_volume()), 40);
    });
    // Dragging the slider writes nothing until the finger lets go, and the
    // drag is no swipe back home.
    lv_point_t start{}, end{};
    harness->on_ui([&] {
        lv_obj_t* s = page()->brightness();
        lv_obj_update_layout(s);
        const lv_area_t a = coords_of(s);
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
    // The value beside the name follows the finger.
    harness->on_ui([&] {
        const int v = lv_slider_get_value(page()->brightness());
        EXPECT_EQ(text_of(page()->brightness_value()),
                  std::to_string(SettingsPage::percent(
                      v, wisp::core::setting_spec(wisp::core::SettingId::Brightness))) +
                      "%");
    });
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
        lv_slider_set_value(page()->notify_volume(), 25, LV_ANIM_OFF);
        lv_obj_send_event(page()->notify_volume(), LV_EVENT_VALUE_CHANGED, nullptr);
        EXPECT_EQ(text_of(page()->notify_value()), "25%");
        lv_obj_send_event(page()->notify_volume(), LV_EVENT_RELEASED, nullptr);
    });
    w = written();
    ASSERT_EQ(w.size(), 3u);
    EXPECT_EQ(w[1]["brightness"].as_int(), 120);
    EXPECT_EQ(w[2]["notify_vol"].as_int(), 25);

    harness->on_ui([&] {
        // Spoken replies off: tts_mute on.
        lv_obj_remove_state(page()->speak(), LV_STATE_CHECKED);
        lv_obj_send_event(page()->speak(), LV_EVENT_VALUE_CHANGED, nullptr);
    });
    w = written();
    ASSERT_EQ(w.size(), 4u);
    EXPECT_TRUE(w[3]["tts_mute"].as_bool());
    std::lock_guard<std::mutex> lock(mu);
    EXPECT_TRUE(all_on_worker);
}

namespace {

struct ChoiceCase {
    Row row;
    const char* title;
    std::vector<std::string> labels;  // empty: not checked (the time zones)
    int current;
    size_t pick;
    const char* key;
    Value value;
    const char* shown;
};

class SettingsChoice : public SettingsPageTest, public ::testing::WithParamInterface<ChoiceCase> {};

}  // namespace

// Each value row's choice page: it opens with the current choice marked;
// a tap writes the setting, comes back and the row shows the new value.
TEST_P(SettingsChoice, PickWritesAndComesBack) {
    const ChoiceCase& c = GetParam();
    boot();
    tap_row(c.row);
    ASSERT_TRUE(harness->ui_until([&] { return choice() != nullptr; }));
    harness->on_ui([&] {
        EXPECT_EQ(lv_screen_active(), harness->shell().choice_screen());
        EXPECT_EQ(harness->shell().page(), Page::Settings);
        EXPECT_NE(wisp::test::find_label_containing(harness->shell().choice_screen(), c.title),
                  nullptr);
        EXPECT_EQ(choice()->current(), c.current);
    });
    if (!c.labels.empty()) EXPECT_EQ(choice_labels(), c.labels);
    harness->on_ui([&] {
        lv_obj_t* mark = lv_obj_get_child(choice()->row(static_cast<size_t>(c.current)), 1);
        EXPECT_FALSE(lv_obj_has_flag(mark, LV_OBJ_FLAG_HIDDEN));
        lv_obj_t* other = lv_obj_get_child(choice()->row(c.pick), 1);
        EXPECT_TRUE(lv_obj_has_flag(other, LV_OBJ_FLAG_HIDDEN));
    });
    tap_choice(c.pick);
    ASSERT_TRUE(back_on_settings());
    const auto w = written();
    ASSERT_EQ(w.size(), 1u);
    ASSERT_EQ(w[0].count(c.key), 1u);
    EXPECT_EQ(w[0].at(c.key), c.value);
    EXPECT_EQ(value_of(c.row), c.shown);
}

// A swipe to the right goes back without writing anything.
TEST_P(SettingsChoice, SwipeBackWritesNothing) {
    const ChoiceCase& c = GetParam();
    boot();
    const std::string before = value_of(c.row);
    tap_row(c.row);
    ASSERT_TRUE(harness->ui_until([&] { return choice() != nullptr; }));
    const int32_t y = harness->shape().height / 2;
    harness->drag({60, y}, {60 + harness->shape().width / 2, y});
    ASSERT_TRUE(back_on_settings());
    EXPECT_TRUE(written().empty());
    EXPECT_EQ(value_of(c.row), before);
    EXPECT_EQ(harness->shell().page(), Page::Settings);
}

// The choices (outside the macro, which splits its arguments at commas).
const ChoiceCase kChoiceCases[] = {
    ChoiceCase{Row::ScreenOff,
               "自动熄屏",
               {"15 秒", "30 秒", "1 分钟", "2 分钟", "5 分钟", "10 分钟", "从不"},
               2,
               6,
               "screen_off_s",
               Value(int64_t{0}),
               "从不"},
    ChoiceCase{Row::DeepStandby,
               "深度待机",
               {"1 分钟", "2 分钟", "5 分钟", "10 分钟", "30 分钟", "1 小时", "从不"},
               2,
               5,
               "deep_after_s",
               Value(int64_t{3600}),
               "1 小时"},
    ChoiceCase{Row::Language,
               "系统语言",
               {"简体中文", "English", "日本語"},
               0,
               2,
               "language",
               Value("ja"),
               "日本語"},
    ChoiceCase{Row::TimeZone,
               "时区",
               {},
               27,
               22,
               "utc_offset_min",
               Value(int64_t{330}),
               "UTC+05:30 新德里"},
    ChoiceCase{Row::TimeFormat,
               "时间格式",
               {"24 小时制", "12 小时制"},
               0,
               1,
               "time_24h",
               Value(false),
               "12 小时制"}};

std::string choice_case_name(const ::testing::TestParamInfo<ChoiceCase>& info) {
    const char* names[] = {"ScreenOff", "DeepStandby", "Wifi",    "Language",
                           "TimeZone",  "TimeFormat",  "Version", "Address"};
    return names[static_cast<size_t>(info.param.row)];
}

INSTANTIATE_TEST_SUITE_P(Rows, SettingsChoice, ::testing::ValuesIn(kChoiceCases), choice_case_name);

// The time zones run from UTC-12:00 to UTC+14:00, each with its city, and
// the list opens scrolled to the current one.
TEST_F(SettingsPageTest, TimeZonesOpenAtTheCurrentOne) {
    stored.utc_offset_min = 765;
    boot("circle", 466, 466);
    EXPECT_EQ(value_of(Row::TimeZone), "UTC+12:45 查塔姆群岛");
    tap_row(Row::TimeZone);
    ASSERT_TRUE(harness->ui_until([&] { return choice() != nullptr; }));
    const std::vector<std::string> labels = choice_labels();
    ASSERT_EQ(labels.size(), SettingsPage::time_zones().size());
    EXPECT_EQ(labels.front(), "UTC-12:00 贝克岛");
    EXPECT_EQ(labels.back(), "UTC+14:00 莱恩群岛");
    EXPECT_NE(std::find(labels.begin(), labels.end(), "UTC+05:45 加德满都"), labels.end());
    EXPECT_NE(std::find(labels.begin(), labels.end(), "UTC+09:30 阿德莱德"), labels.end());
    harness->on_ui([&] {
        lv_obj_update_layout(choice()->list());
        EXPECT_GT(lv_obj_get_scroll_y(choice()->list()), 0);
        const size_t cur = static_cast<size_t>(choice()->current());
        EXPECT_EQ(labels[cur], "UTC+12:45 查塔姆群岛");
        EXPECT_TRUE(wisp::test::box_visible(harness->shape(), coords_of(choice()->row(cur))));
    });
}

// A value the list lacks (set on the admin page) is listed in its place
// and marked.
TEST_F(SettingsPageTest, StoredValueOutsideTheListIsListed) {
    stored.screen_off_s = 45;
    stored.utc_offset_min = 15;
    boot();
    EXPECT_EQ(value_of(Row::ScreenOff), "45 秒");
    EXPECT_EQ(value_of(Row::TimeZone), "UTC+00:15");
    tap_row(Row::ScreenOff);
    ASSERT_TRUE(harness->ui_until([&] { return choice() != nullptr; }));
    EXPECT_EQ(choice_labels(), (std::vector<std::string>{"15 秒", "30 秒", "45 秒", "1 分钟",
                                                         "2 分钟", "5 分钟", "10 分钟", "从不"}));
    harness->on_ui([&] { EXPECT_EQ(choice()->current(), 2); });
    harness->drag({60, harness->shape().height / 2},
                  {60 + harness->shape().width / 2, harness->shape().height / 2});
    ASSERT_TRUE(back_on_settings());
    // The time zones: UTC+00:15 between UTC+00:00 and UTC+01:00, not before
    // the zero offset as if it meant "never".
    tap_row(Row::TimeZone);
    ASSERT_TRUE(harness->ui_until([&] { return choice() != nullptr; }));
    const std::vector<std::string> zones = choice_labels();
    harness->on_ui([&] {
        const size_t cur = static_cast<size_t>(choice()->current());
        ASSERT_GT(cur, 0u);
        ASSERT_LT(cur + 1, zones.size());
        EXPECT_EQ(zones[cur], "UTC+00:15");
        EXPECT_EQ(zones[cur - 1].rfind("UTC+00:00", 0), 0u) << zones[cur - 1];
        EXPECT_EQ(zones[cur + 1].rfind("UTC+01:00", 0), 0u) << zones[cur + 1];
    });
}

// A new language re-texts the settings page at once, in place and where
// it was scrolled to, and the home screen with it.
TEST_F(SettingsPageTest, LanguageChangeRetextsThePages) {
    boot();
    tap_row(Row::Language);
    ASSERT_TRUE(harness->ui_until([&] { return choice() != nullptr; }));
    tap_choice(1);
    ASSERT_TRUE(back_on_settings());
    auto w = written();
    ASSERT_EQ(w.size(), 1u);
    EXPECT_EQ(w[0]["language"], Value("en"));
    int32_t y = 0;
    harness->on_ui([&] { y = lv_obj_get_scroll_y(page()->list()); });
    // What the system duties do with the system.settings event.
    harness->on_ui([&] { wisp::ui::set_lang(wisp::ui::Lang::En); });
    ASSERT_TRUE(harness->ui_until(
        [&] { return page() != nullptr && text_of(page()->heading(0)) == "Display & sound"; }));
    harness->on_ui([&] {
        EXPECT_EQ(lv_screen_active(), harness->shell().page_screen());
        EXPECT_EQ(harness->shell().page(), Page::Settings);
        EXPECT_NE(wisp::test::find_label_containing(page()->list(), "Settings"), nullptr);
        EXPECT_EQ(text_of(page()->value(Row::Language)), "English");
        EXPECT_EQ(text_of(page()->value(Row::ScreenOff)), "1 min");
        EXPECT_EQ(lv_obj_get_scroll_y(page()->list()), y);
    });
    harness->show(Page::Home);
    harness->on_ui([&] {
        // The home screen follows too.
        wisp::shell::Home* home = harness->shell().home();
        ASSERT_NE(home, nullptr);
        wisp::shell::TurnView v;
        v.not_heard = true;
        home->show_view(v);
        EXPECT_EQ(text_of(home->hint_label()),
                  wisp::ui::tr(wisp::ui::StrId::HomeNotHeard, wisp::ui::Lang::En));
    });
    harness->on_ui([&] { wisp::ui::set_lang(wisp::ui::Lang::Zh); });
}

TEST_F(SettingsPageTest, WifiAndSystem) {
    boot();
    tap_row(Row::Wifi);
    ASSERT_TRUE(harness->ui_until([&] { return harness->shell().connections_page() != nullptr; }));
    EXPECT_EQ(wifi_setups.load(), 0);
    harness->on_ui([&] { harness->shell().show(Page::Settings); });
    EXPECT_EQ(harness->shell().choice_page(), nullptr);

    // System -> hardware diagnostics, over the settings page. (Its back
    // swipe goes home, as from the serial `diag`.)
    harness->on_ui([&] { lv_obj_scroll_to_y(page()->list(), LV_COORD_MAX, LV_ANIM_OFF); });
    harness->tap(harness->centre_of(page()->row(Row::Diagnostics)));
    ASSERT_TRUE(harness->ui_until([&] { return harness->shell().diagnostics_open(); }));
    EXPECT_EQ(diag_opens.load(), 1);
    EXPECT_EQ(harness->active(), diag);
}

// Restart and power off ask first: No does nothing, Yes does it (on the UI
// thread, after the dialog's own event).
TEST_F(SettingsPageTest, RestartAndPowerOffAskFirst) {
    boot();
    struct Case {
        Row row;
        const char* prompt;
        std::atomic<int>* calls;
    };
    for (const Case& c : {Case{Row::Restart, "是否重启设备？", &restarts},
                          Case{Row::PowerOff, "是否关机？", &power_offs}}) {
        SCOPED_TRACE(c.prompt);
        ASSERT_NE(page()->row(c.row), nullptr);
        EXPECT_EQ(page()->value(c.row), nullptr);
        tap_row(c.row);
        ASSERT_TRUE(harness->ui_until([&] { return page()->confirm() != nullptr; }));
        harness->on_ui([&] {
            EXPECT_NE(wisp::test::find_label_containing(lv_layer_top(), c.prompt), nullptr);
        });
        ASSERT_TRUE(harness->click_dialog("否"));
        ASSERT_TRUE(harness->ui_until([&] { return !harness->layer().confirm_open(); }));
        harness->on_ui([] {});  // what a No posted, if anything, has run
        EXPECT_EQ(c.calls->load(), 0);
        EXPECT_EQ(page()->confirm(), nullptr);

        tap_row(c.row);
        ASSERT_TRUE(harness->ui_until([&] { return page()->confirm() != nullptr; }));
        ASSERT_TRUE(harness->click_dialog("是"));
        ASSERT_TRUE(harness->ui_until([&] { return c.calls->load() == 1; }));
    }
    EXPECT_EQ(restarts.load(), 1);
    EXPECT_EQ(power_offs.load(), 1);
    EXPECT_TRUE(written().empty());
}

// Leaving the page with the question open takes the question away
// unanswered.
TEST_F(SettingsPageTest, LeavingClosesTheQuestion) {
    boot();
    tap_row(Row::PowerOff);
    ASSERT_TRUE(harness->ui_until([&] { return page()->confirm() != nullptr; }));
    harness->show(Page::Home);
    ASSERT_TRUE(harness->ui_until([&] { return !harness->layer().confirm_open(); }));
    EXPECT_EQ(power_offs.load(), 0);
}

// Without a way to restart or power off, neither row is there.
TEST_F(SettingsPageTest, NoRestartOrPowerOffWithoutThePlatform) {
    boot("rect", 480, 480, false);
    EXPECT_EQ(page()->row(Row::Restart), nullptr);
    EXPECT_EQ(page()->row(Row::PowerOff), nullptr);
    EXPECT_NE(page()->row(Row::Diagnostics), nullptr);
}

// A change stored elsewhere (by voice, on the admin page) shows on the
// open page at once, on every kind of control.
TEST_F(SettingsPageTest, ChangeWhileOpenIsShown) {
    boot();
    wisp::core::SettingsSnapshot v = cell().get();
    v.volume = 25;
    v.screen_off_s = 300;
    v.language = wisp::core::LangCode::Ja;
    v.tts_mute = true;
    v.time_24h = false;
    cell().set(v);
    settle();
    harness->on_ui([&] {
        EXPECT_EQ(lv_slider_get_value(page()->volume()), 25);
        EXPECT_EQ(text_of(page()->volume_value()), "25%");
        EXPECT_EQ(text_of(page()->value(Row::ScreenOff)), "5 分钟");
        EXPECT_EQ(text_of(page()->value(Row::Language)), "日本語");
        EXPECT_EQ(text_of(page()->value(Row::TimeFormat)), "12 小时制");
        EXPECT_FALSE(lv_obj_has_state(page()->speak(), LV_STATE_CHECKED));
        EXPECT_EQ(page()->values(), v);
    });
    EXPECT_TRUE(written().empty());
}

// A write the store refuses shows until it has run; then the row is back
// at the stored value, with no other change needed.
TEST_F(SettingsPageTest, RefusedWriteShowsTheStoredValue) {
    boot();
    refuse = true;
    // Held, so the row is seen while the write is in flight.
    std::promise<void> gate;
    ASSERT_TRUE(worker.submit([f = gate.get_future().share()] { f.wait(); }));
    tap_row(Row::ScreenOff);
    ASSERT_TRUE(harness->ui_until([&] { return choice() != nullptr; }));
    tap_choice(6);
    ASSERT_TRUE(back_on_settings());
    EXPECT_EQ(value_of(Row::ScreenOff), "从不");
    gate.set_value();
    ASSERT_EQ(written().size(), 1u);
    // The write's completion was posted before written()'s drain returned.
    settle();
    EXPECT_EQ(value_of(Row::ScreenOff), "1 分钟");
    harness->on_ui([&] { EXPECT_EQ(page()->values().screen_off_s, 60); });
}

// A write still in flight keeps its row when an earlier write's delivery
// (a whole snapshot, older for that row) arrives.
TEST_F(SettingsPageTest, EarlierDeliveryLeavesALaterWriteAlone) {
    boot();
    std::promise<void> first, second;
    ASSERT_TRUE(worker.submit([f = first.get_future().share()] { f.wait(); }));
    harness->on_ui([&] {
        lv_slider_set_value(page()->volume(), 30, LV_ANIM_OFF);
        lv_obj_send_event(page()->volume(), LV_EVENT_RELEASED, nullptr);
    });
    ASSERT_TRUE(worker.submit([f = second.get_future().share()] { f.wait(); }));
    harness->on_ui([&] {
        // Spoken replies off: tts_mute on.
        lv_obj_remove_state(page()->speak(), LV_STATE_CHECKED);
        lv_obj_send_event(page()->speak(), LV_EVENT_VALUE_CHANGED, nullptr);
    });
    first.set_value();
    // The volume is stored and delivered; tts_mute is not stored yet.
    ASSERT_TRUE(wisp::test::wait_until([&] { return cell().get().volume == 30; }, 2000));
    settle();
    harness->on_ui([&] {
        EXPECT_FALSE(lv_obj_has_state(page()->speak(), LV_STATE_CHECKED));
        EXPECT_TRUE(page()->values().tts_mute);
        EXPECT_EQ(lv_slider_get_value(page()->volume()), 30);
    });
    EXPECT_FALSE(cell().get().tts_mute);
    second.set_value();
    ASSERT_EQ(written().size(), 2u);
    settle();
    harness->on_ui([&] {
        EXPECT_FALSE(lv_obj_has_state(page()->speak(), LV_STATE_CHECKED));
        EXPECT_EQ(page()->values(), cell().get());
    });
}

// A write the storage queue does not take puts the row back at once.
TEST_F(SettingsPageTest, WriteNotQueuedPutsTheRowBack) {
    boot();
    queue_full = true;
    tap_row(Row::ScreenOff);
    ASSERT_TRUE(harness->ui_until([&] { return choice() != nullptr; }));
    tap_choice(6);
    ASSERT_TRUE(back_on_settings());
    EXPECT_EQ(value_of(Row::ScreenOff), "1 分钟");
    EXPECT_TRUE(written().empty());
}

// Leaving the page releases its subscription; a change after that runs no
// page code. Opened again, it follows again.
TEST_F(SettingsPageTest, ClosingThePageReleasesItsSubscription) {
    boot();
    settle();
    const uint32_t open = harness->bus().cell_subscriptions();
    harness->show(Page::Home);
    EXPECT_EQ(page(), nullptr);
    EXPECT_EQ(harness->bus().cell_subscriptions(), open - 1);
    wisp::core::SettingsSnapshot v = cell().get();
    v.volume = 10;
    cell().set(v);
    settle();
    harness->show(Page::Settings);
    ASSERT_NE(page(), nullptr);
    EXPECT_EQ(harness->bus().cell_subscriptions(), open);
    v.volume = 35;
    cell().set(v);
    settle();
    harness->on_ui([&] { EXPECT_EQ(text_of(page()->volume_value()), "35%"); });
}

// A delivery while the finger holds a slider leaves that slider to the
// finger (the rest of the page follows); the release writes the finger's
// value.
TEST_F(SettingsPageTest, HeldSliderKeepsTheFingersValue) {
    boot();
    lv_point_t start{}, end{};
    harness->on_ui([&] {
        lv_obj_t* s = page()->volume();
        lv_obj_update_layout(s);
        const lv_area_t a = coords_of(s);
        const int32_t y = (a.y1 + a.y2) / 2;
        start = {a.x1 + (a.x2 - a.x1) * 60 / 100, y};
        end = {a.x1 + (a.x2 - a.x1) * 90 / 100, y};
    });
    for (int i = 0; i <= 8; ++i) {
        const int32_t x = start.x + (end.x - start.x) * i / 8;
        harness->feed({{static_cast<int16_t>(x), static_cast<int16_t>(start.y), 0}});
    }
    int held = 0;
    harness->on_ui([&] { held = lv_slider_get_value(page()->volume()); });
    ASSERT_GT(held, 75);
    wisp::core::SettingsSnapshot v = cell().get();
    v.volume = 5;
    v.screen_off_s = 300;
    cell().set(v);
    // Read right behind the page's delivery, on the UI thread: LVGL puts
    // the finger's value back at its next touch read, so a later look
    // could not tell a slider the delivery moved from one it left alone.
    std::atomic<int> right_after{-1};
    {
        wisp::core::CellSubscription probe = cell().subscribe(
            [&](const wisp::core::SettingsSnapshot&) {
                right_after = lv_slider_get_value(page()->volume());
            },
            &harness->ui());
        ASSERT_TRUE(wisp::test::wait_until([&] { return right_after.load() != -1; }, 2000));
    }
    EXPECT_EQ(right_after.load(), held);
    harness->on_ui([&] {
        EXPECT_EQ(lv_slider_get_value(page()->volume()), held);
        EXPECT_EQ(text_of(page()->volume_value()), std::to_string(held) + "%");
        EXPECT_EQ(text_of(page()->value(Row::ScreenOff)), "5 分钟");
    });
    harness->feed({});
    const auto w = written();
    ASSERT_EQ(w.size(), 1u);
    EXPECT_EQ(w[0].at("volume").as_int(), held);
    settle();
    harness->on_ui([&] { EXPECT_EQ(lv_slider_get_value(page()->volume()), held); });
    EXPECT_EQ(cell().get().volume, held);
}

// A delivery while a choice page is open redraws the settings page under
// it and leaves the choice page as it opened; the pick is what is written.
TEST_F(SettingsPageTest, OpenChoicePageIsLeftAlone) {
    boot();
    tap_row(Row::ScreenOff);
    ASSERT_TRUE(harness->ui_until([&] { return choice() != nullptr; }));
    wisp::core::SettingsSnapshot v = cell().get();
    v.screen_off_s = 300;
    cell().set(v);
    settle();
    harness->on_ui([&] {
        ASSERT_NE(choice(), nullptr);
        EXPECT_EQ(choice()->current(), 2);
        EXPECT_EQ(text_of(page()->value(Row::ScreenOff)), "5 分钟");
    });
    tap_choice(0);
    ASSERT_TRUE(back_on_settings());
    const auto w = written();
    ASSERT_EQ(w.size(), 1u);
    EXPECT_EQ(w[0].at("screen_off_s"), Value(int64_t{15}));
    settle();
    EXPECT_EQ(value_of(Row::ScreenOff), "15 秒");
}

namespace {

struct PreviewShape {
    const char* shape;
    int32_t size;
};

class SettingsPreview : public SettingsPageTest,
                        public ::testing::WithParamInterface<PreviewShape> {
protected:
    // Writes the screen as <dir>/<shape>-<size>-<name>.ppm when
    // WISP_SETTINGS_SCREENSHOTS names a directory.
    void screenshot(const std::string& name) {
        const char* dir = std::getenv("WISP_SETTINGS_SCREENSHOTS");
        if (!dir) return;
        const PreviewShape& p = GetParam();
        harness->on_ui([] { lv_obj_update_layout(lv_screen_active()); });
        auto shot = harness->ui().snapshot_rgb888();
        ASSERT_TRUE(shot);
        std::ofstream file(
            std::string(dir) + "/" + p.shape + "-" + std::to_string(p.size) + "-" + name + ".ppm",
            std::ios::binary);
        ASSERT_TRUE(file);
        file << "P6\n" << p.size << " " << p.size << "\n255\n";
        file.write(reinterpret_cast<const char*>(shot.value().data()), shot.value().size());
    }
};

}  // namespace

// A manual preview (docs/testing.md): the settings page from its top to its
// last row, and the screen-off choice page at both ends.
TEST_P(SettingsPreview, DISABLED_PageAndChoice) {
    const PreviewShape& p = GetParam();
    boot(p.shape, p.size, p.size);
    harness->on_ui([&] { lv_obj_scroll_to_y(page()->list(), 0, LV_ANIM_OFF); });
    screenshot("settings-0");
    // A screen's height less a row at a time, so each shot overlaps the last.
    for (int step = 1; step < 20; ++step) {
        bool at_end = false;
        harness->on_ui([&] {
            lv_obj_scroll_by(page()->list(), 0, -(p.size - 96), LV_ANIM_OFF);
            lv_obj_update_layout(page()->list());
            at_end = lv_obj_get_scroll_bottom(page()->list()) <= 0;
        });
        screenshot("settings-" + std::to_string(step));
        if (at_end) break;
    }
    tap_row(Row::ScreenOff);
    ASSERT_TRUE(harness->ui_until([&] { return choice() != nullptr; }));
    harness->on_ui([&] { lv_obj_scroll_to_y(choice()->list(), 0, LV_ANIM_OFF); });
    screenshot("screen-off-top");
    harness->on_ui([&] { lv_obj_scroll_to_y(choice()->list(), LV_COORD_MAX, LV_ANIM_OFF); });
    screenshot("screen-off-bottom");
}

INSTANTIATE_TEST_SUITE_P(ScreenShapes, SettingsPreview,
                         ::testing::Values(PreviewShape{"rect", 480}, PreviewShape{"circle", 466},
                                           PreviewShape{"rounded_rect", 480}),
                         [](const ::testing::TestParamInfo<PreviewShape>& info) {
                             return std::string(info.param.shape) + "_" +
                                    std::to_string(info.param.size);
                         });
