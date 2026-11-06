// /scenes/{id}/files (scene-device spec 7.2): the list with location,
// declaration, quota and usage, a download streamed through
// HttpResponse::stream, and the delete. A real SceneRepository and
// SceneFiles over the fixture's storage (internal only, except where a
// test adds an SD card it pulls mid-download).

#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <memory>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "admin/admin_fixture.hpp"
#include "support/fake_devices.hpp"
#include "support/fake_http.hpp"
#include "support/wait.hpp"
#include "wisp/core/scene_files.hpp"
#include "wisp/core/scene_repository.hpp"
#include "wisp/core/storage_worker.hpp"
#include "wisp/core/zip.hpp"
#include "wisp/net/http.hpp"

using wisp::ErrorCode;
using wisp::Result;
using wisp::Value;
using wisp::core::FilesWant;
using wisp::core::SceneFiles;
using wisp::core::SceneFilesConfig;
using wisp::core::ZipWriter;
using wisp::net::HttpRequest;
using wisp::net::HttpResponse;
using wisp::net::Method;
using wisp::test::AdminFixture;
using wisp::test::body_json;
using wisp::test::error_code;
using wisp::test::make_req;
using wisp::test::response_header;
using wisp::test::with_csrf;
using wisp::test::with_token;

namespace {

std::span<const uint8_t> as_bytes(const std::string& s) {
    return {reinterpret_cast<const uint8_t*>(s.data()), s.size()};
}

std::vector<uint8_t> scene_zip(const std::string& id, const std::string& files) {
    ZipWriter w;
    const std::string manifest = R"({"id":")" + id + R"(","name":")" + id +
                                 R"(","version":"1.0.0","schema_version":1)" +
                                 (files.empty() ? "" : R"(,"files":")" + files + "\"") + "}";
    w.add("scene.json", as_bytes(manifest));
    w.add("main.lua", as_bytes("return ui.label{text='hi'}\n"));
    wisp::os::LargeBuffer zip = w.finish();
    return {zip.data(), zip.data() + zip.size()};
}

std::string pattern(size_t size) {
    std::string s(size, '\0');
    for (size_t i = 0; i < size; ++i) s[i] = static_cast<char>((i * 31 + 7) & 0xff);
    return s;
}

const Value& at(const Value& v, const char* key) {
    static const Value null;
    const Value* found = v.find(key);
    EXPECT_NE(found, nullptr) << "missing \"" << key << "\" in " << wisp::to_json(v);
    return found != nullptr ? *found : null;
}

struct FilesFixture {
    explicit FilesFixture(bool with_files = true, size_t max_list = 256,
                          std::shared_ptr<wisp::test::FakeStorage> sd = nullptr,
                          wisp::core::StorageWorker* worker = nullptr)
        : f(true, std::move(sd)) {
        SceneFilesConfig cfg;
        cfg.data_root = f.dir + "/scenes";
        cfg.max_list = max_list;
        wisp::core::SceneRepository* repo = f.scenes.get();
        cfg.declared = [repo](std::string_view id) -> Result<FilesWant> {
            auto info = repo->info(std::string(id));
            if (!info) return Result<FilesWant>::err(info.error().code, info.error().message);
            return Result<FilesWant>::ok(info.value().manifest.files);
        };
        files = std::make_unique<SceneFiles>(*f.storage, cfg);
        f.srv.reset();
        wisp::admin::AdminDeps deps = f.make_deps();
        deps.files = with_files ? files.get() : nullptr;
        deps.worker = worker;
        f.srv = std::make_unique<wisp::admin::AdminServer>(std::move(deps));
        token = f.token();
    }

    ~FilesFixture() { f.srv.reset(); }

    void install(const std::string& id, const std::string& want = {}) {
        ASSERT_TRUE(f.scenes->install(scene_zip(id, want), {}));
    }

    HttpResponse call(Method m, const std::string& path, bool csrf = true) {
        HttpRequest req = with_token(make_req(m, path), token);
        if (csrf && m != Method::Get) req = with_csrf(std::move(req));
        return f.srv->handle(req);
    }

    AdminFixture f;
    std::unique_ptr<SceneFiles> files;
    std::string token;
};

// The whole streamed body of `res`, pulled the way a server does.
std::string drain(const HttpResponse& res, size_t* pieces = nullptr) {
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

}  // namespace

