// /scenes (scene-runtime-core spec 7.1): the list, the streamed zip upload,
// export, open/close/reload through core::SceneHost and the delete request
// that the device confirms. A real SceneRepository over the fixture's
// storage; a fake SceneHost records what the routes ask of it.

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <map>
#include <memory>
#include <mutex>
#include <optional>
#include <span>
#include <string>
#include <utility>
#include <vector>

#include "admin/admin_fixture.hpp"
#include "support/fake_http.hpp"
#include "wisp/core/scene_host.hpp"
#include "wisp/core/scene_repository.hpp"
#include "wisp/core/zip.hpp"
#include "wisp/net/http.hpp"

using wisp::ErrorCode;
using wisp::Result;
using wisp::Value;
using wisp::core::SceneHost;
using wisp::core::SceneRepoConfig;
using wisp::core::SceneRepository;
using wisp::core::SceneState;
using wisp::core::SceneStatus;
using wisp::core::SourceFile;
using wisp::core::ZipReader;
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

namespace fs = std::filesystem;

namespace {

constexpr const char* kScenes = "/api/v1/scenes";

using Files = std::vector<std::pair<std::string, std::string>>;

std::span<const uint8_t> as_bytes(const std::string& s) {
    return {reinterpret_cast<const uint8_t*>(s.data()), s.size()};
}

std::string scene_json(const std::string& version = "1.0.0") {
    return R"({"id":"todo","name":"待办","version":")" + version +
           R"(","schema_version":1,"permissions":{"http":["api.example.com"]}})";
}

Files todo_files(const std::string& version = "1.0.0") {
    return {{"scene.json", scene_json(version)},
            {"main.lua", "local u = wisp.import('lib/util')\nreturn ui.label{text='hi'}\n"},
            {"lib/util.lua", "return {}\n"}};
}

std::vector<uint8_t> zip_of(const Files& files) {
    ZipWriter w;
    for (const auto& [path, body] : files) w.add(path, as_bytes(body));
    wisp::os::LargeBuffer zip = w.finish();
    return {zip.data(), zip.data() + zip.size()};
}

// v[key], failing the test (and yielding Null) when it is missing.
const Value& at(const Value& v, const char* key) {
    static const Value null;
    const Value* f = v.find(key);
    EXPECT_NE(f, nullptr) << "missing \"" << key << "\" in " << wisp::to_json(v);
    return f != nullptr ? *f : null;
}

// Records every call. check_sources() "compiles" by looking for "@@": the
// first file holding it fails with "<path>:<line>: unexpected symbol".
class FakeSceneHost final : public SceneHost {
public:
    Result<void> open(const std::string& id, Value) override {
        return record("open " + id, SceneState::Running);
    }
    Result<void> close(const std::string& id) override {
        return record("close " + id, SceneState::Stopped);
    }
    Result<void> exit(const std::string& id) override {
        return record("exit " + id, SceneState::Stopped);
    }
    Result<void> reload(const std::string& id) override {
        return record("reload " + id, SceneState::Running);
    }
    Result<void> open_background(const std::string& id) override {
        return record("background " + id, SceneState::Background);
    }
    SceneStatus status(const std::string& id) const override {
        std::lock_guard<std::mutex> lock(mu);
        auto it = states.find(id);
        return it != states.end() ? it->second : SceneStatus{};
    }
    Result<void> check_sources(std::span<const SourceFile> files) override {
        std::lock_guard<std::mutex> lock(mu);
        ++checks;
        for (const SourceFile& f : files) {
            const size_t at = f.text.find("@@");
            if (at == std::string_view::npos) continue;
            const auto line = 1 + std::count(f.text.begin(), f.text.begin() + at, '\n');
            return Result<void>::err(
                ErrorCode::InvalidArgument,
                std::string(f.path) + ":" + std::to_string(line) + ": unexpected symbol near '@'");
        }
        return Result<void>::ok();
    }
    void on_installed(const std::string& id) override {
        std::lock_guard<std::mutex> lock(mu);
        calls.push_back("installed " + id);
    }

