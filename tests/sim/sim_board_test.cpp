// Task 13 fix round 2, items N2/N3: SimBoard::init() must degrade
// audio_out.main/audio_in.main (rather than crash, or silently register
// a device that will fail every open() call) when SDL's audio subsystem
// never came up at all -- main.cpp now warns and continues instead of
// aborting if SDL_Init(SDL_INIT_AUDIO) fails (item N2). This test file
// deliberately never calls SDL_Init() itself, so SDL_WasInit(AUDIO)
// reliably reports "not initialized" within this process -- the same
// condition SimBoard::init() must handle.

#include "wisp/port/sim/sim_board.hpp"

#include <SDL.h>
#include <gtest/gtest.h>

#include <filesystem>
#include <memory>

#include "wisp/core/boot_report.hpp"
#include "wisp/core/device_registry.hpp"
#include "wisp/hal/audio.hpp"
#include "wisp/hal/touch.hpp"
#include "wisp/port/sim/sim_input.hpp"

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
