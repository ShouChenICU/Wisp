// The media library and player wired into the App (media spec 2, 9): built
// from the card and the main speaker, the scene lifecycle and the volume
// setting followed, the media.* events published, and everything released
// by App::stop().

#include <gtest/gtest.h>

#include <algorithm>
#include <atomic>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <set>
#include <string>
#include <vector>

#include "storage_io.hpp"
#include "support/cell_barrier.hpp"
#include "support/fake_audio.hpp"
#include "support/fake_http.hpp"
#include "support/lvgl_probe.hpp"
#include "support/media_data.hpp"
#include "support/wait.hpp"
#include "ui_net_harness.hpp"
#include "wisp/base/json.hpp"
#include "wisp/core/log.hpp"
#include "wisp/core/topics.hpp"
#include "wisp/core/wakelock.hpp"
#include "wisp/media/library.hpp"
#include "wisp/media/player.hpp"
#include "wisp/net/pairing_service.hpp"
#include "wisp/os/os.hpp"
#include "wisp/port/sim/minimp3_decoder.hpp"

using wisp::media::MediaPlayer;
using wisp::media::PlayerState;
using wisp::media::PlayerStatus;
using wisp::net::Method;
using wisp::test::find_button_with_label;
using wisp::test::make_req;
using wisp::test::run_on_ui_thread;
using wisp::test::UiNetHarness;
using wisp::test::UiNetOptions;
using wisp::test::wait_until;
using wisp::test::with_csrf;
using wisp::test::with_token;

namespace {

namespace fs = std::filesystem;
namespace topics = wisp::core::topics;

constexpr uint32_t kWaitMs = 5000;

void append_le(std::string& s, uint32_t v, int bytes) {
    for (int i = 0; i < bytes; ++i) s.push_back(static_cast<char>((v >> (8 * i)) & 0xff));
}

// A mono 16-bit WAV of `seconds` of a quiet constant level.
std::string make_wav(uint32_t seconds, uint32_t rate = 8000) {
    const uint32_t data = seconds * rate * 2;
    std::string s = "RIFF";
    append_le(s, 36 + data, 4);
    s += "WAVEfmt ";
    append_le(s, 16, 4);
    append_le(s, 1, 2);
    append_le(s, 1, 2);
    append_le(s, rate, 4);
    append_le(s, rate * 2, 4);
    append_le(s, 2, 2);
    append_le(s, 16, 2);
    s += "data";
    append_le(s, data, 4);
    for (uint32_t i = 0; i < seconds * rate; ++i) append_le(s, 100, 2);
    return s;
}

void put(const fs::path& p, const std::string& bytes) {
    fs::create_directories(p.parent_path());
    std::ofstream(p, std::ios::binary) << bytes;
}

// A scene package in <int>/scenes/<id> whose private files live on internal
// storage, with `files` in them.
void put_scene(const std::string& root, const std::string& id,
               const std::vector<std::pair<std::string, std::string>>& files) {
    const fs::path dir = fs::path(root) / "scenes" / id;
    put(dir / "scene.json",
        R"({"id":")" + id + R"(","name":")" + id + R"(","version":"1.0.0","schema_version":1})");
    put(dir / "main.lua", "return ui.screen { ui.label { text = 'x' } }");
    put(dir / "data" / "files_loc.json", R"({"location":"int"})");
    for (const auto& [name, bytes] : files) put(dir / "files" / name, bytes);
}

struct MediaApp {
    explicit MediaApp(bool with_sd, bool with_audio = true, bool with_server = false) {
        wisp::log::reset_for_test();
        wisp::log::init();
        opts.with_sd = with_sd;
        opts.with_server = with_server;
        if (with_audio) opts.audio_out = out;
        opts.audio_decoder = [](wisp::media::Format f) -> std::unique_ptr<wisp::media::Decoder> {
            if (f == wisp::media::Format::Mp3) return wisp::port::sim::make_minimp3_decoder();
            return nullptr;
        };
        h = std::make_unique<UiNetHarness>(opts);
    }
    ~MediaApp() {
        // The test may hold the output's gate; stop() must not wait on it.
        out->open_gate();
        h.reset();
        wisp::log::reset_for_test();
    }

    fs::path sd() const { return fs::path(h->root) / "sd"; }

    bool start() { return h->start("media."); }