    std::vector<std::string> log() const {
        std::lock_guard<std::mutex> lock(mu);
        return calls;
    }

    mutable std::mutex mu;
    std::vector<std::string> calls;
    std::map<std::string, SceneStatus> states;
    int checks = 0;
    // What open/close/reload return; Ok by default.
    std::optional<wisp::Error> fail;

private:
    Result<void> record(std::string call, SceneState next) {
        std::lock_guard<std::mutex> lock(mu);
        const std::string id = call.substr(call.find(' ') + 1);
        calls.push_back(std::move(call));
        if (fail) return Result<void>::err(fail->code, fail->message);
        states[id] = SceneStatus{next, {}};
        return Result<void>::ok();
    }
};

struct ScenesFixture {
    struct Options {
        bool with_host = true;
        bool with_repo = true;
        bool dev = false;
    };

    ScenesFixture() : ScenesFixture(Options{}) {}
    explicit ScenesFixture(Options o) {
        root = f.dir + "/scenes";
        SceneRepoConfig cfg;
        cfg.code_root = root;
        cfg.data_root = root;
        cfg.dev = o.dev;
        repo = std::make_unique<SceneRepository>(*f.storage, cfg);
        f.srv.reset();
        wisp::admin::AdminDeps deps = f.make_deps();
        deps.scenes = o.with_repo ? repo.get() : nullptr;
        deps.scene_host = o.with_host ? &host : nullptr;
        deps.request_scene_delete = [this](const std::string& id, bool with_data) -> Result<void> {
            deletes.emplace_back(id, with_data);
            if (delete_result)
                return Result<void>::err(delete_result->code, delete_result->message);
            return Result<void>::ok();
        };
        f.srv = std::make_unique<wisp::admin::AdminServer>(std::move(deps));
        token = f.token();
        stream = f.srv->stream_handler();
    }

    HttpRequest authed(Method m, const std::string& path) const {
        HttpRequest req = with_token(make_req(m, path), token);
        if (m != Method::Get) req = with_csrf(std::move(req));
        return req;
    }

    HttpResponse call(Method m, const std::string& path, const std::string& query = {}) {
        HttpRequest req = authed(m, path);
        req.query = query;
        return f.srv->handle(req);
    }

    HttpRequest upload_head(uint64_t length) const {
        HttpRequest req = authed(Method::Post, kScenes);
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
            ++chunks;
            if (auto early = start->sink->on_data(body.first(n))) return *early;
            body = body.subspan(n);
        }
        return start->sink->on_end();
    }

    HttpResponse upload(const std::vector<uint8_t>& zip) {
        return send(upload_head(zip.size()), zip);
    }

    // Every file under the scenes root, relative to it.
    std::vector<std::string> files_on_disk() const {
        std::vector<std::string> out;
        std::error_code ec;
        if (!fs::exists(root, ec)) return out;
        for (const auto& e : fs::recursive_directory_iterator(root, ec)) {
            if (e.is_regular_file()) out.push_back(fs::relative(e.path(), root).string());
        }
        std::sort(out.begin(), out.end());
        return out;
    }

    AdminFixture f;
    std::string root;
    std::unique_ptr<SceneRepository> repo;
    FakeSceneHost host;
    std::vector<std::pair<std::string, bool>> deletes;
    std::optional<wisp::Error> delete_result;
    std::string token;
    wisp::net::StreamHandler stream;
    int chunks = 0;
};

// The names in a zip.
std::vector<std::string> zip_names(const HttpResponse& res) {
    std::span<const uint8_t> bytes = res.static_body;
    if (bytes.empty()) bytes = as_bytes(res.body);
    auto reader = ZipReader::open(bytes);
    EXPECT_TRUE(reader) << (reader ? "" : reader.error().message);
    std::vector<std::string> names;
    if (!reader) return names;
    for (const auto& e : reader.value().entries()) {
        if (!e.is_dir) names.emplace_back(e.path.data(), e.path.size());
    }
    std::sort(names.begin(), names.end());
    return names;
}

}  // namespace

