// Smoke/regression tests for the real self-check screen, on top
// of the same App/Board test harness app_boot_test.cpp uses. Boots an App
// with every fake device wired up, waits for the UI to actually render,
// and checks on *behavior* (does it render at all, does it render
// differently under an IMU error, does a persisted setting survive a
// restart, does WISP_PERF get emitted) rather than on exact pixel
// content, which would make these tests brittle against any layout
// tweak.

#include <gtest/gtest.h>

#include <algorithm>
#include <array>
#include <atomic>
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <mutex>
#include <span>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "self_check_layout.hpp"
#include "support/fake_devices.hpp"
#include "support/font_image.hpp"
#include "support/lvgl_probe.hpp"
#include "support/temp_dir.hpp"
#include "support/wait.hpp"
#include "wisp/app/app.hpp"
#include "wisp/app/board.hpp"
#include "wisp/app/power_manager.hpp"
#include "wisp/base/error.hpp"
#include "wisp/base/json.hpp"
#include "wisp/base/value.hpp"
#include "wisp/core/device_registry.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/core/occupancy.hpp"
#include "wisp/core/sensor_service.hpp"
#include "wisp/core/settings.hpp"
#include "wisp/core/system_settings.hpp"
#include "wisp/hal/display_geometry.hpp"
#include "wisp/os/os.hpp"
#include "wisp/port/sim/settings_json_file.hpp"
#include "wisp/ui/fonts.hpp"
#include "wisp/ui/strings.hpp"
#include "wisp/ui/ui_runtime.hpp"

using wisp::app::SummaryLevel;
using wisp::test::all_label_glyphs_resolvable;
using wisp::test::collect_labels;
using wisp::test::FakeAudioIn;
using wisp::test::FakeAudioOut;
using wisp::test::FakeButton;
using wisp::test::FakeClock;
using wisp::test::FakeDisplay;
using wisp::test::FakeImu;
using wisp::test::FakePower;
using wisp::test::FakeStorage;
using wisp::test::find_button_with_label;
using wisp::test::find_label_containing;
using wisp::test::find_label_text_containing;
using wisp::test::find_msgbox_width;
using wisp::test::page_lang_name;
using wisp::test::run_on_ui_blocking;
using wisp::test::run_on_ui_thread;
using wisp::test::select_page_lang;
using wisp::test::test_font_image;
using wisp::test::wait_until;
using wisp::ui::Lang;
using wisp::ui::StrId;

namespace {

// A Board that registers one of every fake device the self-check screen
// knows how to look up (display and imu are supplied by the caller, so
// tests can keep a handle to inject errors/timing into them; touch is
// intentionally left out -- FakeDisplay/FakeImu/etc. don't include a
// FakeTouch, and the screen must tolerate that the same way it tolerates
// a missing power/clock/SD device), so building the screen exercises
// every section of spec 8's layout rather than just the "device missing"
// fallback text.
// What a test adds to FullFakeBoard: boot-report entries of its own (e.g. a
// device that failed to start), and the audio pair the loopback test needs.
struct BoardExtras {
    std::vector<wisp::core::DeviceReport> reports;
    bool with_audio = false;
    // The board's name() and Platform::target.
    std::string name = "fake-full";
    std::string target;
};

class FullFakeBoard : public wisp::app::Board {
public:
    FullFakeBoard(std::shared_ptr<FakeDisplay> display, std::shared_ptr<FakeImu> imu,
                  std::shared_ptr<FakePower> power, uint32_t init_sleep_ms = 0,
                  std::shared_ptr<FakeStorage> sd = nullptr, BoardExtras extras = {})
        : display_(std::move(display)),
          imu_(std::move(imu)),
          power_(std::move(power)),
          init_sleep_ms_(init_sleep_ms),
          sd_(std::move(sd)),
          extras_(std::move(extras)) {}

    std::string name() const override { return extras_.name; }

    void init(wisp::core::DeviceRegistry& registry, wisp::core::BootReport& report) override {
        if (init_sleep_ms_ > 0) {
            // Gives boot_ms (used by both WISP_BOOT_REPORT and the
            // self-check header) a deterministic, non-flaky non-zero
            // floor to assert against -- see
            // BootTimeIsSetAndHeaderRefreshedAfterScreenIsBuilt below.
            wisp::os::sleep_ms(init_sleep_ms_);
        }
        registry.add(display_);
        registry.add(std::make_shared<FakeStorage>("storage.int", wisp::test::temp_dir().string(),
                                                   /*removable=*/false));
        registry.add(imu_);
        registry.add(std::make_shared<FakeButton>("button.power"));
        registry.add(power_);
        registry.add(std::make_shared<FakeClock>("clock.main"));
        if (sd_) {
            registry.add(sd_);
            if (!sd_->online()) {
                report.record({"storage.sd", wisp::hal::DeviceClass::Storage,
                               wisp::core::DeviceState::Missing, "no SD card"});
            }
        }
        if (extras_.with_audio) {
            registry.add(std::make_shared<FakeAudioIn>("audio_in.main"));
            registry.add(std::make_shared<FakeAudioOut>("audio_out.main"));
        }
        for (const auto& entry : extras_.reports) {
            report.record(entry);
        }
    }

private:
    std::shared_ptr<FakeDisplay> display_;
    std::shared_ptr<FakeImu> imu_;
    std::shared_ptr<FakePower> power_;
    uint32_t init_sleep_ms_;
    std::shared_ptr<FakeStorage> sd_;
    BoardExtras extras_;
};

// Bundles everything a test needs to build one App over FullFakeBoard: the
// externally-held fakes it can poke, the captured emit_line() lines, and
// the settings file path (shared across Apps in the persistence test).
struct Harness {
    std::shared_ptr<FakeDisplay> display;
    std::shared_ptr<FakeImu> imu;
    std::shared_ptr<FakePower> power;
    std::shared_ptr<FakeStorage> sd;
    std::string settings_path;
    std::mutex lines_mutex;
    std::vector<std::string> lines;
    // Platform::request_power_off call count -- see
    // RequestsPlatformPowerOffAndTurnsPanelOffWhenPowerOffFails below.
    std::atomic<int> request_power_off_calls{0};
    wisp::app::App app;

    explicit Harness(std::string settings_path_in, uint32_t init_sleep_ms = 0,
                     std::string display_shape = "rounded_rect", bool with_sd = false,
                     int display_size = 480, BoardExtras extras = {},
                     std::span<const uint8_t> font_image = {},
                     std::function<wisp::Value()> heap_info = {})
        : display(std::make_shared<FakeDisplay>("display.main", display_size, display_size,
                                                /*area_align=*/1, std::move(display_shape))),
          imu(std::make_shared<FakeImu>("imu.main")),
          power(std::make_shared<FakePower>("power.main")),
          sd(with_sd ? std::make_shared<FakeStorage>("storage.sd", wisp::test::temp_dir().string(),
                                                     /*removable=*/true)
                     : nullptr),
          settings_path(std::move(settings_path_in)),
          app(build_platform(init_sleep_ms, std::move(extras), font_image, std::move(heap_info))) {
        if (sd) {
            sd->online_flag = false;
        }
    }

    bool wait_for_flushes(size_t more_than, uint32_t timeout_ms = 3000) {
        return wait_until([&] { return display->flush_count() > more_than; }, timeout_ms);
    }

private:
    wisp::app::Platform build_platform(uint32_t init_sleep_ms, BoardExtras extras,
                                       std::span<const uint8_t> font_image,
                                       std::function<wisp::Value()> heap_info) {
        wisp::app::Platform platform;
        platform.target = extras.target;
        platform.board = std::make_unique<FullFakeBoard>(display, imu, power, init_sleep_ms, sd,
                                                         std::move(extras));
        platform.settings =
            std::make_unique<wisp::port::sim::JsonFileSettingsBackend>(settings_path);
        platform.request_power_off = [this] { request_power_off_calls.fetch_add(1); };
        platform.emit_line = [this](const std::string& line) {
            std::lock_guard<std::mutex> lock(lines_mutex);
            lines.push_back(line);
        };
        platform.font_image = font_image;
        platform.heap_info = std::move(heap_info);
        return platform;
    }
};

// The devices card's heading in each page language, used to tell which
// language the page shows.
struct LangCase {
    Lang lang;
    const char* devices_heading;
};
constexpr LangCase kLangCases[] = {
    {Lang::Zh, "设备"},
    {Lang::Ja, "デバイス"},
    {Lang::En, "Devices"},
};

const std::string kShapes[] = {"rect", "rounded_rect", "circle"};

// The section cards' headings, top to bottom, in English (the page's
// language until the switcher changes it).
const std::vector<std::string> kCardHeadings = {"Board",   "Devices",           "Network",
                                                "Sensors", "Power & clock",     "Performance",
                                                "Storage", "Interactive tests", "Scenes"};

uint32_t rgb_of(lv_color_t c) {
    return (static_cast<uint32_t>(c.red) << 16) | (static_cast<uint32_t>(c.green) << 8) | c.blue;
}

std::string hex(uint32_t rgb) {
    char buf[8];
    std::snprintf(buf, sizeof(buf), "%06x", static_cast<unsigned>(rgb));
    return buf;
}

// UI thread: the scrolling root column, the active screen's first child.
lv_obj_t* root_column() { return lv_obj_get_child(lv_screen_active(), 0); }

// UI thread: the root column's child whose first child is a label reading
// exactly `heading` (a section card), or nullptr.
lv_obj_t* find_card(const std::string& heading) {
    lv_obj_t* col = root_column();
    for (uint32_t i = 0; i < lv_obj_get_child_count(col); ++i) {
        lv_obj_t* c = lv_obj_get_child(col, static_cast<int32_t>(i));
        if (lv_obj_get_child_count(c) == 0) continue;
        lv_obj_t* first = lv_obj_get_child(c, 0);
        if (lv_obj_check_type(first, &lv_label_class) && heading == lv_label_get_text(first)) {
            return c;
        }
    }
    return nullptr;
}

// UI thread: "<text>|<background rgb>/<text rgb>" of the summary chip
// whose label contains `needle`, or empty.
std::string chip_state(const char* needle) {
    lv_obj_t* label = find_label_containing(root_column(), needle);
    if (label == nullptr) return {};
    lv_obj_t* chip = lv_obj_get_parent(label);
    return std::string(lv_label_get_text(label)) + "|" +
           hex(rgb_of(lv_obj_get_style_bg_color(chip, LV_PART_MAIN))) + "/" +
           hex(rgb_of(lv_obj_get_style_text_color(label, LV_PART_MAIN)));
}

// What chip_state() reports for a chip at `level`: a badge, the level's
// colour on its soft tint.
std::string chip(SummaryLevel level) {
    return hex(wisp::app::level_tint(level)) + "/" + hex(wisp::app::level_color(level));
}

// UI thread: one line per row of the devices card, top to bottom:
// "<dot rgb>|<each shown label's text>|...". A row is a status dot
// followed by a body holding the id, driver and reason labels.
std::string device_rows() {
    lv_obj_t* card = find_card("Devices");
    if (card == nullptr) return "no devices card";
    std::string out;
    for (uint32_t i = 1; i < lv_obj_get_child_count(card); ++i) {
        lv_obj_t* row = lv_obj_get_child(card, static_cast<int32_t>(i));
        if (lv_obj_get_child_count(row) < 2) return "row " + std::to_string(i) + " malformed";
        out += hex(rgb_of(lv_obj_get_style_bg_color(lv_obj_get_child(row, 0), LV_PART_MAIN)));
        lv_obj_t* body = lv_obj_get_child(row, 1);
        for (uint32_t k = 0; k < lv_obj_get_child_count(body); ++k) {
            lv_obj_t* label = lv_obj_get_child(body, static_cast<int32_t>(k));
            if (lv_obj_check_type(label, &lv_label_class) &&
                !lv_obj_has_flag(label, LV_OBJ_FLAG_HIDDEN)) {
                out += "|";
                out += lv_label_get_text(label);
            }
        }
        out += "\n";
    }
    return out;
}

// UI thread: how many labels on the screen contain `needle`.
int count_labels_containing(const char* needle) {
    std::vector<std::pair<std::string, lv_area_t>> labels;
    collect_labels(lv_screen_active(), labels);
    int n = 0;
    for (const auto& [text, area] : labels) {
        if (text.find(needle) != std::string::npos) ++n;
    }
    return n;
}

// Writes Settings sys/lang into the settings file a Harness is about to
// boot from, the same key SystemSettings stores the device language in.
void seed_language(const std::string& settings_path, const char* code) {
    wisp::port::sim::JsonFileSettingsBackend backend(settings_path);
    ASSERT_TRUE(wisp::core::Settings(backend, "sys").set("lang", code));
}

// UI thread: "<key>|<value>" per row of the board card's table.
std::string board_rows() {
    lv_obj_t* card = find_card("Board");
    if (card == nullptr) card = find_card("设备信息");
    if (card == nullptr) card = find_card("ボード情報");
    if (card == nullptr || lv_obj_get_child_count(card) < 2) return "no board card";
    lv_obj_t* table = lv_obj_get_child(card, 1);
    std::string out;
    uint32_t n = lv_obj_get_child_count(table);
    for (uint32_t i = 0; i + 1 < n; i += 2) {
        out += lv_label_get_text(lv_obj_get_child(table, static_cast<int32_t>(i)));
        out += "|";
        out += lv_label_get_text(lv_obj_get_child(table, static_cast<int32_t>(i + 1)));
        out += "\n";
    }
    return out;
}

}  // namespace

