// /media/* (media spec 3.7): the library's files, folders, tracks and
// rescan, and the player, over a real MediaLibrary on a temporary card
// (worked by a real wisp.sd.io through a StorageRouter, as in the app) and
// a MediaPlayer with a fake output. Uploads go through the server's
// StreamHandler the way a server feeds one; downloads are pulled with
// net::pump_stream().

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <memory>
#include <optional>
#include <span>
#include <string>
#include <vector>

#include "admin/admin_fixture.hpp"
#include "support/fake_audio.hpp"
#include "support/fake_devices.hpp"
#include "support/fake_http.hpp"
#include "support/media_data.hpp"
#include "support/temp_dir.hpp"
#include "support/wait.hpp"
#include "wisp/core/audio_arbiter.hpp"
#include "wisp/core/storage_router.hpp"
#include "wisp/core/storage_worker.hpp"
#include "wisp/core/wakelock.hpp"
#include "wisp/media/decoder.hpp"
#include "wisp/media/library.hpp"
#include "wisp/media/player.hpp"
#include "wisp/net/http.hpp"

namespace {

using wisp::Value;
using wisp::media::PlayerStatus;
using wisp::net::HttpRequest;
using wisp::net::HttpResponse;
using wisp::net::Method;
using wisp::test::AdminFixture;
using wisp::test::body_json;
using wisp::test::error_code;
using wisp::test::make_req;
using wisp::test::response_header;
using wisp::test::wait_until;
using wisp::test::with_csrf;
using wisp::test::with_token;
namespace fs = std::filesystem;

constexpr uint32_t kWaitMs = 5000;

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

std::string pattern(size_t size) {
    std::string s(size, '\0');
    for (size_t i = 0; i < size; ++i) s[i] = static_cast<char>((i * 31 + 7) & 0xff);
    return s;
}

std::string read_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

std::span<const uint8_t> as_bytes(const std::string& s) {
    return {reinterpret_cast<const uint8_t*>(s.data()), s.size()};
}

const Value& at(const Value& v, const char* key) {
    static const Value null;
    const Value* found = v.type() == Value::Type::Map ? v.find(key) : nullptr;
    EXPECT_NE(found, nullptr) << "missing \"" << key << "\" in " << wisp::to_json(v);
    return found != nullptr ? *found : null;
}

// The whole streamed body of `res`, pulled the way a server does.
std::string drain(const HttpResponse& res, size_t* pieces = nullptr) {
    if (!res.stream) return wisp::test::body_text(res);
    std::vector<uint8_t> buf(wisp::net::kStreamChunkBytes);
    std::string out;
    size_t n = 0;
    const bool ok = wisp::net::pump_stream(res, buf, [&](std::span<const uint8_t> piece) {
        out.append(reinterpret_cast<const char*>(piece.data()), piece.size());
        ++n;
        return true;
    });
    EXPECT_TRUE(ok);
    if (pieces != nullptr) *pieces = n;
    return out;
}

struct MediaFixture {
    struct Options {
        bool card = true;
        bool with_library = true;
        bool with_player = true;
    };