TEST(AdminSceneFiles, ListDownloadDelete) {
    FilesFixture fx;
    fx.install("notes");
    HttpResponse empty = fx.call(Method::Get, "/api/v1/scenes/notes/files");
    ASSERT_EQ(empty.status, 200) << empty.body;
    Value e = body_json(empty);
    // Nothing written yet: no location, and the quota it would get now.
    EXPECT_EQ(at(e, "location"), Value());
    EXPECT_EQ(at(e, "want"), Value("auto"));
    EXPECT_EQ(at(e, "quota"), Value(int64_t{512 * 1024}));
    EXPECT_EQ(at(e, "used"), Value(int64_t{0}));
    EXPECT_EQ(at(e, "truncated"), Value(false));
    EXPECT_EQ(at(e, "files"), Value(wisp::List{}));

    const std::string big = pattern(100 * 1024);
    ASSERT_TRUE(fx.files->write("notes", "big.bin", as_bytes(big)));
    ASSERT_TRUE(fx.files->write("notes", "a.txt", as_bytes("hello")));

    HttpResponse listed = fx.call(Method::Get, "/api/v1/scenes/notes/files");
    ASSERT_EQ(listed.status, 200) << listed.body;
    Value l = body_json(listed);
    EXPECT_EQ(at(l, "location"), Value("int"));
    EXPECT_EQ(at(l, "used"), Value(static_cast<int64_t>(big.size() + 5)));
    const Value& entries = at(l, "files");
    ASSERT_EQ(entries.type(), Value::Type::List);
    ASSERT_EQ(entries.as_list().size(), 2u);
    EXPECT_EQ(at(entries.as_list()[0], "name"), Value("a.txt"));
    EXPECT_EQ(at(entries.as_list()[0], "size"), Value(int64_t{5}));
    EXPECT_GT(at(entries.as_list()[0], "mtime").as_int(), 0);
    EXPECT_EQ(at(entries.as_list()[1], "name"), Value("big.bin"));
    EXPECT_EQ(at(entries.as_list()[1], "size"), Value(static_cast<int64_t>(big.size())));

    HttpResponse dl = fx.call(Method::Get, "/api/v1/scenes/notes/files/big.bin");
    ASSERT_EQ(dl.status, 200) << dl.body;
    EXPECT_EQ(dl.content_type, "application/octet-stream");
    EXPECT_EQ(response_header(dl, "content-disposition"), "attachment; filename=\"big.bin\"");
    EXPECT_EQ(response_header(dl, "cache-control"), "no-store");
    ASSERT_TRUE(dl.stream);
    EXPECT_EQ(dl.stream_length, big.size());
    size_t pieces = 0;
    EXPECT_TRUE(drain(dl, &pieces) == big);
    // 32 KiB per read: 100 KiB takes four.
    EXPECT_EQ(pieces, 4u);

    HttpResponse small = fx.call(Method::Get, "/api/v1/scenes/notes/files/a.txt");
    ASSERT_EQ(small.status, 200);
    EXPECT_EQ(drain(small), "hello");

    HttpResponse del = fx.call(Method::Delete, "/api/v1/scenes/notes/files/a.txt");
    EXPECT_EQ(del.status, 204) << del.body;
    Value after = body_json(fx.call(Method::Get, "/api/v1/scenes/notes/files"));
    ASSERT_EQ(at(after, "files").as_list().size(), 1u);
    EXPECT_EQ(at(after, "used"), Value(static_cast<int64_t>(big.size())));
    EXPECT_EQ(fx.call(Method::Get, "/api/v1/scenes/notes/files/a.txt").status, 404);
}

TEST(AdminSceneFiles, ListReportsDeclarationAndTruncation) {
    FilesFixture fx(true, 2);
    fx.install("big");
    for (const char* name : {"a", "b", "c"}) {
        ASSERT_TRUE(fx.files->write("big", name, as_bytes("x")));
    }
    Value v = body_json(fx.call(Method::Get, "/api/v1/scenes/big/files"));
    EXPECT_EQ(at(v, "truncated"), Value(true));
    EXPECT_EQ(at(v, "files").as_list().size(), 2u);
    EXPECT_EQ(at(v, "used"), Value(int64_t{3}));

    fx.install("local", "int");
    Value local = body_json(fx.call(Method::Get, "/api/v1/scenes/local/files"));
    EXPECT_EQ(at(local, "want"), Value("int"));

    // Declared "sd" without a card: nothing can be listed.
    fx.install("music", "sd");
    HttpResponse sd = fx.call(Method::Get, "/api/v1/scenes/music/files");
    EXPECT_EQ(sd.status, 503);
    EXPECT_EQ(error_code(sd), "unavailable");
}