TEST(SelfCheckScreen, RendersNonUniformFrame) {
    Harness h(wisp::test::temp_dir().string() + "/settings.json");
    ASSERT_TRUE(h.app.start());
    ASSERT_NE(h.app.ui(), nullptr);

    // Give the sampling task and the 100ms UI refresh timer a few ticks,
    // then wait for several real flushes so the screen has actually been
    // drawn (not just the first, possibly-still-empty frame).
    ASSERT_TRUE(h.wait_for_flushes(5));

    auto snapshot = h.app.ui()->snapshot_rgb888();
    ASSERT_TRUE(snapshot);
    const auto& pixels = snapshot.value();
    ASSERT_EQ(pixels.size(), 480u * 480u * 3u);

    // A uniform frame (every pixel byte identical) would mean the screen
    // is effectively blank. The self-check screen has text, colored
    // device-state labels and the IMU tilt ball frame, so the real thing
    // must vary.
    bool all_same = true;
    for (size_t i = 1; i < pixels.size(); ++i) {
        if (pixels[i] != pixels[0]) {
            all_same = false;
            break;
        }
    }
    EXPECT_FALSE(all_same);

    h.app.stop();
}

// boot_ms is recorded right after Step 4 (UI bring-up) finishes -- i.e.
// after the self-check screen is already built, per spec 6.1's boot
// sequence, which only *ends* once the screen is up -- not any earlier,
// since the header needs a real value to show. The header renders once
// during build() (with boot_ms still at its
// default, 0) and is then explicitly refreshed (refresh_self_check_header()
// in self_check.hpp) once the real value is known; this checks both the
// *report* (used for WISP_BOOT_REPORT) and the header label itself end up
// showing that same real value by the time start() returns (FullFakeBoard's
// deliberate sleep makes this non-flaky: without it, a sub-millisecond boot
// could legitimately still round to 0ms on a fast machine, and any 0 would
// then be indistinguishable from the stale pre-refresh header text).
TEST(SelfCheckScreen, BootTimeIsSetAndHeaderRefreshedAfterScreenIsBuilt) {
    Harness h(wisp::test::temp_dir().string() + "/settings.json", /*init_sleep_ms=*/20);
    ASSERT_TRUE(h.app.start());
    ASSERT_TRUE(h.wait_for_flushes(5));

    auto rep = h.app.report().to_value(h.app.platform().board->name());
    const wisp::Value* boot_ms = rep.find("boot_ms");
    ASSERT_NE(boot_ms, nullptr);
    EXPECT_GT(boot_ms->as_int(), 0);

    std::string rows = run_on_ui_thread(h.app.ui(), [] { return board_rows(); });
    EXPECT_NE(rows.find("Boot|" + std::to_string(boot_ms->as_int()) + " ms\n"), std::string::npos)
        << "board card: " << rows << " boot_ms: " << boot_ms->as_int();

    h.app.stop();
}

// Spec 8: a runtime IMU read failure must show "Read error" for that row,
// not crash. This reads the IMU row's own label text (on the UI thread)
// rather than diffing the whole
// screen's snapshot_rgb888() -- a full-frame diff only proved *something*
// changed (it could just as easily have been boot_ms jitter or an
// unrelated timer tick), not specifically that the IMU row renders the
// error text.
//
// Waiting for "N more flushes" to infer that the 100ms refresh_timer_
// has ticked since the error was injected is only a
// coincidental proxy -- any other source of a same-sized flush burst
// (e.g. a full-screen redraw triggered by unrelated background work,
// such as another os::Timer thread starting up around the same time)
// satisfies the flush-count threshold just as well without the IMU row
// having actually been refreshed yet, making the test flaky. Polling the
// label text itself with a bounded timeout (as done below) checks the
// thing the test actually cares about, directly, instead of inferring it
// from an unrelated side channel.
TEST(SelfCheckScreen, ImuReadFailureRendersDifferentlyAndDoesNotCrash) {
    Harness h(wisp::test::temp_dir().string() + "/settings.json");
    ASSERT_TRUE(h.app.start());
    ASSERT_TRUE(h.wait_for_flushes(5));

    auto imu_row_text = [&] {
        return run_on_ui_thread(
            h.app.ui(), [] { return find_label_text_containing(lv_screen_active(), "IMU:"); });
    };

    EXPECT_NE(imu_row_text(), "IMU: Read error");
    // The sensor card is below the fold; only what is on screen is flushed.
    run_on_ui_blocking(h.app.ui(), [] {
        lv_obj_t* label = find_label_containing(root_column(), "Accel");
        if (label != nullptr) lv_obj_scroll_to_view_recursive(label, LV_ANIM_OFF);
    });

    h.imu->set_next_error(wisp::ErrorCode::IoError, "boom");
    ASSERT_TRUE(wait_until([&] { return imu_row_text() == "IMU: Read error"; }, 3000));

    // Still alive and rendering -- the "不崩溃" half of spec 8's
    // requirement: once reads work again, the error goes and the new
    // values are drawn. (An unchanged screen is not redrawn, so waiting
    // for flushes alone would prove nothing.)
    size_t flush_before_more = h.display->flush_count();
    wisp::hal::ImuSample sample{};
    sample.ax = 0.5f;
    h.imu->set_next_sample(sample);
    ASSERT_TRUE(wait_until([&] { return imu_row_text().empty(); }, 3000));
    EXPECT_TRUE(wait_until(
        [&] {
            return !run_on_ui_thread(h.app.ui(), [] {
                        return find_label_text_containing(lv_screen_active(), "+0.50");
                    }).empty();
        },
        3000));
    EXPECT_TRUE(h.wait_for_flushes(flush_before_more));

    h.app.stop();
}

// Standby must stop this screen's own "wisp.sample" IMU polling task,
// not just UiRuntime's rendering -- spec 6.7's "待机
// 时... 没有高频轮询" applies to it too (it was previously still
// reading the IMU at ~50Hz throughout standby). Drives
// PowerManager::enter_standby()/exit_standby() directly (the same calls
// a power-key press makes) rather than simulating a button press, since
// that wiring is already covered by power_manager_test.cpp's own tests.
TEST(SelfCheckScreen, ImuSamplingStopsDuringStandbyAndResumesOnWake) {
    Harness h(wisp::test::temp_dir().string() + "/settings.json");
    ASSERT_TRUE(h.app.start());
    ASSERT_TRUE(h.wait_for_flushes(5));

    wisp::app::PowerManager* pm = h.app.power_manager();
    ASSERT_NE(pm, nullptr);

    // Let a few IMU reads happen first, so the check below proves
    // sampling actually *stopped*, not merely that it "hasn't started
    // yet".
    ASSERT_TRUE(wait_until([&] { return h.imu->read_count() > 0; }, 2000));

    run_on_ui_blocking(h.app.ui(), [&] { pm->enter_standby(); });
    ASSERT_TRUE(wait_until([&] { return pm->standby(); }, 2000));
    // pm->standby() flips synchronously inside enter_standby(), but
    // SelfCheck's own standby_ flag (consulted by sample_loop(), which
    // runs on its own task) only catches up once the EventBus dispatcher
    // has actually delivered the "power.state" event to on_power_event()
    // -- asynchronously, on the bus's own dispatcher task. Without
    // waiting for that dispatch to finish first, there's a race where
    // sample_loop() still takes one more real IMU read() between the
    // wait_until() above and the snapshot below, making this assertion
    // flaky under scheduler pressure.
    ASSERT_TRUE(h.app.bus().wait_idle(2000));

    size_t reads_at_standby = h.imu->read_count();
    // Real time for the (now-paused) sampling task to prove it does NOT
    // keep reading the IMU while in standby.
    wisp::os::sleep_ms(300);
    EXPECT_EQ(h.imu->read_count(), reads_at_standby);

    run_on_ui_blocking(h.app.ui(), [&] { pm->exit_standby(); });
    ASSERT_TRUE(wait_until([&] { return !pm->standby(); }, 2000));
    EXPECT_TRUE(wait_until([&] { return h.imu->read_count() > reads_at_standby; }, 2000));

    h.app.stop();
}