TEST(AdminScenes, RequiresAuthAndCsrf) {
    ScenesFixture fx;
    EXPECT_EQ(fx.f.srv->handle(make_req(Method::Get, kScenes)).status, 401);
    EXPECT_EQ(fx.f.srv->handle(make_req(Method::Get, "/api/v1/scenes/todo/export")).status, 401);
    EXPECT_EQ(
        fx.f.srv->handle(with_csrf(make_req(Method::Post, "/api/v1/scenes/todo/open"))).status,
        401);
    // Authenticated but without X-Wisp.
    for (const char* path :
         {"/api/v1/scenes/todo/open", "/api/v1/scenes/todo/close", "/api/v1/scenes/todo/reload"}) {
        HttpResponse res = fx.f.srv->handle(with_token(make_req(Method::Post, path), fx.token));
        EXPECT_EQ(res.status, 403) << path;
        EXPECT_EQ(error_code(res), "csrf");
    }
    EXPECT_EQ(
        fx.f.srv->handle(with_token(make_req(Method::Delete, "/api/v1/scenes/todo"), fx.token))
            .status,
        403);
    EXPECT_TRUE(fx.host.log().empty());
    EXPECT_TRUE(fx.deletes.empty());

    // The streamed upload runs the same gate before reading any body.
    const std::vector<uint8_t> zip = zip_of(todo_files());
    HttpRequest no_token = make_req(Method::Post, kScenes);
    no_token.content_length = zip.size();
    EXPECT_EQ(fx.send(with_csrf(no_token), zip).status, 401);
    HttpRequest no_csrf = with_token(make_req(Method::Post, kScenes), fx.token);
    no_csrf.content_length = zip.size();
    EXPECT_EQ(fx.send(no_csrf, zip).status, 403);
    HttpRequest foreign = fx.upload_head(zip.size());
    foreign.headers["host"] = "evil.example.com";
    EXPECT_EQ(fx.send(foreign, zip).status, 403);
    EXPECT_EQ(fx.chunks, 0);
    EXPECT_TRUE(fx.files_on_disk().empty());

    // Other requests are not streamed.
    EXPECT_FALSE(fx.stream(fx.authed(Method::Post, "/api/v1/scenes/todo/open")).has_value());
    EXPECT_FALSE(fx.stream(fx.authed(Method::Get, kScenes)).has_value());
}

TEST(AdminScenes, ListShape) {
    ScenesFixture fx;
    HttpResponse empty = fx.call(Method::Get, kScenes);
    ASSERT_EQ(empty.status, 200) << empty.body;
    EXPECT_EQ(at(body_json(empty), "scenes"), Value(wisp::List{}));

    ASSERT_TRUE(fx.repo->install(zip_of(todo_files()), {}));
    fx.host.states["todo"] = SceneStatus{SceneState::Error, "main.lua:2: boom"};
    HttpResponse res = fx.call(Method::Get, kScenes);
    ASSERT_EQ(res.status, 200) << res.body;
    const Value list = body_json(res);
    const Value& scenes = at(list, "scenes");
    ASSERT_EQ(scenes.type(), Value::Type::List);
    ASSERT_EQ(scenes.as_list().size(), 1u);
    const Value& s = scenes.as_list()[0];
    EXPECT_EQ(at(s, "id"), Value("todo"));
    EXPECT_EQ(at(s, "name"), Value("待办"));
    EXPECT_EQ(at(s, "version"), Value("1.0.0"));
    EXPECT_GT(at(s, "code_bytes").as_int(), 0);
    EXPECT_EQ(at(s, "data_bytes"), Value(int64_t{0}));
    EXPECT_EQ(at(s, "state"), Value("error"));
    EXPECT_EQ(at(s, "hosts"), Value(wisp::List{Value("api.example.com")}));
    EXPECT_EQ(at(s, "error"), Value("main.lua:2: boom"));

    // A scene the host has not run is "stopped" with no error.
    fx.host.states.clear();
    const Value relisted = body_json(fx.call(Method::Get, kScenes));
    const Value& again = at(relisted, "scenes").as_list()[0];
    EXPECT_EQ(at(again, "state"), Value("stopped"));
    EXPECT_EQ(at(again, "error"), Value());
}

