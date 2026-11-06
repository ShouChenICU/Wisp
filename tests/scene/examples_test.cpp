// The example scenes in scenes/examples (scene-runtime-core spec 9.3): each
// package's scene.json parses, every .lua file compiles, and each one opens
// in a SceneRuntime on a fake display and is still running two seconds
// later without the error panel, with nothing it shows cut off by the
// screen's shape (scenes own the whole screen and lay themselves out from
// wisp.device.info() and row_span(), AGENTS.md section 3). wisp.http goes
// to a fake client that answers the weather example with a fixed
// forecast: no test here touches the network. wisp.sensor reads a fake
// IMU (the level example), and wisp.notify posts to a NotificationService
// in memory. The UI has the real vector font image, so large text is laid
// out at its real size.

#include <gtest/gtest.h>

#include <atomic>
#include <ctime>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include "support/fake_http.hpp"
#include "support/font_image.hpp"
#include "support/lvgl_probe.hpp"
#include "support/scene_runtime_fixture.hpp"
#include "support/wait.hpp"
#include "wisp/core/notification_service.hpp"
#include "wisp/core/occupancy.hpp"
#include "wisp/core/scene_manifest.hpp"
#include "wisp/core/sensor_service.hpp"
#include "wisp/hal/display_geometry.hpp"
#include "wisp/net/scene_http.hpp"
#include "wisp/scene/vm.hpp"

namespace {

namespace fs = std::filesystem;

using wisp::core::SceneState;
using wisp::test::wait_until;

const char* const kExamples[] = {"counter", "pomodoro", "todo",    "ledger",
                                 "clock",   "sysinfo",  "weather", "level"};

// A forecast the way api.open-meteo.com answers ?current=...&daily=...
const char* kForecast = R"({"latitude":39.9,"longitude":116.4,"timezone":"Asia/Shanghai",
"current_units":{"temperature_2m":"°C"},
"current":{"time":"2026-11-04T10:00","temperature_2m":12.3,"relative_humidity_2m":41,
"weather_code":2,"wind_speed_10m":9.4},
"daily":{"time":["2026-11-04","2026-11-05","2026-11-06"],
"weather_code":[2,61,0],"temperature_2m_max":[15.1,11.0,16.4],
"temperature_2m_min":[4.2,3.9,2.0]}})";

fs::path examples_dir() { return fs::path(WISP_ROOT) / "scenes" / "examples"; }

std::string read_file(const fs::path& p) {
    std::ifstream f(p, std::ios::binary);
    return {std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>()};
}

// What a scene shows that must not be cut off by the screen's shape;
// nullptr for anything else (layout objects, a canvas).
const char* content_kind(const lv_obj_t* o) {
    static const struct {
        const lv_obj_class_t* cls;
        const char* name;
    } kinds[] = {{&lv_label_class, "label"},       {&lv_button_class, "button"},
                 {&lv_checkbox_class, "checkbox"}, {&lv_textarea_class, "input"},
                 {&lv_switch_class, "switch"},     {&lv_slider_class, "slider"},
                 {&lv_bar_class, "bar"},           {&lv_chart_class, "chart"},
                 {&lv_image_class, "image"},       {&lv_arc_class, "arc"}};
    for (const auto& k : kinds)
        if (lv_obj_check_type(o, k.cls)) return k.name;
    return nullptr;
}

bool hidden_or_under_hidden(lv_obj_t* o) {
    for (; o != nullptr; o = lv_obj_get_parent(o))
        if (lv_obj_has_flag(o, LV_OBJ_FLAG_HIDDEN)) return true;
    return false;
}

// Whether an ancestor below `top` scrolls vertically: what is in it may
// pass through invisible areas (only the ends of a list are checked,
// with list_end_visible()).
bool in_scroller(lv_obj_t* o, lv_obj_t* top) {
    for (lv_obj_t* p = lv_obj_get_parent(o); p != nullptr && p != top; p = lv_obj_get_parent(p))
        if (lv_obj_get_scroll_top(p) > 0 || lv_obj_get_scroll_bottom(p) > 0) return true;
    return false;
}

