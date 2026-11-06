// The scene runtime wired into App (scene-runtime-core spec 3.4, 3.5, 7.1,
// 8): the self-check screen's "Scenes" card, opening a scene from it and
// coming back, standby, deleting a scene through the device's confirmation
// dialog, and the headless case.

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <utility>
#include <vector>

#include "scene_wiring.hpp"
#include "self_check_layout.hpp"
#include "support/fake_devices.hpp"
#include "support/lvgl_probe.hpp"
#include "support/wait.hpp"
#include "ui_net_harness.hpp"
#include "wisp/app/power_manager.hpp"
#include "wisp/core/log.hpp"
#include "wisp/core/notification_service.hpp"
#include "wisp/core/scene_repository.hpp"
#include "wisp/core/sensor_service.hpp"
#include "wisp/core/system_settings.hpp"
#include "wisp/net/pairing_service.hpp"
#include "wisp/net/wifi_service.hpp"
#include "wisp/os/os.hpp"
#include "wisp/port/sim/sim_os.hpp"
#include "wisp/scene/scene_runtime.hpp"
#include "wisp/ui/strings.hpp"
#include "wisp/ui/system_layer.hpp"
#include "wisp/ui/theme.hpp"

using wisp::net::Method;
using wisp::test::count_obj_of_class;
using wisp::test::find_button_with_label;
using wisp::test::find_label_containing;
using wisp::test::find_label_text_containing;
using wisp::test::make_req;
using wisp::test::run_on_ui_blocking;
using wisp::test::run_on_ui_thread;
using wisp::test::UiNetHarness;
using wisp::test::UiNetOptions;
using wisp::test::wait_until;
using wisp::test::with_csrf;
using wisp::test::with_token;

namespace {

namespace fs = std::filesystem;

const char* kCounter = R"(local count = state(0)
scene.on_show = function() print("show") end
scene.on_hide = function() print("hide") end
return ui.screen {
  ui.column { pad = 16, gap = 12,
    ui.label { text = bind(count, function(v) return "已点击 " .. v .. " 次" end) },
    ui.button { ref = "plus", text = "+1", on_click = function() count:set(count:get() + 1) end },
  },
}
)";

// Writes a package into <int>/scenes/<id>, as an install leaves it.
void put_scene(const std::string& root, const std::string& id, const std::string& name,
               const std::string& main_lua) {
    const fs::path dir = fs::path(root) / "scenes" / id;
    fs::create_directories(dir);
    std::ofstream(dir / "scene.json") << R"({"id":")" << id << R"(","name":")" << name
                                      << R"(","version":"1.0.0","schema_version":1})";
    std::ofstream(dir / "main.lua") << main_lua;
}

std::string pair_browser(UiNetHarness& h) {
    wisp::net::PairingService* pairing = h.app->pairing();
    EXPECT_TRUE(pairing->request_code(wisp::os::now_ms()));
    auto code = pairing->active_code(wisp::os::now_ms()).value_or("");
    auto token = pairing->confirm(code, wisp::net::ClientInfo{}, wisp::os::now_ms());
    EXPECT_TRUE(token);
    return token ? token.value() : std::string();
}

wisp::net::HttpResponse api(UiNetHarness& h, Method method, const std::string& path,
                            const std::string& token, const std::string& query = {}) {
    wisp::net::HttpRequest req = with_csrf(with_token(make_req(method, path), token));
    req.query = query;
    return h.server->handle(req);
}

// UI thread: the self-check screen's "Scenes" card, or nullptr. The screen is
// found from the self-check root, not from whatever screen is active.
lv_obj_t* scenes_card(lv_obj_t* screen) {
    lv_obj_t* col = lv_obj_get_child(screen, 0);
    if (col == nullptr) return nullptr;
    for (uint32_t i = 0; i < lv_obj_get_child_count(col); ++i) {
        lv_obj_t* c = lv_obj_get_child(col, static_cast<int32_t>(i));
        if (lv_obj_get_child_count(c) == 0) continue;
        lv_obj_t* first = lv_obj_get_child(c, 0);
        if (lv_obj_check_type(first, &lv_label_class) &&
            std::string(lv_label_get_text(first)) == "Scenes") {
            return c;
        }
    }
    return nullptr;
}

// UI thread: "<label>|<label>..." for each row of the card under its
// heading, one line per row ("" for no card).
std::string scene_rows(lv_obj_t* screen) {
    lv_obj_t* card = scenes_card(screen);
    if (card == nullptr) return "";
    std::vector<std::pair<std::string, lv_area_t>> labels;
    std::string out;
    for (uint32_t i = 1; i < lv_obj_get_child_count(card); ++i) {
        labels.clear();
        wisp::test::collect_labels(lv_obj_get_child(card, static_cast<int32_t>(i)), labels);
        std::string line;
        for (const auto& [text, area] : labels) line += (line.empty() ? "" : "|") + text;
        out += line + "\n";
    }
    return out;
}

// UI thread: clicks the row of the "Scenes" card whose name is `name`.
bool click_scene_row(lv_obj_t* screen, const char* name) {
    lv_obj_t* card = scenes_card(screen);
    if (card == nullptr) return false;
    lv_obj_t* label = find_label_containing(card, name);
    lv_obj_t* row = label;
    while (row != nullptr && !lv_obj_has_flag(row, LV_OBJ_FLAG_CLICKABLE)) {
        row = lv_obj_get_parent(row);
    }
    if (row == nullptr || row == card) return false;
    lv_obj_send_event(row, LV_EVENT_CLICKED, nullptr);
    return true;
}

// The log ring outlives each test's App when the whole binary runs in one
// process; only lines logged after the current test booted count.
uint32_t g_log_floor = 0;