// A scene running in the background (scene-device spec 3.7), and where
// each scene wants its files with whether a card is in: the page marks a
// scene that needs one.
TEST(AdminScenes, ListShowsBackground) {
    ScenesFixture fx;
    ASSERT_TRUE(fx.repo->install(zip_of(todo_files()), {}));
    fx.host.states["todo"] = SceneStatus{SceneState::Background, {}};
    const Value list = body_json(fx.call(Method::Get, kScenes));
    const Value& s = at(list, "scenes").as_list()[0];
    EXPECT_EQ(at(s, "state"), Value("background"));
    EXPECT_EQ(at(s, "files"), Value("auto"));
    // The fixture has internal storage only.
    EXPECT_EQ(at(list, "sd_card"), Value(false));
}

TEST(AdminScenes, ListWithoutRepositoryIsUnavailable) {
    ScenesFixture fx(ScenesFixture::Options{.with_host = false, .with_repo = false});
    HttpResponse res = fx.call(Method::Get, kScenes);
    EXPECT_EQ(res.status, 503);
    EXPECT_EQ(error_code(res), "unavailable");
    const std::vector<uint8_t> zip = zip_of(todo_files());
    EXPECT_EQ(fx.upload(zip).status, 503);
}

TEST(AdminScenes, UploadStreamsAndInstalls) {
    ScenesFixture fx;
    // Big enough to arrive in several pieces: a lib file of incompressible
    // bytes keeps the zip itself large.
    Files files = todo_files();
    std::string big;
    uint32_t x = 12345;
    for (int i = 0; i < 20000; ++i) {
        x = x * 1103515245u + 12345u;
        big += static_cast<char>('a' + (x >> 16) % 26);
    }
    files.push_back({"lib/big.lua", "return '" + big + "'\n"});
    const std::vector<uint8_t> zip = zip_of(files);

    HttpResponse res = fx.upload(zip);
    ASSERT_EQ(res.status, 201) << res.body;
    EXPECT_GT(fx.chunks, 2);
    EXPECT_EQ(response_header(res, "cache-control"), "no-store");
    Value v = body_json(res);
    EXPECT_EQ(at(v, "id"), Value("todo"));
    EXPECT_EQ(at(v, "name"), Value("待办"));
    EXPECT_EQ(at(v, "version"), Value("1.0.0"));
    EXPECT_EQ(at(v, "hosts"), Value(wisp::List{Value("api.example.com")}));
    EXPECT_EQ(at(v, "state"), Value("stopped"));
    EXPECT_EQ(fx.host.checks, 1);
    // install_scene() closes the scene before every replace, running or not.
    EXPECT_EQ(fx.host.log(), (std::vector<std::string>{"exit todo", "installed todo"}));
    EXPECT_TRUE(fx.repo->info("todo"));
}

