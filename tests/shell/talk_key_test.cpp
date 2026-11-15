// wisp::shell::TalkKey and find_talk_button() (wisp/shell/talk_key.hpp):
// hold-to-talk on the button whose caps "role" is "talk".

#include "wisp/shell/talk_key.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <string>
#include <string_view>

#include "shell/shell_fixture.hpp"
#include "support/bus_flood.hpp"
#include "support/fake_devices.hpp"
#include "support/wait.hpp"
#include "wisp/core/button_service.hpp"
#include "wisp/core/device_registry.hpp"
#include "wisp/core/log.hpp"
#include "wisp/hal/roles.hpp"
#include "wisp/shell/conversation.hpp"
#include "wisp/voice/limits.hpp"

using wisp::test::wait_until;
using wisp::test::audio::silence;
using wisp::test::audio::sine;
using wisp::voice::VoiceState;
using Edge = wisp::core::ButtonEdge::Kind;

namespace {

constexpr const char* kKey = "button.vol_down";

class TalkKeyTest : public wisp::test::ShellFixture {
protected:
    void make_key() {
        start();
        buttons = std::make_unique<wisp::core::ButtonService>(registry, h->bus);
        key = std::make_unique<wisp::shell::TalkKey>(
            h->bus, *buttons, kKey, *conv, [this] { return screen.load(); },
            [this] {
                ++clock_reads;
                return key_ms.load();
            });
        key->start();
        mic->set_script({silence(300), sine(1000)});
    }

    // The key before the button service it listens to (which reports a
    // listener left over), the service before the harness's bus.
    void TearDown() override {
        key.reset();
        buttons.reset();
        ShellFixture::TearDown();
    }

    // A mic that only reads live silence, in real time: a scripted one
    // moves the voice's clock as it is read, which would run a priming
    // out (kPrimeMaxMs) by itself.
    void live_mic() { mic->set_script({}); }

    // An edge of the key stamped `at_ms` on the key's clock, through the
    // button service's listeners (and its bus copy).
    void emit(Edge kind, uint64_t at_ms) { buttons->emit_for_test(kKey, kind, at_ms * 1000); }
    // An edge stamped now, then everything it caused on wisp.bus.
    void press(Edge kind) {
        emit(kind, key_ms.load());
        h->bus.wait_idle(1000);
    }
    // Returns once the talk key's timer has read the clock twice more: the
    // first of those ticks saw the clock as it is now and has returned.
    bool ticked() {
        const uint64_t from = clock_reads.load();
        return wait_until([&] { return clock_reads.load() >= from + 2; }, 1000);
    }

    wisp::core::DeviceRegistry registry;
    std::unique_ptr<wisp::core::ButtonService> buttons;
    std::unique_ptr<wisp::shell::TalkKey> key;
    std::atomic<bool> screen{true};
    std::atomic<uint64_t> key_ms{10000};
    std::atomic<uint64_t> clock_reads{0};
};

// Counts the talk-key inbox's overflow warnings.
struct OverflowWarnings : wisp::LogSink {
    OverflowWarnings() { wisp::log::add_sink(this); }
    ~OverflowWarnings() override { wisp::log::remove_sink(this); }
    OverflowWarnings(const OverflowWarnings&) = delete;
    OverflowWarnings& operator=(const OverflowWarnings&) = delete;
    void write(const wisp::LogRecord& rec) override {
        if (std::string_view(rec.msg).find("edge inbox talk_key overflowed") !=
            std::string_view::npos)
            ++count;
    }
    std::atomic<size_t> count{0};
};

}  // namespace

// A short press primes the mic and closes it again at the release, long
// before the priming would time out: no capture, no hold.
TEST_F(TalkKeyTest, ShortPressPrimesThenUnprimes) {
    make_key();
    live_mic();
    press(Edge::Down);
    ASSERT_TRUE(wait_until([&] { return mic->is_open(); }, 1000));
    key_ms += 250;
    wisp::os::sleep_ms(3 * wisp::shell::TalkKey::kPollMs);
    press(Edge::Up);
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
    press(Edge::Down);
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
    press(Edge::Down);
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
    press(Edge::Down);
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
    press(Edge::Up);
    EXPECT_FALSE(conv->holding());
    ASSERT_TRUE(wait_turns(1));
    ASSERT_TRUE(settled());
    EXPECT_EQ(requests(wisp::test::kAsrPath), 1u);
}

