// Task 13 fix round 1, item 1: exercises the real SDL-backed
// SimAudioOut/SimAudioIn devices under SDL's "dummy" audio driver --
// the same driver simulator/main.cpp selects in --headless mode. The
// dummy driver always provides a (fake, but functional) playback
// device, so SimAudioOut's open/write/close path is exercised for real
// unconditionally below.
//
// Whether the dummy driver also provides a *capture* device varies by
// SDL build: historically it provided none, but some SDL2
// implementations (e.g. sdl2-compat backed by SDL3, as used while
// developing this) emulate a fake microphone that yields silence.
// SimBoard::init() probes SDL_GetNumAudioDevices(1) at startup and
// only ever registers audio_in.main if that reports at least one
// device (see sim_board.cpp), so both outcomes are legitimate on
// different hosts/CI images. The capture test below branches on that
// same probe so it documents and exercises whichever path this host's
// SDL actually takes, instead of assuming one of them.

#include <SDL.h>
#include <gtest/gtest.h>

#include <vector>

#include "dev/sim_devices.hpp"
#include "wisp/hal/audio.hpp"

namespace {

class SimAudioTest : public ::testing::Test {
protected:
    void SetUp() override {
        SDL_SetHint(SDL_HINT_AUDIODRIVER, "dummy");
        ASSERT_EQ(SDL_Init(SDL_INIT_AUDIO), 0) << SDL_GetError();
    }

    void TearDown() override { SDL_Quit(); }
};

}  // namespace

TEST_F(SimAudioTest, PlaybackOpenWriteCloseDoesNotHang) {
    wisp::port::sim::SimAudioOut out("audio_out.main");
    wisp::hal::AudioFormat format;
    format.sample_rate = 16000;
    format.channels = 1;
    ASSERT_TRUE(out.open(format));

    // A handful of 100ms chunks: enough to exercise write()'s "block
    // while more than ~200ms is already queued" throttle (so this test
    // would hang if that throttle never drained) without the test
    // itself taking long.
    std::vector<int16_t> chunk(1600, 0);  // 100ms of silence at 16kHz mono
    for (int i = 0; i < 4; ++i) {
        auto written = out.write(chunk.data(), chunk.size());
        ASSERT_TRUE(written);
        EXPECT_EQ(written.value(), chunk.size());
    }
    out.close();

    // close() (and the destructor) must be safe to call again/on an
    // already-closed device.
    out.close();
}

TEST_F(SimAudioTest, SetVolumeClampsAndDoesNotFail) {
    wisp::port::sim::SimAudioOut out("audio_out.main");
    EXPECT_TRUE(out.set_volume(50));
    EXPECT_TRUE(out.set_volume(255));  // clamps to 100, does not fail
}

TEST_F(SimAudioTest, WriteBeforeOpenFails) {
    wisp::port::sim::SimAudioOut out("audio_out.main");
    std::vector<int16_t> chunk(160, 0);
    EXPECT_FALSE(out.write(chunk.data(), chunk.size()));
}

TEST_F(SimAudioTest, CaptureAbsenceOrPresenceIsHandledWithoutHanging) {
    // Mirrors SimBoard::init()'s own probe (sim_board.cpp): on hosts
    // where the dummy driver has no capture device, open() must fail
    // cleanly (DeviceFailure) rather than hang or crash -- exercising
    // the fallback fix round 1, item 1 requires even though
    // SimBoard::init() itself is expected to avoid ever reaching it.
    // On hosts where the dummy driver does emulate a microphone, a full
    // open/read/close round trip must complete (bounded by read()'s own
    // timeout) without hanging.
    wisp::port::sim::SimAudioIn in("audio_in.main");
    wisp::hal::AudioFormat format;
    format.sample_rate = 16000;
    format.channels = 1;
    auto opened = in.open(format);

    if (SDL_GetNumAudioDevices(/*iscapture=*/1) <= 0) {
        EXPECT_FALSE(opened);
        if (!opened) {
            EXPECT_EQ(opened.error().code, wisp::ErrorCode::DeviceFailure);
        }
        return;
    }

    ASSERT_TRUE(opened) << opened.error().message;
    std::vector<int16_t> buf(1600, 0);  // 100ms at 16kHz mono
    auto read = in.read(buf.data(), buf.size());
    ASSERT_TRUE(read);
    EXPECT_LE(read.value(), buf.size());
    in.close();
    in.close();  // safe to call again
}

TEST_F(SimAudioTest, ReadBeforeOpenFails) {
    wisp::port::sim::SimAudioIn in("audio_in.main");
    std::vector<int16_t> buf(160, 0);
    EXPECT_FALSE(in.read(buf.data(), buf.size()));
}