// The points of `o` that must be visible: its corners, or for an arc
// (a ring in a square box) the middle of each edge.
std::vector<std::pair<int32_t, int32_t>> key_points(lv_obj_t* o) {
    lv_area_t a;
    lv_obj_get_coords(o, &a);
    if (lv_obj_check_type(o, &lv_arc_class)) {
        const int32_t mx = (a.x1 + a.x2) / 2, my = (a.y1 + a.y2) / 2;
        return {{mx, a.y1}, {mx, a.y2}, {a.x1, my}, {a.x2, my}};
    }
    return {{a.x1, a.y1}, {a.x2, a.y1}, {a.x1, a.y2}, {a.x2, a.y2}};
}

// Every content object under `root` (not hidden, not zero-sized, and,
// when `skip_scrolling`, not inside a scrolling container) that has a
// key point outside the visible screen; empty when nothing is cut off.
void find_clipped(lv_obj_t* root, lv_obj_t* top, const wisp::hal::ScreenShape& shape,
                  bool skip_scrolling, std::vector<std::string>& out) {
    const uint32_t n = lv_obj_get_child_count(root);
    for (uint32_t i = 0; i < n; ++i) {
        lv_obj_t* c = lv_obj_get_child(root, i);
        if (hidden_or_under_hidden(c)) continue;
        const char* kind = content_kind(c);
        if (kind != nullptr && lv_obj_get_width(c) > 0 && lv_obj_get_height(c) > 0 &&
            !(skip_scrolling && in_scroller(c, top))) {
            for (auto [x, y] : key_points(c)) {
                if (wisp::hal::visible(shape, x, y)) continue;
                lv_area_t a;
                lv_obj_get_coords(c, &a);
                std::string what = lv_obj_check_type(c, &lv_label_class)
                                       ? std::string("label '") + lv_label_get_text(c) + "'"
                                       : std::string(kind);
                out.push_back(what + " at " + std::to_string(a.x1) + "," + std::to_string(a.y1) +
                              "-" + std::to_string(a.x2) + "," + std::to_string(a.y2));
                break;
            }
        }
        // A button's, input's or checkbox's own parts are inside its box.
        if (kind == nullptr) find_clipped(c, top, shape, skip_scrolling, out);
    }
}

lv_obj_t* find_checkbox_with(lv_obj_t* obj, const char* text) {
    if (lv_obj_check_type(obj, &lv_checkbox_class) &&
        std::string(lv_checkbox_get_text(obj)).find(text) != std::string::npos)
        return obj;
    const uint32_t n = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < n; ++i)
        if (lv_obj_t* f = find_checkbox_with(lv_obj_get_child(obj, i), text)) return f;
    return nullptr;
}

lv_obj_t* find_scroller(lv_obj_t* obj) {
    const uint32_t n = lv_obj_get_child_count(obj);
    for (uint32_t i = 0; i < n; ++i) {
        lv_obj_t* c = lv_obj_get_child(obj, i);
        if (lv_obj_get_scroll_top(c) > 0 || lv_obj_get_scroll_bottom(c) > 0) return c;
        if (lv_obj_t* f = find_scroller(c)) return f;
    }
    return nullptr;
}