// The key wakes the screen (PowerManager) and, held, talks at once.
TEST_F(TalkKeyTest, HoldFromAScreenOffPressStartsTheCapture) {
    make_key();
    screen = false;
    press(Edge::Down);
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
    press(Edge::Up);
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
    press(Edge::Down);
    screen = true;
    key_ms += 250;
    wisp::os::sleep_ms(3 * wisp::shell::TalkKey::kPollMs);
    press(Edge::Up);
    key_ms += 1000;
    wisp::os::sleep_ms(4 * wisp::shell::TalkKey::kPollMs);
    EXPECT_FALSE(conv->holding());
    EXPECT_FALSE(voice->capturing());
    EXPECT_EQ(mic->opens(), 0);
    EXPECT_FALSE(saw_state("listening"));
}

// The release of a hold needs no bus: with wisp.bus held and flooded (its
// events dropped) the up edge still reaches the key, which ends the hold
// and sends the recording.
TEST_F(TalkKeyTest, UpIsNeverLostFlooded) {
    make_key();
    mic->hold_at(0);
    press(Edge::Down);
    key_ms += wisp::voice::kHoldStartMs;
    ASSERT_TRUE(wait_until([&] { return conv->holding(); }, 1000));
    mic->release();
    ASSERT_TRUE(wait_until([&] { return voice->state() == VoiceState::Listening; }, 2000));
    ASSERT_TRUE(wait_until([&] { return mic->script_done(); }, 3000));
    server.push_asr(wisp::test::asr_text("你好"));
    server.llm().push(wisp::test::llm_text("<say>好。</say>"));
    server.push_tts(wisp::test::tts_pcm());

    const uint64_t dropped = h->bus.dropped();
    {
        wisp::test::BusFlood flood(h->bus);
        flood.hold();
        flood.flood(512);
        key_ms += 500;
        emit(Edge::Up, key_ms.load());
        EXPECT_TRUE(conv->holding());  // the inbox waits for wisp.bus
        flood.release();
    }
    EXPECT_GT(h->bus.dropped(), dropped);
    ASSERT_TRUE(wait_until([&] { return !conv->holding(); }, 2000));
    ASSERT_TRUE(wait_turns(1));
    ASSERT_TRUE(settled());
    EXPECT_EQ(requests(wisp::test::kAsrPath), 1u);
    EXPECT_EQ(key->overflows(), 0u);
}

// The hold is timed from the down edge's own time: a down delivered 600 ms
// late (wisp.bus held) starts the hold at the first tick, without the
// clock moving on, and its up then ends it.
TEST_F(TalkKeyTest, HoldIsTimedFromTheEdge) {
    make_key();
    mic->hold_at(0);
    {
        wisp::test::BusFlood flood(h->bus);
        flood.hold();
        emit(Edge::Down, key_ms.load());
        key_ms += 600;
        flood.release();
    }
    ASSERT_TRUE(wait_until([&] { return conv->holding(); }, 1000));
    mic->release();
    ASSERT_TRUE(wait_until([&] { return voice->state() == VoiceState::Listening; }, 2000));
    ASSERT_TRUE(wait_until([&] { return mic->script_done(); }, 3000));
    server.push_asr(wisp::test::asr_text("你好"));
    server.llm().push(wisp::test::llm_text("<say>好。</say>"));
    server.push_tts(wisp::test::tts_pcm());
    key_ms += 900;
    press(Edge::Up);
    EXPECT_FALSE(conv->holding());
    ASSERT_TRUE(wait_turns(1));
    ASSERT_TRUE(settled());
    EXPECT_EQ(requests(wisp::test::kAsrPath), 1u);
}

// A short press (up 100 ms after down) delivered 600 ms late, both edges
// at once: a press, not a near-empty capture sent to the recognizer.
TEST_F(TalkKeyTest, ShortPressDoesNotHold) {
    make_key();
    {
        wisp::test::BusFlood flood(h->bus);
        flood.hold();
        flood.flood(512);
        const uint64_t down = key_ms.load();
        emit(Edge::Down, down);
        emit(Edge::Up, down + 100);
        key_ms = down + 700;
        flood.release();
    }
    ASSERT_TRUE(h->bus.wait_idle(2000));
    EXPECT_FALSE(conv->holding());
    ASSERT_TRUE(settled());
    EXPECT_FALSE(saw_state("listening"));
    EXPECT_EQ(requests(wisp::test::kAsrPath), 0u);
}

