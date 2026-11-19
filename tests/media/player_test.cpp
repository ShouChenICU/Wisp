// media::MediaPlayer (media spec 4, 5.2, 6): the music session on its own
// task, decoding real files from a MediaLibrary on a temporary card and
// from SceneFiles, through an AudioArbiter and wake locks on a clock the
// tests move by hand.

#include "wisp/media/player.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <memory>
#include <mutex>
#include <string>
#include <utility>
#include <vector>

#include "support/fake_audio.hpp"
#include "support/fake_devices.hpp"
#include "support/media_data.hpp"
#include "support/temp_dir.hpp"
#include "support/wait.hpp"
#include "wisp/core/device_registry.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/core/scene_files.hpp"
#include "wisp/core/storage_manager.hpp"
#include "wisp/core/storage_router.hpp"
#include "wisp/core/storage_worker.hpp"
#include "wisp/media/library.hpp"
#include "wisp/os/os.hpp"
#include "wisp/port/sim/minimp3_decoder.hpp"

using wisp::ErrorCode;
using wisp::Result;
using wisp::core::AudioArbiter;
using wisp::core::AudioKind;
using wisp::core::IoLane;
using wisp::core::Medium;
using wisp::core::StorageWorker;
using wisp::core::WakeLockKind;
using wisp::media::MediaLibrary;
using wisp::media::MediaPlayer;
using wisp::media::PauseReason;
using wisp::media::PlayerState;
using wisp::media::PlayerStatus;
using wisp::media::PlayMode;
using wisp::test::wait_until;

namespace fs = std::filesystem;

namespace {

constexpr uint32_t kWaitMs = 5000;
// Long enough for the task to have acted had it been going to.
constexpr uint32_t kSettleMs = 50;

// Every state the cell published, through its mirror (a plain function
// pointer, so a global).
std::mutex g_log_mu;
std::vector<PlayerState> g_log;

void record(wisp::core::EventBus&, const PlayerState&, const PlayerState& after) {
    std::lock_guard<std::mutex> lock(g_log_mu);
    g_log.push_back(after);
}

std::vector<PlayerState> logged() {
    std::lock_guard<std::mutex> lock(g_log_mu);
    return g_log;
}

void append_le(std::string& s, uint32_t v, int bytes) {
    for (int i = 0; i < bytes; ++i) s.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}

// A PCM 16-bit WAV whose samples tell the second they belong to:
// 100 * (second + 1) (both channels alike).
std::string make_wav(uint32_t seconds, uint32_t rate = 8000, uint16_t channels = 1) {
    const uint32_t frames = seconds * rate;
    const uint32_t data = frames * channels * 2;
    std::string s = "RIFF";
    append_le(s, 36 + data, 4);
    s += "WAVEfmt ";
    append_le(s, 16, 4);
    append_le(s, 1, 2);
    append_le(s, channels, 2);
    append_le(s, rate, 4);
    append_le(s, rate * channels * 2, 4);
    append_le(s, channels * 2, 2);
    append_le(s, 16, 2);
    s += "data";
    append_le(s, data, 4);
    for (uint32_t f = 0; f < frames; ++f) {
        const uint32_t v = 100 * (f / rate + 1);
        for (uint16_t c = 0; c < channels; ++c) append_le(s, v, 2);
    }
    return s;
}

std::string read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

struct Fixture {
    explicit Fixture(bool real_arbiter_clock = false)
        : in(wisp::test::temp_dir().string()),
          sd(wisp::test::temp_dir().string()),
          arbiter(AudioArbiter::Config{},
                  real_arbiter_clock ? std::function<uint64_t()>(wisp::os::now_ms)
                                     : std::function<uint64_t()>([this] { return now.load(); })) {
        {
            std::lock_guard<std::mutex> lock(g_log_mu);
            g_log.clear();
        }
        int_dev = std::make_shared<wisp::test::FakeStorage>("storage.int", in, false);
        sd_dev = std::make_shared<wisp::test::FakeStorage>("storage.sd", sd, true);
        registry.add(int_dev);
        registry.add(sd_dev);
        EXPECT_TRUE(bus.start());
        storage = std::make_unique<wisp::core::StorageManager>(registry, bus);
        EXPECT_TRUE(storage->init());
        EXPECT_TRUE(flash.start());
        EXPECT_TRUE(sd_worker.start());
        router = std::make_unique<wisp::core::StorageRouter>(flash, &sd_worker);
        locks = std::make_unique<wisp::core::WakeLockRegistry>(bus, [this] { return now.load(); });

        wisp::core::SceneFilesConfig fc;
        fc.data_root = in + "/scenes";
        fc.submit_io = [this](Medium m, IoLane lane, std::function<void()> job) {
            if (refuse_scene_io.load()) {
                refusals.fetch_add(1);
                return false;
            }
            if (drop_scene_io.load() > 0) {
                // Accepted, then destroyed without running.
                drop_scene_io.fetch_sub(1);
                return true;
            }
            return router->submit(m, std::move(job), lane);
        };
        files = std::make_unique<wisp::core::SceneFiles>(*storage, fc);

        fs::create_directories(fs::path(root()) / "music");
        copy("sine_44k_stereo.wav", "music/a44.wav");
        copy("sine_16k_mono.wav", "music/b16.wav");
        copy("float32.wav", "music/float.wav");
        copy("garbage.mp3", "music/garbage.mp3");
        copy("sine_44k_stereo_cbr128.mp3", "music/s.mp3");
        put("music/long.wav", make_wav(30));
        put("music/six.wav", make_wav(6));
        fs::create_directories(fs::path(root()) / "music/sub");
        put("music/sub/long.wav", make_wav(30));

        lib = std::make_unique<MediaLibrary>(wisp::media::MediaLibraryDeps{
            .storage = *storage, .router = router.get(), .bus = bus, .root = root()});
        lib->start();
        EXPECT_TRUE(wait_until(
            [this] {
                auto s = lib->state().get();
                return s.available && !s.scanning;
            },
            kWaitMs));
        EXPECT_TRUE(router->drain(kWaitMs));
    }