TEST(AdminSceneFiles, Errors) {
    FilesFixture fx;
    fx.install("notes");
    ASSERT_TRUE(fx.files->write("notes", "a.txt", as_bytes("hello")));

    HttpResponse bad = fx.call(Method::Get, "/api/v1/scenes/notes/files/.x");
    EXPECT_EQ(bad.status, 400);
    EXPECT_EQ(error_code(bad), "bad_name");
    HttpResponse bad_del = fx.call(Method::Delete, "/api/v1/scenes/notes/files/.x");
    EXPECT_EQ(bad_del.status, 400);
    EXPECT_EQ(error_code(bad_del), "bad_name");
    // A %-escaped slash is still not a flat name.
    EXPECT_EQ(error_code(fx.call(Method::Get, "/api/v1/scenes/notes/files/a%2Fb")), "bad_name");

    HttpResponse missing = fx.call(Method::Get, "/api/v1/scenes/notes/files/b.txt");
    EXPECT_EQ(missing.status, 404);
    EXPECT_EQ(error_code(missing), "not_found");
    EXPECT_EQ(fx.call(Method::Delete, "/api/v1/scenes/notes/files/b.txt").status, 404);
    // A scene that is not installed.
    EXPECT_EQ(fx.call(Method::Get, "/api/v1/scenes/ghost/files").status, 404);
    EXPECT_EQ(fx.call(Method::Get, "/api/v1/scenes/ghost/files/a.txt").status, 404);
    EXPECT_EQ(fx.call(Method::Get, "/api/v1/scenes/Bad!/files").status, 400);

    HttpResponse no_csrf = fx.call(Method::Delete, "/api/v1/scenes/notes/files/a.txt", false);
    EXPECT_EQ(no_csrf.status, 403);
    EXPECT_EQ(error_code(no_csrf), "csrf");
    EXPECT_TRUE(fx.files->stat("notes", "a.txt"));

    // Unpaired: 401 before anything else.
    EXPECT_EQ(fx.f.srv->handle(make_req(Method::Get, "/api/v1/scenes/notes/files/a.txt")).status,
              401);

    FilesFixture none(false);
    for (auto [m, path] : {std::pair{Method::Get, "/api/v1/scenes/notes/files"},
                           std::pair{Method::Get, "/api/v1/scenes/notes/files/a.txt"},
                           std::pair{Method::Delete, "/api/v1/scenes/notes/files/a.txt"}}) {
        HttpResponse res = none.call(m, path);
        EXPECT_EQ(res.status, 503) << path;
        EXPECT_EQ(error_code(res), "unavailable") << path;
    }
}

// A file that shrinks under a download ends it short: the server is told
// to close rather than pass the short body off as complete.
TEST(AdminSceneFiles, DownloadOfAShrunkFileFails) {
    FilesFixture fx;
    fx.install("notes");
    ASSERT_TRUE(fx.files->write("notes", "a.bin", as_bytes(pattern(40 * 1024))));
    HttpResponse dl = fx.call(Method::Get, "/api/v1/scenes/notes/files/a.bin");
    ASSERT_EQ(dl.status, 200);
    ASSERT_TRUE(fx.files->write("notes", "a.bin", as_bytes("tiny")));
    std::vector<uint8_t> buf(wisp::net::kStreamChunkBytes);
    EXPECT_FALSE(wisp::net::pump_stream(dl, buf, [](std::span<const uint8_t>) { return true; }));
}

