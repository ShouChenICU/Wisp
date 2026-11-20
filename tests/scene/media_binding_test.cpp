// wisp.media, scene.on_media and wisp.http.download into the media library
// (media spec 7.1, 7.2) on a running SceneRuntime over a MediaLibrary on a
// temporary card, a MediaPlayer with a fake output and a fake HTTP client.

#include <gtest/gtest.h>

#include <atomic>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <thread>
#include <vector>

#include "support/fake_audio.hpp"
#include "support/fake_devices.hpp"
#include "support/fake_http.hpp"
#include "support/scene_runtime_fixture.hpp"
#include "support/wait.hpp"
#include "wisp/core/audio_arbiter.hpp"
#include "wisp/core/scene_files.hpp"
#include "wisp/core/storage_router.hpp"
#include "wisp/core/storage_worker.hpp"
#include "wisp/core/wakelock.hpp"
#include "wisp/media/library.hpp"
#include "wisp/media/player.hpp"
#include "wisp/net/scene_http.hpp"

namespace {

using wisp::media::PlayerStatus;
using wisp::test::wait_until;
namespace fs = std::filesystem;

constexpr uint32_t kWaitMs = 5000;
const char* kScreen = "return ui.screen { ui.label { text = \"x\" } }\n";

void append_le(std::string& s, uint32_t v, int bytes) {
    for (int i = 0; i < bytes; ++i) s.push_back(static_cast<char>((v >> (8 * i)) & 0xFF));
}

// A mono 16-bit PCM WAV of `seconds` at 8 kHz.
std::string make_wav(uint32_t seconds) {
    const uint32_t rate = 8000;
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
    s.append(data, '\0');
    return s;
}

std::string read_raw(const fs::path& path) {
    std::ifstream f(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

// scene.json with an http host and, unless empty, permissions.media.
std::string manifest_with(const std::string& id, const std::string& media) {
    std::string perms = R"("http":["api.example.com"])";
    if (!media.empty()) perms += R"(,"media":")" + media + "\"";
    return R"({"id":")" + id +
           R"(","name":"Demo","version":"1.0.0","schema_version":1,)"
           R"("permissions":{)" +
           perms + "}}";
}

class MediaBinding : public wisp::test::SceneRuntimeFixture {
protected:
    void SetUp() override {
        with_sd_card = true;
        SceneRuntimeFixture::SetUp();
        ASSERT_TRUE(sd_worker.start());
        http = std::make_unique<wisp::net::SceneHttp>(client);
        ASSERT_TRUE(http->start());
        wisp::core::SceneFilesConfig fc;
        fc.data_root = in + "/scenes";
        fc.submit_io = [this](wisp::core::Medium m, wisp::core::IoLane lane,
                              std::function<void()> job) {
            return router.submit(m, std::move(job), lane);
        };
        files = std::make_unique<wisp::core::SceneFiles>(*storage, fc);

        fs::create_directories(fs::path(root()) / "music");
        put("music/six.wav", make_wav(6));
        put("music/two.wav", make_wav(2));
        lib = std::make_unique<wisp::media::MediaLibrary>(wisp::media::MediaLibraryDeps{
            .storage = *storage, .router = &router, .bus = bus, .root = root()});
        lib->start();
        ASSERT_TRUE(wait_until(
            [this] {
                auto s = lib->state().get();
                return s.available && !s.scanning && s.track_count == 2;
            },
            kWaitMs));

        locks = std::make_unique<wisp::core::WakeLockRegistry>(bus);
        wisp::media::MediaPlayerDeps pd{
            .out = out,
            .arbiter = arbiter,
            .locks = *locks,
            .bus = bus,
            .library = lib.get(),
            .scene_files = files.get(),
            .router = &router,
            .decoders = wisp::media::with_wav({}),
            .volume = [] { return uint8_t{50}; },
            .random = [] { return 7u; },
        };
        player = std::make_unique<wisp::media::MediaPlayer>(std::move(pd));
        ASSERT_TRUE(player->start());

        configure_deps = [this](wisp::scene::SceneRuntimeDeps& d) {
            d.http = http.get();
            d.files = files.get();
            d.media_library = lib.get();
            d.media_player = player.get();
        };
    }

    void TearDown() override {
        out->open_gate();
        rt.reset();
        http.reset();
        player.reset();
        lib.reset();
        sd_worker.stop();
        SceneRuntimeFixture::TearDown();
        files.reset();
        locks.reset();
    }

    std::string root() const { return sd + "/media"; }