// Refinement 19 of docs/superpowers/plans/2026-11-05-scene-device.md: the
// screen reads the IMU through SensorService (accel and gyro at 50 Hz) as
// holder "system.selfcheck", not the driver.
TEST(SelfCheckScreen, ImuReadsThroughSensorService) {
    Harness h(wisp::test::temp_dir().string() + "/settings.json");
    ASSERT_TRUE(h.app.start());
    ASSERT_TRUE(h.wait_for_flushes(5));
    ASSERT_NE(h.app.sensors(), nullptr);
    auto entries = h.app.occupancy().list();
    ASSERT_EQ(entries.size(), 1u);
    EXPECT_EQ(entries[0].device, "imu.main");
    EXPECT_EQ(entries[0].holder, "system.selfcheck");
    EXPECT_EQ(entries[0].mode, wisp::core::OccupancyMode::Shared);
    EXPECT_EQ(h.app.sensors()->imu_rate(), 50u);
    EXPECT_EQ(h.imu->rate_hz(), 50u);

    // What the IMU reads reaches the screen in g, as before.
    run_on_ui_blocking(h.app.ui(), [] {
        lv_obj_t* label = find_label_containing(root_column(), "Accel");
        if (label != nullptr) lv_obj_scroll_to_view_recursive(label, LV_ANIM_OFF);
    });
    wisp::hal::ImuSample sample{};
    sample.ay = -0.25f;
    sample.gz = 12.5f;
    h.imu->set_next_sample(sample);
    EXPECT_TRUE(wait_until(
        [&] {
            return !run_on_ui_thread(h.app.ui(), [] {
                        return find_label_text_containing(lv_screen_active(), "-0.25");
                    }).empty();
        },
        3000));

    h.app.stop();
    EXPECT_TRUE(h.app.occupancy().list().empty());
}

// Spec 5.6: the root column fills the whole screen so its background is
// uniform edge to edge, and keeps content inside safe_rect() purely via
// padding. Horizontally nothing ever lands outside the safe columns;
// vertically content may scroll through the top/bottom bands, but at
// rest at the top the top band is untouched. Checked against a pixel
// known to always be outside any safe_rect (the very corner, (0, 0))
// rather than a hardcoded guess at the theme's background color.
TEST(SelfCheckScreen, NoContentOutsideSafeRect) {
    for (const LangCase& lang : kLangCases) {
        for (const std::string& shape : kShapes) {
            SCOPED_TRACE(std::string(wisp::ui::lang_code(lang.lang)) + " " + shape);
            Harness h(wisp::test::temp_dir().string() + "/settings.json", /*init_sleep_ms=*/0,
                      shape);
            ASSERT_TRUE(h.app.start());
            ASSERT_TRUE(h.wait_for_flushes(5));
            ASSERT_TRUE(select_page_lang(h.app.ui(), lang.lang));
            ASSERT_FALSE(run_on_ui_thread(h.app.ui(), [&] {
                             return find_label_text_containing(lv_screen_active(),
                                                               lang.devices_heading);
                         }).empty());

            auto snapshot = h.app.ui()->snapshot_rgb888();
            ASSERT_TRUE(snapshot);
            const auto& pixels = snapshot.value();
            constexpr int32_t kSize = 480;
            ASSERT_EQ(pixels.size(), static_cast<size_t>(kSize) * kSize * 3u);

            auto shape_result = wisp::hal::screen_shape_from_caps(h.display->info().caps);
            ASSERT_TRUE(shape_result) << shape;
            wisp::hal::Area safe = wisp::hal::safe_rect(shape_result.value());

            auto pixel_at = [&](int32_t x, int32_t y) {
                size_t idx = (static_cast<size_t>(y) * kSize + x) * 3;
                return std::array<uint8_t, 3>{pixels[idx], pixels[idx + 1], pixels[idx + 2]};
            };
            auto background = pixel_at(0, 0);

            for (int32_t y = 0; y < kSize; ++y) {
                for (int32_t x = 0; x < kSize; ++x) {
                    bool side_band = x < safe.x1 || x > safe.x2;
                    bool top_band = y < safe.y1;
                    if (!side_band && !top_band) {
                        continue;
                    }
                    ASSERT_EQ(pixel_at(x, y), background) << shape << " " << x << "," << y;
                }
            }

            h.app.stop();
        }
    }
}

// Spec 5.6 scroll rule: the scrolling root column spans the full screen
// and pads its content by the safe inset, so at either scroll extreme the
// first/last row rests fully inside safe_rect() -- nothing is reachable
// only by looking into a corner.
TEST(SelfCheckScreen, ScrollExtremesRestInsideSafeRect) {
    for (const LangCase& lang : kLangCases) {
        for (const std::string& shape : kShapes) {
            SCOPED_TRACE(std::string(wisp::ui::lang_code(lang.lang)) + " " + shape);
            Harness h(wisp::test::temp_dir().string() + "/settings.json", /*init_sleep_ms=*/0,
                      shape);
            ASSERT_TRUE(h.app.start());
            ASSERT_TRUE(h.wait_for_flushes(5));
            ASSERT_TRUE(select_page_lang(h.app.ui(), lang.lang));

            auto shape_result = wisp::hal::screen_shape_from_caps(h.display->info().caps);
            ASSERT_TRUE(shape_result) << shape;
            wisp::hal::Area safe = wisp::hal::safe_rect(shape_result.value());

            // Returns "x1,y1,x2,y2" of the root column, then of its first and
            // last child, after scrolling to `bottom` (or the top).
            auto probe = [&](bool bottom) {
                return run_on_ui_thread(h.app.ui(), [bottom] {
                    lv_obj_t* col = lv_obj_get_child(lv_screen_active(), 0);
                    if (bottom) {
                        lv_obj_scroll_to_y(col, LV_COORD_MAX, LV_ANIM_OFF);
                    } else {
                        lv_obj_scroll_to_y(col, 0, LV_ANIM_OFF);
                    }
                    lv_obj_update_layout(col);
                    std::string out;
                    auto add = [&](lv_obj_t* o) {
                        lv_area_t a;
                        lv_obj_get_coords(o, &a);
                        out += std::to_string(a.x1) + "," + std::to_string(a.y1) + "," +
                               std::to_string(a.x2) + "," + std::to_string(a.y2) + ";";
                    };
                    add(col);
                    uint32_t n = lv_obj_get_child_count(col);
                    add(lv_obj_get_child(col, 0));
                    add(lv_obj_get_child(col, n - 1));
                    return out;
                });
            };
            auto parse = [](const std::string& text) {
                std::vector<wisp::hal::Area> areas;
                std::stringstream ss(text);
                std::string item;
                while (std::getline(ss, item, ';')) {
                    wisp::hal::Area a{};
                    std::sscanf(item.c_str(), "%d,%d,%d,%d", &a.x1, &a.y1, &a.x2, &a.y2);
                    areas.push_back(a);
                }
                return areas;
            };
            auto inside = [&](const wisp::hal::Area& a) {
                return a.x1 >= safe.x1 && a.x2 <= safe.x2 && a.y1 >= safe.y1 && a.y2 <= safe.y2;
            };

            auto top = parse(probe(false));
            ASSERT_EQ(top.size(), 3u);
            EXPECT_EQ(top[0].x1, 0) << shape;
            EXPECT_EQ(top[0].y1, 0) << shape;
            EXPECT_EQ(top[0].x2, 479) << shape;
            EXPECT_EQ(top[0].y2, 479) << shape;
            EXPECT_TRUE(inside(top[1])) << shape << " first row at scroll top";

            auto bottom = parse(probe(true));
            ASSERT_EQ(bottom.size(), 3u);
            EXPECT_TRUE(inside(bottom[2])) << shape << " last row at scroll end";

            h.app.stop();
        }
    }
}

// Longer Japanese/English text must still wrap inside the safe columns,
// never run past them, and every glyph must be in the built-in font.
TEST(SelfCheckScreen, LabelsFitTheSafeAreaInEveryLanguage) {
    for (const LangCase& lang : kLangCases) {
        for (const std::string& shape : kShapes) {
            SCOPED_TRACE(std::string(wisp::ui::lang_code(lang.lang)) + " " + shape);
            Harness h(wisp::test::temp_dir().string() + "/settings.json", /*init_sleep_ms=*/0,
                      shape);
            ASSERT_TRUE(h.app.start());
            ASSERT_TRUE(h.wait_for_flushes(5));
            ASSERT_TRUE(select_page_lang(h.app.ui(), lang.lang));

            auto shape_result = wisp::hal::screen_shape_from_caps(h.display->info().caps);
            ASSERT_TRUE(shape_result);
            wisp::hal::Area safe = wisp::hal::safe_rect(shape_result.value());

            std::vector<std::pair<std::string, lv_area_t>> labels;
            run_on_ui_blocking(h.app.ui(), [&] {
                lv_obj_update_layout(lv_screen_active());
                collect_labels(lv_screen_active(), labels);
            });
            ASSERT_GT(labels.size(), 10u);
            for (const auto& [text, a] : labels) {
                EXPECT_GE(a.x1, safe.x1) << text;
                EXPECT_LE(a.x2, safe.x2) << text;
            }
            EXPECT_EQ(run_on_ui_thread(
                          h.app.ui(),
                          [] {
                              return std::string(
                                  all_label_glyphs_resolvable(lv_screen_active()) ? "ok" : "");
                          }),
                      "ok");

            h.app.stop();
        }
    }
}

// The page opens in English whatever the system language is, and never
// writes the system language: here the system language is Chinese.
TEST(SelfCheckScreen, PageOpensInEnglishWhateverTheSystemLanguage) {
    std::string settings_path = wisp::test::temp_dir().string() + "/settings.json";
    seed_language(settings_path, "zh");
    Harness h(settings_path);
    ASSERT_TRUE(h.app.start());
    ASSERT_TRUE(h.wait_for_flushes(5));
    EXPECT_EQ(wisp::ui::lang(), Lang::Zh);

    std::string seen = run_on_ui_thread(h.app.ui(), [] {
        return std::string(lv_label_get_text(lv_obj_get_child(root_column(), 0))) + "|" +
               (find_card("Devices") != nullptr ? "Devices" : "") + "|" +
               (find_card("设备") != nullptr ? "设备" : "") + "|" + chip_state("Devices ");
    });
    EXPECT_EQ(seen, "Self Check|Devices||Devices 6/6 OK|" + chip(SummaryLevel::Good));
    // The selected button is the English one.
    EXPECT_EQ(run_on_ui_thread(h.app.ui(),
                               [] {
                                   std::string out;
                                   for (Lang lang : {Lang::En, Lang::Zh, Lang::Ja}) {
                                       lv_obj_t* b = find_button_with_label(root_column(),
                                                                            page_lang_name(lang));
                                       out += b == nullptr                            ? "-"
                                              : lv_obj_has_state(b, LV_STATE_CHECKED) ? "1"
                                                                                      : "0";
                                   }
                                   return out;
                               }),
              "100");
    EXPECT_EQ(h.app.system_settings().get().language, "zh");

    h.app.stop();
}