TEST(ExamplesPackages, ManifestsParseAndLuaCompiles) {
    for (const char* id : kExamples) {
        SCOPED_TRACE(id);
        const fs::path dir = examples_dir() / id;
        ASSERT_TRUE(fs::is_directory(dir)) << dir;
        auto m = wisp::core::parse_scene_manifest(read_file(dir / "scene.json"));
        ASSERT_TRUE(m) << m.error().message;
        EXPECT_EQ(m.value().id, id);
        EXPECT_TRUE(fs::is_regular_file(dir / m.value().entry));
        int lua_files = 0;
        for (const auto& e : fs::recursive_directory_iterator(dir)) {
            if (!e.is_regular_file() || e.path().extension() != ".lua") continue;
            ++lua_files;
            const std::string rel = fs::relative(e.path(), dir).generic_string();
            auto err = wisp::scene::compile_check(rel, read_file(e.path()));
            EXPECT_FALSE(err) << err->text();
        }
        EXPECT_GE(lua_files, 1);
    }
    // Only the weather example talks to the network, and only to the
    // host it declares.
    auto weather =
        wisp::core::parse_scene_manifest(read_file(examples_dir() / "weather" / "scene.json"));
    ASSERT_TRUE(weather);
    EXPECT_EQ(weather.value().http_hosts, std::vector<std::string>{"api.open-meteo.com"});
    // Going back sends the timer and the forecast to the background; the
    // rest close. Every example keeps schema_version 1 (scene-device spec 8).
    for (const char* id : kExamples) {
        SCOPED_TRACE(id);
        auto m = wisp::core::parse_scene_manifest(read_file(examples_dir() / id / "scene.json"));
        ASSERT_TRUE(m);
        const std::string name = id;
        EXPECT_EQ(m.value().background, name == "pomodoro" || name == "weather");
        EXPECT_EQ(m.value().schema_version, 1);
    }
}

class Examples : public wisp::test::SceneRuntimeFixture {
protected:
    void SetUp() override {
        SceneRuntimeFixture::SetUp();
        ui_config.font_image = wisp::test::test_font_image();
        client.next = {.status = 200,
                       .body = kForecast,
                       .headers = {{"content-type", "application/json; charset=utf-8"}}};
        http = std::make_unique<wisp::net::SceneHttp>(client);
        ASSERT_TRUE(http->start());
        imu->set_next_sample({.az = 1});  // lying flat
        sensors = std::make_unique<wisp::core::SensorService>(imu, occupancy);
        ASSERT_TRUE(sensors->start());
        notes = std::make_unique<wisp::core::NotificationService>(wisp::core::NotificationDeps{
            .bus = bus, .storage = *storage, .submit_io = {}, .make_timer = {}});
        configure_deps = [this](wisp::scene::SceneRuntimeDeps& d) {
            d.http = http.get();
            d.sensors = sensors.get();
            d.notifications = notes.get();
        };
    }

    void TearDown() override {
        rt.reset();
        sensors->stop();
        SceneRuntimeFixture::TearDown();
        http.reset();
        sensors.reset();
        notes.reset();
    }

    // Copies scenes/examples/<id> into the repository's code root.
    void install_example(const std::string& id) {
        const fs::path to = fs::path(in) / "scenes" / id;
        std::error_code ec;
        fs::create_directories(to, ec);
        fs::copy(examples_dir() / id, to, fs::copy_options::recursive, ec);
        ASSERT_FALSE(ec) << ec.message();
    }

    void publish_offset(int minutes) {
        wisp::core::Event e;
        e.topic = "system.settings";
        e.source = "test";
        e.payload = {{"language", wisp::Value("zh")},
                     {"utc_offset_min", wisp::Value(static_cast<int64_t>(minutes))}};
        bus.publish(std::move(e));
    }

    // The pixels of the one canvas on screen, once it has been drawn ("" before).
    std::string canvas_pixels() {
        std::string out;
        on_ui([&] {
            lv_obj_t* c = wisp::test::find_obj_of_class(lv_screen_active(), &lv_canvas_class);
            if (c == nullptr) return;
            lv_draw_buf_t* buf = lv_canvas_get_draw_buf(c);
            if (buf == nullptr || buf->data == nullptr) return;
            out.assign(reinterpret_cast<const char*>(buf->data), buf->data_size);
        });
        return out;
    }

    // Writes `lines` as the scene's collection log, the way SceneStore
    // leaves it, so the scene opens with records and its list builds rows.
    void seed(const std::string& id, const std::string& collection,
              const std::vector<std::string>& lines) {
        const fs::path dir = fs::path(in) / "scenes" / id / "data";
        std::error_code ec;
        fs::create_directories(dir, ec);
        std::ofstream f(dir / (collection + ".log"), std::ios::binary | std::ios::trunc);
        for (const auto& line : lines) f << line << "\n";
    }