// The SD card pulled while the admin page downloads one of its files: the
// stream fails (the server drops the connection; the browser keeps no
// truncated file), the download's lock goes with the response, the list
// says the card is unavailable, and once the card is back the same file
// downloads whole.
TEST(AdminSceneFiles, CardPulledMidDownload) {
    const std::string sd_dir = wisp::test::temp_dir().string();
    auto sd = std::make_shared<wisp::test::FakeStorage>("storage.sd", sd_dir, true);
    FilesFixture fx(true, 256, sd);
    fx.install("music", "sd");
    const std::string big = pattern(100 * 1024);
    ASSERT_TRUE(fx.files->write("music", "song.bin", as_bytes(big)));
    auto locks = [&] {
        int n = 0;
        for (const auto& l : fx.f.wakelocks.list()) n += l.holder == "system.download";
        return n;
    };
    {
        HttpResponse dl = fx.call(Method::Get, "/api/v1/scenes/music/files/song.bin");
        ASSERT_EQ(dl.status, 200) << dl.body;
        EXPECT_EQ(locks(), 1);
        std::vector<uint8_t> buf(wisp::net::kStreamChunkBytes);
        size_t got = 0;
        const bool ok = wisp::net::pump_stream(dl, buf, [&](std::span<const uint8_t> piece) {
            got += piece.size();
            if (got == piece.size()) {
                // After the first piece the card goes: its files at once,
                // the storage on its next refresh.
                sd->online_flag = false;
                std::filesystem::rename(sd_dir, sd_dir + ".pulled");
                std::ofstream(sd_dir) << "no card";
                fx.f.storage->refresh();
                fx.files->on_sd_changed();
            }
            return true;
        });
        EXPECT_FALSE(ok);
        EXPECT_LT(got, big.size());
    }
    EXPECT_EQ(locks(), 0);
    HttpResponse listed = fx.call(Method::Get, "/api/v1/scenes/music/files");
    EXPECT_EQ(listed.status, 503);
    EXPECT_EQ(error_code(listed), "unavailable");

    std::filesystem::remove(sd_dir);
    std::filesystem::rename(sd_dir + ".pulled", sd_dir);
    sd->online_flag = true;
    fx.f.storage->refresh();
    fx.files->on_sd_changed();
    HttpResponse again = fx.call(Method::Get, "/api/v1/scenes/music/files/song.bin");
    ASSERT_EQ(again.status, 200) << again.body;
    EXPECT_TRUE(drain(again) == big);
}

// A full list (max_list names, more on disk) is written straight into one
// PSRAM buffer rather than built as a value tree, and is still valid JSON
// in the same shape.
TEST(AdminSceneFiles, FullListIsValidJsonOutOfPsram) {
    FilesFixture fx;
    fx.install("tiles");
    std::vector<std::string> names;
    for (int i = 0; i < 257; ++i) {
        char name[48];
        std::snprintf(name, sizeof name, "tile_%03d-abcdefghijklmnopqrstu.png", i);
        names.emplace_back(name);
        ASSERT_TRUE(fx.files->write("tiles", name, as_bytes("x")));
    }
    HttpResponse res = fx.call(Method::Get, "/api/v1/scenes/tiles/files");
    ASSERT_EQ(res.status, 200);
    EXPECT_EQ(res.content_type, "application/json");
    EXPECT_TRUE(res.body.empty());
    EXPECT_FALSE(res.static_body.empty());
    EXPECT_NE(res.body_owner, nullptr);
    const Value v = body_json(res);
    EXPECT_EQ(at(v, "location"), Value("int"));
    EXPECT_EQ(at(v, "want"), Value("auto"));
    EXPECT_EQ(at(v, "quota"), Value(int64_t{512 * 1024}));
    EXPECT_EQ(at(v, "used"), Value(int64_t{257}));
    EXPECT_EQ(at(v, "truncated"), Value(true));
    const Value& files = at(v, "files");
    ASSERT_EQ(files.type(), Value::Type::List);
    ASSERT_EQ(files.as_list().size(), 256u);
    for (size_t i = 0; i < 256; ++i) {
        const Value& f = files.as_list()[i];
        EXPECT_EQ(at(f, "name"), Value(names[i]));
        EXPECT_EQ(at(f, "size"), Value(int64_t{1}));
        EXPECT_EQ(at(f, "mtime").type(), Value::Type::Int);
    }
}

// An empty file is a plain empty 200 (Content-Length 0), not a stream.
TEST(AdminSceneFiles, EmptyFileIsAPlainEmptyResponse) {
    FilesFixture fx;
    fx.install("notes");
    ASSERT_TRUE(fx.files->write("notes", "empty.txt", {}));
    HttpResponse res = fx.call(Method::Get, "/api/v1/scenes/notes/files/empty.txt");
    ASSERT_EQ(res.status, 200) << res.body;
    EXPECT_FALSE(res.stream);
    EXPECT_TRUE(res.body.empty());
    EXPECT_TRUE(res.static_body.empty());
    EXPECT_EQ(res.content_type, "application/octet-stream");
    EXPECT_EQ(response_header(res, "content-disposition"), "attachment; filename=\"empty.txt\"");
}