// Choosing a language re-texts the page in place: the same widgets, now in
// that language, while the system language stays what it was.
TEST(SelfCheckScreen, SwitchingThePageLanguageRetextsItInPlace) {
    Harness h(wisp::test::temp_dir().string() + "/settings.json");
    ASSERT_TRUE(h.app.start());
    ASSERT_TRUE(h.wait_for_flushes(5));
    const std::string system_lang = h.app.system_settings().get().language;
    const Lang ui_lang = wisp::ui::lang();

    // "<title>|<board heading>|<devices heading>|<devices chip>|<a test
    // status>|<root column>|<title object>"
    auto sample = [&] {
        return run_on_ui_thread(h.app.ui(), [] {
            lv_obj_t* col = root_column();
            lv_obj_t* title = lv_obj_get_child(col, 0);
            lv_obj_t* board = lv_obj_get_child(col, 2);
            lv_obj_t* devices = lv_obj_get_child(col, 3);
            char ptrs[64];
            std::snprintf(ptrs, sizeof(ptrs), "%p|%p", static_cast<void*>(col),
                          static_cast<void*>(title));
            return std::string(lv_label_get_text(title)) + "|" +
                   lv_label_get_text(lv_obj_get_child(board, 0)) + "|" +
                   lv_label_get_text(lv_obj_get_child(devices, 0)) + "|" +
                   lv_label_get_text(
                       lv_obj_get_child(lv_obj_get_child(lv_obj_get_child(col, 1), 0), 0)) +
                   "|" +
                   std::to_string(count_labels_containing(
                       wisp::ui::tr(StrId::TestUntested, wisp::ui::Lang::En))) +
                   "," +
                   std::to_string(count_labels_containing(
                       wisp::ui::tr(StrId::TestUntested, wisp::ui::Lang::Zh))) +
                   "," +
                   std::to_string(count_labels_containing(
                       wisp::ui::tr(StrId::TestUntested, wisp::ui::Lang::Ja))) +
                   "|" + ptrs;
        });
    };
    const std::string en = sample();
    const std::string ptrs = en.substr(en.rfind('|', en.rfind('|') - 1) + 1);
    EXPECT_EQ(en, "Self Check|Board|Devices|Devices 6/6 OK|3,0,0|" + ptrs);

    ASSERT_TRUE(select_page_lang(h.app.ui(), Lang::Zh));
    EXPECT_EQ(sample(), "自检|设备信息|设备|设备 6/6 正常|0,3,0|" + ptrs);
    EXPECT_EQ(wisp::ui::lang(), ui_lang);
    EXPECT_EQ(h.app.system_settings().get().language, system_lang);

    ASSERT_TRUE(select_page_lang(h.app.ui(), Lang::Ja));
    EXPECT_EQ(sample(), "セルフチェック|ボード情報|デバイス|デバイス 6/6 正常|0,0,3|" + ptrs);
    EXPECT_EQ(wisp::ui::lang(), ui_lang);
    EXPECT_EQ(h.app.system_settings().get().language, system_lang);
    EXPECT_EQ(run_on_ui_thread(h.app.ui(),
                               [] {
                                   return std::string(
                                       all_label_glyphs_resolvable(lv_screen_active()) ? "ok" : "");
                               }),
              "ok");
    EXPECT_EQ(
        run_on_ui_thread(
            h.app.ui(),
            [] {
                lv_obj_t* ja = find_button_with_label(root_column(), page_lang_name(Lang::Ja));
                lv_obj_t* en = find_button_with_label(root_column(), page_lang_name(Lang::En));
                return std::string(lv_obj_has_state(ja, LV_STATE_CHECKED) ? "ja" : "") +
                       (lv_obj_has_state(en, LV_STATE_CHECKED) ? "en" : "");
            }),
        "ja");

    h.app.stop();
}

// A loopback status other than "Ready" is kept across a language switch and
// re-rendered in the new language. The harness board has no audio devices,
// so the run fails at once with not_found.
TEST(SelfCheckScreen, LoopbackStatusIsRetranslatedOnLanguageSwitch) {
    Harness h(wisp::test::temp_dir().string() + "/settings.json");
    ASSERT_TRUE(h.app.start());
    ASSERT_TRUE(h.wait_for_flushes(5));
    auto find_on_screen = [&](const char* needle) {
        return run_on_ui_thread(h.app.ui(), [needle] {
            return find_label_text_containing(lv_screen_active(), needle);
        });
    };

    EXPECT_EQ(run_on_ui_thread(h.app.ui(),
                               [] {
                                   lv_obj_t* btn =
                                       find_button_with_label(lv_screen_active(), "Mic test");
                                   if (btn == nullptr) return std::string();
                                   lv_obj_send_event(btn, LV_EVENT_CLICKED, nullptr);
                                   return std::string("clicked");
                               }),
              "clicked");
    ASSERT_TRUE(wait_until([&] { return !find_on_screen("Failed: not_found").empty(); }, 3000));

    ASSERT_TRUE(select_page_lang(h.app.ui(), Lang::Zh));
    EXPECT_FALSE(find_on_screen("失败: not_found").empty());
    EXPECT_TRUE(find_on_screen("就绪").empty());
    EXPECT_FALSE(find_on_screen("录音回放").empty());

    h.app.stop();
}

// A switch keeps the column where the user had scrolled it, and this
// boot's test results. On the small 220 px panel the column scrolls well
// past 200 px.
TEST(SelfCheckScreen, PageLanguageSwitchKeepsScrollAndTests) {
    Harness h(wisp::test::temp_dir().string() + "/settings.json", /*init_sleep_ms=*/0,
              "rounded_rect", /*with_sd=*/false, /*display_size=*/220);
    ASSERT_TRUE(h.app.start());
    ASSERT_TRUE(h.wait_for_flushes(0));

    ASSERT_EQ(run_on_ui_thread(
                  h.app.ui(),
                  [] {
                      lv_obj_t* label = find_label_containing(root_column(), "Touch test area");
                      if (label == nullptr) return std::string();
                      lv_obj_send_event(lv_obj_get_parent(label), LV_EVENT_PRESSED, nullptr);
                      lv_obj_send_event(lv_obj_get_parent(label), LV_EVENT_RELEASED, nullptr);
                      return std::string("pressed");
                  }),
              "pressed");
    ASSERT_TRUE(wait_until(
        [&] {
            return run_on_ui_thread(h.app.ui(), [] { return chip_state("Tests "); }) ==
                   "Tests 1/3 done|" + chip(SummaryLevel::Neutral);
        },
        3000));

    auto scroll_y = [&] {
        return run_on_ui_thread(h.app.ui(), [] {
            lv_obj_t* col = root_column();
            lv_obj_update_layout(col);
            return std::to_string(lv_obj_get_scroll_y(col));
        });
    };
    run_on_ui_blocking(h.app.ui(), [] {
        lv_obj_t* col = root_column();
        lv_obj_update_layout(col);
        lv_obj_scroll_to_y(col, 200, LV_ANIM_OFF);
    });
    ASSERT_EQ(scroll_y(), "200") << "the column must be tall enough to scroll to 200";

    ASSERT_TRUE(select_page_lang(h.app.ui(), Lang::Ja));
    EXPECT_EQ(scroll_y(), "200");
    EXPECT_EQ(
        run_on_ui_thread(h.app.ui(),
                         [] {
                             return chip_state("完了") + "|" +
                                    std::to_string(count_labels_containing(LV_SYMBOL_OK " 合格")) +
                                    "|" + std::to_string(count_labels_containing("未テスト"));
                         }),
        "テスト 1/3 完了|" + chip(SummaryLevel::Neutral) + "|1|2");

    h.app.stop();
}

// A system language change (as PUT /settings makes) leaves the page alone
// but re-texts an open confirm dialog, which follows the system language.
TEST(SelfCheckScreen, SystemLanguageChangeLeavesThePageAndRetextsTheDialog) {
    Harness h(wisp::test::temp_dir().string() + "/settings.json");
    ASSERT_TRUE(h.app.start());
    ASSERT_TRUE(h.wait_for_flushes(5));
    auto find_on_screen = [&](const char* needle) {
        return run_on_ui_thread(h.app.ui(), [needle] {
            return find_label_text_containing(lv_screen_active(), needle);
        });
    };
    ASSERT_TRUE(select_page_lang(h.app.ui(), Lang::Ja));
    EXPECT_EQ(wisp::ui::lang(), Lang::Zh);

    ASSERT_TRUE(h.app.system_settings().set({{"language", "en"}}));
    ASSERT_TRUE(wait_until([&] { return wisp::ui::lang() == Lang::En; }, 3000));
    ASSERT_TRUE(h.app.bus().wait_idle(2000));
    // Give a (wrong) page re-text a chance to land.
    wisp::os::sleep_ms(200);
    EXPECT_FALSE(find_on_screen("デバイス").empty());
    EXPECT_TRUE(find_on_screen("Devices").empty());

    h.app.bus().publish(wisp::core::Event{"button.power.long", "button.power"});
    auto has_button = [&](const char* text) {
        return run_on_ui_thread(h.app.ui(), [text] {
                   return find_button_with_label(lv_layer_top(), text) != nullptr
                              ? std::string("found")
                              : std::string();
               }) == "found";
    };
    ASSERT_TRUE(wait_until([&] { return has_button("Yes"); }, 3000));

    ASSERT_TRUE(h.app.system_settings().set({{"language", "zh"}}));
    ASSERT_TRUE(wait_until([&] { return has_button("是"); }, 3000));
    EXPECT_FALSE(has_button("Yes"));
    EXPECT_FALSE(find_on_screen("デバイス").empty());

    // The dialog still answers.
    ASSERT_TRUE(wait_until(
        [&] {
            return run_on_ui_thread(h.app.ui(), [] {
                       lv_obj_t* no = find_button_with_label(lv_layer_top(), "否");
                       if (no == nullptr) return std::string();
                       lv_obj_send_event(no, LV_EVENT_CLICKED, nullptr);
                       return std::string("clicked");
                   }) == "clicked";
        },
        3000));
    EXPECT_TRUE(wait_until([&] { return !has_button("是"); }, 3000));

    h.app.stop();
}

// If the system.settings event never reaches the screen (the bus queue
// drops its oldest event when full), the refresh timer still brings the
// system language (which the confirm dialogs use) in line with the stored
// one. The bus dispatcher is held inside a handler so the event cannot be
// delivered, then flooded so it is dropped.
TEST(SelfCheckScreen, SystemLanguageFollowsSettingsWhenTheEventIsLost) {
    Harness h(wisp::test::temp_dir().string() + "/settings.json");
    ASSERT_TRUE(h.app.start());
    ASSERT_TRUE(h.wait_for_flushes(5));

    wisp::os::Semaphore entered(1, 0);
    wisp::os::Semaphore release(1, 0);
    auto block_sub = h.app.bus().subscribe("test.block", [&](const wisp::core::Event&) {
        entered.give();
        release.take(wisp::os::kForever);
    });
    h.app.bus().publish(wisp::core::Event{"test.block", "test"});
    ASSERT_TRUE(entered.take(3000));

    ASSERT_TRUE(h.app.system_settings().set({{"language", "en"}}));
    for (int i = 0; i < 256; ++i) {
        h.app.bus().publish(wisp::core::Event{"test.flood", "test"});
    }

    bool switched = wait_until([&] { return wisp::ui::lang() == Lang::En; }, 3000);
    release.give();
    EXPECT_TRUE(switched);

    h.app.bus().unsubscribe(block_sub);
    h.app.stop();
}