std::vector<std::string> scene_log(const std::string& id) {
    std::vector<std::string> out;
    for (auto& e : wisp::log::ring_entries()) {
        if (e.seq > g_log_floor && e.tag == "scene." + id) out.push_back(e.msg);
    }
    return out;
}

int count_of(const std::vector<std::string>& v, const std::string& s) {
    return static_cast<int>(std::count(v.begin(), v.end(), s));
}

// Keeps the loudest sample written to it.
class PeakAudioOut : public wisp::hal::AudioOut {
public:
    PeakAudioOut() {
        info_.id = "audio_out.main";
        info_.cls = kClass;
        info_.model = "FAKE";
    }
    const wisp::hal::DeviceInfo& info() const override { return info_; }
    wisp::Result<void> open(const wisp::hal::AudioFormat&) override {
        return wisp::Result<void>::ok();
    }
    wisp::Result<size_t> write(const int16_t* frames, size_t count) override {
        int peak = peak_.load();
        for (size_t i = 0; i < count; ++i) peak = std::max(peak, std::abs(int{frames[i]}));
        peak_.store(peak);
        frames_ += count;
        return wisp::Result<size_t>::ok(count);
    }
    void close() override {}
    wisp::Result<void> set_volume(uint8_t) override { return wisp::Result<void>::ok(); }
    // The loudest sample since the last take, and how many frames came.
    int take_peak() { return peak_.exchange(0); }
    size_t frames() const { return frames_.load(); }

private:
    wisp::hal::DeviceInfo info_;
    std::atomic<int> peak_{0};
    std::atomic<size_t> frames_{0};
};

struct Booted {
    explicit Booted(UiNetOptions opts = {}) : h(opts) {}

    bool start() {
        auto lines = wisp::log::ring_entries();
        g_log_floor = lines.empty() ? 0 : lines.back().seq;
        if (!h.start("scene.")) return false;
        screen = nullptr;
        run_on_ui_blocking(h.app->ui(), [&] { screen = lv_screen_active(); });
        return screen != nullptr;
    }

    std::string rows() {
        return run_on_ui_thread(h.app->ui(), [&] { return scene_rows(screen); });
    }

    bool wait_rows(const std::string& want, uint32_t ms = 3000) {
        return wait_until([&] { return rows() == want; }, ms);
    }

    std::vector<std::string> states(const std::string& id) {
        std::vector<std::string> out;
        for (const auto& e : h.topic("scene.state")) {
            if (e.payload.at("id").as_string() == id) {
                out.push_back(e.payload.at("state").as_string());
            }
        }
        return out;
    }

    UiNetHarness h;
    lv_obj_t* screen = nullptr;
};

}  // namespace

TEST(AppScenes, SelfCheckListsInstalledScenes) {
    Booted b;
    put_scene(b.h.root, "counter", "计数器", kCounter);
    put_scene(b.h.root, "clock", "时钟", "return ui.screen { ui.label { text = 'tick' } }");
    ASSERT_TRUE(b.start());
    ASSERT_NE(b.h.app->scenes(), nullptr);
    ASSERT_NE(b.h.app->scene_repository(), nullptr);
    // Sorted by id; a stopped scene shows its version.
    EXPECT_TRUE(b.wait_rows("时钟|1.0.0\n计数器|1.0.0\n")) << b.rows();
}

// Every row is a tap target at least 48 px tall, the whole row clickable,
// with its name and state centred vertically in it.
TEST(AppScenes, RowsAreTallEnoughToTap) {
    Booted b;
    put_scene(b.h.root, "counter", "计数器", kCounter);
    put_scene(b.h.root, "clock", "时钟", "return ui.screen { ui.label { text = 'tick' } }");
    ASSERT_TRUE(b.start());
    ASSERT_TRUE(b.wait_rows("时钟|1.0.0\n计数器|1.0.0\n")) << b.rows();
    // "<height>,<clickable>,<name centred>;" per row.
    std::string rows = run_on_ui_thread(b.h.app->ui(), [&] {
        lv_obj_t* card = scenes_card(b.screen);
        lv_obj_update_layout(card);
        std::string out;
        for (uint32_t i = 1; i < lv_obj_get_child_count(card); ++i) {
            lv_obj_t* row = lv_obj_get_child(card, static_cast<int32_t>(i));
            // The name and state line.
            lv_obj_t* name = lv_obj_get_child(row, 0);
            lv_area_t r, n;
            lv_obj_get_coords(row, &r);
            lv_obj_get_coords(name, &n);
            const int32_t gap_top = n.y1 - r.y1;
            const int32_t gap_bottom = r.y2 - n.y2;
            out += std::to_string(lv_area_get_height(&r)) + "," +
                   (lv_obj_has_flag(row, LV_OBJ_FLAG_CLICKABLE) ? "1" : "0") + "," +
                   (std::abs(gap_top - gap_bottom) <= 1 ? "1" : "0") + ";";
        }
        return out;
    });
    int count = 0;
    for (size_t pos = 0; pos < rows.size(); pos = rows.find(';', pos) + 1, ++count) {
        int height = 0, clickable = 0, centred = 0;
        ASSERT_EQ(std::sscanf(rows.c_str() + pos, "%d,%d,%d", &height, &clickable, &centred), 3)
            << rows;
        EXPECT_GE(height, 48) << rows;
        EXPECT_EQ(clickable, 1) << rows;
        EXPECT_EQ(centred, 1) << rows;
    }
    EXPECT_EQ(count, 2) << rows;
}

TEST(AppScenes, EmptyListSaysSo) {
    Booted b;
    ASSERT_TRUE(b.start());
    EXPECT_TRUE(b.wait_rows("No scenes installed\n")) << b.rows();
}

