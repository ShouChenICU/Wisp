#include "wisp/core/tone_player.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <cstdlib>
#include <memory>
#include <mutex>
#include <vector>

#include "support/wait.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/core/wakelock.hpp"
#include "wisp/os/os.hpp"

using wisp::Result;
using wisp::core::synth_tone;
using wisp::core::ToneKind;
using wisp::core::TonePlayer;
using wisp::core::TonePlayerConfig;
using wisp::core::WakeLockKind;
using wisp::core::WakeLockRegistry;
using wisp::test::wait_until;

namespace {

// Records every frame written. `pace_ms` makes each write take that long,
// as a real output does; `fail_open` makes open() fail.
class RecordingAudioOut : public wisp::hal::AudioOut {
public:
    explicit RecordingAudioOut(uint32_t pace_ms = 0, bool fail_open = false)
        : pace_ms_(pace_ms), fail_open_(fail_open) {
        info_.id = "audio_out.main";
        info_.cls = kClass;
        info_.model = "FAKE";
    }

    const wisp::hal::DeviceInfo& info() const override { return info_; }

    Result<void> open(const wisp::hal::AudioFormat& format) override {
        std::lock_guard<std::mutex> lock(mu_);
        ++opens_;
        if (fail_open_) return Result<void>::err(wisp::ErrorCode::DeviceFailure, "no codec");
        rate_ = format.sample_rate;
        return Result<void>::ok();
    }

    Result<size_t> write(const int16_t* frames, size_t count) override {
        if (pace_ms_ > 0) wisp::os::sleep_ms(pace_ms_);
        std::lock_guard<std::mutex> lock(mu_);
        samples_.insert(samples_.end(), frames, frames + count);
        return Result<size_t>::ok(count);
    }

    void close() override {
        std::lock_guard<std::mutex> lock(mu_);
        ++closes_;
    }

    Result<void> set_volume(uint8_t) override { return Result<void>::ok(); }

    size_t frames() const {
        std::lock_guard<std::mutex> lock(mu_);
        return samples_.size();
    }
    std::vector<int16_t> samples() const {
        std::lock_guard<std::mutex> lock(mu_);
        return samples_;
    }
    int opens() const {
        std::lock_guard<std::mutex> lock(mu_);
        return opens_;
    }
    int closes() const {
        std::lock_guard<std::mutex> lock(mu_);
        return closes_;
    }
    uint32_t rate() const {
        std::lock_guard<std::mutex> lock(mu_);
        return rate_;
    }

private:
    wisp::hal::DeviceInfo info_;
    uint32_t pace_ms_;
    bool fail_open_;
    mutable std::mutex mu_;
    std::vector<int16_t> samples_;
    int opens_ = 0;
    int closes_ = 0;
    uint32_t rate_ = 0;
};

int peak(const std::vector<int16_t>& s) {
    int p = 0;
    for (int16_t v : s) p = std::max(p, std::abs(static_cast<int>(v)));
    return p;
}

struct TonePlayerTest : ::testing::Test {
    void SetUp() override { ASSERT_TRUE(bus.start()); }
    void TearDown() override { bus.stop(); }