    // The content of the scene on screen that the screen's shape cuts off.
    std::vector<std::string> clipped() {
        std::vector<std::string> out;
        on_ui([&] {
            lv_obj_t* screen = lv_screen_active();
            lv_obj_update_layout(screen);
            find_clipped(screen, screen, shape, true, out);
        });
        return out;
    }

    // Scrolls the one scrolling container (a list) to its top or its
    // bottom, waits for the row showing `text` (a label or a checkbox)
    // and returns what of that row is cut off.
    std::vector<std::string> list_end_clipped(bool bottom, const char* text) {
        on_ui([&] {
            lv_obj_t* screen = lv_screen_active();
            lv_obj_update_layout(screen);
            lv_obj_t* list = find_scroller(screen);
            if (list == nullptr) return;
            if (bottom)
                lv_obj_scroll_by(list, 0, -lv_obj_get_scroll_bottom(list), LV_ANIM_OFF);
            else
                lv_obj_scroll_by(list, 0, lv_obj_get_scroll_top(list), LV_ANIM_OFF);
        });
        lv_obj_t* hit = nullptr;
        const auto find = [&] {
            on_ui([&] {
                lv_obj_t* screen = lv_screen_active();
                hit = find_checkbox_with(screen, text);
                if (hit == nullptr) hit = wisp::test::find_label_containing(screen, text);
            });
            return hit != nullptr;
        };
        if (!wait_until(find, 2000)) return {std::string("no row showing ") + text};
        std::vector<std::string> out;
        on_ui([&] {
            lv_obj_t* screen = lv_screen_active();
            lv_obj_update_layout(screen);
            lv_obj_t* list = find_scroller(screen);
            lv_obj_t* row = hit;
            while (row != nullptr && lv_obj_get_parent(row) != list) row = lv_obj_get_parent(row);
            if (row == nullptr) {
                out.push_back("the row is not in the list");
                return;
            }
            lv_area_t a;
            lv_obj_get_coords(row, &a);
            find_clipped(row, screen, shape, false, out);
        });
        return out;
    }

    int count_of(const lv_obj_class_t* cls) {
        int n = 0;
        on_ui([&] { n = wisp::test::count_obj_of_class(lv_screen_active(), cls); });
        return n;
    }

    // What shows that the example has built its view (todo and ledger:
    // the rows of their seeded records).
    bool ready(const std::string& id) {
        constexpr uint32_t kMs = 2000;
        if (id == "counter") return wait_label("已点击", kMs);
        if (id == "pomodoro") return wait_label("25:00", kMs);
        if (id == "todo") return wait_until([&] { return count_of(&lv_checkbox_class) == 2; }, kMs);
        if (id == "ledger") return wait_label("午饭", kMs) && wait_label("今天", kMs);
        if (id == "clock") return wait_until([&] { return count_of(&lv_canvas_class) == 1; }, kMs);
        if (id == "sysinfo") return wait_label("安全区", kMs);
        if (id == "weather") return wait_label("12°", kMs);
        if (id == "level") return wait_label("0°", kMs);
        return false;
    }