    wisp::media::MediaLibrary* lib() { return h->app->media_library(); }
    MediaPlayer* player() { return h->app->media_player(); }
    PlayerState st() { return player()->state().get(); }

    bool library_ready(uint32_t tracks) {
        return wait_until(
            [&] {
                auto s = lib()->state().get();
                return s.available && !s.scanning && s.track_count == tracks;
            },
            kWaitMs);
    }

    // Plays `items` with the output's gate closed, so the first chunk waits
    // in write() until open_gate().
    void play_held(std::vector<std::string> items) {
        out->close_gate();
        player()->play(std::move(items));
        ASSERT_TRUE(wait_until([&] { return out->writer_waiting(); }, kWaitMs));
    }

    bool media_locks_held() {
        for (const auto& l : h->app->wakelocks().list()) {
            if (l.holder == wisp::core::kMediaWakeLockHolder) return true;
        }
        return false;
    }

    UiNetOptions opts;
    std::shared_ptr<wisp::test::audio::FakeAudioOut> out =
        std::make_shared<wisp::test::audio::FakeAudioOut>();
    std::unique_ptr<UiNetHarness> h;
};

std::string pair_browser(UiNetHarness& h) {
    wisp::net::PairingService* pairing = h.app->pairing();
    EXPECT_TRUE(pairing->request_code(wisp::os::now_ms()));
    auto code = pairing->active_code(wisp::os::now_ms()).value_or("");
    auto token = pairing->confirm(code, wisp::net::ClientInfo{}, wisp::os::now_ms());
    EXPECT_TRUE(token);
    return token ? token.value() : std::string();
}

// DELETE /scenes/<id>, then "yes" on the device's confirm dialog.
void confirm_delete(UiNetHarness& h, const std::string& token, const std::string& id) {
    wisp::net::HttpRequest req =
        with_csrf(with_token(make_req(Method::Delete, "/api/v1/scenes/" + id), token));
    auto res = h.server->handle(req);
    ASSERT_EQ(res.status, 202) << res.body;
    ASSERT_EQ(run_on_ui_thread(h.app->ui(),
                               [] {
                                   lv_obj_t* yes = find_button_with_label(lv_layer_top(), "是");
                                   if (yes == nullptr) return "";
                                   lv_obj_send_event(yes, LV_EVENT_CLICKED, nullptr);
                                   return "ok";
                               }),
              "ok");
}

std::string str(const wisp::core::Event& e, const char* key) {
    auto it = e.payload.find(key);
    return it != e.payload.end() && it->second.type() == wisp::Value::Type::String
               ? it->second.as_string()
               : std::string();
}

}  // namespace

// With a card and a speaker both are built: the library indexes the card's
// music and the player decodes MP3 through the platform's decoder.
TEST(MediaWiring, BuildsWithCardAndAudio) {
    MediaApp m(/*with_sd=*/true);
    put(m.sd() / "media" / "music" / "a.mp3", [] {
        std::ifstream f(wisp::test::media_data("sine_44k_stereo_cbr128.mp3"), std::ios::binary);
        return std::string(std::istreambuf_iterator<char>(f), {});
    }());
    put(m.sd() / "media" / "music" / "b.wav", make_wav(1));
    ASSERT_TRUE(m.start());
    ASSERT_NE(m.lib(), nullptr);
    ASSERT_NE(m.player(), nullptr);
    ASSERT_TRUE(m.library_ready(2));
    for (const char* dir : {"music", "download", "document"}) {
        EXPECT_TRUE(fs::is_directory(m.sd() / "media" / dir)) << dir;
    }

    m.player()->play({"music/a.mp3"});
    ASSERT_TRUE(wait_until([&] { return m.out->frames() > 0; }, kWaitMs));
    ASSERT_TRUE(wait_until([&] { return m.st().status == PlayerStatus::Idle; }, kWaitMs));
    ASSERT_FALSE(m.out->formats().empty());
    EXPECT_EQ(m.out->formats()[0].sample_rate, 44100u);
    EXPECT_EQ(m.out->formats()[0].channels, 1u);
    EXPECT_TRUE(m.h->topic(topics::kMediaError).empty());
}