    ~Fixture() {
        out->open_gate();
        player.reset();
        lib.reset();
        files.reset();
        router.reset();
        sd_worker.stop();
        flash.stop();
        locks.reset();
        storage.reset();
        bus.stop();
    }

    std::string root() const { return sd + "/media"; }

    void copy(const char* name, const std::string& rel) {
        std::error_code ec;
        fs::copy_file(wisp::test::media_data(name), fs::path(root()) / rel,
                      fs::copy_options::overwrite_existing, ec);
        ASSERT_FALSE(ec) << name;
    }

    void put(const std::string& rel, const std::string& bytes) {
        std::ofstream f(fs::path(root()) / rel, std::ios::binary | std::ios::trunc);
        f << bytes;
    }

    // A scene's private file, written as the scene would (on wisp.fs).
    void put_scene_file(const std::string& scene, const std::string& name,
                        const std::string& bytes) {
        Result<void> w = Result<void>::ok();
        ASSERT_TRUE(flash.run_sync(
            [&] {
                w = files->write(scene, name,
                                 {reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()});
            },
            kWaitMs));
        ASSERT_TRUE(w) << w.error().message;
    }

    void make_player(MediaPlayer::Config c = {}) {
        wisp::media::MediaPlayerDeps deps{
            .out = out,
            .arbiter = arbiter,
            .locks = *locks,
            .bus = bus,
            .library = lib.get(),
            .scene_files = files.get(),
            .router = router.get(),
            .decoders = wisp::media::with_wav(
                [](wisp::media::Format) { return wisp::port::sim::make_minimp3_decoder(); }),
            .volume =
                [this] {
                    volume_calls.fetch_add(1);
                    while (hold_volume.load()) wisp::os::sleep_ms(1);
                    return volume.load();
                },
            .random = [] { return 7u; },
            .now_ms = [this] { return now.load(); },
            .on_error =
                [this](const std::string& reason, const std::string& item) {
                    std::lock_guard<std::mutex> lock(err_mu);
                    errors.emplace_back(reason, item);
                },
            .mirror = &record,
        };
        player = std::make_unique<MediaPlayer>(std::move(deps), c);
        ASSERT_TRUE(player->start());
    }

    PlayerState st() const { return player->state().get(); }

    bool idle_after_play() {
        return wait_until([this] { return st().status == PlayerStatus::Idle && out->opens() > 0; },
                          kWaitMs);
    }

    std::vector<std::pair<std::string, std::string>> errs() {
        std::lock_guard<std::mutex> lock(err_mu);
        return errors;
    }

    bool locks_held() const {
        return locks->held(WakeLockKind::Cpu) && locks->held(WakeLockKind::Audio);
    }
    bool locks_free() const {
        return !locks->held(WakeLockKind::Cpu) && !locks->held(WakeLockKind::Audio);
    }

    // Starts `items` with the output's gate shut, and waits until the task
    // is blocked in its first write: the test then acts at a known point.
    void play_held(std::vector<std::string> items, size_t start = 0) {
        out->close_gate();
        player->play(std::move(items), start);
        ASSERT_TRUE(wait_until([this] { return out->writer_waiting(); }, kWaitMs));
    }

    // The locks linger for the full linger_ms after an exit, then go.
    void expect_linger_then_release(uint32_t linger_ms = 10000) {
        ASSERT_TRUE(locks_held());
        EXPECT_FALSE(out->is_open());
        now += linger_ms - 1;
        wisp::os::sleep_ms(kSettleMs);
        EXPECT_TRUE(locks_held());
        now += 1;
        EXPECT_TRUE(wait_until([this] { return locks_free(); }, kWaitMs));
    }

    std::string in, sd;
    std::atomic<uint64_t> now{1000};
    wisp::core::DeviceRegistry registry;
    wisp::core::EventBus bus;
    std::shared_ptr<wisp::test::FakeStorage> int_dev, sd_dev;
    std::unique_ptr<wisp::core::StorageManager> storage;
    StorageWorker flash;
    StorageWorker sd_worker{StorageWorker::sd_io_config()};
    std::unique_ptr<wisp::core::StorageRouter> router;
    std::unique_ptr<wisp::core::WakeLockRegistry> locks;
    std::atomic<bool> refuse_scene_io{false};
    std::atomic<int> refusals{0};
    std::atomic<int> drop_scene_io{0};
    std::unique_ptr<wisp::core::SceneFiles> files;
    std::unique_ptr<MediaLibrary> lib;
    AudioArbiter arbiter;
    std::shared_ptr<wisp::test::audio::FakeAudioOut> out =
        std::make_shared<wisp::test::audio::FakeAudioOut>();
    std::atomic<uint8_t> volume{50};
    std::atomic<bool> hold_volume{false};
    std::atomic<int> volume_calls{0};
    std::mutex err_mu;
    std::vector<std::pair<std::string, std::string>> errors;
    std::unique_ptr<MediaPlayer> player;
};

}  // namespace