    // Opens the example (todo and ledger with two records each), waits
    // until it is running and has built its view, and checks that there
    // is no error panel and nothing logged as a warning or an error.
    void open_and_run(const std::string& id, const std::string& shape) {
        SCOPED_TRACE(id + " on " + shape);
        install_example(id);
        if (id == "todo")
            seed(id, "todos",
                 {R"({"op":"put","id":1,"v":{"text":"买牛奶","done":false}})",
                  R"({"op":"put","id":2,"v":{"text":"交电费","done":true}})"});
        if (id == "ledger") {
            const int64_t today = (static_cast<int64_t>(std::time(nullptr)) + 8 * 3600) / 86400;
            seed(id, "entries",
                 {R"({"op":"put","id":1,"v":{"day":)" + std::to_string(today) +
                      R"(,"amount":32.5,"note":"午饭"}})",
                  R"({"op":"put","id":2,"v":{"day":)" + std::to_string(today - 3) +
                      R"(,"amount":12,"note":"其他"}})"});
        }
        auto opened = rt->open(id);
        ASSERT_TRUE(opened) << opened.error().message;
        EXPECT_TRUE(wait_until([&] { return rt->status(id).state == SceneState::Running; }, 2000))
            << rt->status(id).error;
        EXPECT_TRUE(ready(id)) << ::testing::PrintToString(labels());
        EXPECT_EQ(clipped(), std::vector<std::string>{});
        EXPECT_EQ(rt->status(id).state, SceneState::Running) << rt->status(id).error;
        EXPECT_FALSE(error_panel_has(""));
        for (auto& e : wisp::log::ring_entries()) {
            const bool ours = e.tag == "scene." + id || e.tag == "scene";
            EXPECT_FALSE(ours &&
                         (e.level == wisp::LogLevel::Error || e.level == wisp::LogLevel::Warn))
                << e.tag << ": " << e.msg;
        }
        ASSERT_TRUE(rt->close(id));
    }

    // Opens weather with `next` as the forecast's answer and waits for a
    // label holding `text`; the scene must stay running without the panel.
    void weather_shows(const std::string& text) {
        install_example("weather");
        ASSERT_TRUE(rt->open("weather"));
        EXPECT_TRUE(wait_label(text.c_str(), 2000)) << ::testing::PrintToString(labels());
        EXPECT_EQ(rt->status("weather").state, SceneState::Running) << rt->status("weather").error;
        EXPECT_FALSE(error_panel_has(""));
    }

    void check_list_ends(const std::string& id, const std::string& shape_name,
                         int corner_radius = 40);

    wisp::test::FakeHttpClient client;
    std::unique_ptr<wisp::net::SceneHttp> http;
    std::shared_ptr<wisp::test::FakeImu> imu = std::make_shared<wisp::test::FakeImu>("imu.main");
    wisp::core::OccupancyRegistry occupancy;
    std::unique_ptr<wisp::core::SensorService> sensors;
    std::unique_ptr<wisp::core::NotificationService> notes;
};

TEST_F(Examples, EachRunsOnACircle) {
    start("circle");
    for (const char* id : kExamples) open_and_run(id, "circle");
}

TEST_F(Examples, EachRunsOnARoundedRect) {
    start("rounded_rect");
    for (const char* id : kExamples) open_and_run(id, "rounded_rect");
}

// The reference board's panel: 480x480 with 70 px corners.
TEST_F(Examples, EachRunsOnTheBoardsRoundedRect) {
    start("rounded_rect", 70);
    for (const char* id : kExamples) open_and_run(id, "rounded_rect r70");
}

TEST_F(Examples, EachRunsOnARect) {
    start("rect");
    for (const char* id : kExamples) open_and_run(id, "rect");
}

// A long list scrolls edge to edge, but its first row at the top and its
// last row at the bottom are whole on the visible screen.
void Examples::check_list_ends(const std::string& id, const std::string& shape_name,
                               int corner_radius) {
    SCOPED_TRACE(id + " on " + shape_name + " r" + std::to_string(corner_radius));
    start(shape_name, corner_radius);
    install_example(id);
    std::vector<std::string> lines;
    const int64_t today = (static_cast<int64_t>(std::time(nullptr)) + 8 * 3600) / 86400;
    for (int i = 1; i <= 30; ++i) {
        const std::string n = std::to_string(i);
        if (id == "todo")
            lines.push_back(R"({"op":"put","id":)" + n + R"(,"v":{"text":"事项)" + n +
                            R"(","done":false}})");
        else
            lines.push_back(R"({"op":"put","id":)" + n + R"(,"v":{"day":)" +
                            std::to_string(today - i) + R"(,"amount":)" + n + R"(,"note":"笔)" + n +
                            R"("}})");
    }
    seed(id, id == "todo" ? "todos" : "entries", lines);
    ASSERT_TRUE(rt->open(id));
    ASSERT_TRUE(wait_until([&] { return rt->status(id).state == SceneState::Running; }, 2000));
    // todo lists by id, ledger newest day first: item 1 first, 30 last.
    const char* first = id == "todo" ? "事项1" : "笔1";
    const char* last = id == "todo" ? "事项30" : "笔30";
    EXPECT_EQ(list_end_clipped(false, first), std::vector<std::string>{});
    EXPECT_EQ(list_end_clipped(true, last), std::vector<std::string>{});
    EXPECT_EQ(list_end_clipped(false, first), std::vector<std::string>{});
    EXPECT_EQ(clipped(), std::vector<std::string>{});
    EXPECT_FALSE(error_panel_has(""));
}