// The perf-overlay toggle persists to Settings
// (ns="ui", key="perf_overlay") and is restored on the next boot. This
// drives that restore path directly (writing the setting the same way a
// prior session's long-press would have left it) rather than simulating
// the long-press gesture itself, which would need a touch indev this
// test harness doesn't have.
//
// Asserts via UiRuntime::perf_overlay_enabled(), not a pixel diff of
// snapshot_rgb888(): the sysmon overlay lives in LVGL's sys layer, a
// sibling of the active screen that lv_snapshot_take() structurally
// never renders (it only walks the one object -- here, the screen --
// it's given), so no snapshot comparison could ever actually detect it.
// An earlier version of this test diffed snapshots and was flaky (it
// "passed" only when something unrelated, like boot_ms jitter in the
// header, happened to differ between the two App instances).
TEST(SelfCheckScreen, PerfOverlaySettingPersistsAcrossRestarts) {
    std::string settings_path = wisp::test::temp_dir().string() + "/settings.json";

    {
        Harness h(settings_path);
        ASSERT_TRUE(h.app.start());
        ASSERT_NE(h.app.ui(), nullptr);
        EXPECT_FALSE(h.app.ui()->perf_overlay_enabled());
        h.app.stop();
    }

    // Simulate a previous session having toggled the overlay on, by
    // writing the setting directly through the same backend/file a real
    // long-press would have used (core::Settings(ns="ui")).
    {
        wisp::port::sim::JsonFileSettingsBackend backend(settings_path);
        wisp::core::Settings ui_settings(backend, "ui");
        ASSERT_TRUE(ui_settings.set("perf_overlay", true));
    }

    {
        Harness h(settings_path);
        ASSERT_TRUE(h.app.start());
        ASSERT_NE(h.app.ui(), nullptr);
        // start() blocks until the self-check screen's build() -- which
        // reads "perf_overlay" and, if true, calls
        // ui_->set_perf_overlay(true) -- has run, but set_perf_overlay()
        // itself only *posts* the actual lv_sysmon_show_performance()
        // call rather than waiting for it (see its doc comment); give
        // the UI task a moment to drain that second, chained post().
        EXPECT_TRUE(
            wisp::test::wait_until([&] { return h.app.ui()->perf_overlay_enabled(); }, 1000));
        h.app.stop();
    }
}

// WISP_PERF is emitted every 5s via Platform::emit_line.
// Not "cheap" in wall-clock terms, but bounded and worth the ~5s to cover
// end to end (format, device lookups, heap_info being empty) rather than
// only unit-testing pieces of it.
// docs/architecture.md's "机器可读输出" section documents WISP_PERF's
// six fields; parse the line (not just its prefix) and check each one
// is present with the expected type, so a future change to this set is
// caught here rather than silently drifting out of sync with that doc.
TEST(SelfCheckScreen, EmitsWispPerfLine) {
    Harness h(wisp::test::temp_dir().string() + "/settings.json");
    ASSERT_TRUE(h.app.start());

    std::string perf_line;
    bool saw_perf_line = wait_until(
        [&] {
            std::lock_guard<std::mutex> lock(h.lines_mutex);
            for (const auto& line : h.lines) {
                if (line.rfind("WISP_PERF ", 0) == 0) {
                    perf_line = line;
                    return true;
                }
            }
            return false;
        },
        6000);
    ASSERT_TRUE(saw_perf_line);

    auto parsed = wisp::from_json(perf_line.substr(std::string("WISP_PERF ").size()));
    ASSERT_TRUE(parsed);
    const wisp::Value& v = parsed.value();

    // fps/flush_ms are computed as doubles, but Value/JSON round-tripping
    // keeps Int and Double distinct based on the actual value (see
    // value_json_test.cpp's Json.IntAndDoubleStayDistinct) -- a
    // momentarily whole-number measurement (e.g. "fps":5) comes back as
    // Int, not Double, so this accepts either rather than asserting one
    // specific Value::Type for them.
    auto is_number = [](const wisp::Value* value) {
        return value != nullptr && (value->type() == wisp::Value::Type::Int ||
                                    value->type() == wisp::Value::Type::Double);
    };
    EXPECT_TRUE(is_number(v.find("fps")));
    EXPECT_TRUE(is_number(v.find("flush_ms")));

    const wisp::Value* cpu = v.find("cpu");
    ASSERT_NE(cpu, nullptr);
    EXPECT_EQ(cpu->type(), wisp::Value::Type::Int);

    const wisp::Value* frames = v.find("frames");
    ASSERT_NE(frames, nullptr);
    EXPECT_EQ(frames->type(), wisp::Value::Type::Int);

    const wisp::Value* heap_internal_free = v.find("heap_internal_free");
    ASSERT_NE(heap_internal_free, nullptr);
    EXPECT_EQ(heap_internal_free->type(), wisp::Value::Type::Int);

    const wisp::Value* heap_large_free = v.find("heap_large_free");
    ASSERT_NE(heap_large_free, nullptr);
    EXPECT_EQ(heap_large_free->type(), wisp::Value::Type::Int);

    h.app.stop();
}

// The Performance card's internal-RAM and PSRAM rows (PerfSection::
// update_ram()), fed from Platform::heap_info() on the same 100 ms
// cadence as the FPS/CPU/flush line. KB/largest-block wording comes from
// the string table (ui::StrId), so this runs in the page's starting
// language (English); the KB/MB units themselves are not translated
// (format_kb()/format_mb() in self_check_readouts.cpp).
TEST(SelfCheckScreen, PerformanceCardShowsInternalRamAndPsram) {
    auto heap_info = [] {
        return wisp::Value(wisp::Map{
            {"internal_free", int64_t{79 * 1024}},
            {"internal_largest", int64_t{31 * 1024}},
            {"large_free", int64_t{4089446}},           // ~3.9 MiB
            {"large_total", int64_t{8 * 1024 * 1024}},  // 8.0 MiB
        });
    };
    Harness h(wisp::test::temp_dir().string() + "/settings.json", 0, "rounded_rect", false, 480, {},
              {}, heap_info);
    ASSERT_TRUE(h.app.start());
    ASSERT_TRUE(h.wait_for_flushes(5));
    // refresh_perf_ram() runs on SelfCheck's 100 ms lv_timer, not tied to a
    // flush (set_text_if_changed() only triggers one once the row's "--"
    // placeholder actually changes); give it a few ticks.
    wisp::os::sleep_ms(300);

    std::string ram_text = run_on_ui_thread(
        h.app.ui(), [] { return find_label_text_containing(root_column(), "largest block"); });
    EXPECT_EQ(ram_text, "free 79 KB | largest block 31 KB");

    std::string psram_text = run_on_ui_thread(
        h.app.ui(), [] { return find_label_text_containing(root_column(), "MB / total"); });
    EXPECT_EQ(psram_text, "free 3.9 MB / total 8.0 MB");

    h.app.stop();
}

// A platform that reports no PSRAM (heap_info missing entirely, like the
// simulator today, or a board with "large_total" 0) shows "-" rather than
// "free 0.0 MB / total 0.0 MB", which would misleadingly read as "completely full".
TEST(SelfCheckScreen, PerformanceCardShowsDashWithNoHeapInfo) {
    Harness h(wisp::test::temp_dir().string() + "/settings.json");
    ASSERT_TRUE(h.app.start());
    ASSERT_TRUE(h.wait_for_flushes(5));
    wisp::os::sleep_ms(300);

    std::string texts = run_on_ui_thread(h.app.ui(), [] {
        lv_obj_t* ram_key = find_label_containing(root_column(), "Internal RAM");
        lv_obj_t* psram_key = find_label_containing(root_column(), "PSRAM");
        if (ram_key == nullptr || psram_key == nullptr) return std::string("missing key");
        lv_obj_t* table = lv_obj_get_parent(ram_key);
        auto value_of = [&](lv_obj_t* key) {
            return std::string(
                lv_label_get_text(lv_obj_get_child(table, lv_obj_get_index(key) + 1)));
        };
        return value_of(ram_key) + "|" + value_of(psram_key);
    });
    EXPECT_EQ(texts, "-|-");

    h.app.stop();
}

// The device list's own "storage.sd" row is rendered from BootReport,
// which is a boot-time snapshot: without a live update, it would keep
// showing a missing card even after one was inserted later and the
// dedicated storage section below it started reporting it online -- two
// sections of the same screen visibly disagreeing. It must track
// StorageManager::status() live instead, and so must the device summary.
TEST(SelfCheckScreen, DeviceListSdRowTracksLiveStorageStatus) {
    Harness h(wisp::test::temp_dir().string() + "/settings.json", /*init_sleep_ms=*/0,
              "rounded_rect", /*with_sd=*/true);
    ASSERT_TRUE(h.app.start());
    ASSERT_TRUE(h.wait_for_flushes(5));

    auto sd_row = [&] {
        std::string rows = run_on_ui_thread(h.app.ui(), [] { return device_rows(); });
        std::stringstream ss(rows);
        std::string line;
        while (std::getline(ss, line)) {
            if (line.find("|storage.sd|") != std::string::npos) return line;
        }
        return rows;
    };
    EXPECT_EQ(sd_row(), hex(wisp::app::kColorMissing) + "|storage.sd|FAKE|Missing: no SD card");
    EXPECT_EQ(run_on_ui_thread(h.app.ui(), [] { return chip_state("Devices "); }),
              "Devices 6/7 OK|" + chip(SummaryLevel::Neutral));

    h.sd->online_flag = true;
    ASSERT_TRUE(wait_until(
        [&] { return sd_row() == hex(wisp::app::kColorGood) + "|storage.sd|FAKE"; }, 3000))
        << sd_row();
    EXPECT_EQ(run_on_ui_thread(h.app.ui(), [] { return chip_state("Devices "); }),
              "Devices 7/7 OK|" + chip(SummaryLevel::Good));

    h.app.stop();
}