TEST(MediaPlayer, PlaysAWavToTheEnd) {
    Fixture f;
    f.make_player();
    f.player->play({"music/a44.wav"});
    ASSERT_TRUE(f.idle_after_play());
    EXPECT_EQ(f.out->frames(), 11025u);
    ASSERT_EQ(f.out->formats().size(), 1u);
    EXPECT_EQ(f.out->formats()[0].sample_rate, 44100u);
    EXPECT_EQ(f.out->formats()[0].channels, 1);
    EXPECT_FALSE(f.out->is_open());
    EXPECT_EQ(f.st().queue_length, 0u);
    EXPECT_TRUE(f.errs().empty());
    // It went through Playing with the track.
    bool played = false;
    for (const PlayerState& s : logged()) {
        if (s.status == PlayerStatus::Playing && s.track.id == "music/a44.wav") played = true;
    }
    EXPECT_TRUE(played);
}

TEST(MediaPlayer, PlaysMp3AndDownmixes) {
    Fixture f;
    f.make_player();
    f.player->play({"music/s.mp3"});
    ASSERT_TRUE(f.idle_after_play());
    ASSERT_EQ(f.out->formats().size(), 1u);
    EXPECT_EQ(f.out->formats()[0].sample_rate, 44100u);
    EXPECT_EQ(f.out->formats()[0].channels, 1);

    // The same file decoded here, downmixed by hand.
    const std::string bytes = read_file(wisp::test::media_data("sine_44k_stereo_cbr128.mp3"));
    wisp::media::MemorySource src({reinterpret_cast<const uint8_t*>(bytes.data()), bytes.size()});
    auto dec = wisp::port::sim::make_minimp3_decoder();
    auto info = dec->open(src);
    ASSERT_TRUE(info);
    ASSERT_EQ(info.value().channels, 2);
    std::vector<int16_t> expect;
    std::vector<int16_t> pcm(MediaPlayer::kChunkFrames * 2);
    for (;;) {
        auto n = dec->decode(pcm.data(), MediaPlayer::kChunkFrames);
        ASSERT_TRUE(n);
        if (n.value() == 0) break;
        for (size_t i = 0; i < n.value(); ++i) {
            expect.push_back(static_cast<int16_t>(
                (static_cast<int32_t>(pcm[2 * i]) + static_cast<int32_t>(pcm[2 * i + 1])) / 2));
        }
    }
    EXPECT_GT(expect.size(), 0u);
    EXPECT_EQ(f.out->samples(), expect);
}

TEST(MediaPlayer, ReopensOnRateChange) {
    Fixture f;
    f.make_player();
    f.player->play({"music/a44.wav", "music/b16.wav"});
    ASSERT_TRUE(wait_until([&] { return f.out->frames() == 11025u + 16000u; }, kWaitMs));
    ASSERT_TRUE(f.idle_after_play());
    auto formats = f.out->formats();
    ASSERT_EQ(formats.size(), 2u);
    EXPECT_EQ(formats[0].sample_rate, 44100u);
    EXPECT_EQ(formats[1].sample_rate, 16000u);
    EXPECT_EQ(f.out->opens(), 2);
    EXPECT_EQ(f.out->overlapping_opens(), 0);
}

TEST(MediaPlayer, SkipsUnsupportedAndCorrupt) {
    Fixture f;
    f.make_player();
    f.player->play({"music/float.wav", "music/garbage.mp3", "music/b16.wav"});
    ASSERT_TRUE(wait_until([&] { return f.out->frames() == 16000u; }, kWaitMs));
    ASSERT_TRUE(f.idle_after_play());
    auto e = f.errs();
    ASSERT_EQ(e.size(), 2u);
    EXPECT_EQ(e[0], (std::pair<std::string, std::string>{"unsupported_format", "music/float.wav"}));
    EXPECT_EQ(e[1].second, "music/garbage.mp3");
    EXPECT_TRUE(e[1].first == "unsupported_format" || e[1].first == "decode_error") << e[1].first;
}

TEST(MediaPlayer, StopsAfterThreeFailures) {
    Fixture f;
    f.make_player();
    f.player->play({"music/float.wav", "music/garbage.mp3", "music/missing.wav", "music/b16.wav"});
    ASSERT_TRUE(wait_until([&] { return f.errs().size() == 4u; }, kWaitMs));
    auto e = f.errs();
    EXPECT_EQ(e[2], (std::pair<std::string, std::string>{"not_found", "music/missing.wav"}));
    EXPECT_EQ(e[3].first, "too_many_errors");
    EXPECT_TRUE(wait_until([&] { return f.st().status == PlayerStatus::Idle; }, kWaitMs));
    EXPECT_EQ(f.st().queue_length, 0u);
    EXPECT_EQ(f.out->opens(), 0);
    EXPECT_TRUE(f.locks_free());
}

