// wisp::shell::TalkKey and find_talk_button() (wisp/shell/talk_key.hpp):
// hold-to-talk on the button whose caps "role" is "talk".

#include "wisp/shell/talk_key.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <string>

#include "shell/shell_fixture.hpp"
#include "support/fake_devices.hpp"
#include "support/wait.hpp"
#include "wisp/core/device_registry.hpp"
#include "wisp/hal/roles.hpp"
#include "wisp/shell/conversation.hpp"
#include "wisp/voice/limits.hpp"

using wisp::test::wait_until;
using wisp::test::audio::silence;
using wisp::test::audio::sine;
using wisp::voice::VoiceState;

namespace {

constexpr const char* kKey = "button.vol_down";

class TalkKeyTest : public wisp::test::ShellFixture {
protected:
    void make_key() {
        start();
        key = std::make_unique<wisp::shell::TalkKey>(
            h->bus, kKey, *conv, [this] { return screen.load(); },
            [this] { return key_ms.load(); });
        key->start();
        mic->set_script({silence(300), sine(1000)});
    }

    void TearDown() override {
        key.reset();
        ShellFixture::TearDown();
    }

    // A mic that only reads live silence, in real time: a scripted one
    // moves the voice's clock as it is read, which would run a priming
    // out (kPrimeMaxMs) by itself.
    void live_mic() { mic->set_script({}); }

    void press(const char* what) {
        h->bus.publish({std::string(kKey) + "." + what, kKey, 0, {}});
        h->bus.wait_idle(1000);
    }

    std::unique_ptr<wisp::shell::TalkKey> key;
    std::atomic<bool> screen{true};
    std::atomic<uint64_t> key_ms{10000};
};

}  // namespace

// A short press primes the mic and closes it again at the release, long
// before the priming would time out: no capture, no hold.
TEST_F(TalkKeyTest, ShortPressPrimesThenUnprimes) {
    make_key();
    live_mic();
    press("down");
    ASSERT_TRUE(wait_until([&] { return mic->is_open(); }, 1000));
    key_ms += 250;
    wisp::os::sleep_ms(3 * wisp::shell::TalkKey::kPollMs);
    press("up");
    ASSERT_TRUE(wait_until([&] { return !mic->is_open(); }, 500));
    key_ms += 1000;
    wisp::os::sleep_ms(3 * wisp::shell::TalkKey::kPollMs);
    EXPECT_FALSE(conv->holding());
    EXPECT_FALSE(voice->capturing());
    EXPECT_EQ(mic->opens(), 1);
}

// The hold takes the mic its press primed: it is opened once.
TEST_F(TalkKeyTest, HoldKeepsThePrimedMic) {
    make_key();
    live_mic();
    press("down");
    ASSERT_TRUE(wait_until([&] { return mic->is_open(); }, 1000));
    key_ms += wisp::voice::kHoldStartMs;
    ASSERT_TRUE(wait_until([&] { return conv->holding(); }, 1000));
    ASSERT_TRUE(wait_until([&] { return voice->state() == VoiceState::Listening; }, 2000));
    EXPECT_EQ(mic->opens(), 1);
    key->stop();
    EXPECT_FALSE(conv->holding());
    ASSERT_TRUE(settled());
    EXPECT_EQ(mic->opens(), 1);
}

// Stopped mid-press (no hold yet): the primed mic closes.
TEST_F(TalkKeyTest, StopMidPressUnprimes) {
    make_key();
    live_mic();
    press("down");
    ASSERT_TRUE(wait_until([&] { return mic->is_open(); }, 1000));
    key->stop();
    ASSERT_TRUE(wait_until([&] { return !mic->is_open(); }, 500));
    key_ms += 1000;
    wisp::os::sleep_ms(3 * wisp::shell::TalkKey::kPollMs);
    EXPECT_FALSE(conv->holding());
    EXPECT_FALSE(voice->capturing());
}