TEST(AppScenes, TapOpensSceneAndBackReturns) {
    UiNetOptions opts;
    opts.with_touch = true;
    Booted b(opts);
    put_scene(b.h.root, "counter", "计数器", kCounter);
    ASSERT_TRUE(b.start());
    ASSERT_TRUE(b.wait_rows("计数器|1.0.0\n")) << b.rows();

    ASSERT_EQ(run_on_ui_thread(b.h.app->ui(),
                               [&] { return click_scene_row(b.screen, "计数器") ? "ok" : ""; }),
              "ok");
    wisp::scene::SceneRuntime* rt = b.h.app->scenes();
    ASSERT_TRUE(wait_until(
        [&] { return rt->foreground() == std::optional<std::string>("counter"); }, 3000));
    ASSERT_TRUE(wait_until(
        [&] {
            return !run_on_ui_thread(b.h.app->ui(), [] {
                        return find_label_text_containing(lv_screen_active(), "已点击 0 次");
                    }).empty();
        },
        3000));
    // The list follows: running.
    EXPECT_TRUE(b.wait_rows("计数器|Running\n")) << b.rows();

    // The back gesture, as the touch panel reports it.
    lv_indev_t* indev = nullptr;
    run_on_ui_blocking(b.h.app->ui(), [&] { indev = lv_indev_get_next(nullptr); });
    ASSERT_NE(indev, nullptr);
    auto feed = [&](std::vector<wisp::hal::TouchPoint> points) {
        b.h.touch->set_points(std::move(points));
        run_on_ui_blocking(b.h.app->ui(), [&] { lv_indev_read(indev); });
    };
    for (int16_t x : {4, 4, 60, 120, 180, 240}) feed({{x, 240, 0}});
    feed({});

    ASSERT_TRUE(wait_until([&] { return !rt->foreground().has_value(); }, 3000));
    ASSERT_TRUE(wait_until(
        [&] {
            return run_on_ui_thread(b.h.app->ui(), [&] {
                       return lv_screen_active() == b.screen ? "home" : "";
                   }) == "home";
        },
        3000));
    EXPECT_TRUE(b.wait_rows("计数器|1.0.0\n")) << b.rows();
    EXPECT_EQ(b.states("counter"), (std::vector<std::string>{"running", "stopped"}));
}

// The pull-down panel's header names the Wi-Fi state only on a device
// that has Wi-Fi.
TEST(AppScenes, PanelHeaderShowsWifiOnlyWithWifi) {
    for (bool with_wifi : {true, false}) {
        SCOPED_TRACE(with_wifi);
        UiNetOptions opts;
        opts.with_wifi = with_wifi;
        Booted b(opts);
        ASSERT_TRUE(b.start());
        b.h.app->notifications()->post_system(wisp::core::notice::kBatteryLow, "9");
        // Opens the panel from the capsule once the capsule shows.
        auto open = [&] {
            std::string result;
            run_on_ui_blocking(b.h.app->ui(), [&] {
                // The capsule's own text: the banner shows a bell too.
                lv_obj_t* cap = find_label_containing(lv_layer_top(), LV_SYMBOL_BELL "1");
                while (cap != nullptr && !lv_obj_has_flag(cap, LV_OBJ_FLAG_CLICKABLE))
                    cap = lv_obj_get_parent(cap);
                if (cap == nullptr) {
                    result = "no capsule";
                    return;
                }
                lv_obj_send_event(cap, LV_EVENT_CLICKED, nullptr);
                result = "ok";
            });
            return result;
        };
        ASSERT_TRUE(wait_until([&] { return open() == "ok"; }, 3000)) << open();
        std::string wifi;
        std::string clear_all;
        run_on_ui_blocking(b.h.app->ui(), [&] {
            wifi = find_label_text_containing(lv_layer_top(), LV_SYMBOL_WIFI);
            clear_all = find_label_text_containing(lv_layer_top(),
                                                   wisp::ui::tr(wisp::ui::StrId::PanelClearAll));
        });
        EXPECT_FALSE(clear_all.empty());  // the panel is open
        if (with_wifi) {
            EXPECT_FALSE(wifi.empty());
        } else {
            EXPECT_TRUE(wifi.empty()) << wifi;
        }
    }
}

TEST(AppScenes, StandbyHidesAndWakeShows) {
    Booted b;
    put_scene(b.h.root, "counter", "计数器", kCounter);
    ASSERT_TRUE(b.start());
    ASSERT_TRUE(b.h.app->scenes()->open("counter"));
    ASSERT_TRUE(wait_until([] { return count_of(scene_log("counter"), "show") == 1; }, 3000));

    wisp::app::PowerManager* pm = b.h.app->power_manager();
    run_on_ui_blocking(b.h.app->ui(), [&] { pm->enter_standby(); });
    EXPECT_TRUE(wait_until([] { return count_of(scene_log("counter"), "hide") == 1; }, 3000));
    run_on_ui_blocking(b.h.app->ui(), [&] { pm->exit_standby(); });
    EXPECT_TRUE(wait_until([] { return count_of(scene_log("counter"), "show") == 2; }, 3000));
    EXPECT_EQ(scene_log("counter"), (std::vector<std::string>{"show", "hide", "show"}));
}