// Without a card there is no library, but a scene's private files on
// internal storage still play.
TEST(MediaWiring, NoCardStillHasAPlayer) {
    MediaApp m(/*with_sd=*/false);
    put_scene(m.h->root, "demo", {{"a.wav", make_wav(1)}});
    ASSERT_TRUE(m.start());
    EXPECT_EQ(m.lib(), nullptr);
    ASSERT_NE(m.player(), nullptr);
    m.player()->play({"scene:demo/a.wav"});
    ASSERT_TRUE(wait_until([&] { return m.out->frames() == 8000u; }, kWaitMs));
    EXPECT_TRUE(wait_until([&] { return m.st().status == PlayerStatus::Idle; }, kWaitMs));
}

// Without a main speaker there is no player; the library is still there.
TEST(MediaWiring, NoAudioOutHasNoPlayer) {
    MediaApp m(/*with_sd=*/true, /*with_audio=*/false);
    ASSERT_TRUE(m.start());
    EXPECT_EQ(m.player(), nullptr);
    ASSERT_NE(m.lib(), nullptr);
    EXPECT_TRUE(m.library_ready(0));
}

// A deleted scene's items leave the queue: the one playing stops after its
// current chunk.
TEST(MediaWiring, DeletedSceneLeavesTheQueue) {
    MediaApp m(/*with_sd=*/false, /*with_audio=*/true, /*with_server=*/true);
    put_scene(m.h->root, "demo", {{"long.wav", make_wav(10)}});
    ASSERT_TRUE(m.start());
    const std::string token = pair_browser(*m.h);
    ASSERT_NO_FATAL_FAILURE(m.play_held({"scene:demo/long.wav"}));
    EXPECT_EQ(m.st().status, PlayerStatus::Playing);
    ASSERT_NO_FATAL_FAILURE(confirm_delete(*m.h, token, "demo"));
    // The scene's data stays (a delete without its data); its package goes.
    ASSERT_TRUE(wait_until(
        [&] { return !fs::exists(fs::path(m.h->root) / "scenes" / "demo" / "scene.json"); },
        kWaitMs));
    // The delete's job has pushed its Deleted edge once it is done; two
    // barriers on the bus then cover the inbox's run (as in
    // app_scenes_test.cpp), which queues the forget on the player. The
    // held chunk then lets the task take it.
    ASSERT_TRUE(m.h->app->storage_worker()->drain(kWaitMs));
    ASSERT_TRUE(wisp::test::slot_barrier(m.h->app->bus(), &m.h->app->bus()));
    ASSERT_TRUE(wisp::test::slot_barrier(m.h->app->bus(), &m.h->app->bus()));
    m.out->open_gate();
    ASSERT_TRUE(wait_until(
        [&] {
            PlayerState s = m.st();
            return s.status == PlayerStatus::Idle && s.queue_length == 0;
        },
        kWaitMs));
    // One chunk, not the whole ten seconds.
    EXPECT_EQ(m.out->frames(), MediaPlayer::kChunkFrames);
}