    explicit MediaFixture(Options o)
        : sd_dir(wisp::test::temp_dir().string()),
          sd_dev(std::make_shared<wisp::test::FakeStorage>("storage.sd", sd_dir, true)),
          f((sd_dev->online_flag = o.card, true), sd_dev) {
        // Pairing writes on this thread: before a flash thread exists.
        token = f.token();
        EXPECT_TRUE(flash.start());
        EXPECT_TRUE(sd_worker.start());
        router = std::make_unique<wisp::core::StorageRouter>(flash, &sd_worker);
        put("music/six.wav", make_wav(6));
        put("music/two.wav", make_wav(2));
        put("document/notes.txt", "hello");
        if (o.with_library) {
            lib = std::make_unique<wisp::media::MediaLibrary>(wisp::media::MediaLibraryDeps{
                .storage = *f.storage, .router = router.get(), .bus = f.bus, .root = root()});
            lib->start();
            if (o.card) {
                EXPECT_TRUE(wait_until(
                    [this] {
                        auto s = lib->state().get();
                        return s.available && !s.scanning && s.track_count == 2;
                    },
                    kWaitMs));
            }
        }
        if (o.with_player) {
            player = std::make_unique<wisp::media::MediaPlayer>(wisp::media::MediaPlayerDeps{
                .out = out,
                .arbiter = arbiter,
                .locks = f.wakelocks,
                .bus = f.bus,
                .library = lib.get(),
                .scene_files = nullptr,
                .router = router.get(),
                .decoders = wisp::media::with_wav({}),
                .volume = [] { return uint8_t{50}; },
                .random = [] { return 7u; },
            });
            EXPECT_TRUE(player->start());
        }
        f.srv.reset();
        wisp::admin::AdminDeps deps = f.make_deps();
        deps.media_library = lib.get();
        deps.media_player = player.get();
        f.srv = std::make_unique<wisp::admin::AdminServer>(std::move(deps));
        stream = f.srv->stream_handler();
    }
    MediaFixture() : MediaFixture(Options{}) {}

    ~MediaFixture() {
        out->open_gate();
        f.srv.reset();
        player.reset();
        lib.reset();
        router.reset();
        sd_worker.stop();
        flash.stop();
    }

    std::string root() const { return sd_dir + "/media"; }

    void put(const std::string& rel, const std::string& bytes) {
        fs::create_directories((fs::path(root()) / rel).parent_path());
        std::ofstream file(fs::path(root()) / rel, std::ios::binary | std::ios::trunc);
        file << bytes;
    }

    HttpRequest authed(Method m, const std::string& path, std::string body = {}) const {
        HttpRequest req = with_token(make_req(m, path, std::move(body)), token);
        if (m != Method::Get) req = with_csrf(std::move(req));
        return req;
    }

    HttpResponse call(Method m, const std::string& path, const std::string& query = {},
                      std::string body = {}) {
        HttpRequest req = authed(m, path, std::move(body));
        req.query = query;
        return f.srv->handle(req);
    }

    HttpRequest upload_head(const std::string& rel, uint64_t length) const {
        HttpRequest req = authed(Method::Put, "/api/v1/media/files/" + rel);
        req.headers["content-type"] = "application/octet-stream";
        req.content_length = length;
        return req;
    }

    // Feeds `body` the way a server does: 4 KiB pieces until a response.
    HttpResponse send(const HttpRequest& head, std::span<const uint8_t> body) {
        std::optional<wisp::net::StreamStart> start = stream(head);
        EXPECT_TRUE(start.has_value()) << "upload was not streamed";
        if (!start) return {};
        if (!start->sink) return start->response;
        while (!body.empty()) {
            const size_t n = std::min(body.size(), wisp::net::kBodyChunkBytes);
            if (auto early = start->sink->on_data(body.first(n))) return *early;
            body = body.subspan(n);
        }
        return start->sink->on_end();
    }

    HttpResponse upload(const std::string& rel, const std::string& bytes) {
        return send(upload_head(rel, bytes.size()), as_bytes(bytes));
    }

    // Every entry under `rel` of the media root on disk (hidden ones too).
    std::vector<std::string> on_disk(const std::string& rel) const {
        std::vector<std::string> names;
        std::error_code ec;
        for (const auto& e : fs::directory_iterator(fs::path(root()) / rel, ec))
            names.push_back(e.path().filename().string());
        std::sort(names.begin(), names.end());
        return names;
    }

    std::vector<std::string> names(const HttpResponse& listed) {
        std::vector<std::string> out;
        const Value body = body_json(listed);
        const Value& entries = at(body, "entries");
        if (entries.type() != Value::Type::List) return out;
        for (const Value& e : entries.as_list()) out.push_back(at(e, "name").as_string());
        return out;
    }