TEST(MediaPlayer, OutputRefusingTheRateSkips) {
    Fixture f;
    f.out->set_fail_rate(16000);
    f.make_player();
    f.player->play({"music/b16.wav", "music/a44.wav"});
    ASSERT_TRUE(wait_until([&] { return f.out->frames() == 11025u; }, kWaitMs));
    ASSERT_TRUE(f.idle_after_play());
    auto e = f.errs();
    ASSERT_EQ(e.size(), 1u);
    EXPECT_EQ(e[0], (std::pair<std::string, std::string>{"unsupported_format", "music/b16.wav"}));
    ASSERT_EQ(f.out->formats().size(), 1u);
    EXPECT_EQ(f.out->formats()[0].sample_rate, 44100u);
}

TEST(MediaPlayer, PositionTicksOncePerSecond) {
    Fixture f;
    f.make_player();
    f.player->play({"music/six.wav"});
    ASSERT_TRUE(f.idle_after_play());
    // While it played, the position moved one second at a time and never
    // published the same second twice.
    std::vector<uint32_t> seen;
    for (const PlayerState& s : logged()) {
        if (s.status != PlayerStatus::Playing) continue;
        if (seen.empty() || seen.back() != s.position_s) seen.push_back(s.position_s);
    }
    EXPECT_EQ(seen, (std::vector<uint32_t>{0, 1, 2, 3, 4, 5, 6}));
    size_t playing = 0;
    for (const PlayerState& s : logged()) playing += s.status == PlayerStatus::Playing;
    EXPECT_EQ(playing, 7u);
}

TEST(MediaPlayer, SeekJumps) {
    Fixture f;
    f.make_player();
    f.play_held({"music/six.wav"});
    f.player->pause();
    f.out->open_gate();
    ASSERT_TRUE(wait_until([&] { return f.st().status == PlayerStatus::Paused; }, kWaitMs));
    f.player->seek(3);
    ASSERT_TRUE(wait_until([&] { return f.st().position_s == 3; }, kWaitMs));
    // Past the end clamps to the duration.
    f.player->seek(600);
    ASSERT_TRUE(wait_until([&] { return f.st().position_s == 6; }, kWaitMs));
    f.player->seek(3);
    ASSERT_TRUE(wait_until([&] { return f.st().position_s == 3; }, kWaitMs));
    const size_t before = f.out->frames();
    f.player->resume();
    ASSERT_TRUE(wait_until([&] { return f.out->frames() > before; }, kWaitMs));
    EXPECT_EQ(f.out->samples()[before], 400);  // second 3
    ASSERT_TRUE(f.idle_after_play());
    EXPECT_EQ(f.out->frames(), before + 3u * 8000u);
}

TEST(MediaPlayer, PrevRestartsAfter3s) {
    Fixture f;
    f.make_player();
    f.play_held({"music/six.wav", "music/long.wav"}, 1);
    f.player->pause();
    f.out->open_gate();
    ASSERT_TRUE(wait_until([&] { return f.st().status == PlayerStatus::Paused; }, kWaitMs));
    EXPECT_EQ(f.st().index, 1u);
    // At 0 s: the previous item, still paused.
    f.player->prev();
    ASSERT_TRUE(wait_until([&] { return f.st().index == 0u; }, kWaitMs));
    EXPECT_EQ(f.st().track.id, "music/six.wav");
    EXPECT_EQ(f.st().status, PlayerStatus::Paused);
    // Past 3 s: the same item from its start.
    f.player->seek(4);
    ASSERT_TRUE(wait_until([&] { return f.st().position_s == 4; }, kWaitMs));
    f.player->prev();
    ASSERT_TRUE(wait_until([&] { return f.st().position_s == 0; }, kWaitMs));
    EXPECT_EQ(f.st().index, 0u);
    // At 3 s exactly: the previous item (sequence stays at the first).
    f.player->seek(3);
    ASSERT_TRUE(wait_until([&] { return f.st().position_s == 3; }, kWaitMs));
    f.player->prev();
    ASSERT_TRUE(wait_until([&] { return f.st().position_s == 0; }, kWaitMs));
    EXPECT_EQ(f.st().index, 0u);
}

TEST(MediaPlayer, UserPauseHoldsPositionAndReleasesLocksAfterLinger) {
    Fixture f;
    f.make_player();
    f.play_held({"music/long.wav"});
    EXPECT_TRUE(f.locks_held());
    f.player->seek(2);
    f.player->pause();
    f.out->open_gate();
    ASSERT_TRUE(wait_until([&] { return f.st().status == PlayerStatus::Paused; }, kWaitMs));
    EXPECT_EQ(f.st().pause_reason, PauseReason::User);
    EXPECT_EQ(f.st().position_s, 2u);
    EXPECT_FALSE(f.arbiter.held(AudioKind::Music));
    f.expect_linger_then_release();
    EXPECT_EQ(f.st().position_s, 2u);
    // Resuming takes them again and goes on from the same place.
    const size_t before = f.out->frames();
    f.player->resume();
    ASSERT_TRUE(wait_until([&] { return f.out->frames() > before; }, kWaitMs));
    EXPECT_TRUE(f.locks_held());
    EXPECT_EQ(f.out->samples()[before], 300);  // second 2
}