// media.player follows the cell's changes but not its position ticks;
// media.library its changes; media.error each failed item.
TEST(MediaWiring, PublishesPlayerEventsNotPerPositionTick) {
    MediaApp m(/*with_sd=*/true);
    put(m.sd() / "media" / "music" / "five.wav", make_wav(5));
    ASSERT_TRUE(m.start());
    ASSERT_TRUE(m.library_ready(1));

    m.player()->play({"music/five.wav"});
    ASSERT_TRUE(wait_until([&] { return m.out->frames() == 5 * 8000u; }, kWaitMs));
    ASSERT_TRUE(wait_until([&] { return m.st().status == PlayerStatus::Idle; }, kWaitMs));
    std::vector<wisp::core::Event> ev;
    ASSERT_TRUE(wait_until(
        [&] {
            ev = m.h->topic(topics::kMediaPlayer);
            return !ev.empty() && str(ev.back(), "status") == "idle";
        },
        kWaitMs));
    // Five whole seconds went by in the cell; none of them is an event, so
    // no two events in a row carry the same payload.
    bool played = false;
    for (size_t i = 0; i < ev.size(); ++i) {
        std::set<std::string> keys;
        for (const auto& [k, v] : ev[i].payload) keys.insert(k);
        EXPECT_EQ(keys,
                  (std::set<std::string>{"status", "pause_reason", "track_id", "index", "mode"}));
        if (str(ev[i], "status") == "playing" && str(ev[i], "track_id") == "music/five.wav") {
            played = true;
            EXPECT_EQ(str(ev[i], "mode"), "sequence");
            EXPECT_EQ(str(ev[i], "pause_reason"), "none");
        }
        if (i > 0) EXPECT_NE(ev[i].payload, ev[i - 1].payload) << i;
    }
    EXPECT_TRUE(played);
    EXPECT_LE(ev.size(), 4u);

    // A missing file is skipped with media.error.
    m.player()->play({"music/missing.mp3"});
    std::vector<wisp::core::Event> errs;
    ASSERT_TRUE(wait_until(
        [&] {
            errs = m.h->topic(topics::kMediaError);
            return !errs.empty();
        },
        kWaitMs));
    EXPECT_EQ(str(errs[0], "reason"), "not_found");
    EXPECT_EQ(str(errs[0], "track_id"), "music/missing.mp3");

    // A track written through the library moves its cell.
    wisp::os::LargeBuffer data = wisp::os::LargeBuffer::allocate(make_wav(1).size());
    ASSERT_TRUE(data.ok());
    const std::string bytes = make_wav(1);
    std::copy(bytes.begin(), bytes.end(), data.data());
    std::atomic<bool> written{false};
    m.lib()->write("music/one.wav", std::move(data), [&](wisp::Result<void> r) {
        EXPECT_TRUE(r);
        written = true;
    });
    ASSERT_TRUE(wait_until([&] { return written.load(); }, kWaitMs));
    ASSERT_TRUE(wait_until(
        [&] {
            for (const auto& e : m.h->topic(topics::kMediaLibrary)) {
                auto it = e.payload.find("track_count");
                if (it != e.payload.end() && it->second == wisp::Value(int64_t{2})) {
                    return e.payload.at("available") == wisp::Value(true) &&
                           e.payload.count("scanning") == 1;
                }
            }
            return false;
        },
        kWaitMs));
}

// The card taken out while a track plays ends the session at once, through
// App's card follower: the held chunk is the last one (the reader's ring
// would otherwise play on for seconds), media.error says card_removed, and
// the wake locks go after their linger (media spec 6: 10 s).
TEST(MediaWiring, CardRemovedStopsThePlayer) {
    MediaApp m(/*with_sd=*/true);
    put(m.sd() / "media" / "music" / "long.wav", make_wav(10));
    ASSERT_TRUE(m.start());
    ASSERT_TRUE(m.library_ready(1));
    ASSERT_NO_FATAL_FAILURE(m.play_held({"music/long.wav"}));
    ASSERT_TRUE(m.media_locks_held());

    m.h->sd->online_flag = false;
    ASSERT_TRUE(wait_until([&] { return wisp::app::queue_storage_refresh(*m.h->app); }, kWaitMs));
    ASSERT_TRUE(wait_until([&] { return !m.h->app->storage().sd_cell().get().online; }, kWaitMs));
    // The follower's run (on wisp.bus) has queued the stop on the player.
    ASSERT_TRUE(wisp::test::slot_barrier(m.h->app->bus(), &m.h->app->bus()));
    m.out->open_gate();
    ASSERT_TRUE(wait_until(
        [&] {
            PlayerState s = m.st();
            return s.status == PlayerStatus::Idle && s.queue_length == 0;
        },
        kWaitMs));
    EXPECT_EQ(m.out->frames(), MediaPlayer::kChunkFrames);
    EXPECT_FALSE(m.out->is_open());
    std::vector<wisp::core::Event> errs;
    ASSERT_TRUE(wait_until(
        [&] {
            errs = m.h->topic(topics::kMediaError);
            return !errs.empty();
        },
        kWaitMs));
    EXPECT_EQ(str(errs[0], "reason"), "card_removed");
    EXPECT_EQ(str(errs[0], "track_id"), "music/long.wav");
    EXPECT_TRUE(wait_until([&] { return !m.media_locks_held(); }, 12000));
}