    void put(const std::string& rel, const std::string& bytes) {
        fs::create_directories((fs::path(root()) / rel).parent_path());
        std::ofstream f(fs::path(root()) / rel, std::ios::binary | std::ios::trunc);
        f << bytes;
    }

    void install_scene(const std::string& id, const std::string& media, const std::string& lua) {
        install(id, lua + kScreen, {{"scene.json", manifest_with(id, media)}});
    }

    // What the scene printed, without the proxy's "http: ..." lines.
    std::vector<std::string> printed(const std::string& id) {
        std::vector<std::string> out;
        for (auto& line : scene_log(id))
            if (line.rfind("http: ", 0) != 0) out.push_back(std::move(line));
        return out;
    }

    std::vector<std::string> wait_log(const std::string& id, size_t n) {
        EXPECT_TRUE(wait_until([&] { return printed(id).size() >= n; }, kWaitMs))
            << "logged: " << ::testing::PrintToString(scene_log(id));
        return printed(id);
    }

    wisp::media::PlayerState st() const { return player->state().get(); }

    wisp::core::StorageWorker sd_worker{wisp::core::StorageWorker::sd_io_config()};
    wisp::core::StorageRouter router{worker, &sd_worker};
    wisp::test::FakeHttpClient client;
    std::unique_ptr<wisp::net::SceneHttp> http;
    std::unique_ptr<wisp::core::SceneFiles> files;
    std::unique_ptr<wisp::media::MediaLibrary> lib;
    std::unique_ptr<wisp::core::WakeLockRegistry> locks;
    wisp::core::AudioArbiter arbiter;
    std::shared_ptr<wisp::test::audio::FakeAudioOut> out =
        std::make_shared<wisp::test::audio::FakeAudioOut>();
    std::unique_ptr<wisp::media::MediaPlayer> player;
};

using BindMedia = MediaBinding;
using BindHttp = MediaBinding;

}  // namespace

// Spec 7.1: controlling playback needs no permission.
TEST_F(BindMedia, PlaybackNeedsNoPermission) {
    start();
    out->close_gate();
    install_scene("p", "", R"(
wisp.media.play({"music/two.wav", "music/six.wav"}, {start = 2, mode = "repeat_one"},
  function(err, res)
    print("play", err, res.truncated)
    wisp.media.pause()
  end)
)");
    ASSERT_TRUE(rt->open("p"));
    EXPECT_EQ(wait_log("p", 1), (std::vector<std::string>{"play\tnil\tfalse"}));
    ASSERT_TRUE(wait_until(
        [this] {
            auto s = st();
            return s.status == PlayerStatus::Paused &&
                   s.pause_reason == wisp::media::PauseReason::User;
        },
        kWaitMs));
    EXPECT_EQ(st().track.id, "music/six.wav");
    EXPECT_EQ(st().index, 1u);
    EXPECT_EQ(st().mode, wisp::media::PlayMode::RepeatOne);
}

