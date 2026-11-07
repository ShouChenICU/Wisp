// SimBoard::init() must degrade audio_out.main/audio_in.main (rather
// than crash, or silently register a device that will fail every
// open() call) when SDL's audio subsystem never came up at all --
// main.cpp warns and continues rather than aborting if
// SDL_Init(SDL_INIT_AUDIO) fails. This test file deliberately never
// calls SDL_Init() itself, so SDL_WasInit(AUDIO) reliably reports "not
// initialized" within this process -- the same condition
// SimBoard::init() must handle.

#include "wisp/port/sim/sim_board.hpp"

#include <SDL.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <memory>
#include <string>

#include "dev/sim_devices.hpp"
#include "wisp/core/boot_report.hpp"
#include "wisp/core/device_registry.hpp"
#include "wisp/hal/audio.hpp"
#include "wisp/hal/display.hpp"
#include "wisp/hal/touch.hpp"
#include "wisp/port/sim/sim_input.hpp"
#include "wisp/shell/conversation.hpp"

TEST(SimBoardTest, AudioDevicesDegradeWhenSdlAudioNeverInitialized) {
    ASSERT_EQ(SDL_WasInit(SDL_INIT_AUDIO), 0u)
        << "this test must run without SDL audio ever having been initialized";

    auto input = std::make_shared<wisp::port::sim::SimInput>(/*sd_present_initial=*/true);
    wisp::port::sim::SimBoardConfig config;
    config.data_dir = (std::filesystem::temp_directory_path() / "wisp_sim_board_test").string();
    wisp::port::sim::SimBoard board(config, input, [] {});

    wisp::core::DeviceRegistry registry;
    wisp::core::BootReport report;
    board.init(registry, report);

    // Neither audio device was registered at all.
    EXPECT_EQ(registry.get<wisp::hal::AudioOut>("audio_out.main"), nullptr);
    EXPECT_EQ(registry.get<wisp::hal::AudioIn>("audio_in.main"), nullptr);

    auto audio_out_report = report.find("audio_out.main");
    ASSERT_TRUE(audio_out_report.has_value());
    EXPECT_EQ(audio_out_report->state, wisp::core::DeviceState::Failed);
    EXPECT_EQ(audio_out_report->reason, "SDL audio unavailable");

    auto audio_in_report = report.find("audio_in.main");
    ASSERT_TRUE(audio_in_report.has_value());
    EXPECT_EQ(audio_in_report->state, wisp::core::DeviceState::Failed);
    EXPECT_EQ(audio_in_report->reason, "SDL audio unavailable");

    // Every other device is unaffected by audio being unavailable.
    EXPECT_NE(registry.get<wisp::hal::Touch>("touch.main"), nullptr);
}

namespace {

// Boots a SimBoard under SDL's "dummy" audio driver so the audio devices
// are registered too. SDL is shut down again on destruction (after the
// registry), so the test above still sees SDL audio uninitialized when
// the whole binary runs in one process.
class SimBoard : public ::testing::Test {
protected:
    void SetUp() override {
        SDL_SetHint(SDL_HINT_AUDIODRIVER, "dummy");
        ASSERT_EQ(SDL_Init(SDL_INIT_AUDIO), 0) << SDL_GetError();
        auto input = std::make_shared<wisp::port::sim::SimInput>(/*sd_present_initial=*/true);
        wisp::port::sim::SimBoardConfig config;
        config.data_dir =
            (std::filesystem::temp_directory_path() / "wisp_sim_board_power_test").string();
        board_ = std::make_unique<wisp::port::sim::SimBoard>(config, input, [] {});
        registry_ = std::make_unique<wisp::core::DeviceRegistry>();
        board_->init(*registry_, report_);
    }

    void TearDown() override {
        registry_.reset();
        board_.reset();
        SDL_Quit();
    }

    std::string standby_of(const std::string& id) const {
        auto device = registry_->find(id);
        if (!device) return "<missing device>";
        const auto& caps = device->info().caps;
        auto it = caps.find("standby");
        if (it == caps.end()) return "";
        if (it->second.type() != wisp::Value::Type::String) return "<not a string>";
        return it->second.as_string();
    }

    std::unique_ptr<wisp::port::sim::SimBoard> board_;
    std::unique_ptr<wisp::core::DeviceRegistry> registry_;
    wisp::core::BootReport report_;
};

}  // namespace

TEST_F(SimBoard, DevicesDeclareStandbyPolicy) {
    EXPECT_EQ(standby_of("display.main"), "off");
    EXPECT_EQ(standby_of("touch.main"), "off");
    EXPECT_EQ(standby_of("imu.main"), "off");
    EXPECT_EQ(standby_of("button.power"), "wake_source");
    EXPECT_EQ(standby_of("button.vol_up"), "wake_source");
    EXPECT_EQ(standby_of("button.vol_down"), "wake_source");
    EXPECT_EQ(standby_of("audio_out.main"), "off");
    // Whether the dummy driver offers a capture device depends on the
    // host's SDL build (see sim_audio_test.cpp).
    if (registry_->find("audio_in.main")) {
        EXPECT_EQ(standby_of("audio_in.main"), "off");
    }
    EXPECT_EQ(standby_of("power.main"), "");
    EXPECT_EQ(standby_of("clock.main"), "");
    EXPECT_EQ(standby_of("storage.int"), "");
    EXPECT_EQ(standby_of("storage.sd"), "");
}

TEST_F(SimBoard, DisplayFollowsPowerLevel) {
    auto display = registry_->get<wisp::hal::Display>("display.main");
    ASSERT_NE(display, nullptr);
    ASSERT_TRUE(display->set_power(wisp::hal::PowerLevel::Standby));
    EXPECT_FALSE(display->powered());
    ASSERT_TRUE(display->set_power(wisp::hal::PowerLevel::Dim));
    EXPECT_TRUE(display->powered());
}

TEST_F(SimBoard, AudioFollowsPowerLevel) {
    auto out = registry_->get<wisp::hal::AudioOut>("audio_out.main");
    ASSERT_NE(out, nullptr);
    auto& sim_out = static_cast<wisp::port::sim::SimAudioOut&>(*out);
    EXPECT_TRUE(sim_out.powered());
    ASSERT_TRUE(out->set_power(wisp::hal::PowerLevel::Standby));
    EXPECT_FALSE(sim_out.powered());
    ASSERT_TRUE(out->set_power(wisp::hal::PowerLevel::DeepStandby));
    EXPECT_FALSE(sim_out.powered());
    ASSERT_TRUE(out->set_power(wisp::hal::PowerLevel::Active));
    EXPECT_TRUE(sim_out.powered());

    auto in = registry_->get<wisp::hal::AudioIn>("audio_in.main");
    if (in) {
        auto& sim_in = static_cast<wisp::port::sim::SimAudioIn&>(*in);
        EXPECT_TRUE(sim_in.powered());
        ASSERT_TRUE(in->set_power(wisp::hal::PowerLevel::DeepStandby));
        EXPECT_FALSE(sim_in.powered());
        ASSERT_TRUE(in->set_power(wisp::hal::PowerLevel::Dim));
        EXPECT_TRUE(sim_in.powered());
    }
}

TEST_F(SimBoard, VolDownIsTheTalkKey) {
    EXPECT_EQ(wisp::shell::find_talk_button(*registry_),
              std::optional<std::string>("button.vol_down"));
}