TEST(MediaPlayer, PreemptPausesSystemAndResumes) {
    Fixture f;
    f.out->set_write_ms(5);
    f.make_player();
    f.play_held({"music/long.wav"});
    f.player->seek(2);
    f.out->open_gate();
    ASSERT_TRUE(wait_until([&] { return f.st().position_s >= 3; }, kWaitMs));

    AudioArbiter::Lease voice = f.arbiter.acquire(AudioKind::Voice);
    ASSERT_TRUE(voice.valid());
    ASSERT_TRUE(wait_until(
        [&] {
            auto s = f.st();
            return s.status == PlayerStatus::Paused && s.pause_reason == PauseReason::System;
        },
        kWaitMs));
    EXPECT_FALSE(f.out->is_open());
    EXPECT_FALSE(f.arbiter.held(AudioKind::Music));
    EXPECT_TRUE(f.locks_held());
    const uint32_t kept = f.st().position_s;
    const size_t before = f.out->frames();

    voice.release();
    f.now += 799;
    f.arbiter.tick();
    wisp::os::sleep_ms(kSettleMs);
    EXPECT_EQ(f.st().status, PlayerStatus::Paused);
    f.now += 1;
    f.arbiter.tick();
    ASSERT_TRUE(wait_until([&] { return f.st().status == PlayerStatus::Playing; }, kWaitMs));
    ASSERT_TRUE(wait_until([&] { return f.out->frames() > before; }, kWaitMs));
    EXPECT_GE(f.st().position_s, kept);
    EXPECT_EQ(f.out->samples()[before], static_cast<int16_t>(100 * (kept + 1)));
    EXPECT_EQ(f.out->opens(), 2);
}

TEST(MediaPlayer, PauseRacingTheResumeNotificationWins) {
    Fixture f;
    f.out->set_write_ms(5);
    f.make_player();
    f.play_held({"music/long.wav"});
    f.out->open_gate();
    AudioArbiter::Lease voice = f.arbiter.acquire(AudioKind::Voice);
    ASSERT_TRUE(wait_until([&] { return f.st().pause_reason == PauseReason::System; }, kWaitMs));
    voice.release();
    f.now += 800;

    // Hold the task inside a command, so the notification and the user's
    // pause both wait behind it, in that order.
    f.hold_volume = true;
    const int calls = f.volume_calls.load();
    f.player->on_volume_changed();
    ASSERT_TRUE(wait_until([&] { return f.volume_calls.load() > calls; }, kWaitMs));
    f.arbiter.tick();  // the resume notification
    f.player->pause();
    f.hold_volume = false;

    ASSERT_TRUE(wait_until([&] { return f.st().pause_reason == PauseReason::User; }, kWaitMs));
    wisp::os::sleep_ms(kSettleMs);
    EXPECT_EQ(f.st().status, PlayerStatus::Paused);
    EXPECT_EQ(f.st().pause_reason, PauseReason::User);
    EXPECT_EQ(f.out->opens(), 1);
    EXPECT_FALSE(f.out->is_open());
    EXPECT_FALSE(f.arbiter.held(AudioKind::Music));
    // A later notification does not resume a user pause either.
    f.arbiter.acquire(AudioKind::Tone).release();
    f.now += 800;
    f.arbiter.tick();
    wisp::os::sleep_ms(kSettleMs);
    EXPECT_EQ(f.st().pause_reason, PauseReason::User);
    EXPECT_EQ(f.out->opens(), 1);
}

TEST(MediaPlayer, PreemptedWhileWriteBlocksNeverReopens) {
    Fixture f(/*real_arbiter_clock=*/true);
    f.out->set_write_ms(500);
    f.make_player();
    f.player->play({"music/long.wav"});
    ASSERT_TRUE(wait_until([&] { return f.out->is_open(); }, kWaitMs));
    wisp::os::sleep_ms(kSettleMs);  // well inside the first 500 ms write

    const uint64_t t0 = wisp::os::now_ms();
    AudioArbiter::Lease voice = f.arbiter.acquire(AudioKind::Voice);
    const uint64_t took = wisp::os::now_ms() - t0;
    ASSERT_TRUE(voice.valid());
    EXPECT_GE(took, 190u);
    // Granted at the 200 ms grant, not when the 500 ms write ends.
    EXPECT_LT(took, 500u);

    ASSERT_TRUE(wait_until(
        [&] { return f.st().pause_reason == PauseReason::System && !f.out->is_open(); }, 2000));
    // Held well past the write that was in progress: never reopened.
    wisp::os::sleep_ms(600);
    EXPECT_EQ(f.out->opens(), 1);
    EXPECT_FALSE(f.out->is_open());
    EXPECT_EQ(f.st().status, PlayerStatus::Paused);
}

TEST(MediaPlayer, PlaysASceneFile) {
    Fixture f;
    f.put_scene_file("demo", "a.wav", read_file(wisp::test::media_data("sine_16k_mono.wav")));
    f.make_player();
    f.player->play({"scene:demo/a.wav"});
    ASSERT_TRUE(wait_until([&] { return f.out->frames() == 16000u; }, kWaitMs));
    ASSERT_TRUE(f.idle_after_play());
    EXPECT_TRUE(f.errs().empty());
    ASSERT_EQ(f.out->formats().size(), 1u);
    EXPECT_EQ(f.out->formats()[0].sample_rate, 16000u);
}