    wisp::core::EventBus bus;
    WakeLockRegistry locks{bus};
};

TEST(TonePlayer, SynthLengthsAndFades) {
    std::vector<int16_t> tone(4000, 1);
    const size_t n = synth_tone(ToneKind::Tone, 16000, 0, tone);
    ASSERT_EQ(n, 2400u);  // 16 kHz x 150 ms
    EXPECT_LT(std::abs(tone[0]), 100);
    EXPECT_LT(std::abs(tone[n - 1]), 100);
    tone.resize(n);
    EXPECT_GT(peak(tone), 10000);
    // Past its end there is nothing more.
    std::vector<int16_t> rest(16);
    EXPECT_EQ(synth_tone(ToneKind::Tone, 16000, 2400, rest), 0u);
    // A tail: only what is left.
    EXPECT_EQ(synth_tone(ToneKind::Tone, 16000, 2300, rest), 16u);
    std::vector<int16_t> tail(500);
    EXPECT_EQ(synth_tone(ToneKind::Tone, 16000, 2300, tail), 100u);

    // An alarm segment is 16000 frames, and the second is the first again.
    std::vector<int16_t> alarm(32000);
    ASSERT_EQ(synth_tone(ToneKind::Alarm, 16000, 0, alarm), 32000u);
    EXPECT_TRUE(std::equal(alarm.begin(), alarm.begin() + 16000, alarm.begin() + 16000));
    EXPECT_GT(peak({alarm.begin(), alarm.begin() + 3200}), 10000);         // 0-200 ms: sound
    EXPECT_EQ(peak({alarm.begin() + 3300, alarm.begin() + 4700}), 0);      // 200-300 ms: silence
    EXPECT_GT(peak({alarm.begin() + 4800, alarm.begin() + 8000}), 10000);  // 300-500 ms: sound
    EXPECT_EQ(peak({alarm.begin() + 8100, alarm.begin() + 16000}), 0);     // 500-1000 ms: silence
    // From an offset it continues where it was.
    std::vector<int16_t> part(1000);
    synth_tone(ToneKind::Alarm, 16000, 16100, part);
    EXPECT_TRUE(std::equal(part.begin(), part.end(), alarm.begin() + 100));
}

TEST_F(TonePlayerTest, PlaysOneTone) {
    auto out = std::make_shared<RecordingAudioOut>();
    TonePlayer player(out, &locks);
    ASSERT_TRUE(player.start());
    player.play(ToneKind::Tone, 100);
    ASSERT_TRUE(wait_until([&] { return !player.playing(); }, 2000));
    EXPECT_EQ(out->frames(), 2400u);
    EXPECT_EQ(out->rate(), 16000u);
    EXPECT_EQ(out->opens(), 1);
    EXPECT_EQ(out->closes(), 1);
}

TEST_F(TonePlayerTest, AlarmStopsAt30Seconds) {
    auto out = std::make_shared<RecordingAudioOut>();
    TonePlayer player(out, &locks);
    ASSERT_TRUE(player.start());
    player.play(ToneKind::Alarm, 60);
    ASSERT_TRUE(wait_until([&] { return !player.playing(); }, 10000));
    EXPECT_EQ(out->frames(), 16000u * TonePlayer::kAlarmMaxMs / 1000);
    EXPECT_EQ(out->closes(), 1);
}

TEST_F(TonePlayerTest, SilenceStopsAlarm) {
    auto out = std::make_shared<RecordingAudioOut>(/*pace_ms=*/5);
    TonePlayer player(out, &locks);
    ASSERT_TRUE(player.start());
    player.play(ToneKind::Alarm, 60);
    ASSERT_TRUE(wait_until([&] { return out->frames() > 1000; }, 2000));
    EXPECT_TRUE(player.playing());
    player.silence();
    ASSERT_TRUE(wait_until([&] { return !player.playing(); }, 1000));
    const size_t stopped_at = out->frames();
    wisp::os::sleep_ms(50);
    EXPECT_EQ(out->frames(), stopped_at);
    EXPECT_LT(stopped_at, 16000u);
    EXPECT_EQ(out->closes(), 1);
}

TEST_F(TonePlayerTest, PlayReplacesWhatIsPlaying) {
    auto out = std::make_shared<RecordingAudioOut>(/*pace_ms=*/5);
    TonePlayer player(out, &locks);
    ASSERT_TRUE(player.start());
    player.play(ToneKind::Alarm, 60);
    ASSERT_TRUE(wait_until([&] { return out->frames() > 1000; }, 2000));
    player.play(ToneKind::Tone, 60);
    ASSERT_TRUE(wait_until([&] { return !player.playing(); }, 2000));
    // The alarm stopped well short of a second; the tone played whole.
    EXPECT_LT(out->frames(), 16000u);
    EXPECT_EQ(out->opens(), 2);
}

TEST_F(TonePlayerTest, NoSpeakerIsSilent) {
    {
        TonePlayer player(nullptr, &locks);
        EXPECT_TRUE(player.start());
        player.play(ToneKind::Alarm, 60);
        EXPECT_FALSE(player.playing());
        player.silence();
    }
    auto out = std::make_shared<RecordingAudioOut>(0, /*fail_open=*/true);
    TonePlayer player(out, &locks);
    ASSERT_TRUE(player.start());
    player.play(ToneKind::Alarm, 60);
    ASSERT_TRUE(wait_until([&] { return !player.playing(); }, 1000));
    EXPECT_EQ(out->opens(), 1);
    EXPECT_EQ(out->frames(), 0u);
    EXPECT_FALSE(locks.held(WakeLockKind::Cpu));
}

TEST_F(TonePlayerTest, HoldsCpuLockWhilePlaying) {
    auto out = std::make_shared<RecordingAudioOut>(/*pace_ms=*/5);
    TonePlayer player(out, &locks);
    ASSERT_TRUE(player.start());
    player.play(ToneKind::Alarm, 60);
    ASSERT_TRUE(wait_until([&] { return out->frames() > 0; }, 2000));
    EXPECT_TRUE(locks.held(WakeLockKind::Cpu));
    auto held = locks.list();
    ASSERT_EQ(held.size(), 1u);
    EXPECT_EQ(held[0].holder, "system.tone");
    player.silence();
    ASSERT_TRUE(wait_until([&] { return !player.playing(); }, 1000));
    EXPECT_TRUE(wait_until([&] { return !locks.held(WakeLockKind::Cpu); }, 1000));
}

TEST_F(TonePlayerTest, VolumeScalesAmplitude) {
    auto loud = std::make_shared<RecordingAudioOut>();
    auto soft = std::make_shared<RecordingAudioOut>();
    {
        TonePlayer player(loud, &locks);
        ASSERT_TRUE(player.start());
        player.play(ToneKind::Tone, 100);
        ASSERT_TRUE(wait_until([&] { return !player.playing(); }, 2000));
    }
    {
        TonePlayer player(soft, &locks);
        ASSERT_TRUE(player.start());
        player.play(ToneKind::Tone, 30);
        ASSERT_TRUE(wait_until([&] { return !player.playing(); }, 2000));
    }
    const double ratio = static_cast<double>(peak(soft->samples())) / peak(loud->samples());
    EXPECT_NEAR(ratio, 0.3, 0.3 * 0.05);

    // Volume 0 plays nothing.
    auto mute = std::make_shared<RecordingAudioOut>();
    TonePlayer player(mute, &locks);
    ASSERT_TRUE(player.start());
    player.play(ToneKind::Tone, 0);
    EXPECT_FALSE(player.playing());
    wisp::os::sleep_ms(20);
    EXPECT_EQ(mute->opens(), 0);
}

TEST(TonePlayer, TaskConfigIsPsramStack) {
    const wisp::os::TaskConfig c = TonePlayer::task_config(TonePlayerConfig{});
    EXPECT_STREQ(c.name, "wisp.tone");
    EXPECT_FALSE(c.flash_access);
    EXPECT_EQ(c.stack_bytes, 4096u);
    EXPECT_EQ(c.priority, 3);
}

}  // namespace