TEST_F(Examples, TodoListEndsOnACircle) { check_list_ends("todo", "circle"); }
TEST_F(Examples, TodoListEndsOnARoundedRect) { check_list_ends("todo", "rounded_rect"); }
TEST_F(Examples, TodoListEndsOnTheBoardsRoundedRect) {
    check_list_ends("todo", "rounded_rect", 70);
}
TEST_F(Examples, TodoListEndsOnARect) { check_list_ends("todo", "rect"); }
TEST_F(Examples, LedgerListEndsOnACircle) { check_list_ends("ledger", "circle"); }
TEST_F(Examples, LedgerListEndsOnARoundedRect) { check_list_ends("ledger", "rounded_rect"); }
TEST_F(Examples, LedgerListEndsOnTheBoardsRoundedRect) {
    check_list_ends("ledger", "rounded_rect", 70);
}
TEST_F(Examples, LedgerListEndsOnARect) { check_list_ends("ledger", "rect"); }

// The clock's canvas is the whole screen: the dial is sized from it.
TEST_F(Examples, ClockUsesTheWholeScreen) {
    start("rounded_rect");
    install_example("clock");
    ASSERT_TRUE(rt->open("clock"));
    ASSERT_TRUE(wait_until([&] { return count_of(&lv_canvas_class) == 1; }, 2000));
    on_ui([&] {
        lv_obj_t* c = wisp::test::find_obj_of_class(lv_screen_active(), &lv_canvas_class);
        ASSERT_NE(c, nullptr);
        lv_obj_update_layout(lv_screen_active());
        lv_area_t a;
        lv_obj_get_coords(c, &a);
        EXPECT_EQ(a.x1, 0);
        EXPECT_EQ(a.y1, 0);
        EXPECT_EQ(a.x2, 479);
        EXPECT_EQ(a.y2, 479);
    });
}

TEST_F(Examples, CounterRemembersItsCount) {
    start();
    install_example("counter");
    ASSERT_TRUE(rt->open("counter"));
    ASSERT_TRUE(wait_label("0"));
    ASSERT_TRUE(rt->click_ref("plus"));
    ASSERT_TRUE(rt->click_ref("plus"));
    EXPECT_TRUE(wait_until([&] { return log_has("scene.counter", "count=2"); }, 1000));
    ASSERT_TRUE(rt->reload("counter"));
    // The new view's widgets are built on the UI thread after the reload
    // returns: a click before then finds no "plus" and does nothing.
    ASSERT_TRUE(wait_until([&] { return bool(rt->click_ref("plus")); }, 1000));
    EXPECT_TRUE(wait_until([&] { return log_has("scene.counter", "count=3"); }, 1000));
}

TEST_F(Examples, WeatherShowsTheFakeForecast) {
    start();
    weather_shows("12°");
    EXPECT_TRUE(wait_label("多云"));
    ASSERT_TRUE(client.last());
    EXPECT_EQ(client.last()->url.rfind("https://api.open-meteo.com/v1/forecast?", 0), 0u)
        << client.last()->url;
}

// Open-Meteo answers null where it has no value: the scene shows that it
// has none instead of failing.
TEST_F(Examples, WeatherSurvivesNulls) {
    client.next.body = R"({"current":{"time":"2026-11-04T10:00","temperature_2m":null,
"relative_humidity_2m":null,"weather_code":null,"wind_speed_10m":null},
"daily":{"time":["2026-11-04",null],"weather_code":[null,1],"temperature_2m_max":[null,9],
"temperature_2m_min":null}})";
    start();
    weather_shows("暂无数据");
    EXPECT_TRUE(wait_label("--"));
}