namespace {

// Removes `path` through the library and waits for its answer.
void remove_through_library(MediaApp& m, const std::string& path) {
    std::atomic<bool> done{false};
    m.lib()->remove(path, [&](wisp::Result<void> r) {
        EXPECT_TRUE(r) << path << ": " << (r ? "" : r.error().message);
        done = true;
    });
    ASSERT_TRUE(wait_until([&] { return done.load(); }, kWaitMs));
}

// The queue ["<current>", "music/b.wav"] played after `current` was removed
// through the library while its first chunk was held: the player skips to
// b.wav at once (the reader's ring still held seconds of the removed file),
// with no media.error (the skip is not a failed read).
void expect_skipped(MediaApp& m, const std::string& current) {
    m.out->open_gate();
    ASSERT_TRUE(wait_until([&] { return m.st().status == PlayerStatus::Idle; }, kWaitMs));
    EXPECT_EQ(m.out->frames(), MediaPlayer::kChunkFrames + 8000u) << current;
    EXPECT_TRUE(m.h->topic(topics::kMediaError).empty());
}

}  // namespace

// The track playing removed through the library: the player skips it.
TEST(MediaWiring, RemovedLibraryTrackIsSkipped) {
    MediaApp m(/*with_sd=*/true);
    put(m.sd() / "media" / "music" / "long.wav", make_wav(10));
    put(m.sd() / "media" / "music" / "b.wav", make_wav(1));
    ASSERT_TRUE(m.start());
    ASSERT_TRUE(m.library_ready(2));
    ASSERT_NO_FATAL_FAILURE(m.play_held({"music/long.wav", "music/b.wav"}));
    ASSERT_NO_FATAL_FAILURE(remove_through_library(m, "music/long.wav"));
    ASSERT_NO_FATAL_FAILURE(expect_skipped(m, "music/long.wav"));
}

// The folder of the track playing removed through the library (emptied
// behind its back first, as a folder must be empty to go): the player
// skips the track inside it.
TEST(MediaWiring, RemovedLibraryFolderSkipsItsTrack) {
    MediaApp m(/*with_sd=*/true);
    put(m.sd() / "media" / "music" / "sub" / "long.wav", make_wav(10));
    put(m.sd() / "media" / "music" / "b.wav", make_wav(1));
    ASSERT_TRUE(m.start());
    ASSERT_TRUE(m.library_ready(2));
    ASSERT_NO_FATAL_FAILURE(m.play_held({"music/sub/long.wav", "music/b.wav"}));
    fs::remove(m.sd() / "media" / "music" / "sub" / "long.wav");
    ASSERT_NO_FATAL_FAILURE(remove_through_library(m, "music/sub"));
    ASSERT_NO_FATAL_FAILURE(expect_skipped(m, "music/sub/long.wav"));
}

// The volume setting reaches the output while it plays.
TEST(MediaWiring, VolumeSettingReachesThePlayer) {
    MediaApp m(/*with_sd=*/false);
    put_scene(m.h->root, "demo", {{"long.wav", make_wav(10)}});
    ASSERT_TRUE(m.start());
    ASSERT_NO_FATAL_FAILURE(m.play_held({"scene:demo/long.wav"}));
    ASSERT_TRUE(m.h->app->system_settings().set({{"volume", int64_t{23}}}));
    ASSERT_TRUE(m.h->app->bus().wait_idle(kWaitMs));
    m.out->open_gate();
    EXPECT_TRUE(wait_until([&] { return m.out->last_volume() == 23; }, kWaitMs));
}

// App::stop() while music plays: the output closed, the wake locks gone,
// and no subscription left on the bus or a cell (~EventBus and ~StateCell
// report one as misuse, which aborts here).
TEST(MediaWiring, StopReleasesEverything) {
    MediaApp m(/*with_sd=*/true);
    put(m.sd() / "media" / "music" / "long.wav", make_wav(10));
    ASSERT_TRUE(m.start());
    ASSERT_TRUE(m.library_ready(1));
    m.out->set_write_ms(5);
    m.player()->play({"music/long.wav"});
    ASSERT_TRUE(wait_until([&] { return m.out->frames() > 0; }, kWaitMs));
    ASSERT_TRUE(m.media_locks_held());
    m.h->app->stop();
    EXPECT_FALSE(m.out->is_open());
    EXPECT_FALSE(m.media_locks_held());
    EXPECT_EQ(m.h->app->media_player(), nullptr);
    EXPECT_EQ(m.h->app->media_library(), nullptr);
    // A second stop() (as ~App does) finds nothing left to do.
    m.h->app->stop();
}