    std::string sd_dir;
    std::shared_ptr<wisp::test::FakeStorage> sd_dev;
    AdminFixture f;
    wisp::core::StorageWorker flash;
    wisp::core::StorageWorker sd_worker{wisp::core::StorageWorker::sd_io_config()};
    std::unique_ptr<wisp::core::StorageRouter> router;
    std::unique_ptr<wisp::media::MediaLibrary> lib;
    wisp::core::AudioArbiter arbiter;
    std::shared_ptr<wisp::test::audio::FakeAudioOut> out =
        std::make_shared<wisp::test::audio::FakeAudioOut>();
    std::unique_ptr<wisp::media::MediaPlayer> player;
    wisp::net::StreamHandler stream;
    std::string token;
};

}  // namespace

TEST(RoutesMedia, ListRootAndFolder) {
    MediaFixture fx;
    HttpResponse rootl = fx.call(Method::Get, "/api/v1/media/files");
    ASSERT_EQ(rootl.status, 200) << wisp::test::body_text(rootl);
    EXPECT_EQ(fx.names(rootl), (std::vector<std::string>{"document", "download", "music"}));
    const Value r = body_json(rootl);
    EXPECT_EQ(at(r, "dir"), Value(""));
    EXPECT_EQ(at(at(r, "entries").as_list()[0], "dir"), Value(true));

    fx.put("music/Artist/Song 1.wav", make_wav(1));
    HttpResponse music = fx.call(Method::Get, "/api/v1/media/files", "dir=music");
    ASSERT_EQ(music.status, 200);
    EXPECT_EQ(fx.names(music), (std::vector<std::string>{"Artist", "six.wav", "two.wav"}));
    const Value m = body_json(music);
    const Value& six = at(m, "entries").as_list()[1];
    EXPECT_EQ(at(six, "dir"), Value(false));
    EXPECT_EQ(at(six, "size"), Value(static_cast<int64_t>(make_wav(6).size())));
    EXPECT_GT(at(six, "mtime").as_int(), 0);

    // The query value is form-encoded: "%2F" and "+" decode.
    HttpResponse nested = fx.call(Method::Get, "/api/v1/media/files", "dir=music%2FArtist");
    ASSERT_EQ(nested.status, 200);
    EXPECT_EQ(fx.names(nested), (std::vector<std::string>{"Song 1.wav"}));

    HttpResponse missing = fx.call(Method::Get, "/api/v1/media/files", "dir=music/none");
    EXPECT_EQ(missing.status, 404);
    EXPECT_EQ(error_code(missing), "not_found");
}

TEST(RoutesMedia, DownloadStreams) {
    MediaFixture fx;
    const std::string big = pattern(100 * 1024);
    fx.put("download/Big File ü.bin", big);
    HttpResponse dl = fx.call(Method::Get, "/api/v1/media/files/download/Big%20File%20%C3%BC.bin");
    ASSERT_EQ(dl.status, 200) << wisp::test::body_text(dl);
    EXPECT_EQ(dl.content_type, "application/octet-stream");
    EXPECT_EQ(response_header(dl, "content-disposition"),
              "attachment; filename=\"Big File _.bin\"; filename*=UTF-8''Big%20File%20%C3%BC.bin");
    EXPECT_EQ(response_header(dl, "cache-control"), "no-store");
    ASSERT_TRUE(dl.stream);
    EXPECT_EQ(dl.stream_length, big.size());
    // The network lock is held while the download is being sent.
    EXPECT_TRUE(fx.f.wakelocks.held(wisp::core::WakeLockKind::Network));
    size_t pieces = 0;
    EXPECT_TRUE(drain(dl, &pieces) == big);
    EXPECT_EQ(pieces, 4u);  // 32 KiB per read
    dl = HttpResponse();
    EXPECT_FALSE(fx.f.wakelocks.held(wisp::core::WakeLockKind::Network));

    HttpResponse wav = fx.call(Method::Get, "/api/v1/media/files/music/two.wav");
    ASSERT_EQ(wav.status, 200);
    EXPECT_EQ(wav.content_type, "audio/wav");
    EXPECT_TRUE(drain(wav) == make_wav(2));

    fx.put("document/empty.txt", "");
    HttpResponse empty = fx.call(Method::Get, "/api/v1/media/files/document/empty.txt");
    ASSERT_EQ(empty.status, 200);
    EXPECT_FALSE(empty.stream);
    EXPECT_EQ(drain(empty), "");

    EXPECT_EQ(fx.call(Method::Get, "/api/v1/media/files/music/none.wav").status, 404);
    HttpResponse folder = fx.call(Method::Get, "/api/v1/media/files/music");
    EXPECT_EQ(folder.status, 400);
    EXPECT_EQ(error_code(folder), "invalid_path");
}