// A release the listener has seen but wisp.bus has not delivered yet: the
// timer, 600 ms past the down, starts no hold for a key already up.
TEST_F(TalkKeyTest, NoHoldForAKeyReleasedButNotYetDelivered) {
    make_key();
    live_mic();
    const uint64_t down = key_ms.load();
    press(Edge::Down);
    ASSERT_TRUE(wait_until([&] { return mic->is_open(); }, 1000));
    {
        wisp::test::BusFlood flood(h->bus);
        flood.hold();
        emit(Edge::Up, down + 100);
        key_ms = down + 700;
        ASSERT_TRUE(ticked());
        EXPECT_FALSE(conv->holding());
        flood.release();
    }
    ASSERT_TRUE(h->bus.wait_idle(2000));
    ASSERT_TRUE(wait_until([&] { return !mic->is_open(); }, 1000));
    EXPECT_FALSE(conv->holding());
    ASSERT_TRUE(settled());
    EXPECT_FALSE(saw_state("listening"));
    EXPECT_EQ(requests(wisp::test::kAsrPath), 0u);
}

// An up stamped less than kHoldStartMs after its down is a press even when
// it arrives after the timer began the hold (the button service itself was
// late): the recording is dropped, not sent.
TEST_F(TalkKeyTest, LateUpOfAShortPressDropsTheHold) {
    make_key();
    mic->hold_at(0);
    const uint64_t down = key_ms.load();
    press(Edge::Down);
    key_ms += 600;
    ASSERT_TRUE(wait_until([&] { return conv->holding(); }, 1000));
    mic->release();
    ASSERT_TRUE(wait_until([&] { return voice->state() == VoiceState::Listening; }, 2000));
    ASSERT_TRUE(wait_until([&] { return mic->script_done(); }, 3000));
    emit(Edge::Up, down + wisp::voice::kHoldStartMs - 1);
    ASSERT_TRUE(h->bus.wait_idle(2000));
    EXPECT_FALSE(conv->holding());
    ASSERT_TRUE(settled());
    EXPECT_EQ(requests(wisp::test::kAsrPath), 0u);
}

// Edges delivered late keep their order: down, up, down (the key held
// again) leave the key down, so the hold starts, from the second down.
TEST_F(TalkKeyTest, EdgesKeepTheirOrderFlooded) {
    make_key();
    mic->hold_at(0);
    const uint64_t down = key_ms.load();
    {
        wisp::test::BusFlood flood(h->bus);
        flood.hold();
        flood.flood(512);
        emit(Edge::Down, down);
        emit(Edge::Up, down + 100);
        emit(Edge::Down, down + 200);
        key_ms = down + 900;
        flood.release();
    }
    ASSERT_TRUE(wait_until([&] { return conv->holding(); }, 1000));
    mic->release();
    ASSERT_TRUE(wait_until([&] { return voice->state() == VoiceState::Listening; }, 2000));
    ASSERT_TRUE(wait_until([&] { return mic->script_done(); }, 3000));
    server.push_asr(wisp::test::asr_text("你好"));
    server.llm().push(wisp::test::llm_text("<say>好。</say>"));
    server.push_tts(wisp::test::tts_pcm());
    key_ms += 900;
    press(Edge::Up);
    EXPECT_FALSE(conv->holding());
    ASSERT_TRUE(wait_turns(1));
    ASSERT_TRUE(settled());
    EXPECT_EQ(requests(wisp::test::kAsrPath), 1u);
}

// More than eight edges while wisp.bus is held overflow the inbox: the
// oldest are dropped, counted and logged once. The edges kept still end
// with the key up: no hold.
TEST_F(TalkKeyTest, InboxOverflowIsCountedAndLogged) {
    make_key();
    OverflowWarnings warnings;
    {
        wisp::test::BusFlood flood(h->bus);
        flood.hold();
        for (int i = 0; i < 5; ++i) {
            emit(Edge::Down, key_ms.load());
            // The key's own short edge: the listener keeps only downs and
            // ups, so it takes no room.
            emit(Edge::Short, key_ms.load() + 50);
            emit(Edge::Up, key_ms.load() + 50);
            key_ms += 100;
        }
        flood.release();
    }
    ASSERT_TRUE(h->bus.wait_idle(2000));
    EXPECT_EQ(key->overflows(), 2u);
    EXPECT_EQ(warnings.count.load(), 1u);
    EXPECT_FALSE(conv->holding());
    ASSERT_TRUE(settled());
    EXPECT_EQ(requests(wisp::test::kAsrPath), 0u);
}