TEST_F(BindMedia, ReadNeedsReadOrWrite) {
    start();
    const std::string lua = R"(
wisp.media.tracks({}, function(err, page) print("tracks", err, page and page.total) end)
wisp.media.list("music", function(err, l) print("list", err, l and #l) end)
wisp.media.stat("music/two.wav", function(err, e) print("stat", err, e and e.size) end)
wisp.media.read("music/two.wav", 0, 4, function(err, d) print("read", err, d) end)
)";
    install_scene("none", "", lua);
    install_scene("reader", "read", lua);
    install_scene("writer", "write", lua);
    ASSERT_TRUE(rt->open("none"));
    EXPECT_EQ(
        wait_log("none", 4),
        (std::vector<std::string>{"tracks\tpermission_denied\tnil", "list\tpermission_denied\tnil",
                                  "stat\tpermission_denied\tnil", "read\tpermission_denied\tnil"}));
    const std::string size = std::to_string(make_wav(2).size());
    const std::vector<std::string> allowed = {"tracks\tnil\t2", "list\tnil\t2",
                                              "stat\tnil\t" + size, "read\tnil\tRIFF"};
    ASSERT_TRUE(rt->open("reader"));
    EXPECT_EQ(wait_log("reader", 4), allowed);
    ASSERT_TRUE(rt->open("writer"));
    EXPECT_EQ(wait_log("writer", 4), allowed);
}

TEST_F(BindMedia, WriteNeedsWrite) {
    start();
    const std::string lua = R"(
wisp.media.write("document/n.txt", "hi", function(err, ok)
  print("write", err, ok)
  wisp.media.mkdir("document/d", function(err, ok)
    print("mkdir", err, ok)
    wisp.media.remove("music/two.wav", function(err, ok) print("remove", err, ok) end)
  end)
end)
)";
    install_scene("reader", "read", lua);
    install_scene("writer", "write", lua);
    ASSERT_TRUE(rt->open("reader"));
    EXPECT_EQ(wait_log("reader", 3), (std::vector<std::string>{"write\tpermission_denied\tnil",
                                                               "mkdir\tpermission_denied\tnil",
                                                               "remove\tpermission_denied\tnil"}));
    EXPECT_FALSE(fs::exists(fs::path(root()) / "document/n.txt"));
    EXPECT_TRUE(fs::exists(fs::path(root()) / "music/two.wav"));

    ASSERT_TRUE(rt->open("writer"));
    EXPECT_EQ(
        wait_log("writer", 3),
        (std::vector<std::string>{"write\tnil\ttrue", "mkdir\tnil\ttrue", "remove\tnil\ttrue"}));
    EXPECT_EQ(read_raw(fs::path(root()) / "document/n.txt"), "hi");
    EXPECT_TRUE(fs::is_directory(fs::path(root()) / "document/d"));
    EXPECT_FALSE(fs::exists(fs::path(root()) / "music/two.wav"));
    // A bad path is the library's answer, through the callback.
    install_scene("bad", "write", R"(
wisp.media.write("../x.txt", "hi", function(err, ok) print("bad", err, ok) end)
wisp.media.write("loose.txt", "hi", function(err, ok) print("root", err, ok) end)
)");
    ASSERT_TRUE(rt->open("bad"));
    EXPECT_EQ(wait_log("bad", 2),
              (std::vector<std::string>{"bad\tinvalid_path\tnil", "root\tinvalid_path\tnil"}));
}

// {file = name} is the calling scene's own file; a string naming a scene
// item is refused, so no scene can play another's files.
TEST_F(BindMedia, FileItemResolvesToThisSceneOnly) {
    start();
    out->close_gate();
    const std::string wav = make_wav(2);
    wisp::Result<void> w = wisp::Result<void>::ok();
    ASSERT_TRUE(worker.run_sync(
        [&] {
            w = files->write("own", "a.wav",
                             {reinterpret_cast<const uint8_t*>(wav.data()), wav.size()});
        },
        kWaitMs));
    ASSERT_TRUE(w) << w.error().message;
    install_scene("own", "", R"(
wisp.media.play({"scene:other/a.wav"}, nil, function(err, res)
  print("other", err, res)
  wisp.media.play({{file = "../x.wav"}}, nil, function(err, res)
    print("bad", err, res)
    wisp.media.play({{file = "a.wav"}}, function(err, res) print("own", err, res.truncated) end, nil)
  end)
end)
)");
    ASSERT_TRUE(rt->open("own"));
    EXPECT_EQ(wait_log("own", 3),
              (std::vector<std::string>{"other\tinvalid_path\tnil", "bad\tinvalid_path\tnil",
                                        "own\tnil\tfalse"}));
    ASSERT_TRUE(wait_until([this] { return st().track.id == "scene:own/a.wav"; }, kWaitMs))
        << st().track.id;
}

// on_media runs on each change of the player's state; position-only
// changes come at most once per second of audio, so each carries a new
// position.
TEST_F(BindMedia, OnMediaFiresOnChange) {
    start();
    install_scene("m", "", R"(
function scene.on_media(s)
  print("media", s.status, s.id, s.position)
end
)");
    ASSERT_TRUE(rt->open("m"));
    ASSERT_TRUE(wait_until([&] { return rt->foreground() == "m"; }, kWaitMs));
    player->play({"music/six.wav"});
    ASSERT_TRUE(wait_until(
        [&] {
            auto l = printed("m");
            return !l.empty() && l.back().rfind("media\tidle", 0) == 0;
        },
        kWaitMs))
        << ::testing::PrintToString(printed("m"));
    const std::vector<std::string> lines = printed("m");
    bool played = false;
    for (size_t i = 0; i < lines.size(); ++i) {
        played = played || lines[i].rfind("media\tplaying\tmusic/six.wav", 0) == 0;
        if (i > 0) EXPECT_NE(lines[i], lines[i - 1]);
    }
    EXPECT_TRUE(played) << ::testing::PrintToString(lines);
    // Six seconds of audio: at most a start, six positions and the end.
    EXPECT_LE(lines.size(), 9u) << ::testing::PrintToString(lines);
}