// Spec 6.1: the IMU is off with no subscribers. The self-check screen reads
// it only while it shows: a scene in front covers it, and with a scene
// that does not use the IMU nobody holds it.
TEST(AppScenes, SelfCheckLetsTheImuGoWhileASceneIsInFront) {
    UiNetOptions opts;
    auto imu = std::make_shared<wisp::test::FakeImu>("imu.main");
    opts.imu = imu;
    Booted b(opts);
    put_scene(b.h.root, "counter", "计数器", kCounter);
    ASSERT_TRUE(b.start());
    wisp::core::SensorService* sensors = b.h.app->sensors();
    ASSERT_NE(sensors, nullptr);
    auto holders = [&] {
        std::vector<std::string> out;
        for (const auto& e : b.h.app->occupancy().list()) out.push_back(e.holder);
        return out;
    };
    const std::vector<std::string> selfcheck = {"system.selfcheck"};
    ASSERT_TRUE(wait_until([&] { return holders() == selfcheck; }, 3000));
    EXPECT_EQ(sensors->imu_rate(), 50u);

    ASSERT_TRUE(b.h.app->scenes()->open("counter"));
    ASSERT_TRUE(wait_until([] { return count_of(scene_log("counter"), "show") == 1; }, 3000));
    EXPECT_TRUE(wait_until([&] { return holders().empty(); }, 3000));
    EXPECT_TRUE(wait_until([&] { return sensors->imu_rate() == 0; }, 3000));

    ASSERT_TRUE(b.h.app->scenes()->exit("counter"));
    EXPECT_TRUE(wait_until([&] { return holders() == selfcheck; }, 3000));
    EXPECT_TRUE(wait_until([&] { return sensors->imu_rate() == 50u; }, 3000));
}

TEST(AppScenes, DeleteConfirmFlow) {
    UiNetOptions opts;
    opts.with_server = true;
    Booted b(opts);
    put_scene(b.h.root, "counter", "计数器", kCounter);
    put_scene(b.h.root, "keep", "保留", "return ui.screen { ui.label { text = 'k' } }");
    fs::create_directories(fs::path(b.h.root) / "scenes" / "counter" / "data");
    std::ofstream(fs::path(b.h.root) / "scenes" / "counter" / "data" / "kv.log") << "x\n";
    ASSERT_TRUE(b.start());
    const std::string token = pair_browser(b.h);
    ASSERT_TRUE(b.h.app->scenes()->open("counter"));

    auto res = api(b.h, Method::Delete, "/api/v1/scenes/counter", token, "data=1");
    ASSERT_EQ(res.status, 202) << res.body;
    auto dialog = [&] {
        return run_on_ui_thread(
            b.h.app->ui(), [] { return find_label_text_containing(lv_layer_top(), "删除场景“"); });
    };
    EXPECT_EQ(dialog(), "删除场景“计数器”？\n场景数据也会一并删除。");
    // Deleting is a danger button.
    EXPECT_EQ(run_on_ui_thread(
                  b.h.app->ui(),
                  [] {
                      lv_obj_t* yes = find_button_with_label(lv_layer_top(), "是");
                      if (yes == nullptr) return std::string("none");
                      const uint32_t fill =
                          lv_color_to_u32(lv_obj_get_style_bg_color(yes, LV_PART_MAIN)) & 0xFFFFFFu;
                      return std::string(fill == wisp::ui::theme::kErr ? "danger" : "other");
                  }),
              "danger");
    ASSERT_EQ(run_on_ui_thread(b.h.app->ui(),
                               [] {
                                   lv_obj_t* yes = find_button_with_label(lv_layer_top(), "是");
                                   if (yes == nullptr) return "";
                                   lv_obj_send_event(yes, LV_EVENT_CLICKED, nullptr);
                                   return "ok";
                               }),
              "ok");
    // Closed first (its last flush lands before the files go), then removed.
    ASSERT_TRUE(
        wait_until([&] { return !fs::exists(fs::path(b.h.root) / "scenes" / "counter"); }, 3000));
    EXPECT_FALSE(b.h.app->scenes()->foreground().has_value());
    EXPECT_TRUE(wait_until(
        [&] {
            auto s = b.states("counter");
            return !s.empty() && s.back() == "deleted";
        },
        3000));
    EXPECT_EQ(b.states("counter"), (std::vector<std::string>{"running", "stopped", "deleted"}));
    EXPECT_TRUE(b.wait_rows("保留|1.0.0\n")) << b.rows();

    // "否": nothing happens.
    res = api(b.h, Method::Delete, "/api/v1/scenes/keep", token);
    ASSERT_EQ(res.status, 202) << res.body;
    EXPECT_EQ(dialog(), "删除场景“保留”？");
    ASSERT_EQ(run_on_ui_thread(b.h.app->ui(),
                               [] {
                                   lv_obj_t* no = find_button_with_label(lv_layer_top(), "否");
                                   if (no == nullptr) return "";
                                   lv_obj_send_event(no, LV_EVENT_CLICKED, nullptr);
                                   return "ok";
                               }),
              "ok");
    wisp::os::sleep_ms(300);
    EXPECT_TRUE(fs::exists(fs::path(b.h.root) / "scenes" / "keep" / "scene.json"));
    EXPECT_TRUE(b.states("keep").empty());
}

// The package is gone but its data stayed (a delete without data, or a
// package that went missing): deleting with data still reaches it.
TEST(AppScenes, DeleteReachesDataLeftBehind) {
    UiNetOptions opts;
    opts.with_server = true;
    Booted b(opts);
    const fs::path data = fs::path(b.h.root) / "scenes" / "gone" / "data";
    fs::create_directories(data);
    std::ofstream(data / "kv.log") << "x\n";
    ASSERT_TRUE(b.start());
    const std::string token = pair_browser(b.h);

    // Without data there is nothing to delete.
    EXPECT_EQ(api(b.h, Method::Delete, "/api/v1/scenes/gone", token).status, 404);
    auto res = api(b.h, Method::Delete, "/api/v1/scenes/gone", token, "data=1");
    ASSERT_EQ(res.status, 202) << res.body;
    ASSERT_EQ(run_on_ui_thread(b.h.app->ui(),
                               [] {
                                   lv_obj_t* yes = find_button_with_label(lv_layer_top(), "是");
                                   if (yes == nullptr) return "";
                                   lv_obj_send_event(yes, LV_EVENT_CLICKED, nullptr);
                                   return "ok";
                               }),
              "ok");
    EXPECT_TRUE(
        wait_until([&] { return !fs::exists(fs::path(b.h.root) / "scenes" / "gone"); }, 3000));
    EXPECT_EQ(api(b.h, Method::Delete, "/api/v1/scenes/nothing", token, "data=1").status, 404);
}