TEST_F(Examples, WeatherExplainsAFailedRequest) {
    client.next = {.status = 500, .body = "oops"};
    start();
    weather_shows("天气服务暂时不可用");
}

TEST_F(Examples, WeatherExplainsATimeout) {
    client.next = {.transport = wisp::net::TransportError::Timeout};
    start();
    weather_shows("网络");
}

// The timer runs: once started, a second later the time left is 24:59.
TEST_F(Examples, PomodoroCountsDown) {
    start();
    install_example("pomodoro");
    ASSERT_TRUE(rt->open("pomodoro"));
    ASSERT_TRUE(wait_label("25:00"));
    ASSERT_TRUE(rt->click_ref("toggle"));
    EXPECT_TRUE(wait_label("暂停"));
    EXPECT_TRUE(wait_label("24:5", 3000)) << ::testing::PrintToString(labels());
    // Skipping a work phase does not count it as done.
    ASSERT_TRUE(rt->click_ref("skip"));
    EXPECT_TRUE(wait_label("休息"));
    EXPECT_TRUE(wait_label("已完成 0 个"));
    EXPECT_EQ(rt->status("pomodoro").state, SceneState::Running);
}

// A refresh that waited for deep standby's network window says when the
// forecast it shows was fetched (scene-device spec 4.4).
TEST_F(Examples, WeatherShowsWhenADeferredRefreshRan) {
    start();
    http->set_deferring(true);
    install_example("weather");
    ASSERT_TRUE(rt->open("weather"));
    EXPECT_TRUE(wait_label("正在获取"));
    http->set_deferring(false);
    EXPECT_TRUE(wait_label("上次更新 10:00", 2000)) << ::testing::PrintToString(labels());
    EXPECT_EQ(rt->status("weather").state, SceneState::Running) << rt->status("weather").error;
}

// Started and sent to the background, the timer runs on: when the work
// phase ends it posts its alarm notification, and a tap on it brings the
// scene back on its timer page, which hears which notification it was.
TEST_F(Examples, PomodoroNotifiesFromTheBackground) {
    std::atomic<int64_t> wall{1793793600};  // 2026-11-04 12:00 UTC
    auto base = configure_deps;
    configure_deps = [base, &wall](wisp::scene::SceneRuntimeDeps& d) {
        base(d);
        d.wall_clock = [&wall] { return wall.load(); };
    };
    start();
    install_example("pomodoro");
    ASSERT_TRUE(rt->open("pomodoro"));
    ASSERT_TRUE(wait_label("25:00"));
    ASSERT_TRUE(rt->click_ref("toggle"));
    ASSERT_TRUE(wait_label("暂停"));
    ASSERT_TRUE(rt->close("pomodoro"));
    ASSERT_TRUE(
        wait_until([&] { return rt->status("pomodoro").state == SceneState::Background; }, 2000));
    wall += 25 * 60;
    ASSERT_TRUE(wait_until([&] { return notes->find("pomodoro", "pomo").has_value(); }, 3000));
    const wisp::core::Notification n = *notes->find("pomodoro", "pomo");
    EXPECT_EQ(n.title, "番茄钟");
    EXPECT_EQ(n.body, "25 分钟到了");
    EXPECT_EQ(n.sound, wisp::core::NotifySound::Alarm);
    EXPECT_TRUE(n.wake);

    ASSERT_TRUE(rt->deliver_notification("pomodoro", "pomo"));
    EXPECT_TRUE(
        wait_until([&] { return rt->status("pomodoro").state == SceneState::Running; }, 2000));
    EXPECT_TRUE(wait_until([&] { return log_has("scene.pomodoro", "notified=pomo"); }, 2000));
    EXPECT_TRUE(wait_label("休息", 2000)) << ::testing::PrintToString(labels());
    EXPECT_TRUE(wait_label("已完成 1 个", 2000));
}