TEST(RoutesMedia, UploadStreamsAndIndexes) {
    MediaFixture fx;
    const std::string mp3 = read_file(wisp::test::media_data("sine_44k_stereo_cbr128.mp3"));
    ASSERT_GT(mp3.size(), 40000u);
    HttpResponse up = fx.upload("music/New%20Album/sine.mp3", mp3);
    ASSERT_EQ(up.status, 201) << wisp::test::body_text(up);
    const Value u = body_json(up);
    EXPECT_EQ(at(u, "path"), Value("music/New Album/sine.mp3"));
    EXPECT_EQ(at(u, "size"), Value(static_cast<int64_t>(mp3.size())));
    EXPECT_TRUE(read_file(fx.root() + "/music/New Album/sine.mp3") == mp3);
    EXPECT_EQ(fx.on_disk("music/New Album"), (std::vector<std::string>{"sine.mp3"}));
    EXPECT_FALSE(fx.f.wakelocks.held(wisp::core::WakeLockKind::Network));

    ASSERT_TRUE(wait_until([&] { return fx.lib->state().get().track_count == 3; }, kWaitMs));
    HttpResponse tracks = fx.call(Method::Get, "/api/v1/media/tracks", "q=sine");
    ASSERT_EQ(tracks.status, 200) << wisp::test::body_text(tracks);
    const Value t = body_json(tracks);
    EXPECT_EQ(at(t, "total"), Value(int64_t{1}));
    ASSERT_EQ(at(t, "items").as_list().size(), 1u);
    const Value& item = at(t, "items").as_list()[0];
    EXPECT_EQ(at(item, "id"), Value("music/New Album/sine.mp3"));
    EXPECT_EQ(at(item, "title"), Value("Sine 440"));  // its ID3 tag
    EXPECT_GT(at(item, "duration").as_int(), 0);
    EXPECT_EQ(at(item, "size"), Value(static_cast<int64_t>(mp3.size())));

    // Paging over everything.
    const Value all = body_json(fx.call(Method::Get, "/api/v1/media/tracks", "offset=1&limit=1"));
    EXPECT_EQ(at(all, "total"), Value(int64_t{3}));
    ASSERT_EQ(at(all, "items").as_list().size(), 1u);
    EXPECT_EQ(at(at(all, "items").as_list()[0], "id"), Value("music/six.wav"));
    EXPECT_EQ(fx.call(Method::Get, "/api/v1/media/tracks", "limit=x").status, 400);

    // A second upload replaces the file.
    HttpResponse again = fx.upload("music/New%20Album/sine.mp3", "short");
    ASSERT_EQ(again.status, 201);
    EXPECT_EQ(read_file(fx.root() + "/music/New Album/sine.mp3"), "short");

    EXPECT_EQ(fx.call(Method::Post, "/api/v1/media/rescan").status, 202);
}