// A delete with data that fails before the scene is gone leaves its
// files: the scene is still installed and still owns them.
TEST(AppScenes, FailedDeleteKeepsFiles) {
    UiNetOptions opts;
    opts.with_server = true;
    Booted b(opts);
    put_scene(b.h.root, "stuck", "卡住", "return ui.screen { ui.label { text = 's' } }");
    const fs::path dir = fs::path(b.h.root) / "scenes" / "stuck";
    fs::create_directories(dir / "data");
    fs::create_directories(dir / "files");
    std::ofstream(dir / "data" / "files_loc.json") << R"({"location":"int"})";
    std::ofstream(dir / "files" / "a.txt") << "keep me";
    ASSERT_TRUE(b.start());
    const std::string token = pair_browser(b.h);

    auto res = api(b.h, Method::Delete, "/api/v1/scenes/stuck", token, "data=1");
    ASSERT_EQ(res.status, 202) << res.body;
    // An interrupted replace whose data/ clashes with the scene's: the
    // repository stops before it removes anything.
    fs::create_directories(fs::path(b.h.root) / "scenes" / ".stuck.newdir" / "data");
    ASSERT_EQ(run_on_ui_thread(b.h.app->ui(),
                               [] {
                                   lv_obj_t* yes = find_button_with_label(lv_layer_top(), "是");
                                   if (yes == nullptr) return "";
                                   lv_obj_send_event(yes, LV_EVENT_CLICKED, nullptr);
                                   return "ok";
                               }),
              "ok");
    EXPECT_TRUE(wait_until(
        [] {
            for (const auto& e : wisp::log::ring_entries())
                if (e.msg.find("stuck: delete failed") != std::string::npos) return true;
            return false;
        },
        3000));
    EXPECT_TRUE(fs::exists(dir / "scene.json"));
    std::stringstream kept;
    kept << std::ifstream(dir / "files" / "a.txt").rdbuf();
    EXPECT_EQ(kept.str(), "keep me");
    EXPECT_TRUE(fs::exists(dir / "data" / "files_loc.json"));
    EXPECT_TRUE(b.states("stuck").empty());
}

TEST(AppScenes, DeleteBusyWhenDialogOpen) {
    UiNetOptions opts;
    opts.with_server = true;
    Booted b(opts);
    put_scene(b.h.root, "counter", "计数器", kCounter);
    ASSERT_TRUE(b.start());
    const std::string token = pair_browser(b.h);
    ASSERT_EQ(api(b.h, Method::Post, "/api/v1/system/factory-reset", token).status, 202);
    ASSERT_TRUE(wait_until(
        [&] {
            return run_on_ui_thread(b.h.app->ui(), [] {
                       return std::to_string(count_obj_of_class(lv_layer_top(), &lv_msgbox_class));
                   }) == "1";
        },
        3000));
    auto res = api(b.h, Method::Delete, "/api/v1/scenes/counter", token);
    EXPECT_EQ(res.status, 409) << res.body;
    EXPECT_EQ(
        run_on_ui_thread(
            b.h.app->ui(),
            [] { return std::to_string(count_obj_of_class(lv_layer_top(), &lv_msgbox_class)); }),
        "1");
    EXPECT_TRUE(run_on_ui_thread(b.h.app->ui(), [] {
                    return find_label_text_containing(lv_layer_top(), "删除场景“");
                }).empty());
    EXPECT_TRUE(fs::exists(fs::path(b.h.root) / "scenes" / "counter" / "scene.json"));
}

// Shutdown destroys the system layer before networking stops, so a delete
// request can still arrive after it is gone: it must be refused, not
// opened on freed memory.
TEST(AppScenes, DeleteAfterStopRuntimeIsRefused) {
    Booted b;
    put_scene(b.h.root, "counter", "计数器", kCounter);
    ASSERT_TRUE(b.start());
    wisp::ui::UiRuntime& ui = *b.h.app->ui();

    wisp::app::Platform platform;
    wisp::app::SceneWiring wiring(
        wisp::app::SceneWiringDeps{platform, b.h.app->bus(), b.h.app->storage()});
    wiring.start_storage();
    ASSERT_NE(wiring.repository(), nullptr);
    std::unique_ptr<wisp::ui::SystemLayer> layer;
    const wisp::hal::ScreenShape shape{wisp::hal::ScreenShapeKind::Rect, 480, 480, 0};
    run_on_ui_blocking(&ui, [&] { layer = std::make_unique<wisp::ui::SystemLayer>(shape); });
    wiring.build_runtime(ui, *layer, shape, [] { return wisp::Value(); }, [] {});
    wiring.start_runtime([] { return false; });

    wiring.stop_runtime();
    run_on_ui_blocking(&ui, [&] { layer.reset(); });
    auto r = wiring.request_delete("counter", false);
    EXPECT_FALSE(r);
    if (!r) {
        EXPECT_EQ(r.error().code, wisp::ErrorCode::Busy);
    }
    EXPECT_EQ(run_on_ui_thread(
                  &ui, [] { return find_label_text_containing(lv_layer_top(), "删除场景“"); }),
              "");
    wiring.stop();
}

