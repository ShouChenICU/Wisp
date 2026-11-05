// The example scenes in scenes/examples (scene-runtime-core spec 9.3): each
// package's scene.json parses, every .lua file compiles, and each one opens
// in a SceneRuntime on a fake display and is still running two seconds
// later without the error panel. wisp.http goes to a fake client that
// answers the weather example with a fixed forecast: no test here touches
// the network.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <sstream>
#include <string>
#include <system_error>
#include <vector>

#include "support/fake_http.hpp"
#include "support/scene_runtime_fixture.hpp"
#include "support/wait.hpp"
#include "wisp/core/scene_manifest.hpp"
#include "wisp/net/scene_http.hpp"
#include "wisp/scene/vm.hpp"

namespace {

namespace fs = std::filesystem;

using wisp::core::SceneState;
using wisp::test::wait_until;

const char* const kExamples[] = {"counter", "pomodoro", "todo",   "ledger",
                                 "clock",   "sysinfo",  "weather"};

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
}

class Examples : public wisp::test::SceneRuntimeFixture {
protected:
    void SetUp() override {
        SceneRuntimeFixture::SetUp();
        client.next = {.status = 200,
                       .body = kForecast,
                       .headers = {{"content-type", "application/json; charset=utf-8"}}};
        http = std::make_unique<wisp::net::SceneHttp>(client);
        ASSERT_TRUE(http->start());
        configure_deps = [this](wisp::scene::SceneRuntimeDeps& d) { d.http = http.get(); };
    }

    void TearDown() override {
        SceneRuntimeFixture::TearDown();
        http.reset();
    }

    // Copies scenes/examples/<id> into the repository's code root.
    void install_example(const std::string& id) {
        const fs::path to = fs::path(in) / "scenes" / id;
        std::error_code ec;
        fs::create_directories(to, ec);
        fs::copy(examples_dir() / id, to, fs::copy_options::recursive, ec);
        ASSERT_FALSE(ec) << ec.message();
    }

    // Opens the example and checks that it is still running, with no
    // error panel and nothing logged as an error, two seconds later.
    void open_and_run(const std::string& id, const std::string& shape) {
        SCOPED_TRACE(id + " on " + shape);
        install_example(id);
        auto opened = rt->open(id);
        ASSERT_TRUE(opened) << opened.error().message;
        wisp::os::sleep_ms(2000);
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

    wisp::test::FakeHttpClient client;
    std::unique_ptr<wisp::net::SceneHttp> http;
};

TEST_F(Examples, EachRunsOnACircle) {
    start("circle");
    for (const char* id : kExamples) open_and_run(id, "circle");
}

TEST_F(Examples, EachRunsOnARoundedRect) {
    start("rounded_rect");
    for (const char* id : kExamples) open_and_run(id, "rounded_rect");
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
    install_example("weather");
    ASSERT_TRUE(rt->open("weather"));
    EXPECT_TRUE(wait_label("12", 2000)) << ::testing::PrintToString(labels());
    ASSERT_TRUE(client.last());
    EXPECT_EQ(client.last()->url.rfind("https://api.open-meteo.com/v1/forecast?", 0), 0u)
        << client.last()->url;
}

}  // namespace