TEST_F(TalkKeyTest, HoldStartsAfter300ms) {
    make_key();
    // The press primes the mic, which reads the script as fast as it can:
    // the words come once the hold has begun.
    mic->hold_at(0);
    press("down");
    key_ms += 299;
    wisp::os::sleep_ms(4 * wisp::shell::TalkKey::kPollMs);
    EXPECT_FALSE(conv->holding());
    key_ms += 1;
    ASSERT_TRUE(wait_until([&] { return conv->holding(); }, 1000));
    mic->release();
    ASSERT_TRUE(wait_until([&] { return voice->state() == VoiceState::Listening; }, 2000));
    ASSERT_TRUE(wait_until([&] { return mic->script_done(); }, 3000));
    server.push_asr(wisp::test::asr_text("你好"));
    server.llm().push(wisp::test::llm_text("<say>好。</say>"));
    server.push_tts(wisp::test::tts_pcm());
    press("up");
    EXPECT_FALSE(conv->holding());
    ASSERT_TRUE(wait_turns(1));
    ASSERT_TRUE(settled());
    EXPECT_EQ(requests(wisp::test::kAsrPath), 1u);
}

// The key wakes the screen (PowerManager) and, held, talks at once.
TEST_F(TalkKeyTest, HoldFromAScreenOffPressStartsTheCapture) {
    make_key();
    screen = false;
    press("down");
    // Still dark at the hold's tick: the screen may take longer to light
    // than the hold to begin.
    key_ms += wisp::voice::kHoldStartMs;
    ASSERT_TRUE(wait_until([&] { return conv->holding(); }, 1000));
    ASSERT_TRUE(wait_until([&] { return voice->state() == VoiceState::Listening; }, 2000));
    screen = true;  // the press woke it
    ASSERT_TRUE(wait_until([&] { return mic->script_done(); }, 3000));
    server.push_asr(wisp::test::asr_text("你好"));
    server.llm().push(wisp::test::llm_text("<say>好。</say>"));
    server.push_tts(wisp::test::tts_pcm());
    press("up");
    EXPECT_FALSE(conv->holding());
    ASSERT_TRUE(wait_turns(1));
    ASSERT_TRUE(settled());
    EXPECT_EQ(requests(wisp::test::kAsrPath), 1u);
}

// A short press from a dark screen only wakes it: no priming (the mic may
// still be powering up), no capture.
TEST_F(TalkKeyTest, ShortPressWithTheScreenOffDoesNothingElse) {
    make_key();
    screen = false;
    press("down");
    screen = true;
    key_ms += 250;
    wisp::os::sleep_ms(3 * wisp::shell::TalkKey::kPollMs);
    press("up");
    key_ms += 1000;
    wisp::os::sleep_ms(4 * wisp::shell::TalkKey::kPollMs);
    EXPECT_FALSE(conv->holding());
    EXPECT_FALSE(voice->capturing());
    EXPECT_EQ(mic->opens(), 0);
    EXPECT_FALSE(saw_state("listening"));
}

TEST(FindTalkButton, UsesRoleCaps) {
    wisp::core::DeviceRegistry reg;
    EXPECT_EQ(wisp::shell::find_talk_button(reg), std::nullopt);
    ASSERT_TRUE(reg.add(std::make_shared<wisp::test::FakeButton>(
        "button.power", wisp::Properties{{"role", wisp::Value(wisp::hal::role::kPower)}})));
    ASSERT_TRUE(reg.add(std::make_shared<wisp::test::FakeButton>(
        "button.vol_up", wisp::Properties{{"role", wisp::Value("volume")}})));
    EXPECT_EQ(wisp::shell::find_talk_button(reg), std::nullopt);
    ASSERT_TRUE(reg.add(std::make_shared<wisp::test::FakeButton>(
        "button.vol_down",
        wisp::Properties{{"standby", wisp::Value("wake_source")}, {"role", wisp::Value("talk")}})));
    EXPECT_EQ(wisp::shell::find_talk_button(reg), std::optional<std::string>("button.vol_down"));
}
