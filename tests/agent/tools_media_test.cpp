// media.* agent tools (media spec 7.3) over a MediaLibrary on a temporary
// card, a MediaPlayer with a fake output and a real SceneHttp over a fake
// HTTP client.

#include <gtest/gtest.h>

#include <filesystem>
#include <fstream>
#include <memory>
#include <string>
#include <vector>

#include "support/fake_audio.hpp"
#include "support/fake_devices.hpp"
#include "support/fake_http.hpp"
#include "support/temp_dir.hpp"
#include "support/wait.hpp"
#include "wisp/agent/io_runner.hpp"
#include "wisp/agent/tool_registry.hpp"
#include "wisp/agent/tools.hpp"
#include "wisp/base/json.hpp"
#include "wisp/core/audio_arbiter.hpp"
#include "wisp/core/device_registry.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/core/storage_manager.hpp"
#include "wisp/core/storage_router.hpp"
#include "wisp/core/storage_worker.hpp"
#include "wisp/core/wakelock.hpp"
#include "wisp/media/library.hpp"
#include "wisp/media/player.hpp"
#include "wisp/net/scene_http.hpp"

namespace {

using wisp::Value;
using wisp::agent::ToolCtx;
using wisp::agent::ToolOutput;
using wisp::media::PlayerStatus;
using wisp::test::wait_until;
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

std::string read_raw(const fs::path& path) {
    std::ifstream f(path, std::ios::binary);
    return std::string(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
}

const Value& field(const Value& v, std::string_view key) {
    static const Value kNull;
    const Value* f = v.type() == Value::Type::Map ? v.find(key) : nullptr;
    return f != nullptr ? *f : kNull;
}

std::string str(const Value& v, std::string_view key) {
    const Value& f = field(v, key);
    return f.type() == Value::Type::String ? f.as_string() : std::string();
}

int64_t integer(const Value& v, std::string_view key) {
    const Value& f = field(v, key);
    return f.type() == Value::Type::Int ? f.as_int() : -1;
}

bool boolean(const Value& v, std::string_view key) {
    const Value& f = field(v, key);
    return f.type() == Value::Type::Bool && f.as_bool();
}

class MediaTools : public ::testing::Test {
protected:
    void SetUp() override { build(true); }

    void build(bool card) {
        int_dev = std::make_shared<wisp::test::FakeStorage>("storage.int", in, false);
        sd_dev = std::make_shared<wisp::test::FakeStorage>("storage.sd", sd, true);
        sd_dev->online_flag = card;
        registry.add(int_dev);
        registry.add(sd_dev);
        ASSERT_TRUE(bus.start());
        storage = std::make_unique<wisp::core::StorageManager>(registry, bus);
        ASSERT_TRUE(storage->init());
        ASSERT_TRUE(flash.start());
        ASSERT_TRUE(sd_worker.start());
        router = std::make_unique<wisp::core::StorageRouter>(flash, &sd_worker);

        put("music/six.wav", make_wav(6));
        put("music/two.wav", make_wav(2));
        put("document/notes.txt", "hello, 世界\n");
        lib = std::make_unique<wisp::media::MediaLibrary>(wisp::media::MediaLibraryDeps{
            .storage = *storage, .router = router.get(), .bus = bus, .root = root()});
        lib->start();
        if (card) {
            ASSERT_TRUE(wait_until(
                [this] {
                    auto s = lib->state().get();
                    return s.available && !s.scanning && s.track_count == 2;
                },
                kWaitMs));
        }

        locks = std::make_unique<wisp::core::WakeLockRegistry>(bus);
        player = std::make_unique<wisp::media::MediaPlayer>(wisp::media::MediaPlayerDeps{
            .out = out,
            .arbiter = arbiter,
            .locks = *locks,
            .bus = bus,
            .library = lib.get(),
            .scene_files = nullptr,
            .router = router.get(),
            .decoders = wisp::media::with_wav({}),
            .volume = [] { return uint8_t{50}; },
            .random = [] { return 7u; },
        });
        ASSERT_TRUE(player->start());

        http = std::make_unique<wisp::net::SceneHttp>(client);
        ASSERT_TRUE(http->start());

        wisp::agent::ToolExecutors ex;
        ex.io = [this](std::function<void()> fn) { return io.post(std::move(fn)); };
        tools = std::make_unique<wisp::agent::ToolRegistry>(std::move(ex));
        ASSERT_TRUE(wisp::agent::add_media_tools(
            *tools, {.library = lib.get(), .player = player.get(), .http = http.get()}));
        ctx.cancelled = [] { return false; };
        ctx.lang = "en";
    }

    void TearDown() override {
        out->open_gate();
        io.join_all();
        tools.reset();
        http.reset();
        player.reset();
        lib.reset();
        router.reset();
        sd_worker.stop();
        flash.stop();
        storage.reset();
        bus.stop();
        locks.reset();
    }

    std::string root() const { return sd + "/media"; }

    void put(const std::string& rel, const std::string& bytes) {
        fs::create_directories((fs::path(root()) / rel).parent_path());
        std::ofstream f(fs::path(root()) / rel, std::ios::binary | std::ios::trunc);
        f << bytes;
    }

    Value call(const std::string& tool, const std::string& args_json) {
        ToolOutput out = tools->call(tool, args_json, ctx);
        if (!out.result_json.empty()) {
            auto parsed =
                wisp::from_json(std::string_view(out.result_json.data(), out.result_json.size()));
            EXPECT_TRUE(parsed);
            if (parsed) out.result = std::move(parsed.value());
        }
        last_note = out.note;
        return out.result;
    }

    std::string in = wisp::test::temp_dir().string();
    std::string sd = wisp::test::temp_dir().string();
    std::shared_ptr<wisp::test::FakeStorage> int_dev, sd_dev;
    wisp::core::DeviceRegistry registry;
    wisp::core::EventBus bus;
    std::unique_ptr<wisp::core::StorageManager> storage;
    wisp::core::StorageWorker flash;
    wisp::core::StorageWorker sd_worker{wisp::core::StorageWorker::sd_io_config()};
    std::unique_ptr<wisp::core::StorageRouter> router;
    std::unique_ptr<wisp::media::MediaLibrary> lib;
    std::unique_ptr<wisp::core::WakeLockRegistry> locks;
    wisp::core::AudioArbiter arbiter;
    std::shared_ptr<wisp::test::audio::FakeAudioOut> out =
        std::make_shared<wisp::test::audio::FakeAudioOut>();
    std::unique_ptr<wisp::media::MediaPlayer> player;
    wisp::test::FakeHttpClient client;
    std::unique_ptr<wisp::net::SceneHttp> http;
    wisp::agent::IoRunner io;
    std::unique_ptr<wisp::agent::ToolRegistry> tools;
    ToolCtx ctx;
    std::string last_note;
};

class MediaToolsNoCard : public MediaTools {
protected:
    void SetUp() override { build(false); }
};

}  // namespace

TEST_F(MediaTools, TracksQuery) {
    const Value all = call("media_tracks", "{}");
    ASSERT_TRUE(boolean(all, "ok")) << wisp::to_json(all);
    EXPECT_EQ(integer(all, "total"), 2);
    const Value& items = field(all, "items");
    ASSERT_EQ(items.type(), Value::Type::List);
    ASSERT_EQ(items.as_list().size(), 2u);
    EXPECT_EQ(str(items.as_list()[0], "id"), "music/six.wav");
    EXPECT_EQ(integer(items.as_list()[0], "duration"), 6);
    EXPECT_EQ(str(items.as_list()[0], "title"), "six");
    for (const char* k : {"artist", "album"})
        EXPECT_EQ(field(items.as_list()[0], k).type(), Value::Type::String) << k;

    const Value page = call("media_tracks", R"({"q":"TWO"})");
    EXPECT_EQ(integer(page, "total"), 1);
    EXPECT_EQ(str(field(page, "items").as_list()[0], "id"), "music/two.wav");

    const Value paged = call("media_tracks", R"({"offset":1,"limit":1})");
    EXPECT_EQ(integer(paged, "total"), 2);
    ASSERT_EQ(field(paged, "items").as_list().size(), 1u);
    EXPECT_EQ(str(field(paged, "items").as_list()[0], "id"), "music/two.wav");

    // limit is capped by the schema (spec 3.4: at most 200).
    EXPECT_EQ(str(call("media_tracks", R"({"limit":201})"), "error"), "bad_args");
}

TEST_F(MediaTools, ListRoot) {
    const Value r = call("media_list", "{}");
    ASSERT_TRUE(boolean(r, "ok")) << wisp::to_json(r);
    std::vector<std::string> names;
    for (const Value& e : field(r, "entries").as_list()) {
        names.push_back(str(e, "name"));
        EXPECT_TRUE(boolean(e, "dir"));
    }
    EXPECT_EQ(names, (std::vector<std::string>{"document", "download", "music"}));

    const Value music = call("media_list", R"({"dir":"music"})");
    ASSERT_EQ(field(music, "entries").as_list().size(), 2u);
    const Value& six = field(music, "entries").as_list()[0];
    EXPECT_EQ(str(six, "name"), "six.wav");
    EXPECT_FALSE(boolean(six, "dir"));
    EXPECT_EQ(integer(six, "size"), static_cast<int64_t>(make_wav(6).size()));

    EXPECT_EQ(str(call("media_list", R"({"dir":"../x"})"), "error"), "invalid_path");
    EXPECT_EQ(str(call("media_list", R"({"dir":"nope"})"), "error"), "not_found");
}

TEST_F(MediaTools, ReadTextCapsAndRejectsBinary) {
    const Value r = call("media_read_text", R"({"path":"document/notes.txt"})");
    ASSERT_TRUE(boolean(r, "ok")) << wisp::to_json(r);
    // Library files are outside data (spec 6.3 of agent-core): wrapped.
    EXPECT_EQ(str(r, "text"), "<data source=\"media\">hello, 世界\n</data>");
    EXPECT_FALSE(boolean(r, "truncated"));

    // Past 32 KiB the text is cut at a character boundary: 32767 'a' and
    // a three-byte character that would straddle the cap.
    put("document/big.txt", std::string(32 * 1024 - 1, 'a') + "世" + "tail");
    const Value big = call("media_read_text", R"({"path":"document/big.txt"})");
    ASSERT_TRUE(boolean(big, "ok")) << wisp::to_json(big);
    EXPECT_TRUE(boolean(big, "truncated"));
    EXPECT_EQ(str(big, "text"),
              "<data source=\"media\">" + std::string(32 * 1024 - 1, 'a') + "</data>");

    EXPECT_EQ(str(call("media_read_text", R"({"path":"music/two.wav"})"), "error"),
              "unsupported_format");
    EXPECT_EQ(str(call("media_read_text", R"({"path":"document/none.txt"})"), "error"),
              "not_found");
    EXPECT_EQ(str(call("media_read_text", R"({"path":"/etc/passwd"})"), "error"), "invalid_path");
}

TEST_F(MediaTools, WriteTextThenRemove) {
    const Value w =
        call("media_write_text", R"({"path":"document/todo/list.md","text":"# 买菜\n- 苹果\n"})");
    ASSERT_TRUE(boolean(w, "ok")) << wisp::to_json(w);
    EXPECT_EQ(read_raw(fs::path(root()) / "document/todo/list.md"), "# 买菜\n- 苹果\n");
    // Writes land only inside music/, download/ or document/.
    EXPECT_EQ(str(call("media_write_text", R"({"path":"top.txt","text":"x"})"), "error"),
              "invalid_path");

    // A folder that holds something is refused; empty, it goes.
    EXPECT_EQ(str(call("media_remove", R"({"path":"document/todo"})"), "error"), "invalid_path");
    EXPECT_TRUE(boolean(call("media_remove", R"({"path":"document/todo/list.md"})"), "ok"));
    EXPECT_FALSE(fs::exists(fs::path(root()) / "document/todo/list.md"));
    EXPECT_TRUE(boolean(call("media_remove", R"({"path":"document/todo"})"), "ok"));
    EXPECT_FALSE(fs::exists(fs::path(root()) / "document/todo"));
    EXPECT_EQ(str(call("media_remove", R"({"path":"document/todo"})"), "error"), "not_found");
}

TEST_F(MediaTools, DownloadStreamsIntoTheLibrary) {
    const std::string body = make_wav(3);
    client.next = {.status = 200, .body = body};
    const Value r =
        call("media_download", R"({"url":"http://files.example.com/a.wav","path":"music/a.wav"})");
    ASSERT_TRUE(boolean(r, "ok")) << wisp::to_json(r);
    EXPECT_EQ(integer(r, "size"), static_cast<int64_t>(body.size()));
    EXPECT_EQ(read_raw(fs::path(root()) / "music/a.wav"), body);
    EXPECT_FALSE(fs::exists(fs::path(root()) / "music/.a.wav.part"));
    ASSERT_TRUE(lib->track("music/a.wav"));
    EXPECT_EQ(lib->track("music/a.wav")->duration_s, 3u);
    // The user sees which host was reached, as for http.fetch.
    EXPECT_NE(last_note.find("files.example.com"), std::string::npos) << last_note;
    ASSERT_EQ(client.calls(), 1u);

    // A non-2xx answer leaves nothing behind.
    client.next = {.status = 404, .body = "missing"};
    const Value nf =
        call("media_download", R"({"url":"http://files.example.com/b.wav","path":"music/b.wav"})");
    EXPECT_EQ(str(nf, "error"), "http_status");
    EXPECT_EQ(integer(nf, "status"), 404);
    EXPECT_FALSE(fs::exists(fs::path(root()) / "music/b.wav"));

    // A bad path costs no request.
    EXPECT_EQ(str(call("media_download", R"({"url":"http://files.example.com/c","path":"c.wav"})"),
                  "error"),
              "invalid_path");
    EXPECT_EQ(client.calls(), 2u);

    // A transport failure comes back by its proxy code.
    client.next = {.transport = wisp::net::TransportError::Dns};
    EXPECT_EQ(str(call("media_download",
                       R"({"url":"http://nowhere.example.com/d","path":"download/d.bin"})"),
                  "error"),
              "dns");
    EXPECT_FALSE(fs::exists(fs::path(root()) / "download/d.bin"));
}

TEST_F(MediaTools, PlayAndControl) {
    // About real time (one 1152-frame chunk is 144 ms at 8 kHz), so the
    // track is still playing when the controls come.
    out->set_write_ms(100);
    const Value p = call("media_play", R"({"items":["music/two.wav","music/six.wav"],"start":1,)"
                                       R"("mode":"repeat_all"})");
    ASSERT_TRUE(boolean(p, "ok")) << wisp::to_json(p);
    EXPECT_FALSE(boolean(p, "truncated"));
    ASSERT_TRUE(wait_until([this] { return player->state().get().status == PlayerStatus::Playing; },
                           kWaitMs));
    const Value s = call("media_state", "{}");
    EXPECT_EQ(str(s, "status"), "playing");
    EXPECT_EQ(str(field(s, "track"), "id"), "music/six.wav");
    EXPECT_EQ(integer(s, "index"), 1);
    EXPECT_EQ(integer(s, "queue_length"), 2);
    EXPECT_EQ(str(s, "mode"), "repeat_all");
    EXPECT_TRUE(boolean(s, "available"));
    EXPECT_EQ(integer(s, "track_count"), 2);

    EXPECT_TRUE(boolean(call("media_control", R"({"action":"pause"})"), "ok"));
    ASSERT_TRUE(wait_until(
        [this] {
            auto st = player->state().get();
            return st.status == PlayerStatus::Paused &&
                   st.pause_reason == wisp::media::PauseReason::User;
        },
        kWaitMs));
    EXPECT_EQ(str(call("media_state", "{}"), "pause_reason"), "user");
    EXPECT_TRUE(boolean(call("media_control", R"({"action":"mode","mode":"shuffle"})"), "ok"));
    ASSERT_TRUE(wait_until(
        [this] { return player->state().get().mode == wisp::media::PlayMode::Shuffle; }, kWaitMs));
    EXPECT_TRUE(boolean(call("media_control", R"({"action":"seek","seconds":2})"), "ok"));
    ASSERT_TRUE(wait_until([this] { return player->state().get().position_s == 2; }, kWaitMs));
    // seek needs seconds, mode needs a mode.
    EXPECT_EQ(str(call("media_control", R"({"action":"seek"})"), "error"), "bad_args");
    EXPECT_EQ(str(call("media_control", R"({"action":"mode"})"), "error"), "bad_args");
    EXPECT_EQ(str(call("media_control", R"({"action":"jump"})"), "error"), "bad_args");
    EXPECT_TRUE(boolean(call("media_control", R"({"action":"stop"})"), "ok"));
    ASSERT_TRUE(
        wait_until([this] { return player->state().get().status == PlayerStatus::Idle; }, kWaitMs));
    EXPECT_EQ(field(call("media_state", "{}"), "track").type(), Value::Type::Null);

    // Scene items and bad paths are not the agent's to play.
    EXPECT_EQ(str(call("media_play", R"({"items":["scene:demo/a.wav"]})"), "error"),
              "invalid_path");
    EXPECT_EQ(str(call("media_play", R"({"items":["../a.wav"]})"), "error"), "invalid_path");
    EXPECT_EQ(str(call("media_play", R"({"items":[]})"), "error"), "bad_args");
}

TEST_F(MediaToolsNoCard, StateWithoutCard) {
    const Value s = call("media_state", "{}");
    ASSERT_TRUE(boolean(s, "ok")) << wisp::to_json(s);
    EXPECT_FALSE(boolean(s, "available"));
    EXPECT_EQ(integer(s, "track_count"), 0);
    EXPECT_EQ(str(s, "status"), "idle");
    EXPECT_EQ(str(call("media_list", "{}"), "error"), "media_unavailable");
    EXPECT_EQ(str(call("media_tracks", "{}"), "error"), "media_unavailable");
    EXPECT_EQ(str(call("media_play", R"({"items":["music/two.wav"]})"), "error"),
              "media_unavailable");
}

// Only what exists is offered: without a library and a player, nothing.
TEST(MediaToolsRegistration, NothingWithoutServices) {
    wisp::agent::ToolRegistry reg{wisp::agent::ToolExecutors{}};
    ASSERT_TRUE(wisp::agent::add_media_tools(reg, {}));
    for (const char* name : {"media.tracks", "media.list", "media.download", "media.play",
                             "media.control", "media.state"})
        EXPECT_EQ(reg.find(name), nullptr) << name;
}

// Without a proxy there is no media.download; the file tools stay.
TEST_F(MediaTools, NoDownloadWithoutHttp) {
    wisp::agent::ToolRegistry reg{wisp::agent::ToolExecutors{}};
    ASSERT_TRUE(wisp::agent::add_media_tools(reg, {.library = lib.get()}));
    EXPECT_EQ(reg.find("media.download"), nullptr);
    EXPECT_NE(reg.find("media.read_text"), nullptr);
    EXPECT_EQ(reg.find("media.play"), nullptr);
    EXPECT_NE(reg.find("media.state"), nullptr);
}
