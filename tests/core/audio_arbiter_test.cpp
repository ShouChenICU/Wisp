// wisp::core::AudioArbiter (wisp/core/audio_arbiter.hpp) with a clock the
// tests move by hand: the waits are timed by it, not by real sleeps.

#include "wisp/core/audio_arbiter.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <memory>
#include <thread>

#include "support/wait.hpp"
#include "wisp/os/os.hpp"

using wisp::core::AudioArbiter;
using wisp::core::AudioKind;
using wisp::test::wait_until;
using Lease = AudioArbiter::Lease;

namespace {

// Long enough for a waiting acquire() to have returned had it not waited.
constexpr uint32_t kSettleMs = 20;

struct AudioArbiterTest : ::testing::Test {
    std::atomic<uint64_t> now{1000};
    AudioArbiter arb{AudioArbiter::Config{}, [this] { return now.load(); }};

    // Runs acquire(kind) on a thread of its own.
    struct Pending {
        std::atomic<bool> done{false};
        Lease lease;
        std::thread t;
        ~Pending() {
            if (t.joinable()) t.join();
        }
    };
    std::unique_ptr<Pending> acquire_async(AudioKind kind,
                                           std::function<bool()> stopped = nullptr) {
        auto p = std::make_unique<Pending>();
        Pending* raw = p.get();
        p->t = std::thread([this, raw, kind, stopped] {
            raw->lease = arb.acquire(kind, stopped);
            raw->done = true;
        });
        return p;
    }
    void advance(uint64_t ms) { now += ms; }
};

}  // namespace

TEST_F(AudioArbiterTest, AlarmStopsVoiceAndWins) {
    std::atomic<int> stops{0};
    arb.set_voice_stop([&] { ++stops; });
    auto voice = std::make_unique<Lease>(arb.acquire(AudioKind::Voice));
    ASSERT_TRUE(voice->valid());

    // The voice is told to stop; the alarm waits for its output to close.
    auto alarm = acquire_async(AudioKind::Alarm);
    ASSERT_TRUE(wait_until([&] { return stops.load() == 1; }, 1000));
    wisp::os::sleep_ms(kSettleMs);
    EXPECT_FALSE(alarm->done.load());
    voice.reset();
    ASSERT_TRUE(wait_until([&] { return alarm->done.load(); }, 1000));
    EXPECT_TRUE(alarm->lease.valid());
    EXPECT_EQ(alarm->lease.kind(), AudioKind::Alarm);
    EXPECT_TRUE(arb.held(AudioKind::Alarm));
    alarm->lease.release();
    EXPECT_FALSE(arb.held(AudioKind::Alarm));

    // A voice that never lets go: the alarm plays after alarm_wait_ms.
    Lease stuck = arb.acquire(AudioKind::Voice);
    ASSERT_TRUE(stuck.valid());
    auto late = acquire_async(AudioKind::Alarm);
    ASSERT_TRUE(wait_until([&] { return stops.load() == 2; }, 1000));
    advance(499);
    wisp::os::sleep_ms(kSettleMs);
    EXPECT_FALSE(late->done.load());
    advance(1);
    ASSERT_TRUE(wait_until([&] { return late->done.load(); }, 1000));
    EXPECT_TRUE(late->lease.valid());
    EXPECT_TRUE(arb.held(AudioKind::Voice));
    late->t.join();
    late->lease.release();
    stuck.release();
    EXPECT_FALSE(arb.held(AudioKind::Voice));
}

TEST_F(AudioArbiterTest, ToneIsRefusedWhileVoiceHolds) {
    {
        Lease voice = arb.acquire(AudioKind::Voice);
        ASSERT_TRUE(voice.valid());
        Lease tone = arb.acquire(AudioKind::Tone);
        EXPECT_FALSE(tone.valid());
        EXPECT_FALSE(arb.held(AudioKind::Tone));
    }
    {
        Lease alarm = arb.acquire(AudioKind::Alarm);
        ASSERT_TRUE(alarm.valid());
        EXPECT_FALSE(arb.acquire(AudioKind::Tone).valid());
    }
    // Recording lets a tone through (the capture's start tone, a
    // notification while the mic is open).
    Lease mic = arb.acquire_input();
    ASSERT_TRUE(mic.valid());
    EXPECT_EQ(mic.kind(), AudioKind::Voice);
    EXPECT_TRUE(arb.held(AudioKind::Voice));
    Lease tone = arb.acquire(AudioKind::Tone);
    EXPECT_TRUE(tone.valid());
    EXPECT_TRUE(arb.held(AudioKind::Tone));
}

TEST_F(AudioArbiterTest, VoiceWaitsOutATone) {
    auto tone = std::make_unique<Lease>(arb.acquire(AudioKind::Tone));
    ASSERT_TRUE(tone->valid());
    auto voice = acquire_async(AudioKind::Voice);
    wisp::os::sleep_ms(kSettleMs);
    advance(299);
    wisp::os::sleep_ms(kSettleMs);
    EXPECT_FALSE(voice->done.load());
    tone.reset();
    ASSERT_TRUE(wait_until([&] { return voice->done.load(); }, 1000));
    EXPECT_TRUE(voice->lease.valid());
    voice->t.join();
    voice->lease.release();

    // A voice that is stopped while it waits gives up at once.
    Lease tone2 = arb.acquire(AudioKind::Tone);
    std::atomic<bool> stop{false};
    auto stopped = acquire_async(AudioKind::Voice, [&] { return stop.load(); });
    wisp::os::sleep_ms(kSettleMs);
    EXPECT_FALSE(stopped->done.load());
    stop = true;
    ASSERT_TRUE(wait_until([&] { return stopped->done.load(); }, 1000));
    EXPECT_FALSE(stopped->lease.valid());
    EXPECT_FALSE(arb.held(AudioKind::Voice));
}

TEST_F(AudioArbiterTest, VoiceGivesUpBehindAnAlarm) {
    Lease alarm = arb.acquire(AudioKind::Alarm);
    ASSERT_TRUE(alarm.valid());
    auto voice = acquire_async(AudioKind::Voice);
    wisp::os::sleep_ms(kSettleMs);
    advance(299);
    wisp::os::sleep_ms(kSettleMs);
    EXPECT_FALSE(voice->done.load());
    advance(1);
    ASSERT_TRUE(wait_until([&] { return voice->done.load(); }, 1000));
    EXPECT_FALSE(voice->lease.valid());
    EXPECT_FALSE(arb.held(AudioKind::Voice));
    EXPECT_TRUE(arb.held(AudioKind::Alarm));
}