TEST_F(BindMedia, StateTable) {
    start();
    out->close_gate();
    player->play({"music/two.wav", "music/six.wav"}, 1);
    ASSERT_TRUE(wait_until([this] { return st().status == PlayerStatus::Playing; }, kWaitMs));
    install_scene("s", "", R"(
local s = wisp.media.state()
print(s.status, s.pause_reason, s.id, s.title, s.artist, s.album, s.duration, s.position,
      s.index, s.length, s.mode)
)");
    ASSERT_TRUE(rt->open("s"));
    EXPECT_EQ(
        wait_log("s", 1),
        (std::vector<std::string>{"playing\tnone\tmusic/six.wav\tsix\t\t\t6\t0\t2\t2\tsequence"}));
}

// Spec 3.5: opts.media streams the body into the library, which indexes a
// track under music/.
TEST_F(BindHttp, DownloadIntoTheLibrary) {
    start();
    const std::string body = make_wav(3);
    client.next = {.status = 200, .body = body};
    const uint32_t revision = lib->state().get().revision;
    install_scene("d", "write", R"(
wisp.http.download("https://api.example.com/t.wav", "t.wav", {media = "music/dl/fresh.wav"},
  function(err, res)
    print("dl", err, res and res.status, res and res.size)
    wisp.media.tracks({q = "fresh"}, function(err, page) print("tracks", err, page.total) end)
  end)
)");
    ASSERT_TRUE(rt->open("d"));
    EXPECT_EQ(wait_log("d", 2),
              (std::vector<std::string>{"dl\tnil\t200\t" + std::to_string(body.size()),
                                        "tracks\tnil\t1"}));
    EXPECT_EQ(read_raw(fs::path(root()) / "music/dl/fresh.wav"), body);
    EXPECT_FALSE(fs::exists(fs::path(root()) / "music/dl/.fresh.wav.part"));
    ASSERT_TRUE(lib->track("music/dl/fresh.wav"));
    EXPECT_EQ(lib->track("music/dl/fresh.wav")->duration_s, 3u);
    EXPECT_GT(lib->state().get().revision, revision);
    // Not into the scene's own files.
    EXPECT_FALSE(fs::exists(fs::path(in) / "scenes" / "d" / "files" / "t.wav"));
}

TEST_F(BindHttp, DownloadIntoTheLibraryDeniedWithoutWrite) {
    start();
    client.next = {.status = 200, .body = "x"};
    const std::string lua = R"(
wisp.http.download("https://api.example.com/t.wav", "t.wav", {media = "music/t.wav"},
  function(err, res) print("dl", err, res) end)
)";
    install_scene("r", "read", lua);
    install_scene("n", "", lua);
    ASSERT_TRUE(rt->open("r"));
    EXPECT_EQ(wait_log("r", 1), (std::vector<std::string>{"dl\tpermission_denied\tnil"}));
    ASSERT_TRUE(rt->open("n"));
    EXPECT_EQ(wait_log("n", 1), (std::vector<std::string>{"dl\tpermission_denied\tnil"}));
    EXPECT_EQ(client.calls(), 0u);
    EXPECT_FALSE(fs::exists(fs::path(root()) / "music/t.wav"));
}

// The scene exits mid-download: what was written goes, nothing replaces
// the target, and the callback never runs in a scene that is gone.
TEST_F(BindHttp, ExitDuringALibraryDownloadLeavesNothing) {
    start();
    std::atomic<bool> release{false};
    client.next = {.status = 200, .body = std::string(256 * 1024, 'p')};
    client.before_piece = [&] {
        if (client.pieces.load() < 12) return;
        while (!release.load()) std::this_thread::sleep_for(std::chrono::milliseconds(1));
    };
    install_scene("d", "write", R"(
wisp.http.download("https://api.example.com/f", "f", {media = "download/f.bin"},
  function(err, res) print("late", err) end)
)");
    ASSERT_TRUE(rt->open("d"));
    ASSERT_TRUE(wait_until([&] { return client.pieces.load() >= 12; }, kWaitMs));
    ASSERT_TRUE(
        wait_until([&] { return fs::exists(fs::path(root()) / "download/.f.bin.part"); }, kWaitMs));
    ASSERT_TRUE(rt->exit("d"));
    release = true;
    EXPECT_TRUE(wait_until([&] { return http->idle(); }, kWaitMs));
    ASSERT_TRUE(router.drain(kWaitMs));
    EXPECT_FALSE(fs::exists(fs::path(root()) / "download/f.bin"));
    EXPECT_FALSE(fs::exists(fs::path(root()) / "download/.f.bin.part"));
    EXPECT_TRUE(printed("d").empty());
}