// The up of a short press whose hold the timer began anyway (the up was
// stored after the tick, as in LateUpOfAShortPressDropsTheHold) is the
// oldest edge when the inbox overflows, so it is dropped. The next down,
// less than kHoldStartMs after that press's down, shows the press was
// short: the hold is dropped, not sent.
TEST_F(TalkKeyTest, LostUpOfAShortPressDropsTheHold) {
    make_key();
    mic->hold_at(0);
    const uint64_t down = key_ms.load();
    press(Edge::Down);
    key_ms += 600;
    ASSERT_TRUE(wait_until([&] { return conv->holding(); }, 1000));
    mic->release();
    ASSERT_TRUE(wait_until([&] { return voice->state() == VoiceState::Listening; }, 2000));
    ASSERT_TRUE(wait_until([&] { return mic->script_done(); }, 3000));
    {
        wisp::test::BusFlood flood(h->bus);
        flood.hold();
        emit(Edge::Up, down + 100);  // dropped below
        // Four short presses: nine edges in a ring of eight.
        for (uint64_t i = 0; i < 4; ++i) {
            emit(Edge::Down, down + 150 + i * 20);
            emit(Edge::Up, down + 160 + i * 20);
        }
        flood.release();
    }
    ASSERT_TRUE(h->bus.wait_idle(2000));
    EXPECT_EQ(key->overflows(), 1u);
    EXPECT_FALSE(conv->holding());
    ASSERT_TRUE(settled());
    EXPECT_EQ(requests(wisp::test::kAsrPath), 0u);
}

// The up of a hold already under way is the oldest edge when the inbox
// overflows, so it is dropped: the next down ends that hold first (its
// recording is sent) and then begins its own press, instead of leaving
// the capture to run to kMaxRecordMs.
TEST_F(TalkKeyTest, LostUpIsEndedByTheNextDown) {
    make_key();
    mic->hold_at(0);
    press(Edge::Down);
    key_ms += wisp::voice::kHoldStartMs;
    ASSERT_TRUE(wait_until([&] { return conv->holding(); }, 1000));
    mic->release();
    ASSERT_TRUE(wait_until([&] { return voice->state() == VoiceState::Listening; }, 2000));
    ASSERT_TRUE(wait_until([&] { return mic->script_done(); }, 3000));
    server.push_asr(wisp::test::asr_text("你好"));
    server.llm().push(wisp::test::llm_text("<say>好。</say>"));
    server.push_tts(wisp::test::tts_pcm());
    {
        wisp::test::BusFlood flood(h->bus);
        flood.hold();
        key_ms += 500;
        emit(Edge::Up, key_ms.load());  // the hold's up: dropped below
        // Four short presses: nine edges in a ring of eight.
        for (int i = 0; i < 4; ++i) {
            emit(Edge::Down, key_ms.load() + 10);
            emit(Edge::Up, key_ms.load() + 60);
            key_ms += 100;
        }
        flood.release();
    }
    ASSERT_TRUE(h->bus.wait_idle(2000));
    EXPECT_EQ(key->overflows(), 1u);
    EXPECT_FALSE(conv->holding());
    ASSERT_TRUE(wait_turns(1));
    ASSERT_TRUE(settled());
    EXPECT_EQ(requests(wisp::test::kAsrPath), 1u);
}

TEST(FindTalkButton, UsesTheTalkRole) {
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

// One key may be both the power and the talk key (wisp/hal/roles.hpp).
TEST(FindTalkButton, AcceptsARoleList) {
    wisp::core::DeviceRegistry reg;
    ASSERT_TRUE(reg.add(std::make_shared<wisp::test::FakeButton>(
        "button.side",
        wisp::Properties{{"role", wisp::Value(wisp::List{wisp::Value(wisp::hal::role::kPower),
                                                         wisp::Value(wisp::hal::role::kTalk)})}})));
    EXPECT_EQ(wisp::shell::find_talk_button(reg), std::optional<std::string>("button.side"));
}