// An upload keeps Wi-Fi on until it ends, however it ends (spec 4.3).
TEST(AdminScenes, UploadHoldsNetworkLock) {
    ScenesFixture fx;
    auto upload_locks = [&fx] {
        int n = 0;
        for (const auto& l : fx.f.wakelocks.list())
            if (l.holder == "system.upload" && l.kind == wisp::core::WakeLockKind::Network) ++n;
        return n;
    };
    const auto zip = zip_of(todo_files());
    {
        std::optional<wisp::net::StreamStart> start = fx.stream(fx.upload_head(zip.size()));
        ASSERT_TRUE(start && start->sink);
        EXPECT_EQ(upload_locks(), 1);
        std::span<const uint8_t> body(zip);
        while (!body.empty()) {
            const size_t n = std::min(body.size(), wisp::net::kBodyChunkBytes);
            ASSERT_FALSE(start->sink->on_data(body.first(n)));
            body = body.subspan(n);
        }
        EXPECT_EQ(upload_locks(), 1);
        EXPECT_EQ(start->sink->on_end().status, 201);
        EXPECT_EQ(upload_locks(), 0);
    }
    {
        std::optional<wisp::net::StreamStart> start = fx.stream(fx.upload_head(zip.size()));
        ASSERT_TRUE(start && start->sink);
        EXPECT_EQ(upload_locks(), 1);
        start->sink->on_abort();
        EXPECT_EQ(upload_locks(), 0);
    }
    {
        std::optional<wisp::net::StreamStart> start = fx.stream(fx.upload_head(zip.size()));
        ASSERT_TRUE(start && start->sink);
        EXPECT_EQ(upload_locks(), 1);
    }
    EXPECT_EQ(upload_locks(), 0);
    // A body longer than announced ends the request from on_data(): the
    // lock goes at once, while the server is still draining the rest.
    {
        std::optional<wisp::net::StreamStart> start = fx.stream(fx.upload_head(zip.size() - 10));
        ASSERT_TRUE(start && start->sink);
        std::optional<HttpResponse> early;
        std::span<const uint8_t> body(zip);
        while (!early && !body.empty()) {
            const size_t n = std::min(body.size(), wisp::net::kBodyChunkBytes);
            early = start->sink->on_data(body.first(n));
            body = body.subspan(n);
        }
        ASSERT_TRUE(early.has_value());
        EXPECT_EQ(early->status, 400);
        EXPECT_EQ(upload_locks(), 0);
    }
}

// Each upload holds up to 576 KiB of PSRAM until it ends, and running out
// of PSRAM aborts the device: one scene upload at a time.
TEST(AdminScenes, OneUploadAtATime) {
    ScenesFixture fx;
    const std::vector<uint8_t> zip = zip_of(todo_files("1.0.0"));
    std::optional<wisp::net::StreamStart> first = fx.stream(fx.upload_head(zip.size()));
    ASSERT_TRUE(first.has_value());
    ASSERT_NE(first->sink, nullptr);

    std::optional<wisp::net::StreamStart> second = fx.stream(fx.upload_head(zip.size()));
    ASSERT_TRUE(second.has_value());
    EXPECT_EQ(second->sink, nullptr);
    EXPECT_EQ(second->response.status, 409);
    EXPECT_EQ(error_code(second->response), "busy");

    // Finished, or dropped without an end (the client went away): the next
    // one may start.
    EXPECT_EQ(first->sink->on_data(zip), std::nullopt);
    EXPECT_EQ(first->sink->on_end().status, 201);
    first.reset();
    std::optional<wisp::net::StreamStart> third = fx.stream(fx.upload_head(zip.size()));
    ASSERT_TRUE(third.has_value());
    ASSERT_NE(third->sink, nullptr);
    third.reset();
    EXPECT_EQ(fx.upload(zip).status, 201);
}

TEST(AdminScenes, UploadErrors) {
    ScenesFixture fx;
    // No Content-Length.
    {
        HttpResponse res = fx.send(fx.upload_head(0), {});
        EXPECT_EQ(res.status, 411);
        EXPECT_EQ(error_code(res), "length_required");
    }
    // Over 576 KiB, refused from the head alone.
    {
        std::optional<wisp::net::StreamStart> start = fx.stream(fx.upload_head(576 * 1024 + 1));
        ASSERT_TRUE(start.has_value());
        EXPECT_EQ(start->sink, nullptr);
        EXPECT_EQ(start->response.status, 413);
        EXPECT_EQ(error_code(start->response), "payload_too_large");
    }
    // A body longer than announced.
    {
        const std::vector<uint8_t> zip = zip_of(todo_files());
        HttpResponse res = fx.send(fx.upload_head(zip.size() - 10), zip);
        EXPECT_EQ(res.status, 400) << res.body;
    }
    // A syntax error: the file and line come back on their own.
    {
        Files files = todo_files();
        files[2].second = "local t = {}\n\nlocal @@ = 1\n";
        HttpResponse res = fx.upload(zip_of(files));
        ASSERT_EQ(res.status, 400) << res.body;
        Value v = body_json(res);
        const Value& err = at(v, "error");
        EXPECT_EQ(at(err, "code"), Value("invalid_scene"));
        EXPECT_EQ(at(err, "message"), Value("lib/util.lua:3: unexpected symbol near '@'"));
        EXPECT_EQ(at(err, "file"), Value("lib/util.lua"));
        EXPECT_EQ(at(err, "line"), Value(int64_t{3}));
    }
    // A bad manifest: invalid_scene without a file or line.
    {
        Files files = todo_files();
        files[0].second = R"({"id":"todo","name":"x","version":"one"})";
        HttpResponse res = fx.upload(zip_of(files));
        ASSERT_EQ(res.status, 400) << res.body;
        const Value v = body_json(res);
        const Value& err = at(v, "error");
        EXPECT_EQ(at(err, "code"), Value("invalid_scene"));
        EXPECT_EQ(err.find("line"), nullptr);
    }
    // Not a zip at all.
    {
        const std::string junk = "definitely not a zip";
        HttpResponse res = fx.send(fx.upload_head(junk.size()), as_bytes(junk));
        EXPECT_EQ(res.status, 400) << res.body;
        EXPECT_EQ(error_code(res), "invalid_scene");
    }
    EXPECT_TRUE(fx.files_on_disk().empty());
    EXPECT_TRUE(fx.host.log().empty());
}

