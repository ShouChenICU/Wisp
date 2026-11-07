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
#include "wisp/shell/conversation.hpp"

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

    void press(const char* what) {
        h->bus.publish({std::string(kKey) + "." + what, kKey, 0, {}});
        h->bus.wait_idle(1000);
    }

    std::unique_ptr<wisp::shell::TalkKey> key;
    std::atomic<bool> screen{true};
    std::atomic<uint64_t> key_ms{10000};
};

}  // namespace

TEST_F(TalkKeyTest, ShortPressDoesNothing) {
    make_key();
    press("down");
    key_ms += 250;
    wisp::os::sleep_ms(3 * wisp::shell::TalkKey::kPollMs);
    press("up");
    key_ms += 1000;
    wisp::os::sleep_ms(3 * wisp::shell::TalkKey::kPollMs);
    EXPECT_FALSE(conv->holding());
    EXPECT_FALSE(voice->capturing());
    EXPECT_EQ(mic->opens(), 0);
}

TEST_F(TalkKeyTest, HoldStartsAfter300ms) {
    make_key();
    press("down");
    key_ms += 299;
    wisp::os::sleep_ms(4 * wisp::shell::TalkKey::kPollMs);
    EXPECT_FALSE(conv->holding());
    key_ms += 1;
    ASSERT_TRUE(wait_until([&] { return conv->holding(); }, 1000));
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

TEST_F(TalkKeyTest, IgnoredWhileScreenOff) {
    make_key();
    screen = false;
    press("down");
    // The press woke the screen; it still does not count.
    screen = true;
    key_ms += 1000;
    wisp::os::sleep_ms(4 * wisp::shell::TalkKey::kPollMs);
    EXPECT_FALSE(conv->holding());
    press("up");
    EXPECT_FALSE(conv->holding());
    EXPECT_EQ(mic->opens(), 0);
}

TEST(FindTalkButton, UsesRoleCaps) {
    wisp::core::DeviceRegistry reg;
    EXPECT_EQ(wisp::shell::find_talk_button(reg), std::nullopt);
    ASSERT_TRUE(reg.add(std::make_shared<wisp::test::FakeButton>("button.power")));
    ASSERT_TRUE(reg.add(std::make_shared<wisp::test::FakeButton>(
        "button.vol_up", wisp::Properties{{"role", wisp::Value("volume")}})));
    EXPECT_EQ(wisp::shell::find_talk_button(reg), std::nullopt);
    ASSERT_TRUE(reg.add(std::make_shared<wisp::test::FakeButton>(
        "button.vol_down",
        wisp::Properties{{"standby", wisp::Value("wake_source")}, {"role", wisp::Value("talk")}})));
    EXPECT_EQ(wisp::shell::find_talk_button(reg), std::optional<std::string>("button.vol_down"));
}