TEST(RoutesMedia, UploadWithoutLengthIs411) {
    MediaFixture fx;
    HttpResponse res = fx.send(fx.upload_head("download/a.bin", 0), {});
    EXPECT_EQ(res.status, 411);
    EXPECT_EQ(error_code(res), "length_required");
    EXPECT_EQ(fx.on_disk("download"), std::vector<std::string>{});
}

TEST(RoutesMedia, ConcurrentUploadIs409) {
    MediaFixture fx;
    std::optional<wisp::net::StreamStart> first = fx.stream(fx.upload_head("download/a.bin", 10));
    ASSERT_TRUE(first && first->sink);
    EXPECT_EQ(first->sink->on_data(as_bytes("01234")), std::nullopt);
    HttpResponse second = fx.upload("download/b.bin", "x");
    EXPECT_EQ(second.status, 409);
    EXPECT_EQ(error_code(second), "busy");
    EXPECT_EQ(first->sink->on_data(as_bytes("56789")), std::nullopt);
    EXPECT_EQ(first->sink->on_end().status, 201);
    first.reset();
    EXPECT_EQ(fx.upload("download/b.bin", "x").status, 201);
    EXPECT_EQ(read_file(fx.root() + "/download/a.bin"), "0123456789");
}

TEST(RoutesMedia, AbortedUploadLeavesNoFile) {
    MediaFixture fx;
    fx.put("download/keep.bin", "old");
    {
        std::optional<wisp::net::StreamStart> s =
            fx.stream(fx.upload_head("download/keep.bin", 200 * 1024));
        ASSERT_TRUE(s && s->sink);
        const std::string part = pattern(100 * 1024);
        for (size_t off = 0; off < part.size(); off += wisp::net::kBodyChunkBytes) {
            ASSERT_EQ(s->sink->on_data(as_bytes(part).subspan(off, wisp::net::kBodyChunkBytes)),
                      std::nullopt);
        }
        EXPECT_TRUE(fx.f.wakelocks.held(wisp::core::WakeLockKind::Network));
        s->sink->on_abort();
    }
    {
        // A sink dropped without on_end/on_abort (the server stopping) ends
        // the same way.
        std::optional<wisp::net::StreamStart> s =
            fx.stream(fx.upload_head("download/new.bin", 100));
        ASSERT_TRUE(s && s->sink);
        ASSERT_EQ(s->sink->on_data(as_bytes("partial")), std::nullopt);
    }
    // A body longer than announced is refused, and its bytes go.
    {
        std::optional<wisp::net::StreamStart> s = fx.stream(fx.upload_head("download/long.bin", 3));
        ASSERT_TRUE(s && s->sink);
        std::optional<HttpResponse> early = s->sink->on_data(as_bytes("toolong"));
        ASSERT_TRUE(early.has_value());
        EXPECT_EQ(early->status, 400);
    }
    EXPECT_TRUE(wait_until(
        [&] { return fx.on_disk("download") == std::vector<std::string>{"keep.bin"}; }, kWaitMs))
        << ::testing::PrintToString(fx.on_disk("download"));
    EXPECT_EQ(read_file(fx.root() + "/download/keep.bin"), "old");
    EXPECT_FALSE(fx.f.wakelocks.held(wisp::core::WakeLockKind::Network));
    // The slot is free again.
    EXPECT_EQ(fx.upload("download/after.bin", "ok").status, 201);
}