TEST(MediaPlayer, ForgetSceneDropsItsItems) {
    Fixture f;
    f.put_scene_file("demo", "long.wav", make_wav(12));
    f.put_scene_file("other", "x.wav", make_wav(1));
    f.make_player();
    f.play_held({"scene:demo/long.wav", "scene:other/x.wav", "scene:demo/x.wav", "music/b16.wav"});
    f.player->forget_scene("demo");
    f.out->open_gate();
    ASSERT_TRUE(f.idle_after_play());
    EXPECT_TRUE(f.errs().empty());
    // The scene's items went at once; the current one handed over to the
    // next remaining item.
    bool handed = false;
    for (const PlayerState& s : logged()) {
        if (s.track.id == "scene:other/x.wav") {
            handed = true;
            EXPECT_EQ(s.queue_length, 2u);
            EXPECT_EQ(s.index, 0u);
        }
        if (s.track.id == "scene:demo/x.wav") ADD_FAILURE() << "a forgotten item played";
    }
    EXPECT_TRUE(handed);
    // Only one chunk of the long item, then the 1 s item and b16.
    EXPECT_EQ(f.out->frames(), MediaPlayer::kChunkFrames + 8000u + 16000u);
}

TEST(MediaPlayer, CardRemovedWithAReadInFlightStops) {
    Fixture f;
    f.out->set_write_ms(1);
    f.make_player();
    // Hold the SD I/O worker, so the next refill waits in its lane.
    wisp::os::Semaphore gate{1, 0}, entered{1, 0};
    f.player->play({"music/long.wav", "music/six.wav"});
    ASSERT_TRUE(wait_until([&] { return f.out->frames() > 0; }, kWaitMs));
    ASSERT_TRUE(f.sd_worker.submit([&] {
        entered.give();
        gate.take(wisp::os::kForever);
    }));
    ASSERT_TRUE(entered.take(kWaitMs));
    ASSERT_TRUE(wait_until([&] { return f.router->pending() >= 2; }, kWaitMs));

    f.sd_dev->online_flag = false;
    ASSERT_TRUE(f.flash.run_sync([&] { f.storage->refresh(); }, kWaitMs));
    f.player->on_card_removed();
    gate.give();

    ASSERT_TRUE(wait_until([&] { return f.st().status == PlayerStatus::Idle; }, kWaitMs));
    EXPECT_EQ(f.st().queue_length, 0u);
    wisp::os::sleep_ms(kSettleMs);
    auto e = f.errs();
    ASSERT_EQ(e.size(), 1u);
    EXPECT_EQ(e[0], (std::pair<std::string, std::string>{"card_removed", "music/long.wav"}));
    f.expect_linger_then_release();
}

TEST(MediaPlayer, RefusedRefillEndsTheItem) {
    Fixture f;
    f.put_scene_file("demo", "long.wav", make_wav(12));
    f.out->set_write_ms(1);
    f.make_player();
    f.player->play({"scene:demo/long.wav"});
    ASSERT_TRUE(wait_until([&] { return f.out->frames() > 0; }, kWaitMs));
    f.refuse_scene_io = true;
    ASSERT_TRUE(wait_until([&] { return f.st().status == PlayerStatus::Idle; }, kWaitMs));
    auto e = f.errs();
    ASSERT_EQ(e.size(), 1u);
    EXPECT_EQ(e[0], (std::pair<std::string, std::string>{"io_error", "scene:demo/long.wav"}));
    EXPECT_GT(f.refusals.load(), 1);
}

TEST(MediaPlayer, StopJoinsWithARefillRefused) {
    Fixture f;
    f.put_scene_file("demo", "long.wav", make_wav(12));
    f.out->set_write_ms(1);
    f.make_player();
    f.player->play({"scene:demo/long.wav"});
    ASSERT_TRUE(wait_until([&] { return f.out->frames() > 0; }, kWaitMs));
    f.refuse_scene_io = true;
    ASSERT_TRUE(wait_until([&] { return f.refusals.load() >= 2; }, kWaitMs));
    const uint64_t t0 = wisp::os::now_ms();
    f.player->stop();
    EXPECT_LT(wisp::os::now_ms() - t0, 300u);
    EXPECT_TRUE(f.locks_free());
    EXPECT_FALSE(f.out->is_open());
    EXPECT_FALSE(f.arbiter.held(AudioKind::Music));
}

TEST(MediaPlayer, StopJoinsWithARefillQueuedBehindABlockedWorker) {
    Fixture f;
    f.out->set_write_ms(1);
    f.make_player();
    wisp::os::Semaphore gate{1, 0}, entered{1, 0};
    f.player->play({"music/long.wav"});
    ASSERT_TRUE(wait_until([&] { return f.out->frames() > 0; }, kWaitMs));
    ASSERT_TRUE(f.sd_worker.submit([&] {
        entered.give();
        gate.take(wisp::os::kForever);
    }));
    ASSERT_TRUE(entered.take(kWaitMs));
    ASSERT_TRUE(wait_until([&] { return f.router->pending() >= 2; }, kWaitMs));
    const uint64_t t0 = wisp::os::now_ms();
    f.player->stop();
    EXPECT_LT(wisp::os::now_ms() - t0, 300u);
    EXPECT_TRUE(f.locks_free());
    gate.give();  // the queued refill then runs and does nothing
    EXPECT_TRUE(f.router->drain(kWaitMs));
}

