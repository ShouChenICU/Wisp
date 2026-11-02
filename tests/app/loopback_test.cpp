// spec 6.6: volume is one of sub-project 1's three Settings-backed items
// (brightness, volume, screen_off_s). Exercises loopback.cpp's
// record_and_play() directly via the public run_loopback()/join_loopback()
// entry points -- loopback.hpp is an internal (not under include/) header,
// reached the same way tests/sim/sim_audio_test.cpp reaches into
// wisp_port_sim's internals: an extra target_include_directories() in
// tests/CMakeLists.txt, not a public API.

#include "loopback.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "support/fake_devices.hpp"
#include "support/temp_dir.hpp"
#include "support/wait.hpp"
#include "wisp/app/app.hpp"
#include "wisp/app/board.hpp"
#include "wisp/core/device_registry.hpp"
#include "wisp/core/settings.hpp"
#include "wisp/port/sim/settings_json_file.hpp"

using wisp::test::FakeAudioIn;
using wisp::test::FakeAudioOut;
using wisp::test::wait_until;

namespace {

// Registers only "audio_in.main"/"audio_out.main" -- loopback doesn't
// need a display, and App::start() tolerates headless boot.
class AudioOnlyBoard : public wisp::app::Board {
public:
    AudioOnlyBoard(std::shared_ptr<FakeAudioIn> audio_in, std::shared_ptr<FakeAudioOut> audio_out)
        : audio_in_(std::move(audio_in)), audio_out_(std::move(audio_out)) {}

    std::string name() const override { return "audio-only"; }

    void init(wisp::core::DeviceRegistry& registry, wisp::core::BootReport&) override {
        registry.add(audio_in_);
        registry.add(audio_out_);
    }

private:
    std::shared_ptr<FakeAudioIn> audio_in_;
    std::shared_ptr<FakeAudioOut> audio_out_;
};

// App is constructed lazily (start_app(), not the constructor): a test
// that wants a non-default "audio"/"volume" Settings value must write it
// to settings_path *before* App builds its own JsonFileSettingsBackend
// over that same file -- that backend loads the file once, at
// construction, and never re-reads it afterwards (see
// JsonFileSettingsBackend's own doc comment), so writing through a
// second, separate backend instance after App already exists would be
// invisible to it.
struct Harness {
    std::shared_ptr<FakeAudioIn> audio_in = std::make_shared<FakeAudioIn>("audio_in.main");
    std::shared_ptr<FakeAudioOut> audio_out = std::make_shared<FakeAudioOut>("audio_out.main");
    std::string settings_path = wisp::test::temp_dir().string() + "/settings.json";
    std::unique_ptr<wisp::app::App> app;

    void set_volume_setting(int64_t volume) {
        wisp::port::sim::JsonFileSettingsBackend backend(settings_path);
        wisp::core::Settings audio_settings(backend, "audio");
        ASSERT_TRUE(audio_settings.set("volume", wisp::Value(volume)));
    }

    void start_app() {
        wisp::app::Platform platform;
        platform.board = std::make_unique<AudioOnlyBoard>(audio_in, audio_out);
        platform.settings =
            std::make_unique<wisp::port::sim::JsonFileSettingsBackend>(settings_path);
        platform.emit_line = [](const std::string&) {};
        app = std::make_unique<wisp::app::App>(std::move(platform));
        ASSERT_TRUE(app->start());
    }

    std::mutex status_mutex;
    std::vector<std::string> statuses;

    std::string last_status() {
        std::lock_guard<std::mutex> lock(status_mutex);
        return statuses.empty() ? std::string() : statuses.back();
    }

    void run_loopback_and_wait() {
        wisp::app::run_loopback(*app, [this](const char* status) {
            std::lock_guard<std::mutex> lock(status_mutex);
            statuses.emplace_back(status);
        });
        ASSERT_TRUE(wait_until(
            [&] {
                std::string s = last_status();
                return s == "完成" || s.rfind("失败", 0) == 0;
            },
            5000))
            << "loopback never finished; last status: " << last_status();
        EXPECT_EQ(last_status(), "完成");
        wisp::app::join_loopback(*app);
    }
};

}  // namespace

TEST(Loopback, AppliesDefaultVolumeWhenSettingsHasNone) {
    Harness h;
    h.start_app();

    h.run_loopback_and_wait();

    EXPECT_EQ(h.audio_out->set_volume_calls(), 1);
    EXPECT_EQ(h.audio_out->last_volume(), 60);

    h.app->stop();
}

TEST(Loopback, AppliesVolumeFromSettings) {
    Harness h;
    h.set_volume_setting(35);
    h.start_app();

    h.run_loopback_and_wait();

    EXPECT_EQ(h.audio_out->last_volume(), 35);

    h.app->stop();
}

TEST(Loopback, ClampsOutOfRangeVolumeFromSettings) {
    Harness h;
    h.set_volume_setting(250);
    h.start_app();

    h.run_loopback_and_wait();

    EXPECT_EQ(h.audio_out->last_volume(), 100);

    h.app->stop();
}
