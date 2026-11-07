// wisp::voice::PcmPlayer (wisp/voice/pcm_player.hpp) against a recording
// fake output.

#include "wisp/voice/pcm_player.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>
#include <vector>

#include "support/fake_audio.hpp"
#include "support/wait.hpp"
#include "wisp/os/os.hpp"
#include "wisp/ui/ui_runtime.hpp"
#include "wisp/voice/limits.hpp"

using wisp::test::wait_until;
using wisp::test::audio::FakeAudioOut;
using wisp::voice::PcmPlayer;

namespace {

std::vector<int16_t> ramp(size_t n) {
    std::vector<int16_t> v(n);
    for (size_t i = 0; i < n; ++i) v[i] = static_cast<int16_t>((i * 7) % 20000 - 10000);
    return v;
}

struct PcmPlayerTest : ::testing::Test {
    void SetUp() override { ASSERT_TRUE(player.start()); }

    std::shared_ptr<FakeAudioOut> out = std::make_shared<FakeAudioOut>();
    PcmPlayer player{out};
};

}  // namespace

TEST_F(PcmPlayerTest, OpensAtRateAndPlaysInOrder) {
    ASSERT_TRUE(player.begin(24000, 60));
    EXPECT_TRUE(player.active());
    ASSERT_EQ(out->formats().size(), 1u);
    EXPECT_EQ(out->formats()[0].sample_rate, 24000u);
    EXPECT_EQ(out->formats()[0].channels, 1);

    const std::vector<int16_t> pcm = ramp(10000);
    // In pieces of odd sizes, as TTS chunks come.
    size_t off = 0;
    for (size_t step : {1u, 333u, 4096u, 5570u}) {
        ASSERT_EQ(player.push({pcm.data() + off, step}, 1000), step);
        off += step;
    }
    ASSERT_EQ(off, pcm.size());
    player.end_of_stream();
    ASSERT_TRUE(wait_until([&] { return !player.active(); }, 2000));
    EXPECT_FALSE(out->is_open());
    EXPECT_EQ(out->closes(), 1);

    const std::vector<int16_t> played = out->samples();
    ASSERT_EQ(played.size(), pcm.size());
    for (size_t i = 0; i < pcm.size(); ++i) {
        ASSERT_EQ(played[i], static_cast<int16_t>(pcm[i] * 60 / 100)) << i;
    }
    // The volume is in the samples; the device's own is left alone.
    EXPECT_EQ(out->set_volume_calls(), 0);
    EXPECT_EQ(player.level(), 0);
    // Nothing is taken once the stream has ended.
    EXPECT_EQ(player.push({pcm.data(), 10}, 10), 0u);
}

TEST_F(PcmPlayerTest, PushBlocksWhileRingFull) {
    out->close_gate();
    ASSERT_TRUE(player.begin(24000, 100));
    const size_t ring = 24000 * wisp::voice::kRingMs / 1000;
    const std::vector<int16_t> pcm = ramp(ring + 4 * PcmPlayer::kChunkFrames);

    const uint64_t t0 = wisp::os::now_ms();
    const size_t taken = player.push(pcm, 100);
    const uint64_t waited = wisp::os::now_ms() - t0;
    // The ring is full, plus at most the one chunk the task took before
    // it stalled at the gate.
    EXPECT_GE(taken, ring);
    EXPECT_LE(taken, ring + PcmPlayer::kChunkFrames);
    EXPECT_GE(waited, 100u);

    // Once playback moves, the rest goes in.
    out->open_gate();
    const size_t rest = player.push({pcm.data() + taken, pcm.size() - taken}, 2000);
    EXPECT_EQ(taken + rest, pcm.size());
    player.end_of_stream();
    ASSERT_TRUE(wait_until([&] { return !player.active(); }, 3000));
    EXPECT_EQ(out->samples(), pcm);
}

TEST_F(PcmPlayerTest, StopDropsAndCloses) {
    out->set_write_ms(5);
    ASSERT_TRUE(player.begin(24000, 100));
    const std::vector<int16_t> pcm = ramp(24000);
    ASSERT_EQ(player.push(pcm, 100), pcm.size());
    ASSERT_TRUE(wait_until([&] { return out->frames() > 0; }, 1000));
    EXPECT_GT(player.level(), 0);

    player.stop();
    ASSERT_TRUE(wait_until([&] { return !player.active(); }, 200));
    EXPECT_FALSE(out->is_open());
    const size_t played = out->frames();
    EXPECT_LT(played, pcm.size() / 2);
    wisp::os::sleep_ms(30);
    EXPECT_EQ(out->frames(), played);
    EXPECT_EQ(player.push(pcm, 10), 0u);
    EXPECT_EQ(player.level(), 0);

    // A new stream starts clean, at its own rate.
    ASSERT_TRUE(player.begin(16000, 100));
    ASSERT_EQ(player.push({pcm.data(), 100}, 100), 100u);
    player.end_of_stream();
    ASSERT_TRUE(wait_until([&] { return !player.active(); }, 1000));
    EXPECT_EQ(out->frames(), played + 100);
    EXPECT_EQ(out->formats().back().sample_rate, 16000u);
    EXPECT_EQ(out->overlapping_opens(), 0);
}

TEST_F(PcmPlayerTest, StopUnblocksAPush) {
    out->close_gate();
    ASSERT_TRUE(player.begin(24000, 100));
    const std::vector<int16_t> pcm = ramp(24000 * 3);
    std::atomic<bool> returned{false};
    std::thread pusher([&] {
        player.push(pcm, wisp::os::kForever);
        returned = true;
    });
    ASSERT_TRUE(wait_until([&] { return out->writer_waiting(); }, 1000));
    wisp::os::sleep_ms(20);
    EXPECT_FALSE(returned.load());
    player.stop();
    EXPECT_TRUE(wait_until([&] { return returned.load(); }, 200));
    pusher.join();
    out->open_gate();
    EXPECT_TRUE(wait_until([&] { return !player.active(); }, 500));
}

TEST(PcmPlayerConfig, PlayTaskOutranksUi) {
    const wisp::os::TaskConfig c = PcmPlayer::task_config(PcmPlayer::Config{});
    EXPECT_STREQ(c.name, "wisp.play");
    EXPECT_FALSE(c.flash_access);
    EXPECT_EQ(c.stack_bytes, 4096u);
    EXPECT_EQ(c.priority, 5);
    EXPECT_GT(c.priority, wisp::ui::UiConfig{}.task_priority);
}

TEST(PcmPlayerConfig, NoOutputPlaysNothing) {
    PcmPlayer player(nullptr);
    EXPECT_TRUE(player.start());
    EXPECT_FALSE(player.begin(24000, 60));
    const int16_t s[4] = {1, 2, 3, 4};
    EXPECT_EQ(player.push(s, 10), 0u);
    EXPECT_FALSE(player.active());
    player.stop();
}