TEST(RoutesMedia, DeleteAndMkdir) {
    MediaFixture fx;
    HttpResponse made = fx.call(Method::Post, "/api/v1/media/folders/music/New%20Folder/Sub");
    ASSERT_EQ(made.status, 201) << wisp::test::body_text(made);
    EXPECT_TRUE(fs::is_directory(fx.root() + "/music/New Folder/Sub"));

    // A folder that is not empty stays.
    HttpResponse full = fx.call(Method::Delete, "/api/v1/media/files/music/New%20Folder");
    EXPECT_EQ(full.status, 400);
    EXPECT_EQ(fx.call(Method::Delete, "/api/v1/media/files/music/New%20Folder/Sub").status, 204);
    EXPECT_EQ(fx.call(Method::Delete, "/api/v1/media/files/music/New%20Folder").status, 204);
    EXPECT_FALSE(fs::exists(fx.root() + "/music/New Folder"));

    EXPECT_EQ(fx.call(Method::Delete, "/api/v1/media/files/music/two.wav").status, 204);
    EXPECT_FALSE(fs::exists(fx.root() + "/music/two.wav"));
    ASSERT_TRUE(wait_until([&] { return fx.lib->state().get().track_count == 1; }, kWaitMs));
    HttpResponse gone = fx.call(Method::Delete, "/api/v1/media/files/music/two.wav");
    EXPECT_EQ(gone.status, 404);
    EXPECT_EQ(error_code(gone), "not_found");
}

TEST(RoutesMedia, BadPathIs400) {
    MediaFixture fx;
    for (const char* path :
         {"/api/v1/media/files/music/..%2F..%2Fx", "/api/v1/media/files/.index.json",
          "/api/v1/media/files/music/%zz", "/api/v1/media/files/music//a"}) {
        HttpResponse res = fx.call(Method::Get, path);
        EXPECT_EQ(res.status, 400) << path;
        EXPECT_EQ(error_code(res), "invalid_path") << path;
    }
    EXPECT_EQ(error_code(fx.call(Method::Get, "/api/v1/media/files", "dir=..")), "invalid_path");
    // Writes stay inside the three folders.
    HttpResponse top = fx.upload("x.bin", "x");
    EXPECT_EQ(top.status, 400);
    EXPECT_EQ(error_code(top), "invalid_path");
    EXPECT_EQ(fx.upload("music/.hidden", "x").status, 400);
    EXPECT_EQ(fx.call(Method::Post, "/api/v1/media/folders/other").status, 400);
    EXPECT_EQ(fx.call(Method::Delete, "/api/v1/media/files/music/..").status, 400);
    EXPECT_FALSE(fs::exists(fx.root() + "/x.bin"));
}

TEST(RoutesMedia, NoCardIs503) {
    {
        MediaFixture fx(MediaFixture::Options{.card = false});
        HttpResponse list = fx.call(Method::Get, "/api/v1/media/files");
        EXPECT_EQ(list.status, 503);
        EXPECT_EQ(error_code(list), "media_unavailable");
        EXPECT_EQ(fx.call(Method::Get, "/api/v1/media/files/music/two.wav").status, 503);
        EXPECT_EQ(fx.upload("music/a.wav", "x").status, 503);
        EXPECT_EQ(fx.call(Method::Post, "/api/v1/media/folders/music/a").status, 503);
        EXPECT_EQ(fx.call(Method::Delete, "/api/v1/media/files/music/two.wav").status, 503);
        EXPECT_EQ(fx.call(Method::Get, "/api/v1/media/tracks").status, 503);
        HttpResponse play = fx.call(Method::Post, "/api/v1/media/player/play", {},
                                    R"({"items":["music/two.wav"]})");
        EXPECT_EQ(play.status, 503);
        EXPECT_EQ(error_code(play), "media_unavailable");
    }
    {
        MediaFixture fx(MediaFixture::Options{.with_library = false, .with_player = false});
        for (const char* path : {"/api/v1/media/files", "/api/v1/media/tracks",
                                 "/api/v1/media/player", "/api/v1/media/files/music/a.wav"}) {
            HttpResponse res = fx.call(Method::Get, path);
            EXPECT_EQ(res.status, 503) << path;
            EXPECT_EQ(error_code(res), "media_unavailable") << path;
        }
        EXPECT_EQ(fx.call(Method::Post, "/api/v1/media/rescan").status, 503);
        EXPECT_EQ(fx.call(Method::Post, "/api/v1/media/player/pause").status, 503);
        EXPECT_EQ(fx.upload("music/a.wav", "x").status, 503);
    }
    {
        // A library without a player (no audio output): the player routes
        // are unavailable, the library's are not.
        MediaFixture fx(MediaFixture::Options{.with_player = false});
        EXPECT_EQ(fx.call(Method::Get, "/api/v1/media/player").status, 503);
        EXPECT_EQ(
            fx.call(Method::Post, "/api/v1/media/player/play", {}, R"({"items":["music/two.wav"]})")
                .status,
            503);
        EXPECT_EQ(fx.call(Method::Get, "/api/v1/media/files").status, 200);
    }
    {
        // A player without a library: library items are refused, as the
        // agent's tool and the scene binding do, not queued to fail later.
        MediaFixture fx(MediaFixture::Options{.with_library = false});
        HttpResponse play = fx.call(Method::Post, "/api/v1/media/player/play", {},
                                    R"({"items":["music/two.wav"]})");
        EXPECT_EQ(play.status, 503);
        EXPECT_EQ(error_code(play), "media_unavailable");
        EXPECT_EQ(fx.player->state().get().status, PlayerStatus::Idle);
    }
}