// Measuring storage walks the file system (LittleFS reads every block in
// use to count them), so the screen never does it on the UI thread: polled
// there ten times a second it took a fifth of the frames from a scrolling
// self-check list once a dozen scenes were installed
// (docs/hardware/waveshare_esp32_s3_touch_amoled_2_16.md). The storage card
// and the SD row are still kept up to date.
TEST(SelfCheckScreen, MeasuresStorageOffTheUiThread) {
    std::atomic<int> on_ui{0};
    std::atomic<int> off_ui{0};
    Harness h(wisp::test::temp_dir().string() + "/settings.json", /*init_sleep_ms=*/0,
              "rounded_rect", /*with_sd=*/true);
    h.sd->on_capacity = [&] {
        wisp::ui::UiRuntime* ui = h.app.ui();
        ++(ui != nullptr && ui->on_ui_thread() ? on_ui : off_ui);
    };
    ASSERT_TRUE(h.app.start());
    ASSERT_TRUE(h.wait_for_flushes(5));

    h.sd->online_flag = true;
    ASSERT_TRUE(wait_until(
        [&] {
            return run_on_ui_thread(h.app.ui(), [] {
                       return device_rows();
                   }).find(hex(wisp::app::kColorGood) + "|storage.sd|FAKE") != std::string::npos;
        },
        3000));
    const int seen = off_ui.load();
    ASSERT_TRUE(wait_until([&] { return off_ui.load() > seen; }, 3000));
    EXPECT_EQ(on_ui.load(), 0);

    h.app.stop();
}

// The confirm dialog's width must respect compute_safe_area(), not LVGL's
// fixed LV_DPI_DEF*2 default (see show_confirm_dialog()): a 480px screen's
// safe area is wide enough that the default alone never clips, so this
// uses a deliberately small 220x220 circular display (safe width well
// under LV_DPI_DEF*2) to actually exercise the clamp. A long press on
// button.power while the screen is on opens the dialog (see
// PowerKeyController::on_long()); publishing that event directly on the
// bus drives exactly the same on_button_event()/PowerKeyController path a
// real press would, without needing the ButtonService's own debounce
// timing.
TEST(SelfCheckScreen, ConfirmDialogWidthClampedToSafeArea) {
    Harness h(wisp::test::temp_dir().string() + "/settings.json", /*init_sleep_ms=*/0, "circle",
              /*with_sd=*/false, /*display_size=*/220);
    ASSERT_TRUE(h.app.start());
    // At 220 px tall a full frame is only ceil(220 / draw_buf_lines) flushes
    // (10 with the default 24), and once the screen settles nothing
    // redraws, so waiting for "more than one frame" depends on a follow-up update landing in its
    // own frame. Any flush means the screen is up; the msgbox wait below does the real
    // synchronisation.
    ASSERT_TRUE(h.wait_for_flushes(0));

    h.app.bus().publish(wisp::core::Event{"button.power.long", "button.power"});

    int32_t width = -1;
    ASSERT_TRUE(wait_until(
        [&] {
            std::string text = run_on_ui_thread(
                h.app.ui(), [] { return std::to_string(find_msgbox_width(lv_layer_top())); });
            width = text.empty() ? -1 : std::stoi(text);
            // > 0, not >= 0: the msgbox object exists (and so is found)
            // for a moment before LVGL's layout pass has resolved its
            // lv_obj_set_width() call into real coords, during which
            // lv_obj_get_width() reads back 0 -- wait past that too.
            return width > 0;
        },
        3000));

    auto shape_result = wisp::hal::screen_shape_from_caps(h.display->info().caps);
    ASSERT_TRUE(shape_result);
    wisp::hal::Area safe = wisp::hal::safe_rect(shape_result.value());
    int32_t safe_width = safe.x2 - safe.x1 + 1;
    ASSERT_LT(safe_width, LV_DPI_DEF * 2) << "test fixture no longer exercises the clamp";
    EXPECT_EQ(width, safe_width);

    // The dialog lives on lv_layer_top(), outside the active screen; its
    // Chinese title, text and buttons must still render.
    EXPECT_EQ(
        run_on_ui_thread(
            h.app.ui(),
            [] { return std::string(all_label_glyphs_resolvable(lv_layer_top()) ? "ok" : ""); }),
        "ok");

    h.app.stop();
}

// When Power::power_off() fails, do_power_off() must fall through to
// Platform::request_power_off() -- the ESP fallback is
// esp_deep_sleep_start(), which never actually powers the panel/PMIC
// down (see docs/hardware/waveshare_esp32_s3_touch_amoled_2_16.md and main.cpp's own
// comment on request_power_off), so do_power_off() must turn the panel
// off itself first. Confirms the dialog via a simulated click on its
// "是" button, the same path a real touch does (rather than publishing
// Action::PowerOff directly), so this also exercises
// PowerKeyController::on_confirm()'s own routing.
TEST(SelfCheckScreen, RequestsPlatformPowerOffAndTurnsPanelOffWhenPowerOffFails) {
    Harness h(wisp::test::temp_dir().string() + "/settings.json");
    ASSERT_TRUE(h.app.start());
    ASSERT_TRUE(h.wait_for_flushes(5));
    h.power->set_fail_power_off(true);

    h.app.bus().publish(wisp::core::Event{"button.power.long", "button.power"});
    ASSERT_TRUE(wait_until(
        [&] {
            return run_on_ui_thread(h.app.ui(), [] {
                       return find_button_with_label(lv_layer_top(), "是") != nullptr
                                  ? std::string("found")
                                  : std::string();
                   }) == "found";
        },
        3000));

    run_on_ui_blocking(h.app.ui(), [] {
        lv_obj_t* yes = find_button_with_label(lv_layer_top(), "是");
        ASSERT_NE(yes, nullptr);
        lv_obj_send_event(yes, LV_EVENT_CLICKED, nullptr);
    });

    EXPECT_TRUE(wait_until([&] { return h.request_power_off_calls.load() > 0; }, 3000));
    EXPECT_FALSE(h.power->powered_off());
    EXPECT_FALSE(h.display->powered());

    h.app.stop();
}

// The page is a header followed by one titled card per section, in a
// fixed order.
TEST(SelfCheckScreen, SectionCardsInOrder) {
    Harness h(wisp::test::temp_dir().string() + "/settings.json");
    ASSERT_TRUE(h.app.start());
    ASSERT_TRUE(h.wait_for_flushes(5));

    std::string order = run_on_ui_thread(h.app.ui(), [] {
        std::string out;
        lv_obj_t* col = root_column();
        for (uint32_t i = 0; i < lv_obj_get_child_count(col); ++i) {
            lv_obj_t* c = lv_obj_get_child(col, static_cast<int32_t>(i));
            if (lv_obj_get_child_count(c) == 0) continue;
            lv_obj_t* first = lv_obj_get_child(c, 0);
            if (!lv_obj_check_type(first, &lv_label_class)) continue;
            std::string text = lv_label_get_text(first);
            if (std::find(kCardHeadings.begin(), kCardHeadings.end(), text) !=
                kCardHeadings.end()) {
                out += text + ";";
            }
        }
        return out;
    });
    std::string expected;
    for (const auto& heading : kCardHeadings) expected += heading + ";";
    EXPECT_EQ(order, expected);

    h.app.stop();
}

// The page's title is "Self Check" in the page language, on a line of its
// own in a larger font than the text under it; the summary chips follow.
TEST(SelfCheckScreen, TitleIsSelfCheck) {
    Harness h(wisp::test::temp_dir().string() + "/settings.json");
    ASSERT_TRUE(h.app.start());
    ASSERT_TRUE(h.wait_for_flushes(5));

    std::string result = run_on_ui_thread(h.app.ui(), [] {
        lv_obj_t* col = root_column();
        lv_obj_t* title = lv_obj_get_child(col, 0);
        lv_obj_t* chips = lv_obj_get_child(col, 1);
        if (!lv_obj_check_type(title, &lv_label_class) || lv_obj_get_child_count(chips) != 2) {
            return std::string("no title and chips");
        }
        lv_obj_t* chip_label = lv_obj_get_child(lv_obj_get_child(chips, 0), 0);
        int32_t title_h = lv_font_get_line_height(lv_obj_get_style_text_font(title, LV_PART_MAIN));
        int32_t chip_h =
            lv_font_get_line_height(lv_obj_get_style_text_font(chip_label, LV_PART_MAIN));
        return std::string(lv_label_get_text(title)) + "|" + lv_label_get_text(chip_label) + "|" +
               (title_h > chip_h ? "larger" : "not larger");
    });
    EXPECT_EQ(result, "Self Check|Devices 6/6 OK|larger");
    // The board name is no longer the title.
    EXPECT_EQ(run_on_ui_thread(h.app.ui(),
                               [] { return std::to_string(count_labels_containing("fake-full")); }),
              "1");

    h.app.stop();
}

// The board card comes right under the summary chips: aligned rows for the
// board name, the target, the firmware version and the boot time, then the
// language switcher.
TEST(SelfCheckScreen, BoardCardListsTheBoard) {
    BoardExtras extras;
    extras.target = "esp32s3";
    Harness h(wisp::test::temp_dir().string() + "/settings.json", /*init_sleep_ms=*/0,
              "rounded_rect", false, 480, extras);
    ASSERT_TRUE(h.app.start());
    ASSERT_TRUE(h.wait_for_flushes(5));
    const wisp::Value report = h.app.report().to_value("fake-full");
    const wisp::Value* boot_ms = report.find("boot_ms");
    ASSERT_NE(boot_ms, nullptr);

    std::string rows = run_on_ui_thread(h.app.ui(), [] {
        lv_obj_t* card = lv_obj_get_child(root_column(), 2);
        if (card != find_card("Board")) return std::string("board card not third");
        return board_rows();
    });
    EXPECT_EQ(rows, "Board|fake-full\nTarget|esp32s3\nFirmware|0.1.0\nBoot|" +
                        std::to_string(boot_ms->as_int()) + " ms\n");

    // The keys are aligned: every value starts at the same x.
    EXPECT_EQ(run_on_ui_thread(h.app.ui(),
                               [] {
                                   lv_obj_t* table = lv_obj_get_child(find_card("Board"), 1);
                                   lv_obj_update_layout(table);
                                   std::string xs;
                                   for (uint32_t i = 1; i < lv_obj_get_child_count(table); i += 2) {
                                       lv_area_t a;
                                       lv_obj_get_coords(
                                           lv_obj_get_child(table, static_cast<int32_t>(i)), &a);
                                       xs += std::to_string(a.x1) + ",";
                                   }
                                   std::string first = xs.substr(0, xs.find(',') + 1);
                                   std::string all;
                                   for (int i = 0; i < 4; ++i) all += first;
                                   return xs == all ? std::string("aligned") : xs;
                               }),
              "aligned");

    ASSERT_TRUE(select_page_lang(h.app.ui(), Lang::Zh));
    rows = run_on_ui_thread(h.app.ui(), [] { return board_rows(); });
    EXPECT_EQ(rows, "开发板|fake-full\n目标平台|esp32s3\n固件|0.1.0\n启动|" +
                        std::to_string(boot_ms->as_int()) + " ms\n");

    h.app.stop();
}