// The scene services are long-lived and small, so malloc would put them
// in internal RAM; they come from Mem::Large, and without it scenes are
// off rather than the boot failing.
TEST(AppScenes, ScenesAreOffWithoutLargeMemory) {
    Booted b;
    ASSERT_TRUE(b.start());
    wisp::ui::UiRuntime& ui = *b.h.app->ui();
    wisp::app::Platform platform;
    const wisp::hal::ScreenShape shape{wisp::hal::ScreenShapeKind::Rect, 480, 480, 0};
    std::unique_ptr<wisp::ui::SystemLayer> layer;
    run_on_ui_blocking(&ui, [&] { layer = std::make_unique<wisp::ui::SystemLayer>(shape); });

    {
        wisp::app::SceneWiring wiring(
            wisp::app::SceneWiringDeps{platform, b.h.app->bus(), b.h.app->storage()});
        wisp::port::sim::set_alloc_failure(wisp::os::Mem::Large, true);
        wiring.start_storage();
        wisp::port::sim::set_alloc_failure(wisp::os::Mem::Large, false);
        EXPECT_EQ(wiring.repository(), nullptr);
    }
    {
        wisp::app::SceneWiring wiring(
            wisp::app::SceneWiringDeps{platform, b.h.app->bus(), b.h.app->storage()});
        wiring.start_storage();
        ASSERT_NE(wiring.repository(), nullptr);
        wisp::port::sim::set_alloc_failure(wisp::os::Mem::Large, true);
        wiring.build_runtime(ui, *layer, shape, [] { return wisp::Value(); }, [] {});
        wisp::port::sim::set_alloc_failure(wisp::os::Mem::Large, false);
        EXPECT_EQ(wiring.runtime(), nullptr);
        EXPECT_FALSE(wiring.request_delete("counter", false));
    }
    run_on_ui_blocking(&ui, [&] { layer.reset(); });
}

TEST(AppScenes, ErrorBadgeInList) {
    Booted b;
    put_scene(b.h.root, "bad", "坏", "error('boom')\n");
    ASSERT_TRUE(b.start());
    ASSERT_TRUE(b.wait_rows("坏|1.0.0\n")) << b.rows();
    wisp::scene::SceneRuntime* rt = b.h.app->scenes();
    EXPECT_FALSE(rt->open("bad"));
    ASSERT_TRUE(rt->close("bad"));
    EXPECT_TRUE(b.wait_rows("坏|Error\n")) << b.rows();
    // The badge is red text on a red tint.
    EXPECT_EQ(
        run_on_ui_thread(
            b.h.app->ui(),
            [&] {
                lv_obj_t* badge = find_label_containing(scenes_card(b.screen), "Error");
                if (badge == nullptr) return std::string("none");
                lv_color_t tint = lv_obj_get_style_bg_color(lv_obj_get_parent(badge), LV_PART_MAIN);
                lv_color_t text = lv_obj_get_style_text_color(badge, LV_PART_MAIN);
                return std::string(lv_color_eq(tint, lv_color_hex(wisp::ui::theme::kErrSoft)) &&
                                           lv_color_eq(text, lv_color_hex(wisp::app::kColorError))
                                       ? "red"
                                       : "other");
            }),
        "red");
}

TEST(AppScenes, HeadlessHasNoRuntime) {
    UiNetOptions opts;
    opts.with_display = false;
    opts.with_server = true;
    Booted b(opts);
    put_scene(b.h.root, "counter", "计数器", kCounter);
    ASSERT_TRUE(b.h.start());
    EXPECT_EQ(b.h.app->scenes(), nullptr);
    ASSERT_NE(b.h.app->scene_repository(), nullptr);
    const std::string token = pair_browser(b.h);
    EXPECT_EQ(api(b.h, Method::Post, "/api/v1/scenes/counter/open", token).status, 503);
    auto list = api(b.h, Method::Get, "/api/v1/scenes", token);
    EXPECT_EQ(list.status, 200);
    EXPECT_NE(list.body.find("\"counter\""), std::string::npos) << list.body;
    EXPECT_EQ(api(b.h, Method::Delete, "/api/v1/scenes/counter", token).status, 503);
}

// wisp.device.info() sees the Wi-Fi state and the battery the rest of the
// system sees, without the scene task reading anything itself.
TEST(AppScenes, DeviceInfoReportsWifi) {
    UiNetOptions opts;
    opts.saved = {{"Home", "password1"}};
    Booted b(opts);
    put_scene(b.h.root, "info", "信息", R"(
scene.on_show = function() print("wifi " .. wisp.device.info().wifi) end
return ui.screen { ui.label { text = "i" } }
)");
    ASSERT_TRUE(b.start());
    ASSERT_TRUE(wait_until(
        [&] { return b.h.app->wifi()->status().state == wisp::net::WifiState::Connected; }, 5000));
    // The snapshot follows the net task's next tick.
    wisp::os::sleep_ms(400);
    ASSERT_TRUE(b.h.app->scenes()->open("info"));
    EXPECT_EQ(scene_log("info"), (std::vector<std::string>{"wifi connected"}));
}

// The stored time zone reaches wisp.device.info() at boot.
TEST(AppScenes, DeviceInfoReportsTheStoredUtcOffset) {
    Booted b;
    ASSERT_TRUE(wisp::core::Settings(*b.h.app->platform().settings, "sys").set("tz_min", 330));
    put_scene(b.h.root, "tz", "时区", R"(
scene.on_show = function() print("tz " .. wisp.device.info().utc_offset) end
return ui.screen { ui.label { text = "t" } }
)");
    ASSERT_TRUE(b.start());
    ASSERT_TRUE(b.h.app->scenes()->open("tz"));
    EXPECT_EQ(scene_log("tz"), (std::vector<std::string>{"tz 330"}));
}