TEST(RoutesMedia, UnpairedIsRefused) {
    MediaFixture fx;
    for (const char* path : {"/api/v1/media/files", "/api/v1/media/files/music/two.wav",
                             "/api/v1/media/tracks", "/api/v1/media/player"}) {
        EXPECT_EQ(fx.f.srv->handle(make_req(Method::Get, path)).status, 401) << path;
    }
    for (const char* path :
         {"/api/v1/media/rescan", "/api/v1/media/folders/music/x", "/api/v1/media/player/pause"}) {
        EXPECT_EQ(fx.f.srv->handle(with_csrf(make_req(Method::Post, path))).status, 401) << path;
        // Paired, but without X-Wisp.
        EXPECT_EQ(fx.f.srv->handle(with_token(make_req(Method::Post, path), fx.token)).status, 403)
            << path;
    }
    EXPECT_EQ(
        fx.f.srv->handle(with_csrf(make_req(Method::Delete, "/api/v1/media/files/music/two.wav")))
            .status,
        401);
    EXPECT_TRUE(fs::exists(fx.root() + "/music/two.wav"));

    // The upload is refused from its head, before any byte is read.
    HttpRequest anon = with_csrf(make_req(Method::Put, "/api/v1/media/files/music/a.wav"));
    anon.content_length = 1;
    std::optional<wisp::net::StreamStart> s = fx.stream(anon);
    ASSERT_TRUE(s.has_value());
    EXPECT_EQ(s->sink, nullptr);
    EXPECT_EQ(s->response.status, 401);
    HttpRequest no_csrf =
        with_token(make_req(Method::Put, "/api/v1/media/files/music/a.wav"), fx.token);
    no_csrf.content_length = 1;
    s = fx.stream(no_csrf);
    ASSERT_TRUE(s.has_value());
    EXPECT_EQ(s->response.status, 403);
    EXPECT_FALSE(fs::exists(fx.root() + "/music/a.wav"));
}