TEST(MediaPlayer, DeletedCurrentFileSkips) {
    Fixture f;
    f.make_player();
    f.lib->set_on_removed([&](const std::string& p) { f.player->on_file_removed(p); });
    f.play_held({"music/long.wav", "music/b16.wav"});
    wisp::os::Semaphore removed{1, 0};
    f.lib->remove("music/long.wav", [&](Result<void> r) {
        EXPECT_TRUE(r);
        removed.give();
    });
    ASSERT_TRUE(removed.take(kWaitMs));
    f.out->open_gate();
    ASSERT_TRUE(
        wait_until([&] { return f.out->frames() == MediaPlayer::kChunkFrames + 16000u; }, kWaitMs));
    ASSERT_TRUE(f.idle_after_play());
    EXPECT_TRUE(f.errs().empty());
}

// A folder removed through the library takes the current item with it.
TEST(MediaPlayer, RemovedFolderOfTheCurrentFileSkips) {
    Fixture f;
    f.make_player();
    f.play_held({"music/sub/long.wav", "music/b16.wav"});
    f.player->on_file_removed("music/su");  // not its folder
    f.player->on_file_removed("music/sub");
    f.out->open_gate();
    ASSERT_TRUE(
        wait_until([&] { return f.out->frames() == MediaPlayer::kChunkFrames + 16000u; }, kWaitMs));
    ASSERT_TRUE(f.idle_after_play());
    EXPECT_TRUE(f.errs().empty());
}

// A preemption while the task waits for a refill (the card slow) closes
// the output at once, so speech can open it; a pause during the same wait
// takes effect, and playing on later resumes at the same sample.
TEST(MediaPlayer, PreemptDuringAnUnderrunClosesTheOutput) {
    Fixture f(/*real_arbiter_clock=*/true);
    f.out->set_write_ms(1);
    f.make_player();
    f.player->play({"music/long.wav"});
    ASSERT_TRUE(wait_until([&] { return f.out->frames() > 0; }, kWaitMs));
    wisp::os::Semaphore gate{1, 0}, entered{1, 0};
    ASSERT_TRUE(f.sd_worker.submit([&] {
        entered.give();
        gate.take(wisp::os::kForever);
    }));
    ASSERT_TRUE(entered.take(kWaitMs));
    // The ring runs dry behind the held worker: an underrun.
    ASSERT_TRUE(wait_until(
        [&] {
            const size_t n = f.out->frames();
            wisp::os::sleep_ms(30);
            return f.router->pending() >= 2 && f.out->frames() == n;
        },
        kWaitMs));
    EXPECT_TRUE(f.out->is_open());

    const uint64_t t0 = wisp::os::now_ms();
    AudioArbiter::Lease voice = f.arbiter.acquire(AudioKind::Voice);
    ASSERT_TRUE(voice.valid());
    EXPECT_LT(wisp::os::now_ms() - t0, 190u);  // released, not revoked
    // Speech opens the output as PcmPlayer would.
    EXPECT_TRUE(f.out->open(wisp::hal::AudioFormat{16000, 1}));
    EXPECT_EQ(f.out->overlapping_opens(), 0);
    f.out->close();
    EXPECT_EQ(f.st().pause_reason, PauseReason::System);
    const uint32_t kept = f.st().position_s;

    // The user pauses while the card is still held.
    f.player->pause();
    ASSERT_TRUE(wait_until([&] { return f.st().pause_reason == PauseReason::User; }, kWaitMs));
    voice.release();
    gate.give();
    const size_t before = f.out->frames();
    f.player->resume();
    ASSERT_TRUE(wait_until([&] { return f.out->frames() > before + 8000; }, kWaitMs));
    EXPECT_EQ(f.out->samples()[before], static_cast<int16_t>(100 * (kept + 1)));
    EXPECT_EQ(f.out->overlapping_opens(), 0);
}

// A pause while an item's file is being looked up takes effect before the
// lookup is done; resuming opens and plays it.
TEST(MediaPlayer, PauseWhileAnItemOpensTakesEffect) {
    Fixture f;
    f.make_player();
    wisp::os::Semaphore gate{1, 0}, entered{1, 0};
    ASSERT_TRUE(f.sd_worker.submit([&] {
        entered.give();
        gate.take(wisp::os::kForever);
    }));
    ASSERT_TRUE(entered.take(kWaitMs));
    f.player->play({"music/a44.wav"});
    ASSERT_TRUE(wait_until([&] { return f.router->pending() >= 2; }, kWaitMs));
    f.player->pause();
    ASSERT_TRUE(wait_until([&] { return f.st().pause_reason == PauseReason::User; }, kWaitMs));
    EXPECT_EQ(f.st().track.id, "music/a44.wav");
    EXPECT_EQ(f.out->opens(), 0);
    gate.give();
    f.player->resume();
    ASSERT_TRUE(wait_until([&] { return f.out->frames() == 11025u; }, kWaitMs));
    ASSERT_TRUE(f.idle_after_play());
    EXPECT_TRUE(f.errs().empty());
}