// The download's network lock lives exactly as long as its response.
TEST(AdminSceneFiles, DownloadHoldsANetworkLockWhileAlive) {
    FilesFixture fx;
    fx.install("notes");
    ASSERT_TRUE(fx.files->write("notes", "a.bin", as_bytes(pattern(1000))));
    auto download_locks = [&] {
        int n = 0;
        for (const auto& l : fx.f.wakelocks.list()) {
            if (l.holder == "system.download") {
                EXPECT_EQ(l.kind, wisp::core::WakeLockKind::Network);
                ++n;
            }
        }
        return n;
    };
    EXPECT_EQ(download_locks(), 0);
    {
        HttpResponse dl = fx.call(Method::Get, "/api/v1/scenes/notes/files/a.bin");
        ASSERT_EQ(dl.status, 200);
        EXPECT_EQ(download_locks(), 1);
        EXPECT_EQ(drain(dl).size(), 1000u);
        // Sent, but the server has not let go of the response yet.
        EXPECT_EQ(download_locks(), 1);
    }
    EXPECT_EQ(download_locks(), 0);
}

// Percent-encoded dots decode to "..", which is not a file name.
TEST(AdminSceneFiles, EncodedDotsAreRefused) {
    FilesFixture fx;
    fx.install("notes");
    for (const char* name : {"%2e%2e", "%2E%2E", "..", "%2e"}) {
        HttpResponse get = fx.call(Method::Get, std::string("/api/v1/scenes/notes/files/") + name);
        EXPECT_EQ(get.status, 400) << name;
        EXPECT_EQ(error_code(get), "bad_name") << name;
        HttpResponse del =
            fx.call(Method::Delete, std::string("/api/v1/scenes/notes/files/") + name);
        EXPECT_EQ(del.status, 400) << name;
    }
}

// Every read of a download runs on the flash thread, one 32 KiB chunk at
// a time: while that thread is busy the stream waits, and the bytes that
// come out once it is free are the file's.
TEST(AdminSceneFiles, DownloadReadsOnTheFlashThread) {
    wisp::core::StorageWorker worker;
    {
        // Set up before the flash thread starts, as at boot; from then on
        // the route itself (the scene check and every read) must go
        // through the worker.
        FilesFixture fx(true, 256, nullptr, &worker);
        fx.install("notes");
        const std::string big = pattern(100 * 1024);
        ASSERT_TRUE(fx.files->write("notes", "big.bin", as_bytes(big)));
        ASSERT_TRUE(worker.start());
        HttpResponse dl = fx.call(Method::Get, "/api/v1/scenes/notes/files/big.bin");
        ASSERT_EQ(dl.status, 200) << dl.body;
        ASSERT_TRUE(dl.stream);

        wisp::os::Semaphore started(1, 0);
        wisp::os::Semaphore release(1, 0);
        // A failed assertion below must not leave the worker parked.
        struct ReleaseOnExit {
            wisp::os::Semaphore& s;
            ~ReleaseOnExit() { s.give(); }
        } guard{release};
        ASSERT_TRUE(worker.submit([&] {
            started.give();
            (void)release.take(wisp::os::kForever);
        }));
        ASSERT_TRUE(started.take(5000));

        std::atomic<size_t> got{0};
        std::string body;
        size_t pieces = 0;
        std::thread pump([&] {
            std::vector<uint8_t> buf(wisp::net::kStreamChunkBytes);
            EXPECT_TRUE(wisp::net::pump_stream(dl, buf, [&](std::span<const uint8_t> piece) {
                body.append(reinterpret_cast<const char*>(piece.data()), piece.size());
                ++pieces;
                got += piece.size();
                return true;
            }));
        });
        // The first read is queued behind the parked job, not run here.
        ASSERT_TRUE(wisp::test::wait_until([&] { return worker.pending() == 2; }, 5000));
        EXPECT_EQ(got.load(), 0u);
        release.give();
        pump.join();
        EXPECT_TRUE(body == big);
        EXPECT_EQ(pieces, 4u);
        worker.stop();
    }
}