TEST(RoutesMedia, PlayerRoutes) {
    MediaFixture fx;
    // About real time, so the track is still playing when the controls come.
    fx.out->set_write_ms(100);
    HttpResponse idle = fx.call(Method::Get, "/api/v1/media/player");
    ASSERT_EQ(idle.status, 200);
    const Value i = body_json(idle);
    EXPECT_EQ(at(i, "status"), Value("idle"));
    EXPECT_EQ(at(i, "track"), Value());
    EXPECT_EQ(at(i, "available"), Value(true));
    EXPECT_EQ(at(i, "track_count"), Value(int64_t{2}));

    EXPECT_EQ(fx.call(Method::Post, "/api/v1/media/player/play", {}, R"({"items":[]})").status,
              400);
    HttpResponse scene =
        fx.call(Method::Post, "/api/v1/media/player/play", {}, R"({"items":["scene:x/a.wav"]})");
    EXPECT_EQ(scene.status, 400);
    EXPECT_EQ(error_code(scene), "invalid_path");
    EXPECT_EQ(fx.call(Method::Post, "/api/v1/media/player/play", {},
                      R"({"items":["music/two.wav"],"mode":"loud"})")
                  .status,
              400);

    HttpResponse play = fx.call(Method::Post, "/api/v1/media/player/play", {},
                                R"({"items":["music/two.wav","music/six.wav"],"start":1,)"
                                R"("mode":"repeat_all"})");
    ASSERT_EQ(play.status, 200) << wisp::test::body_text(play);
    EXPECT_EQ(at(body_json(play), "truncated"), Value(false));
    ASSERT_TRUE(wait_until([&] { return fx.player->state().get().status == PlayerStatus::Playing; },
                           kWaitMs));
    const Value s = body_json(fx.call(Method::Get, "/api/v1/media/player"));
    EXPECT_EQ(at(s, "status"), Value("playing"));
    EXPECT_EQ(at(at(s, "track"), "id"), Value("music/six.wav"));
    EXPECT_EQ(at(at(s, "track"), "title"), Value("six"));
    EXPECT_EQ(at(s, "index"), Value(int64_t{1}));
    EXPECT_EQ(at(s, "queue_length"), Value(int64_t{2}));
    EXPECT_EQ(at(s, "mode"), Value("repeat_all"));

    EXPECT_EQ(fx.call(Method::Post, "/api/v1/media/player/pause").status, 204);
    ASSERT_TRUE(wait_until(
        [&] {
            auto st = fx.player->state().get();
            return st.status == PlayerStatus::Paused &&
                   st.pause_reason == wisp::media::PauseReason::User;
        },
        kWaitMs));
    EXPECT_EQ(at(body_json(fx.call(Method::Get, "/api/v1/media/player")), "pause_reason"),
              Value("user"));
    EXPECT_EQ(fx.call(Method::Post, "/api/v1/media/player/resume").status, 204);
    ASSERT_TRUE(wait_until([&] { return fx.player->state().get().status == PlayerStatus::Playing; },
                           kWaitMs));
    // On six.wav (6 s), so the seek lands inside it.
    EXPECT_EQ(fx.call(Method::Post, "/api/v1/media/player/seek", {}, R"({"seconds":3})").status,
              204);
    ASSERT_TRUE(wait_until([&] { return fx.player->state().get().position_s >= 3; }, kWaitMs));
    EXPECT_EQ(fx.call(Method::Post, "/api/v1/media/player/next").status, 204);
    ASSERT_TRUE(wait_until([&] { return fx.player->state().get().index == 0; }, kWaitMs));
    EXPECT_EQ(fx.call(Method::Post, "/api/v1/media/player/prev").status, 204);
    EXPECT_EQ(fx.call(Method::Post, "/api/v1/media/player/seek", {}, R"({"seconds":-1})").status,
              400);
    EXPECT_EQ(
        fx.call(Method::Post, "/api/v1/media/player/mode", {}, R"({"mode":"shuffle"})").status,
        204);
    ASSERT_TRUE(wait_until(
        [&] { return fx.player->state().get().mode == wisp::media::PlayMode::Shuffle; }, kWaitMs));
    EXPECT_EQ(fx.call(Method::Post, "/api/v1/media/player/mode", {}, R"({"mode":"x"})").status,
              400);
    EXPECT_EQ(fx.call(Method::Post, "/api/v1/media/player/stop").status, 204);
    ASSERT_TRUE(
        wait_until([&] { return fx.player->state().get().status == PlayerStatus::Idle; }, kWaitMs));
    EXPECT_EQ(fx.call(Method::Post, "/api/v1/media/player/rewind").status, 404);
}