// A system language change while a scene is up leaves both screens alone:
// the self-check page keeps its own language (English here) and the
// scene's screen is not touched.
TEST(AppScenes, LanguageChangeWhileASceneIsOpen) {
    Booted b;
    put_scene(b.h.root, "counter", "计数器", kCounter);
    ASSERT_TRUE(b.start());
    ASSERT_TRUE(b.h.app->scenes()->open("counter"));
    ASSERT_TRUE(wait_until(
        [&] {
            return !run_on_ui_thread(b.h.app->ui(), [] {
                        return find_label_text_containing(lv_screen_active(), "已点击 0 次");
                    }).empty();
        },
        3000));
    lv_obj_t* scene_screen = nullptr;
    uint32_t scene_children = 0;
    run_on_ui_blocking(b.h.app->ui(), [&] {
        scene_screen = lv_screen_active();
        scene_children = lv_obj_get_child_count(scene_screen);
    });
    ASSERT_NE(scene_screen, b.screen);

    ASSERT_TRUE(b.h.app->system_settings().set({{"language", wisp::Value("ja")}}));
    ASSERT_TRUE(wait_until([&] { return wisp::ui::lang() == wisp::ui::Lang::Ja; }, 3000));
    run_on_ui_blocking(b.h.app->ui(), [&] {
        EXPECT_EQ(lv_screen_active(), scene_screen);
        EXPECT_EQ(lv_obj_get_child_count(scene_screen), scene_children);
        EXPECT_EQ(find_label_containing(scene_screen, "Devices"), nullptr);
        EXPECT_NE(find_label_containing(b.screen, "Devices"), nullptr);
        EXPECT_EQ(find_label_containing(b.screen, "デバイス"), nullptr);
    });
    ASSERT_TRUE(b.h.app->scenes()->close("counter"));
    run_on_ui_blocking(b.h.app->ui(), [&] { EXPECT_EQ(lv_screen_active(), b.screen); });
}

// The 100 ms refresh and a scene.state that changes nothing leave the card
// alone: nothing over it is flushed again.
TEST(AppScenes, UnchangedListIsNotRedrawn) {
    Booted b;
    put_scene(b.h.root, "counter", "计数器", kCounter);
    ASSERT_TRUE(b.start());
    ASSERT_TRUE(b.wait_rows("计数器|1.0.0\n")) << b.rows();
    std::string band = run_on_ui_thread(b.h.app->ui(), [&] {
        lv_obj_t* card = scenes_card(b.screen);
        lv_obj_scroll_to_view_recursive(card, LV_ANIM_OFF);
        lv_obj_update_layout(card);
        lv_area_t a;
        lv_obj_get_coords(card, &a);
        return std::to_string(a.y1) + "," + std::to_string(a.y2);
    });
    int y1 = 0, y2 = 0;
    ASSERT_EQ(std::sscanf(band.c_str(), "%d,%d", &y1, &y2), 2);
    ASSERT_LT(y1, 480);
    wisp::os::sleep_ms(1500);

    const size_t before = b.h.display->areas().size();
    b.h.app->bus().publish({"scene.state",
                            "scene",
                            0,
                            {{"id", wisp::Value("counter")}, {"state", wisp::Value("stopped")}}});
    wisp::os::sleep_ms(800);
    std::vector<wisp::hal::Area> areas = b.h.display->areas();
    for (size_t i = before; i < areas.size(); ++i) {
        EXPECT_FALSE(areas[i].y1 <= y2 && areas[i].y2 >= y1)
            << "flushed " << areas[i].x1 << "," << areas[i].y1 << "-" << areas[i].x2 << ","
            << areas[i].y2 << " over the card " << y1 << "-" << y2;
    }
}

// Dev mode (spec 6.4): the scene in front reloads when its files change on
// disk; a broken save shows the error (state, log) and the next good save
// brings the scene back. Scenes not in front are left alone.
TEST(AppScenes, DevModeReloadsTheSceneInFront) {
    const fs::path work = fs::path(wisp::test::temp_dir()) / "work";
    const fs::path data = fs::path(wisp::test::temp_dir()) / "data";
    auto put = [&](const std::string& id, const std::string& main_lua) {
        fs::create_directories(work / id);
        std::ofstream(work / id / "scene.json") << R"({"id":")" << id << R"(","name":")" << id
                                                << R"(","version":"1.0.0","schema_version":1})";
        std::ofstream(work / id / "main.lua", std::ios::trunc) << main_lua;
    };
    put("counter", kCounter);
    put("other", "return ui.screen { ui.label { text = 'other' } }");
    UiNetOptions opts;
    opts.scenes_dir = work.string();
    opts.scenes_data_dir = data.string();
    Booted b(opts);
    ASSERT_TRUE(b.start());
    wisp::scene::SceneRuntime* rt = b.h.app->scenes();
    ASSERT_NE(rt, nullptr);
    EXPECT_EQ(b.h.app->scene_repository()->data_dir("counter"), data.string() + "/counter/data");
    ASSERT_TRUE(rt->open("counter"));
    auto shows = [&](const std::string& text) {
        return wait_until(
            [&] {
                return !run_on_ui_thread(b.h.app->ui(), [&] {
                            return find_label_text_containing(lv_screen_active(), text.c_str());
                        }).empty();
            },
            5000);
    };
    ASSERT_TRUE(shows("已点击 0 次"));
    ASSERT_TRUE(rt->click_ref("plus"));
    ASSERT_TRUE(shows("已点击 1 次"));

    std::string edited = kCounter;
    edited.replace(edited.find("已点击"), std::string("已点击").size(), "改过");
    put("counter", edited);
    EXPECT_TRUE(shows("改过 0 次"));

    put("counter", "return ui.screen {\n");
    EXPECT_TRUE(wait_until(
        [&] { return rt->status("counter").state == wisp::core::SceneState::Error; }, 5000));
    EXPECT_FALSE(scene_log("counter").empty());

    put("counter", kCounter);
    EXPECT_TRUE(shows("已点击 0 次"));
    EXPECT_EQ(rt->status("counter").state, wisp::core::SceneState::Running);

    put("other", "return ui.screen { ui.label { text = 'changed' } }");
    wisp::os::sleep_ms(1500);
    EXPECT_EQ(rt->foreground(), std::optional<std::string>("counter"));
    EXPECT_EQ(rt->status("other").state, wisp::core::SceneState::Stopped);
}