TEST(AdminScenes, UploadNeedsHostAndRefusesDevMode) {
    const std::vector<uint8_t> zip = zip_of(todo_files());
    {
        ScenesFixture fx(ScenesFixture::Options{.with_host = false});
        HttpResponse res = fx.upload(zip);
        EXPECT_EQ(res.status, 503);
        EXPECT_EQ(error_code(res), "unavailable");
        EXPECT_EQ(fx.chunks, 0);
    }
    {
        ScenesFixture fx(ScenesFixture::Options{.dev = true});
        HttpResponse res = fx.upload(zip);
        EXPECT_EQ(res.status, 409);
        EXPECT_EQ(error_code(res), "dev_mode");
    }
}

TEST(AdminScenes, UploadRejectsTraversalWithoutWriting) {
    ScenesFixture fx;
    Files files = todo_files();
    files.push_back({"../evil.lua", "return 1\n"});
    HttpResponse res = fx.upload(zip_of(files));
    EXPECT_EQ(res.status, 400) << res.body;
    EXPECT_EQ(error_code(res), "invalid_scene");
    EXPECT_TRUE(fx.files_on_disk().empty());
    std::error_code ec;
    EXPECT_FALSE(fs::exists(fs::path(fx.f.dir) / "evil.lua", ec));
    EXPECT_TRUE(fx.host.log().empty());
}

TEST(AdminScenes, ReuploadKeepsData) {
    ScenesFixture fx;
    ASSERT_EQ(fx.upload(zip_of(todo_files("1.0.0"))).status, 201);
    const fs::path data = fs::path(fx.repo->data_dir("todo")) / "kv.log";
    fs::create_directories(data.parent_path());
    std::ofstream(data) << R"({"op":"put","id":"k","v":{"n":1}})" << "\n";

    HttpResponse res = fx.upload(zip_of(todo_files("1.1.0")));
    ASSERT_EQ(res.status, 201) << res.body;
    Value v = body_json(res);
    EXPECT_EQ(at(v, "version"), Value("1.1.0"));
    EXPECT_GT(at(v, "data_bytes").as_int(), 0);
    EXPECT_TRUE(fs::exists(data));
    EXPECT_EQ(fx.host.log(), (std::vector<std::string>{"exit todo", "installed todo", "exit todo",
                                                       "installed todo"}));
}