// A board name too long for one line wraps inside the safe area on every
// shape; nothing of it is clipped.
TEST(SelfCheckScreen, LongBoardNameWrapsInsideTheSafeArea) {
    const std::string name = "waveshare_esp32_s3_touch_amoled_2_16_with_a_very_long_suffix";
    for (const std::string& shape : kShapes) {
        SCOPED_TRACE(shape);
        BoardExtras extras;
        extras.name = name;
        Harness h(wisp::test::temp_dir().string() + "/settings.json", 0, shape, false, 220, extras);
        ASSERT_TRUE(h.app.start());
        ASSERT_TRUE(h.wait_for_flushes(0));
        auto shape_result = wisp::hal::screen_shape_from_caps(h.display->info().caps);
        ASSERT_TRUE(shape_result);
        wisp::hal::Area safe = wisp::hal::safe_rect(shape_result.value());

        // "<x1>,<x2>,<lines>|<full text>"
        std::string probe = run_on_ui_thread(h.app.ui(), [&name] {
            lv_obj_t* label = find_label_containing(root_column(), name.c_str());
            if (label == nullptr) return std::string("missing");
            lv_obj_update_layout(root_column());
            lv_area_t a;
            lv_obj_get_coords(label, &a);
            const lv_font_t* font = lv_obj_get_style_text_font(label, LV_PART_MAIN);
            int32_t lines = lv_area_get_height(&a) / lv_font_get_line_height(font);
            lv_point_t size;
            lv_text_get_size(&size, lv_label_get_text(label), font, 0, 0, lv_area_get_width(&a),
                             LV_TEXT_FLAG_NONE);
            return std::to_string(a.x1) + "," + std::to_string(a.x2) + "," + std::to_string(lines) +
                   "," + std::to_string(size.y) + "," + std::to_string(lv_area_get_height(&a)) +
                   "|" + lv_label_get_text(label);
        });
        ASSERT_NE(probe, "missing");
        int x1 = 0, x2 = 0, lines = 0, text_h = 0, box_h = 0;
        ASSERT_EQ(std::sscanf(probe.c_str(), "%d,%d,%d,%d,%d", &x1, &x2, &lines, &text_h, &box_h),
                  5);
        EXPECT_GE(x1, safe.x1);
        EXPECT_LE(x2, safe.x2);
        EXPECT_GT(lines, 1) << probe;
        EXPECT_GE(box_h, text_h) << "clipped: " << probe;
        EXPECT_EQ(probe.substr(probe.find('|') + 1), name);
        h.app.stop();
    }
}

// "Devices n/m OK" counts the boot report's Ok entries; its colour is green
// when all are OK, red if any failed, yellow if any is degraded (and none
// failed), and neutral when the only gaps are missing devices.
TEST(SelfCheckScreen, DeviceChipSummarisesTheBootReport) {
    using wisp::core::DeviceState;
    using wisp::hal::DeviceClass;
    struct Case {
        std::vector<wisp::core::DeviceReport> reports;
        std::string expected;
    };
    const Case cases[] = {
        {{}, "Devices 6/6 OK|" + chip(SummaryLevel::Good)},
        {{{"touch.main", DeviceClass::Touch, DeviceState::Degraded, "single touch"}},
         "Devices 6/7 OK|" + chip(SummaryLevel::Warning)},
        {{{"touch.main", DeviceClass::Touch, DeviceState::Degraded, "single touch"},
          {"audio_in.main", DeviceClass::AudioIn, DeviceState::Failed, "init failed"}},
         "Devices 6/8 OK|" + chip(SummaryLevel::Error)},
        {{{"storage.sd", DeviceClass::Storage, DeviceState::Missing, "no SD card"}},
         "Devices 6/7 OK|" + chip(SummaryLevel::Neutral)},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(c.expected);
        BoardExtras extras;
        extras.reports = c.reports;
        Harness h(wisp::test::temp_dir().string() + "/settings.json", 0, "rounded_rect", false, 480,
                  extras);
        ASSERT_TRUE(h.app.start());
        ASSERT_TRUE(h.wait_for_flushes(5));
        EXPECT_EQ(run_on_ui_thread(h.app.ui(), [] { return chip_state("Devices "); }), c.expected);
        h.app.stop();
    }
}

// Devices that are not OK come first, failed before degraded before
// missing, each with its state and reason on a second line; OK rows show
// only the id and the driver. Nothing is drawn in the old link blue.
TEST(SelfCheckScreen, DeviceRowsListProblemsFirstWithTheirReason) {
    using wisp::core::DeviceState;
    using wisp::hal::DeviceClass;
    BoardExtras extras;
    extras.reports = {
        {"storage.sd", DeviceClass::Storage, DeviceState::Missing, "no SD card"},
        {"touch.main", DeviceClass::Touch, DeviceState::Degraded, "single touch"},
        {"audio_in.main", DeviceClass::AudioIn, DeviceState::Failed, "init failed"},
    };
    Harness h(wisp::test::temp_dir().string() + "/settings.json", 0, "rounded_rect", false, 480,
              extras);
    ASSERT_TRUE(h.app.start());
    ASSERT_TRUE(h.wait_for_flushes(5));

    std::string rows = run_on_ui_thread(h.app.ui(), [] { return device_rows(); });
    std::vector<std::string> lines;
    std::stringstream ss(rows);
    for (std::string line; std::getline(ss, line);) lines.push_back(line);
    ASSERT_EQ(lines.size(), 9u) << rows;
    EXPECT_EQ(lines[0], hex(wisp::app::kColorError) + "|audio_in.main|-|Failed: init failed");
    EXPECT_EQ(lines[1], hex(wisp::app::kColorWarning) + "|touch.main|-|Degraded: single touch");
    EXPECT_EQ(lines[2], hex(wisp::app::kColorMissing) + "|storage.sd|-|Missing: no SD card");
    // The rest keep the boot report's order and have no second line.
    std::vector<std::string> ok_ids;
    for (const auto& entry : h.app.report().entries()) {
        if (entry.state == DeviceState::Ok) ok_ids.push_back(entry.id);
    }
    ASSERT_EQ(ok_ids.size(), lines.size() - 3);
    for (size_t i = 3; i < lines.size(); ++i) {
        EXPECT_EQ(lines[i], hex(wisp::app::kColorGood) + "|" + ok_ids[i - 3] + "|FAKE");
    }

    std::string blue = run_on_ui_thread(h.app.ui(), [] {
        std::string found;
        std::function<void(lv_obj_t*)> walk = [&](lv_obj_t* o) {
            if (lv_obj_check_type(o, &lv_label_class) &&
                rgb_of(lv_obj_get_style_text_color(o, LV_PART_MAIN)) == 0x4FC3F7) {
                found += std::string(lv_label_get_text(o)) + ";";
            }
            for (uint32_t i = 0; i < lv_obj_get_child_count(o); ++i) {
                walk(lv_obj_get_child(o, static_cast<int32_t>(i)));
            }
        };
        walk(lv_screen_active());
        return found;
    });
    EXPECT_EQ(blue, "");

    h.app.stop();
}

// Tapping the device chip scrolls the first device that is not OK into
// the safe area; with every device OK it leaves the page where it is.
TEST(SelfCheckScreen, DeviceChipTapScrollsToTheFirstProblem) {
    using wisp::core::DeviceState;
    using wisp::hal::DeviceClass;
    for (bool with_problem : {true, false}) {
        SCOPED_TRACE(with_problem ? "with a failed device" : "all OK");
        BoardExtras extras;
        if (with_problem) {
            extras.reports = {
                {"audio_in.main", DeviceClass::AudioIn, DeviceState::Failed, "init failed"}};
        }
        Harness h(wisp::test::temp_dir().string() + "/settings.json", 0, "rounded_rect", false, 220,
                  extras);
        ASSERT_TRUE(h.app.start());
        ASSERT_TRUE(h.wait_for_flushes(0));

        auto shape_result = wisp::hal::screen_shape_from_caps(h.display->info().caps);
        ASSERT_TRUE(shape_result);
        wisp::hal::Area safe = wisp::hal::safe_rect(shape_result.value());

        std::string bottom = run_on_ui_thread(h.app.ui(), [] {
            lv_obj_t* col = root_column();
            lv_obj_update_layout(col);
            lv_obj_scroll_to_y(col, LV_COORD_MAX, LV_ANIM_OFF);
            lv_obj_update_layout(col);
            return std::to_string(lv_obj_get_scroll_y(col));
        });
        ASSERT_GT(std::stoi(bottom), 0);
        ASSERT_EQ(run_on_ui_thread(
                      h.app.ui(),
                      [] {
                          lv_obj_t* label = find_label_containing(root_column(), "Devices ");
                          if (label == nullptr) return std::string();
                          lv_obj_send_event(lv_obj_get_parent(label), LV_EVENT_CLICKED, nullptr);
                          return std::string("clicked");
                      }),
                  "clicked");

        // "<scroll y>|<y1>,<y2> of the failed row's id label>"
        auto probe = [&] {
            return run_on_ui_thread(h.app.ui(), [] {
                lv_obj_t* col = root_column();
                std::string out = std::to_string(lv_obj_get_scroll_y(col)) + "|";
                lv_obj_t* id = find_label_containing(col, "audio_in.main");
                if (id != nullptr) {
                    lv_obj_t* row = lv_obj_get_parent(lv_obj_get_parent(id));
                    lv_area_t a;
                    lv_obj_get_coords(row, &a);
                    out += std::to_string(a.y1) + "," + std::to_string(a.y2);
                }
                return out;
            });
        };
        if (with_problem) {
            bool in_view = wait_until(
                [&] {
                    std::string p = probe();
                    int scroll = 0, y1 = 0, y2 = 0;
                    if (std::sscanf(p.c_str(), "%d|%d,%d", &scroll, &y1, &y2) != 3) return false;
                    return scroll < std::stoi(bottom) && y1 >= safe.y1 && y2 <= safe.y2;
                },
                3000);
            EXPECT_TRUE(in_view) << probe();
        } else {
            wisp::os::sleep_ms(500);
            EXPECT_EQ(probe(), bottom + "|");
        }
        h.app.stop();
    }
}