// A storage job dropped without running (the size lookup, a refill) is
// queued again instead of leaving the task waiting.
TEST(MediaPlayer, DroppedJobsAreQueuedAgain) {
    Fixture f;
    f.put_scene_file("demo", "a.wav", read_file(wisp::test::media_data("sine_16k_mono.wav")));
    f.make_player();
    f.drop_scene_io = 3;  // the lookup, then the first refills
    f.player->play({"scene:demo/a.wav"});
    ASSERT_TRUE(wait_until([&] { return f.out->frames() == 16000u; }, kWaitMs));
    ASSERT_TRUE(f.idle_after_play());
    EXPECT_EQ(f.drop_scene_io.load(), 0);
    EXPECT_TRUE(f.errs().empty());
}

// The resume notification still resumes when the command queue was too
// full to take its command.
TEST(MediaPlayer, ResumeNotificationSurvivesAFullQueue) {
    Fixture f;
    f.out->set_write_ms(5);
    f.make_player();
    f.play_held({"music/long.wav"});
    f.out->open_gate();
    AudioArbiter::Lease voice = f.arbiter.acquire(AudioKind::Voice);
    ASSERT_TRUE(wait_until([&] { return f.st().pause_reason == PauseReason::System; }, kWaitMs));
    voice.release();
    f.now += 800;

    f.hold_volume = true;
    const int calls = f.volume_calls.load();
    f.player->on_volume_changed();
    ASSERT_TRUE(wait_until([&] { return f.volume_calls.load() > calls; }, kWaitMs));
    for (int i = 0; i < 40; ++i) f.player->set_mode(PlayMode::Sequence);  // fills the queue
    f.arbiter.tick();
    f.hold_volume = false;
    ASSERT_TRUE(wait_until([&] { return f.st().status == PlayerStatus::Playing; }, kWaitMs));
}

TEST(MediaPlayer, EveryExitReleasesTheLocks) {
    {  // stop_playback()
        Fixture f;
        f.make_player();
        f.play_held({"music/long.wav"});
        f.player->stop_playback();
        f.out->open_gate();
        ASSERT_TRUE(wait_until([&] { return f.st().status == PlayerStatus::Idle; }, kWaitMs));
        EXPECT_EQ(f.st().queue_length, 0u);
        f.expect_linger_then_release();
    }
    {  // the end of the queue
        Fixture f;
        f.make_player();
        f.player->play({"music/a44.wav"});
        ASSERT_TRUE(f.idle_after_play());
        f.expect_linger_then_release();
    }
    {  // the card removed
        Fixture f;
        f.make_player();
        f.play_held({"music/long.wav"});
        f.player->on_card_removed();
        f.out->open_gate();
        ASSERT_TRUE(wait_until([&] { return f.st().status == PlayerStatus::Idle; }, kWaitMs));
        f.expect_linger_then_release();
    }
    {  // three failures, after something played
        Fixture f;
        f.make_player();
        f.player->play({"music/a44.wav", "music/float.wav", "music/garbage.mp3",
                        "music/missing.wav", "music/b16.wav"});
        ASSERT_TRUE(wait_until([&] { return f.errs().size() == 4u; }, kWaitMs));
        ASSERT_TRUE(wait_until([&] { return f.st().status == PlayerStatus::Idle; }, kWaitMs));
        f.expect_linger_then_release();
    }
    {  // stop() at shutdown: at once
        Fixture f;
        f.out->set_write_ms(5);
        f.make_player();
        f.player->play({"music/long.wav"});
        ASSERT_TRUE(wait_until([&] { return f.out->frames() > 0; }, kWaitMs));
        EXPECT_TRUE(f.locks_held());
        f.player->stop();
        EXPECT_TRUE(f.locks_free());
        EXPECT_FALSE(f.out->is_open());
        EXPECT_FALSE(f.arbiter.held(AudioKind::Music));
    }
    {  // a scene uninstalled with only its items queued
        Fixture f;
        f.put_scene_file("demo", "long.wav", make_wav(12));
        f.make_player();
        f.play_held({"scene:demo/long.wav"});
        f.player->forget_scene("demo");
        f.out->open_gate();
        ASSERT_TRUE(wait_until([&] { return f.st().status == PlayerStatus::Idle; }, kWaitMs));
        EXPECT_EQ(f.st().queue_length, 0u);
        f.expect_linger_then_release();
    }
}

TEST(MediaPlayer, VolumeFollowsTheSetting) {
    Fixture f;
    f.volume = 40;
    f.make_player();
    f.play_held({"music/long.wav"});
    EXPECT_EQ(f.out->last_volume(), 40);
    f.volume = 70;
    f.player->on_volume_changed();
    f.out->open_gate();
    EXPECT_TRUE(wait_until([&] { return f.out->last_volume() == 70; }, kWaitMs));
}

TEST(MediaPlayer, TaskConfig) {
    MediaPlayer::Config c;
    auto t = MediaPlayer::task_config(c);
    EXPECT_STREQ(t.name, "wisp.media");
    EXPECT_FALSE(t.flash_access);
    EXPECT_FALSE(t.hot_stack);
    EXPECT_EQ(t.stack_bytes, c.stack_bytes);
}