TEST(AdminScenes, ExportZip) {
    ScenesFixture fx;
    ASSERT_EQ(fx.upload(zip_of(todo_files())).status, 201);
    const fs::path data = fs::path(fx.repo->data_dir("todo")) / "kv.log";
    fs::create_directories(data.parent_path());
    std::ofstream(data) << R"({"op":"put","id":"k","v":{"n":1}})" << "\n";

    HttpResponse code = fx.call(Method::Get, "/api/v1/scenes/todo/export", "data=0");
    ASSERT_EQ(code.status, 200) << code.body;
    EXPECT_EQ(code.content_type, "application/zip");
    EXPECT_EQ(response_header(code, "content-disposition"),
              R"(attachment; filename="todo-1.0.0.zip")");
    EXPECT_NE(code.body_owner, nullptr);
    EXPECT_EQ(zip_names(code),
              (std::vector<std::string>{"lib/util.lua", "main.lua", "scene.json"}));

    HttpResponse with_data = fx.call(Method::Get, "/api/v1/scenes/todo/export", "data=1");
    ASSERT_EQ(with_data.status, 200);
    EXPECT_EQ(zip_names(with_data),
              (std::vector<std::string>{"data/kv.log", "lib/util.lua", "main.lua", "scene.json"}));
    // No query: code only.
    EXPECT_EQ(zip_names(fx.call(Method::Get, "/api/v1/scenes/todo/export")).size(), 3u);

    EXPECT_EQ(fx.call(Method::Get, "/api/v1/scenes/nope/export").status, 404);
    EXPECT_EQ(fx.call(Method::Get, "/api/v1/scenes/BAD%20ID/export").status, 400);
}

TEST(AdminScenes, OpenCloseReload) {
    ScenesFixture fx;
    ASSERT_TRUE(fx.repo->install(zip_of(todo_files()), {}));

    HttpResponse opened = fx.call(Method::Post, "/api/v1/scenes/todo/open");
    ASSERT_EQ(opened.status, 200) << opened.body;
    EXPECT_EQ(at(body_json(opened), "state"), Value("running"));
    HttpResponse reloaded = fx.call(Method::Post, "/api/v1/scenes/todo/reload");
    ASSERT_EQ(reloaded.status, 200);
    EXPECT_EQ(at(body_json(reloaded), "state"), Value("running"));
    HttpResponse closed = fx.call(Method::Post, "/api/v1/scenes/todo/close");
    ASSERT_EQ(closed.status, 200);
    EXPECT_EQ(at(body_json(closed), "state"), Value("stopped"));
    EXPECT_EQ(fx.host.log(), (std::vector<std::string>{"open todo", "reload todo", "close todo"}));

    EXPECT_EQ(fx.call(Method::Post, "/api/v1/scenes/nope/open").status, 404);
    EXPECT_EQ(fx.call(Method::Post, "/api/v1/scenes/nope/close").status, 404);
    EXPECT_EQ(fx.host.log().size(), 3u);

    // A host error passes through with its status.
    fx.host.fail = wisp::Error{ErrorCode::Busy, "scene queue full"};
    HttpResponse busy = fx.call(Method::Post, "/api/v1/scenes/todo/open");
    EXPECT_EQ(busy.status, 409);
    EXPECT_EQ(error_code(busy), "busy");

    ScenesFixture headless(ScenesFixture::Options{.with_host = false});
    ASSERT_TRUE(headless.repo->install(zip_of(todo_files()), {}));
    HttpResponse res = headless.call(Method::Post, "/api/v1/scenes/todo/open");
    EXPECT_EQ(res.status, 503);
    EXPECT_EQ(error_code(res), "unavailable");
}

// POST /exit closes a scene for good (spec 3.7), where /close is "back".
TEST(AdminScenes, ExitRoute) {
    ScenesFixture fx;
    ASSERT_TRUE(fx.repo->install(zip_of(todo_files()), {}));
    ASSERT_EQ(fx.call(Method::Post, "/api/v1/scenes/todo/open").status, 200);
    HttpResponse exited = fx.call(Method::Post, "/api/v1/scenes/todo/exit");
    ASSERT_EQ(exited.status, 200) << exited.body;
    EXPECT_EQ(at(body_json(exited), "state"), Value("stopped"));
    EXPECT_EQ(fx.host.log(), (std::vector<std::string>{"open todo", "exit todo"}));

    EXPECT_EQ(fx.call(Method::Post, "/api/v1/scenes/nope/exit").status, 404);
    HttpResponse no_csrf =
        fx.f.srv->handle(with_token(make_req(Method::Post, "/api/v1/scenes/todo/exit"), fx.token));
    EXPECT_EQ(no_csrf.status, 403);
    EXPECT_EQ(error_code(no_csrf), "csrf");
    EXPECT_EQ(fx.host.log().size(), 2u);

    ScenesFixture headless(ScenesFixture::Options{.with_host = false});
    ASSERT_TRUE(headless.repo->install(zip_of(todo_files()), {}));
    HttpResponse res = headless.call(Method::Post, "/api/v1/scenes/todo/exit");
    EXPECT_EQ(res.status, 503);
    EXPECT_EQ(error_code(res), "unavailable");
}