// The level reads the accelerometer: tilted 30 degrees from flat, it
// shows and logs 30, and its bubble moves off the centre.
TEST_F(Examples, LevelShowsTheTilt) {
    start("circle");
    install_example("level");
    imu->set_next_sample({.ax = 0, .ay = 0, .az = 1});
    ASSERT_TRUE(rt->open("level"));
    ASSERT_TRUE(wait_label("0°", 2000)) << ::testing::PrintToString(labels());
    ASSERT_TRUE(wait_until([&] { return log_has("scene.level", "angle=0"); }, 2000));
    std::string flat;
    ASSERT_TRUE(wait_until(
        [&] {
            flat = canvas_pixels();
            return !flat.empty();
        },
        2000));
    imu->set_next_sample({.ax = 0.5f, .ay = 0, .az = 0.87f});
    EXPECT_TRUE(wait_label("30°", 2000)) << ::testing::PrintToString(labels());
    EXPECT_TRUE(wait_until([&] { return log_has("scene.level", "angle=30"); }, 2000));
    EXPECT_TRUE(wait_until([&] { return canvas_pixels() != flat; }, 2000));
    EXPECT_EQ(clipped(), std::vector<std::string>{});
    EXPECT_EQ(rt->status("level").state, SceneState::Running) << rt->status("level").error;
}

// Without an IMU the level says so instead of failing.
TEST_F(Examples, LevelWithoutASensor) {
    auto base = configure_deps;
    configure_deps = [base](wisp::scene::SceneRuntimeDeps& d) {
        base(d);
        d.sensors = nullptr;
    };
    start();
    install_example("level");
    ASSERT_TRUE(rt->open("level"));
    EXPECT_TRUE(wait_label("没有加速度传感器", 2000)) << ::testing::PrintToString(labels());
    EXPECT_EQ(rt->status("level").state, SceneState::Running) << rt->status("level").error;
    EXPECT_FALSE(error_panel_has(""));
}

// sysinfo and clock show local time from info().utc_offset, and follow a
// change of it without being reopened.
TEST_F(Examples, SysinfoAndClockFollowTheUtcOffset) {
    auto base = configure_deps;
    configure_deps = [base](wisp::scene::SceneRuntimeDeps& d) {
        base(d);
        d.wall_clock = [] { return int64_t{1793793600}; };  // 2026-11-04 12:00 UTC
        d.utc_offset_min = 0;
    };
    start();
    install_example("sysinfo");
    ASSERT_TRUE(rt->open("sysinfo"));
    EXPECT_TRUE(wait_label("12:00")) << ::testing::PrintToString(labels());
    EXPECT_TRUE(wait_label("UTC+0"));
    publish_offset(480);
    EXPECT_TRUE(wait_label("20:00")) << ::testing::PrintToString(labels());
    EXPECT_TRUE(wait_label("UTC+8"));
    publish_offset(-210);
    EXPECT_TRUE(wait_label("08:30")) << ::testing::PrintToString(labels());
    EXPECT_TRUE(wait_label("UTC-3:30"));
    ASSERT_TRUE(rt->close("sysinfo"));

    install_example("clock");
    ASSERT_TRUE(rt->open("clock"));
    // Not one colour: the dial is drawn.
    const auto drawn = [](const std::string& px) {
        return px.find_first_not_of(px.empty() ? '\0' : px[0]) != std::string::npos;
    };
    std::string before;
    ASSERT_TRUE(wait_until(
        [&] {
            before = canvas_pixels();
            return drawn(before);
        },
        2000));
    // Drawn for 08:30 (the offset is still -3:30), then for 12:00: the
    // hands move.
    publish_offset(0);
    EXPECT_TRUE(wait_until(
        [&] {
            const std::string now = canvas_pixels();
            return drawn(now) && now != before;
        },
        2000));
    EXPECT_EQ(rt->status("clock").state, SceneState::Running);
}

}  // namespace