// Spec 3.6: once boot completes, the scenes the list names come up in the
// background; one that cannot, and one that fails there later, become
// system notifications (code and the scene's name), and the list drops
// the one that could not be restored.
TEST(AppScenes, RestoresAfterBootCompleted) {
    Booted b;
    auto put_bg = [&](const std::string& id, const std::string& name, const std::string& lua) {
        const fs::path dir = fs::path(b.h.root) / "scenes" / id;
        fs::create_directories(dir);
        std::ofstream(dir / "scene.json") << R"({"id":")" << id << R"(","name":")" << name
                                          << R"(","version":"1.0.0","schema_version":1,)"
                                          << R"("background":true})";
        std::ofstream(dir / "main.lua") << lua;
    };
    put_bg("alpha", "Alpha",
           "print(wisp.scene.launch()) return ui.screen { ui.label { text = 'a' } }");
    put_bg("late", "Late", R"(
wisp.timer.after(300, function() error("late") end)
return ui.screen { ui.label { text = "late" } }
)");
    put_scene(b.h.root, "plain", "Plain", "return ui.screen { ui.label { text = 'p' } }");
    const fs::path list = fs::path(b.h.root) / "system" / "scenes" / "background.json";
    fs::create_directories(list.parent_path());
    std::ofstream(list) << R"({"ids":["alpha","gone","plain","late"]})";

    ASSERT_TRUE(b.start());
    wisp::scene::SceneRuntime* rt = b.h.app->scenes();
    ASSERT_NE(rt, nullptr);
    ASSERT_TRUE(wait_until(
        [&] { return rt->status("alpha").state == wisp::core::SceneState::Background; }, 3000));
    EXPECT_EQ(scene_log("alpha"), (std::vector<std::string>{"boot"}));
    EXPECT_EQ(rt->foreground(), std::nullopt);
    ASSERT_TRUE(wait_until(
        [&] { return rt->status("late").state == wisp::core::SceneState::Error; }, 3000));

    wisp::core::NotificationService* notes = b.h.app->notifications();
    ASSERT_NE(notes, nullptr);
    auto system_notices = [&] {
        std::vector<std::string> out;
        for (const auto& n : notes->unread())
            if (n.source == wisp::core::kSystemSource)
                out.push_back(std::string(n.code) + " " + std::string(n.arg));
        std::sort(out.begin(), out.end());
        return out;
    };
    const std::vector<std::string> want = {
        std::string(wisp::core::notice::kRestoreFailed) + " Plain",
        std::string(wisp::core::notice::kRestoreFailed) + " gone",
        std::string(wisp::core::notice::kSceneError) + " Late",
    };
    EXPECT_TRUE(wait_until([&] { return system_notices() == want; }, 3000))
        << ::testing::PrintToString(system_notices());

    // The list keeps what came up: alpha, and late until it failed.
    auto read_list = [&] {
        std::ifstream f(list);
        std::stringstream ss;
        ss << f.rdbuf();
        return ss.str();
    };
    EXPECT_TRUE(wait_until([&] { return read_list() == R"({"ids":["alpha"]})"; }, 3000))
        << read_list();
}

// A scene's sound="tone" plays at the audio/notify_vol setting, and a
// change to it applies to the next notification.
TEST(AppScenes, NotificationToneFollowsNotifyVolume) {
    auto out = std::make_shared<PeakAudioOut>();
    UiNetOptions opts;
    opts.audio_out = out;
    Booted b(opts);
    ASSERT_TRUE(b.h.app->system_settings().set({{"notify_vol", wisp::Value(80)}}));
    put_scene(b.h.root, "beep", "提示", R"(
return ui.screen {
  ui.button { ref = "beep", text = "beep",
              on_click = function() wisp.notify{ title = "t", sound = "tone" } end },
}
)");
    ASSERT_TRUE(b.start());
    wisp::scene::SceneRuntime* rt = b.h.app->scenes();
    ASSERT_NE(rt, nullptr);
    ASSERT_TRUE(rt->open("beep"));
    auto click_until = [&](size_t frames) {
        return wait_until([&] { return static_cast<bool>(rt->click_ref("beep")); }, 3000) &&
               wait_until([&] { return out->frames() >= frames; }, 3000);
    };

    ASSERT_TRUE(click_until(2400));
    const int at80 = out->take_peak();
    ASSERT_GT(at80, 0);

    ASSERT_TRUE(b.h.app->system_settings().set({{"notify_vol", wisp::Value(20)}}));
    ASSERT_TRUE(b.h.app->bus().wait_idle(2000));
    ASSERT_TRUE(click_until(4800));
    const int at20 = out->take_peak();
    EXPECT_NEAR(static_cast<double>(at20) / at80, 0.25, 0.25 * 0.05);
}