TEST(AdminScenes, DeleteNeedsDeviceConfirm) {
    ScenesFixture fx;
    ASSERT_TRUE(fx.repo->install(zip_of(todo_files()), {}));

    HttpResponse res = fx.call(Method::Delete, "/api/v1/scenes/todo", "data=1");
    ASSERT_EQ(res.status, 202) << res.body;
    EXPECT_EQ(at(body_json(res), "pending"), Value(true));
    HttpResponse code_only = fx.call(Method::Delete, "/api/v1/scenes/todo", "data=0");
    EXPECT_EQ(code_only.status, 202);
    ASSERT_EQ(fx.deletes.size(), 2u);
    EXPECT_EQ(fx.deletes[0], (std::pair<std::string, bool>{"todo", true}));
    EXPECT_EQ(fx.deletes[1], (std::pair<std::string, bool>{"todo", false}));
    // Nothing is removed until the device says yes.
    EXPECT_TRUE(fx.repo->info("todo"));

    fx.delete_result = wisp::Error{ErrorCode::Busy, "a dialog is open"};
    HttpResponse busy = fx.call(Method::Delete, "/api/v1/scenes/todo", "data=1");
    EXPECT_EQ(busy.status, 409);
    EXPECT_EQ(error_code(busy), "busy");

    fx.delete_result.reset();
    EXPECT_EQ(fx.call(Method::Delete, "/api/v1/scenes/nope").status, 404);
    EXPECT_EQ(fx.deletes.size(), 3u);
}

// Spec 6.3: a running scene is closed before its package is replaced (its
// store stops writing into data/) and opened again afterwards.
TEST(AdminScenes, UploadOverARunningSceneClosesAndReopensIt) {
    ScenesFixture fx;
    ASSERT_EQ(fx.upload(zip_of(todo_files("1.0.0"))).status, 201);
    ASSERT_EQ(fx.call(Method::Post, "/api/v1/scenes/todo/open").status, 200);
    HttpResponse res = fx.upload(zip_of(todo_files("1.1.0")));
    ASSERT_EQ(res.status, 201) << res.body;
    EXPECT_EQ(at(body_json(res), "state"), Value("running"));
    EXPECT_EQ(fx.host.log(), (std::vector<std::string>{"exit todo", "installed todo", "open todo",
                                                       "exit todo", "open todo"}));

    // A scene that cannot be closed is not replaced.
    fx.host.fail = wisp::Error{ErrorCode::Busy, "scene queue full"};
    HttpResponse busy = fx.upload(zip_of(todo_files("1.2.0")));
    EXPECT_EQ(busy.status, 409) << busy.body;
    EXPECT_EQ(fx.repo->info("todo").value().manifest.version, "1.1.0");
}

// A package whose scene.json no longer validates can still be deleted.
TEST(AdminScenes, DeleteReachesABrokenPackage) {
    ScenesFixture fx;
    ASSERT_TRUE(fx.repo->install(zip_of(todo_files()), {}));
    std::ofstream(fs::path(fx.root) / "todo" / "scene.json") << "{not json";
    HttpResponse res = fx.call(Method::Delete, "/api/v1/scenes/todo", "data=0");
    EXPECT_EQ(res.status, 202) << res.body;
    ASSERT_EQ(fx.deletes.size(), 1u);
}