// Touch, any button and a completed loopback each turn their "Not tested"
// into "Passed", and the "Tests k/3 done" chip counts them, green at 3/3.
TEST(SelfCheckScreen, InteractiveTestsTurnPassedAndAreCounted) {
    BoardExtras extras;
    extras.with_audio = true;
    Harness h(wisp::test::temp_dir().string() + "/settings.json", 0, "rounded_rect", false, 480,
              extras);
    ASSERT_TRUE(h.app.start());
    ASSERT_TRUE(h.wait_for_flushes(5));
    auto state = [&] {
        return run_on_ui_thread(h.app.ui(), [] {
            return chip_state(" done") + "|" +
                   std::to_string(count_labels_containing("Not tested")) + "|" +
                   std::to_string(count_labels_containing(LV_SYMBOL_OK " Passed"));
        });
    };
    const std::string neutral = chip(SummaryLevel::Neutral);
    EXPECT_EQ(state(), "Tests 0/3 done|" + neutral + "|3|0");

    ASSERT_EQ(run_on_ui_thread(
                  h.app.ui(),
                  [] {
                      lv_obj_t* label = find_label_containing(root_column(), "Touch test area");
                      if (label == nullptr) return std::string();
                      lv_obj_send_event(lv_obj_get_parent(label), LV_EVENT_PRESSED, nullptr);
                      return std::string("pressed");
                  }),
              "pressed");
    EXPECT_TRUE(wait_until([&] { return state() == "Tests 1/3 done|" + neutral + "|2|1"; }, 3000))
        << state();

    h.app.bus().publish(wisp::core::Event{"button.vol_up.short", "button.vol_up"});
    EXPECT_TRUE(wait_until([&] { return state() == "Tests 2/3 done|" + neutral + "|1|2"; }, 3000))
        << state();

    ASSERT_EQ(run_on_ui_thread(h.app.ui(),
                               [] {
                                   lv_obj_t* btn =
                                       find_button_with_label(root_column(), "Mic test");
                                   if (btn == nullptr) return std::string();
                                   lv_obj_send_event(btn, LV_EVENT_CLICKED, nullptr);
                                   return std::string("clicked");
                               }),
              "clicked");
    EXPECT_TRUE(wait_until(
        [&] { return state() == "Tests 3/3 done|" + chip(SummaryLevel::Good) + "|0|3"; }, 8000))
        << state();

    // The results survive a page language switch (they last for this boot).
    ASSERT_TRUE(select_page_lang(h.app.ui(), Lang::Zh));
    EXPECT_EQ(run_on_ui_thread(h.app.ui(), [] { return chip_state("已测"); }),
              "交互 3/3 已测|" + chip(SummaryLevel::Good));

    h.app.stop();
}

// A loopback run that fails (this board has no audio devices) does not
// count as passed.
TEST(SelfCheckScreen, FailedLoopbackDoesNotPass) {
    Harness h(wisp::test::temp_dir().string() + "/settings.json");
    ASSERT_TRUE(h.app.start());
    ASSERT_TRUE(h.wait_for_flushes(5));
    ASSERT_EQ(run_on_ui_thread(h.app.ui(),
                               [] {
                                   lv_obj_t* btn =
                                       find_button_with_label(root_column(), "Mic test");
                                   if (btn == nullptr) return std::string();
                                   lv_obj_send_event(btn, LV_EVENT_CLICKED, nullptr);
                                   return std::string("clicked");
                               }),
              "clicked");
    ASSERT_TRUE(wait_until(
        [&] {
            return !run_on_ui_thread(h.app.ui(), [] {
                        return find_label_text_containing(root_column(), "Failed: not_found");
                    }).empty();
        },
        3000));
    EXPECT_EQ(run_on_ui_thread(h.app.ui(), [] { return chip_state(" done"); }),
              "Tests 0/3 done|" + chip(SummaryLevel::Neutral));

    h.app.stop();
}

// Spec 5.6 on every shape: every section card lies within the safe
// columns, the header's first line rests inside the safe area at the top
// of the page, and the last card rests inside it at the bottom.
TEST(SelfCheckScreen, CardsStayInsideTheSafeArea) {
    for (const std::string& shape : kShapes) {
        SCOPED_TRACE(shape);
        Harness h(wisp::test::temp_dir().string() + "/settings.json", 0, shape);
        ASSERT_TRUE(h.app.start());
        ASSERT_TRUE(h.wait_for_flushes(5));
        auto shape_result = wisp::hal::screen_shape_from_caps(h.display->info().caps);
        ASSERT_TRUE(shape_result);
        wisp::hal::Area safe = wisp::hal::safe_rect(shape_result.value());

        // "x1,y1,x2,y2;" per card (top to bottom), then "|" and the
        // header title's area at the top and the last card's at the bottom.
        std::string text = run_on_ui_thread(h.app.ui(), [] {
            auto fmt = [](lv_obj_t* o) {
                lv_area_t a;
                lv_obj_get_coords(o, &a);
                return std::to_string(a.x1) + "," + std::to_string(a.y1) + "," +
                       std::to_string(a.x2) + "," + std::to_string(a.y2) + ";";
            };
            lv_obj_t* col = root_column();
            lv_obj_scroll_to_y(col, 0, LV_ANIM_OFF);
            lv_obj_update_layout(col);
            std::string cards;
            int found = 0;
            for (const auto& heading : kCardHeadings) {
                if (lv_obj_t* card = find_card(heading)) {
                    cards += fmt(card);
                    ++found;
                }
            }
            if (found != static_cast<int>(kCardHeadings.size())) return std::string("missing");
            std::string out = cards + "|" + fmt(lv_obj_get_child(col, 0));
            lv_obj_scroll_to_y(col, LV_COORD_MAX, LV_ANIM_OFF);
            lv_obj_update_layout(col);
            return out + "|" + fmt(find_card(kCardHeadings.back()));
        });
        ASSERT_NE(text, "missing");
        std::vector<wisp::hal::Area> areas;
        for (size_t pos = 0; pos < text.size();) {
            if (text[pos] == '|') {
                ++pos;
                continue;
            }
            wisp::hal::Area a{};
            ASSERT_EQ(std::sscanf(text.c_str() + pos, "%d,%d,%d,%d", &a.x1, &a.y1, &a.x2, &a.y2),
                      4);
            areas.push_back(a);
            pos = text.find(';', pos) + 1;
        }
        ASSERT_EQ(areas.size(), kCardHeadings.size() + 2);
        for (size_t i = 0; i < kCardHeadings.size(); ++i) {
            EXPECT_GE(areas[i].x1, safe.x1) << kCardHeadings[i];
            EXPECT_LE(areas[i].x2, safe.x2) << kCardHeadings[i];
        }
        auto inside = [&](const wisp::hal::Area& a) {
            return a.x1 >= safe.x1 && a.x2 <= safe.x2 && a.y1 >= safe.y1 && a.y2 <= safe.y2;
        };
        EXPECT_TRUE(inside(areas[kCardHeadings.size()])) << "title at the top";
        EXPECT_TRUE(inside(areas[kCardHeadings.size() + 1])) << "last card at the bottom";
        h.app.stop();
    }
}

// spec 6.7 / AGENTS.md rendering budget: the 100 ms refresh only touches a
// readout when its value changed. With every fake device holding still,
// nothing on the battery, time or storage rows is flushed again.
// The same holds after a page language switch: the switch re-texts once,
// and nothing after it.
TEST(SelfCheckScreen, UnchangedReadoutsAreNotRedrawn) {
    for (Lang lang : {Lang::En, Lang::Ja}) {
        SCOPED_TRACE(wisp::ui::lang_code(lang));
        Harness h(wisp::test::temp_dir().string() + "/settings.json");
        ASSERT_TRUE(h.app.start());
        ASSERT_TRUE(h.wait_for_flushes(5));
        if (lang != Lang::En) ASSERT_TRUE(select_page_lang(h.app.ui(), lang));
        const std::string battery = wisp::ui::tr(StrId::Battery, lang);
        const std::string time = wisp::ui::tr(StrId::Time, lang);
        const std::string flash = wisp::ui::tr(StrId::InternalFlash, lang);

        // Bring the readouts on screen, then let everything settle (the
        // sampler's first slow sample, the scroll's redraw).
        run_on_ui_blocking(h.app.ui(), [&] {
            lv_obj_t* label = find_label_containing(root_column(), flash.c_str());
            if (label != nullptr) lv_obj_scroll_to_view_recursive(label, LV_ANIM_OFF);
        });
        wisp::os::sleep_ms(1500);

        // The rows' bands, full screen width: "y1,y2;" each.
        std::string bands = run_on_ui_thread(h.app.ui(), [&] {
            std::string out;
            for (const std::string* needle : {&battery, &time, &flash}) {
                lv_obj_t* label = find_label_containing(root_column(), needle->c_str());
                if (label == nullptr) return "missing " + *needle;
                lv_area_t a;
                lv_obj_get_coords(label, &a);
                out += std::to_string(a.y1) + "," + std::to_string(a.y2) + ";";
            }
            return out;
        });
        ASSERT_EQ(bands.find("missing"), std::string::npos) << bands;
        std::vector<std::pair<int32_t, int32_t>> rows;
        for (size_t pos = 0; pos < bands.size(); pos = bands.find(';', pos) + 1) {
            int y1 = 0, y2 = 0;
            ASSERT_EQ(std::sscanf(bands.c_str() + pos, "%d,%d", &y1, &y2), 2);
            ASSERT_GE(y1, 0);
            ASSERT_LT(y2, 480) << "row not on screen";
            rows.emplace_back(y1, y2);
        }

        size_t before = h.display->areas().size();
        wisp::os::sleep_ms(800);
        std::vector<wisp::hal::Area> areas = h.display->areas();
        for (size_t i = before; i < areas.size(); ++i) {
            for (const auto& [y1, y2] : rows) {
                EXPECT_FALSE(areas[i].y1 <= y2 && areas[i].y2 >= y1)
                    << "flushed " << areas[i].x1 << "," << areas[i].y1 << "-" << areas[i].x2 << ","
                    << areas[i].y2 << " over row " << y1 << "-" << y2;
            }
        }

        h.app.stop();
    }
}

// Spec 5.2: the self-check title is a vector-font label, in whichever
// language the page switcher currently shows (not necessarily the
// default, English). This only exercises the real vector font (as
// opposed to the bitmap fallback every other test in this file gets,
// since they don't pass a font image) by passing test_font_image() into
// the harness.
TEST(SelfCheckScreen, TitleUsesLargeFontInCurrentLanguage) {
    Harness h(wisp::test::temp_dir().string() + "/settings.json", /*init_sleep_ms=*/0,
              "rounded_rect", /*with_sd=*/false, /*display_size=*/480, /*extras=*/{},
              test_font_image());
    ASSERT_TRUE(h.app.start());
    ASSERT_TRUE(h.wait_for_flushes(5));

    struct Case {
        Lang lang;
        const char* title;
        uint32_t first_char;
    };
    const Case cases[] = {
        {Lang::Zh, "自检", 0x81EA},
        {Lang::Ja, "セルフチェック", 0x30BB},
        {Lang::En, "Self Check", 'S'},
    };
    for (const Case& c : cases) {
        SCOPED_TRACE(wisp::ui::lang_code(c.lang));
        ASSERT_TRUE(select_page_lang(h.app.ui(), c.lang));
        EXPECT_EQ(run_on_ui_thread(h.app.ui(),
                                   [&] {
                                       lv_obj_t* title = lv_obj_get_child(root_column(), 0);
                                       const lv_font_t* big = wisp::ui::font(32);
                                       const lv_font_t* f =
                                           lv_obj_get_style_text_font(title, LV_PART_MAIN);
                                       if (f != big) return std::string("font mismatch");
                                       if (lv_label_get_text(title) != std::string(c.title)) {
                                           return std::string("text: ") + lv_label_get_text(title);
                                       }
                                       lv_font_glyph_dsc_t dsc{};
                                       lv_font_get_glyph_dsc(f, &dsc, c.first_char, 0);
                                       if (dsc.resolved_font != big) {
                                           return std::string("fell back, not the vector font");
                                       }
                                       return std::string("ok");
                                   }),
                  "ok");
    }

    h.app.stop();
}
