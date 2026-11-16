#include "wisp/core/mcp_servers.hpp"

#include <gtest/gtest.h>

#include <cstdio>
#include <filesystem>
#include <memory>
#include <mutex>
#include <string>
#include <vector>

#include "support/fake_devices.hpp"
#include "support/fake_io_queue.hpp"
#include "support/secret_pieces.hpp"
#include "support/temp_dir.hpp"
#include "wisp/base/error.hpp"
#include "wisp/base/json.hpp"
#include "wisp/core/device_registry.hpp"
#include "wisp/core/event_bus.hpp"
#include "wisp/core/http_rules.hpp"
#include "wisp/core/log.hpp"
#include "wisp/core/storage_manager.hpp"

using wisp::ErrorCode;
using wisp::core::DeviceRegistry;
using wisp::core::EventBus;
using wisp::core::McpFailureKind;
using wisp::core::McpHeaderInput;
using wisp::core::McpProbe;
using wisp::core::McpServerInfo;
using wisp::core::McpServerInput;
using wisp::core::McpServerService;
using wisp::core::McpServersState;
using wisp::core::McpServerTarget;
using wisp::core::McpServerUpdate;
using wisp::core::StorageManager;
using wisp::test::FakeIoQueue;
using wisp::test::FakeStorage;
using wisp::test::holds_secret_piece;

namespace {

constexpr const char* kSecret = "tok-SECRET-123456";

struct Fixture {
    Fixture() : dir(wisp::test::temp_dir().string()) {
        registry.add(std::make_shared<FakeStorage>("storage.int", dir, /*removable=*/false));
        bus.start();
        start();
    }

    ~Fixture() {
        svc.reset();
        bus.stop();
    }

    void start() {
        storage = std::make_unique<StorageManager>(registry, bus);
        EXPECT_TRUE(storage->init());
        svc = std::make_unique<McpServerService>(*storage, bus, io.submit_io());
        EXPECT_TRUE(svc->init());
    }

    // Runs the queued saves, then simulates a reboot: a fresh
    // StorageManager and service over the same directory.
    void restart() {
        io.run_all();
        svc.reset();
        start();
    }

    std::string path() const { return dir + "/system/secrets/mcp.json"; }

    static std::string read(const std::string& p) {
        std::string out;
        FILE* f = std::fopen(p.c_str(), "rb");
        if (f == nullptr) return out;
        char buf[512];
        size_t n = 0;
        while ((n = std::fread(buf, 1, sizeof(buf), f)) > 0) out.append(buf, n);
        std::fclose(f);
        return out;
    }

    static void write(const std::string& p, const std::string& text) {
        std::filesystem::create_directories(std::filesystem::path(p).parent_path());
        FILE* f = std::fopen(p.c_str(), "wb");
        ASSERT_NE(f, nullptr);
        std::fputs(text.c_str(), f);
        std::fclose(f);
    }

    McpServerInfo make(McpServerInput in) {
        auto r = svc->create(in);
        EXPECT_TRUE(r) << (r ? "" : r.error().message);
        return r ? r.value() : McpServerInfo{};
    }

    std::string dir;
    FakeIoQueue io;
    EventBus bus;
    DeviceRegistry registry;
    std::unique_ptr<StorageManager> storage;
    std::unique_ptr<McpServerService> svc;
};

McpServerInput server(std::string name, std::string url = "https://mcp.example/mcp",
                      std::vector<McpHeaderInput> headers = {}) {
    return McpServerInput{.name = std::move(name), .url = std::move(url), .headers = headers};
}

std::string str(const wisp::os::LargeString& s) { return std::string(s.begin(), s.end()); }

struct CaptureSink : wisp::LogSink {
    std::mutex mutex;
    std::vector<std::string> lines;
    void write(const wisp::LogRecord& rec) override {
        std::lock_guard<std::mutex> lock(mutex);
        lines.push_back(std::string(rec.tag) + "|" + std::string(rec.msg));
    }
};

}  // namespace

TEST(McpServers, CreateValidatesEveryField) {
    Fixture fx;
    std::vector<McpServerInput> bad;
    bad.push_back(server(""));
    bad.push_back(server(std::string(33, 'n')));
    for (const char* url : {"ftp://x", "http://", "https://u:p@h/mcp", "https://user@h/mcp", ""})
        bad.push_back(server("s", url));
    for (uint32_t t : {999u, 120001u}) {
        McpServerInput in = server("s");
        in.timeout_ms = t;
        bad.push_back(in);
    }
    {
        McpServerInput in = server("s");
        for (int i = 0; i < 9; ++i) in.headers.push_back({"X-H" + std::to_string(i), "v"});
        bad.push_back(in);
    }
    bad.push_back(server("s", "https://mcp.example/mcp", {{"a b", "v"}}));
    bad.push_back(server("s", "https://mcp.example/mcp", {{"X-Key", "a\r\nb"}}));
    bad.push_back(server("s", "https://mcp.example/mcp", {{"X-Key", std::string(1025, 'v')}}));
    bad.push_back(server("s", "https://mcp.example/mcp", {{"X-Key", ""}}));
    bad.push_back(server("s", "https://mcp.example/mcp", {{"X-Key", std::nullopt}}));
    bad.push_back(server("s", "https://mcp.example/mcp", {{"X-Key", "a"}, {"x-key", "b"}}));
    for (const char* reserved : {"accept", "Content-Type", "MCP-Session-Id", "mcp-protocol-version",
                                 "host", "Content-Length", "transfer-encoding", "Connection"}) {
        bad.push_back(server("s", "https://mcp.example/mcp", {{reserved, "v"}}));
    }
    for (const McpServerInput& in : bad) {
        auto r = fx.svc->create(in);
        ASSERT_FALSE(r) << in.name << " " << in.url;
        EXPECT_EQ(r.error().code, ErrorCode::InvalidArgument) << in.url;
    }
    EXPECT_TRUE(fx.svc->list().empty());
    EXPECT_EQ(fx.svc->cell().get(), McpServersState{});
    EXPECT_EQ(fx.io.queued(), 0u);

    McpServerInfo ok = fx.make(server("s", "http://10.0.0.2:8000/mcp/", {{"Authorization", "x"}}));
    EXPECT_EQ(ok.timeout_ms, 60000u);
    EXPECT_EQ(str(ok.url), "http://10.0.0.2:8000/mcp/");
    EXPECT_TRUE(ok.enabled);
    McpServerInput edges = server(std::string(32, 'n'));
    edges.timeout_ms = 1000;
    edges.headers.push_back({"X-Key", std::string(1024, 'v')});
    EXPECT_EQ(fx.make(edges).timeout_ms, 1000u);
    edges.timeout_ms = 120000;
    EXPECT_EQ(fx.make(edges).timeout_ms, 120000u);
    EXPECT_EQ(fx.svc->list().size(), 3u);
}

TEST(McpServers, IdComesFromTheNameAndNeverChanges) {
    Fixture fx;
    McpServerInfo a = fx.make(server("My Tools!"));
    EXPECT_EQ(a.id, "my-tools");
    EXPECT_EQ(fx.make(server("My Tools!")).id, "my-tools-2");
    EXPECT_EQ(fx.make(server("天气")).id, "mcp");
    EXPECT_EQ(fx.make(server("天气")).id, "mcp-2");
    EXPECT_EQ(fx.make(server("__a_b-c__")).id, "__a_b-c__");
    // A name is at most 32 bytes, so the longest one stands in for "long".
    const std::string longest = "abcdefghijklmnopqrstuvwxyzabcdef";
    McpServerInfo longer = fx.make(server(longest));
    EXPECT_EQ(longer.id, longest.substr(0, 16));
    EXPECT_EQ(fx.make(server(longest)).id, longest.substr(0, 14) + "-2");
    // A cut that ends on a separator does not leave it trailing.
    EXPECT_EQ(fx.make(server("abcdefghijklmno pq")).id, "abcdefghijklmno");

    auto r = fx.svc->update(a.id, {.name = "Other"});
    ASSERT_TRUE(r);
    EXPECT_EQ(r.value().id, "my-tools");
    EXPECT_EQ(str(r.value().name), "Other");
    fx.restart();
    ASSERT_TRUE(fx.svc->get("my-tools"));
    EXPECT_EQ(str(fx.svc->get("my-tools")->name), "Other");
}

TEST(McpServers, AtMostEightServers) {
    Fixture fx;
    for (int i = 0; i < 8; ++i) fx.make(server("s"));
    McpFailureKind why = McpFailureKind::None;
    auto r = fx.svc->create(server("s"), &why);
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, ErrorCode::QuotaExceeded);
    EXPECT_EQ(why, McpFailureKind::LimitReached);
    EXPECT_EQ(fx.svc->list().size(), 8u);
    // A refusal for another reason leaves `why` None.
    ASSERT_TRUE(fx.svc->remove("s"));
    why = McpFailureKind::LimitReached;
    EXPECT_FALSE(fx.svc->create(server(""), &why));
    EXPECT_EQ(why, McpFailureKind::None);
}

TEST(McpServers, ReadsNeverCarryHeaderValues) {
    Fixture fx;
    McpServerInfo created =
        fx.make(server("s", "https://mcp.example/mcp", {{"Authorization", kSecret}}));
    auto check = [](const McpServerInfo& s) {
        ASSERT_EQ(s.headers.size(), 1u);
        EXPECT_EQ(str(s.headers[0].name), "Authorization");
        EXPECT_TRUE(s.headers[0].has_value);
        const std::string json = wisp::to_json(wisp::core::to_value(s));
        EXPECT_FALSE(holds_secret_piece(json, kSecret)) << json;
        EXPECT_NE(json.find("\"has_value\":true"), std::string::npos) << json;
        EXPECT_NE(json.find("\"name\":\"Authorization\""), std::string::npos) << json;
    };
    check(created);
    check(fx.svc->list().at(0));
    ASSERT_TRUE(fx.svc->get("s"));
    check(*fx.svc->get("s"));
    auto updated = fx.svc->update("s", {.enabled = false});
    ASSERT_TRUE(updated);
    check(updated.value());
    EXPECT_EQ(wisp::to_json(wisp::core::to_value(created)),
              R"({"enabled":true,"headers":[{"has_value":true,"name":"Authorization"}],)"
              R"("id":"s","name":"s","timeout_ms":60000,"url":"https://mcp.example/mcp"})");

    auto target = fx.svc->resolve_for_request("s");
    ASSERT_TRUE(target);
    ASSERT_EQ(target->headers.size(), 1u);
    EXPECT_EQ(str(target->headers[0].first), "Authorization");
    EXPECT_EQ(str(target->headers[0].second), kSecret);
    EXPECT_EQ(str(target->url), "https://mcp.example/mcp");
    EXPECT_FALSE(fx.svc->resolve_for_request("nope"));
}

TEST(McpServers, UpdateKeepsAValueWhenTheEntryHasNone) {
    Fixture fx;
    fx.make(server("s", "https://mcp.example/mcp", {{"Authorization", "tok-a"}, {"X-Two", "two"}}));
    // The whole list is replaced: X-Two goes, Authorization keeps its value
    // (matched case-insensitively), X-New is added.
    auto r = fx.svc->update(
        "s", {.headers = std::vector<McpHeaderInput>{
                  {"authorization", std::nullopt}, {"X-New", "new"}, {"X-Unknown", std::nullopt}}});
    ASSERT_TRUE(r);
    ASSERT_EQ(r.value().headers.size(), 2u);
    auto t = fx.svc->resolve_for_request("s");
    ASSERT_TRUE(t);
    ASSERT_EQ(t->headers.size(), 2u);
    EXPECT_EQ(str(t->headers[0].first), "authorization");
    EXPECT_EQ(str(t->headers[0].second), "tok-a");
    EXPECT_EQ(str(t->headers[1].first), "X-New");
    EXPECT_EQ(str(t->headers[1].second), "new");
    // A new value replaces the stored one; "" is refused and changes nothing.
    ASSERT_TRUE(
        fx.svc->update("s", {.headers = std::vector<McpHeaderInput>{{"Authorization", "tok-b"}}}));
    EXPECT_EQ(str(fx.svc->resolve_for_request("s")->headers.at(0).second), "tok-b");
    auto empty =
        fx.svc->update("s", {.headers = std::vector<McpHeaderInput>{{"Authorization", ""}}});
    ASSERT_FALSE(empty);
    EXPECT_EQ(empty.error().code, ErrorCode::InvalidArgument);
    EXPECT_EQ(str(fx.svc->resolve_for_request("s")->headers.at(0).second), "tok-b");
    // Headers left out keep the stored list.
    ASSERT_TRUE(fx.svc->update("s", {.name = "n"}));
    EXPECT_EQ(fx.svc->resolve_for_request("s")->headers.size(), 1u);
}

TEST(McpServers, UpdateToAnotherOriginClearsValues) {
    Fixture fx;
    const std::vector<McpHeaderInput> auth{{"Authorization", kSecret}};
    const std::vector<McpHeaderInput> keep{{"Authorization", std::nullopt}};
    fx.make(server("a", "http://a.example/mcp", auth));
    auto moved = fx.svc->update("a", {.url = "https://b.example/mcp", .headers = keep});
    ASSERT_TRUE(moved);
    EXPECT_TRUE(moved.value().headers.empty());
    EXPECT_TRUE(fx.svc->resolve_for_request("a")->headers.empty());
    fx.io.run_all();
    EXPECT_FALSE(holds_secret_piece(Fixture::read(fx.path()), kSecret));

    // Without a headers list the values go too.
    fx.make(server("b", "http://a.example/mcp", auth));
    ASSERT_TRUE(fx.svc->update("b", {.url = "http://a.example:8080/mcp"}));
    EXPECT_TRUE(fx.svc->resolve_for_request("b")->headers.empty());

    // A new value on the new origin is kept.
    fx.make(server("c", "http://a.example/mcp", auth));
    ASSERT_TRUE(fx.svc->update(
        "c", {.url = "https://b.example/mcp",
              .headers = std::vector<McpHeaderInput>{{"Authorization", "tok-new"}}}));
    EXPECT_EQ(str(fx.svc->resolve_for_request("c")->headers.at(0).second), "tok-new");

    // Same origin, another path (and host case): kept.
    fx.make(server("d", "http://a.example/mcp", auth));
    ASSERT_TRUE(fx.svc->update("d", {.url = "http://A.example:80/other/", .headers = keep}));
    EXPECT_EQ(str(fx.svc->resolve_for_request("d")->headers.at(0).second), kSecret);
    ASSERT_TRUE(fx.svc->update("d", {.url = "http://a.example/third"}));
    EXPECT_EQ(str(fx.svc->resolve_for_request("d")->headers.at(0).second), kSecret);

    // An update URL is held to the create rules.
    auto bad = fx.svc->update("d", {.url = "https://u:p@a.example/mcp"});
    ASSERT_FALSE(bad);
    EXPECT_EQ(bad.error().code, ErrorCode::InvalidArgument);
}

TEST(McpServers, CellCountsEnabledAndRevision) {
    Fixture fx;
    EXPECT_EQ(fx.svc->cell().get(), McpServersState{});
    fx.make(server("a"));
    const uint32_t r1 = fx.svc->cell().get().revision;
    fx.make(server("b"));
    const uint32_t r2 = fx.svc->cell().get().revision;
    EXPECT_GT(r2, r1);
    ASSERT_TRUE(fx.svc->update("b", {.enabled = false}));
    McpServersState s = fx.svc->cell().get();
    EXPECT_EQ(s.count, 2u);
    EXPECT_EQ(s.enabled, 1u);
    EXPECT_GT(s.revision, r2);

    // Refused changes leave the cell alone.
    EXPECT_FALSE(fx.svc->update("b", {.url = "ftp://x"}));
    EXPECT_FALSE(fx.svc->update("zz", {.enabled = true}));
    EXPECT_FALSE(fx.svc->remove("zz"));
    EXPECT_FALSE(fx.svc->create(server("")));
    EXPECT_EQ(fx.svc->cell().get(), s);

    ASSERT_TRUE(fx.svc->remove("a"));
    McpServersState after = fx.svc->cell().get();
    EXPECT_EQ(after.count, 1u);
    EXPECT_EQ(after.enabled, 0u);
    EXPECT_GT(after.revision, s.revision);
}

TEST(McpServers, RevisionIsPerServer) {
    Fixture fx;
    McpServerInfo a = fx.make(server("a"));
    McpServerInfo b = fx.make(server("b"));
    EXPECT_NE(a.revision, b.revision);
    auto updated = fx.svc->update("a", {.timeout_ms = 5000u});
    ASSERT_TRUE(updated);
    EXPECT_NE(updated.value().revision, a.revision);
    EXPECT_EQ(fx.svc->get("a")->revision, updated.value().revision);
    EXPECT_EQ(fx.svc->get("b")->revision, b.revision);
    EXPECT_EQ(fx.svc->resolve_for_request("a")->revision, updated.value().revision);
    // A server removed and made again under the same id never reuses a
    // revision a client may have keyed a session by.
    const uint32_t old = fx.svc->get("b")->revision;
    ASSERT_TRUE(fx.svc->remove("b"));
    EXPECT_NE(fx.make(server("b")).revision, old);
}

TEST(McpServers, SavesThroughTheWorkerAndReloads) {
    Fixture fx;
    fx.make(server("a", "https://a.example/mcp/", {{"Authorization", kSecret}}));
    McpServerInput b = server("b", "http://10.0.0.2:8000/mcp");
    b.enabled = false;
    b.timeout_ms = 2000;
    fx.make(b);
    ASSERT_TRUE(fx.svc->update("a", {.name = "Alpha"}));
    // Nothing touched flash on the calling thread; one job holds the newest.
    EXPECT_FALSE(std::filesystem::exists(fx.path()));
    EXPECT_EQ(fx.io.queued(), 1u);
    EXPECT_EQ(fx.io.run_all(), 1u);
    const std::string file = Fixture::read(fx.path());
    EXPECT_NE(file.find("\"version\":1"), std::string::npos) << file;
    EXPECT_NE(file.find("Alpha"), std::string::npos) << file;

    const std::vector<McpServerInfo> before = fx.svc->list();
    fx.restart();
    const std::vector<McpServerInfo> after = fx.svc->list();
    ASSERT_EQ(after.size(), 2u);
    for (size_t i = 0; i < after.size(); ++i) {
        EXPECT_EQ(wisp::to_json(wisp::core::to_value(after[i])),
                  wisp::to_json(wisp::core::to_value(before[i])));
    }
    auto t = fx.svc->resolve_for_request("a");
    ASSERT_TRUE(t);
    EXPECT_EQ(str(t->url), "https://a.example/mcp/");
    EXPECT_EQ(str(t->name), "Alpha");
    ASSERT_EQ(t->headers.size(), 1u);
    EXPECT_EQ(str(t->headers[0].second), kSecret);
    EXPECT_EQ(fx.svc->resolve_for_request("b")->timeout_ms, 2000u);
    McpServersState s = fx.svc->cell().get();
    EXPECT_EQ(s.count, 2u);
    EXPECT_EQ(s.enabled, 1u);
}

TEST(McpServers, ABadStoredEntryIsDroppedAlone) {
    Fixture fx;
    fx.svc.reset();
    Fixture::write(fx.path(),
                   R"({"version":1,"servers":[)"
                   R"({"id":"good","name":"Good","url":"https://g.example/mcp","enabled":true,)"
                   R"("timeout_ms":60000,"headers":[{"name":"X-Key","value":"v1"}]},)"
                   R"({"id":"bad","name":"Bad","url":"ftp://secret-host.example/x","enabled":true,)"
                   R"("timeout_ms":60000,"headers":[]}]})");
    CaptureSink sink;
    wisp::log::add_sink(&sink);
    fx.start();
    wisp::log::remove_sink(&sink);
    const auto list = fx.svc->list();
    ASSERT_EQ(list.size(), 1u);
    EXPECT_EQ(list[0].id, "good");
    EXPECT_EQ(str(fx.svc->resolve_for_request("good")->headers.at(0).second), "v1");
    size_t warnings = 0;
    for (const std::string& line : sink.lines) {
        EXPECT_EQ(line.find("secret-host"), std::string::npos) << line;
        if (line.find("dropped") != std::string::npos) ++warnings;
    }
    EXPECT_EQ(warnings, 1u);
}

TEST(McpServers, CorruptFileStartsEmpty) {
    Fixture fx;
    fx.svc.reset();
    Fixture::write(fx.path(), R"({"version":1,"servers":[{"id":"a", "headers":[{"value":"tok-SE)");
    CaptureSink sink;
    wisp::log::add_sink(&sink);
    fx.start();
    wisp::log::remove_sink(&sink);
    EXPECT_TRUE(fx.svc->list().empty());
    EXPECT_EQ(fx.svc->cell().get().count, 0u);
    for (const std::string& line : sink.lines) EXPECT_EQ(line.find("tok-SE"), std::string::npos);
    // The next change rewrites it.
    fx.make(server("a"));
    fx.restart();
    EXPECT_EQ(fx.svc->list().size(), 1u);
}

TEST(McpServers, ClearForgetsAndDeletesTheFile) {
    Fixture fx;
    fx.make(server("a", "https://a.example/mcp", {{"Authorization", kSecret}}));
    fx.io.run_all();
    ASSERT_TRUE(std::filesystem::exists(fx.path()));
    fx.make(server("b"));  // queued, superseded by clear()
    const uint32_t before = fx.svc->cell().get().revision;
    fx.svc->clear();
    EXPECT_TRUE(fx.svc->list().empty());
    McpServersState s = fx.svc->cell().get();
    EXPECT_EQ(s.count, 0u);
    EXPECT_GT(s.revision, before);
    EXPECT_EQ(fx.io.queued(), 1u);
    fx.io.run_all();
    EXPECT_FALSE(std::filesystem::exists(fx.path()));
    fx.restart();
    EXPECT_TRUE(fx.svc->list().empty());
}

TEST(McpServers, TestRunsTheProberWithTheValues) {
    Fixture fx;
    fx.make(server("a", "https://a.example/mcp", {{"Authorization", kSecret}}));
    auto none = fx.svc->test("a");
    ASSERT_FALSE(none);
    EXPECT_EQ(none.error().code, ErrorCode::Unsupported);

    McpServerTarget seen;
    fx.svc->set_prober([&](const McpServerTarget& t) {
        seen = t;
        McpProbe p;
        p.ok = true;
        p.protocol = "2025-06-18";
        return p;
    });
    auto missing = fx.svc->test("nope");
    ASSERT_FALSE(missing);
    EXPECT_EQ(missing.error().code, ErrorCode::NotFound);
    auto r = fx.svc->test("a");
    ASSERT_TRUE(r);
    EXPECT_TRUE(r.value().ok);
    EXPECT_EQ(r.value().protocol, "2025-06-18");
    EXPECT_EQ(seen.id, "a");
    ASSERT_EQ(seen.headers.size(), 1u);
    EXPECT_EQ(str(seen.headers[0].second), kSecret);

    fx.svc->set_prober(nullptr);
    EXPECT_EQ(fx.svc->test("a").error().code, ErrorCode::Unsupported);
}

TEST(McpServers, NoLogLineCarriesAValue) {
    CaptureSink sink;
    wisp::log::add_sink(&sink);
    {
        Fixture fx;
        fx.make(server("a", "https://a.example/mcp", {{"Authorization", kSecret}}));
        ASSERT_TRUE(
            fx.svc->update("a", {.headers = std::vector<McpHeaderInput>{
                                     {"Authorization", std::nullopt}, {"X-Other", kSecret}}}));
        EXPECT_FALSE(fx.svc->update(
            "a", {.headers = std::vector<McpHeaderInput>{{"X-Bad", std::string(kSecret) + "\n"}}}));
        fx.restart();
        ASSERT_TRUE(fx.svc->remove("a"));
        fx.io.run_all();
    }
    wisp::log::remove_sink(&sink);
    ASSERT_FALSE(sink.lines.empty());
    for (const std::string& line : sink.lines) {
        EXPECT_FALSE(holds_secret_piece(line, kSecret)) << line;
    }
}

TEST(HttpRules, MatchesTheProfilesRules) {
    using wisp::core::http_header_syntax_ok;
    using wisp::core::url_has_userinfo;
    using wisp::core::url_origin;
    using wisp::core::valid_http_url;
    for (const char* bad :
         {"ftp://x", "api.example.com", "", "https://", "http:///", "javascript:1",
          "https://ex ample.com", "https://a.example/\r\nX: y", "https://@/", "https://:443",
          "https://user@:443/v1", "http://@", "https://[]/v1", "https://[]:443/v1"}) {
        EXPECT_FALSE(valid_http_url(bad)) << bad;
    }
    for (const char* good : {"https://api.example.com/v1", "http://10.0.0.2:8000/v1/",
                             "http://[fd00::2]:8000/v1", "https://a.example"}) {
        EXPECT_TRUE(valid_http_url(good)) << good;
    }
    EXPECT_EQ(url_origin("https://a.example/v1"), "https://a.example:443");
    EXPECT_EQ(url_origin("https://A.Example:443/v2"), "https://a.example:443");
    EXPECT_EQ(url_origin("https://user@a.example/v1"), "https://a.example:443");
    EXPECT_EQ(url_origin("http://10.0.0.2"), "http://10.0.0.2:80");
    EXPECT_EQ(url_origin("http://[FD00::2]:8000/v2"), "http://[fd00::2]:8000");
    EXPECT_EQ(url_origin(""), "");
    EXPECT_NE(url_origin("http://a.example/v1"), url_origin("https://a.example/v1"));
    EXPECT_NE(url_origin("https://a.example/v1"), url_origin("https://a.example:8443/v1"));

    EXPECT_TRUE(url_has_userinfo("https://u:p@h/mcp"));
    EXPECT_TRUE(url_has_userinfo("https://u@h"));
    EXPECT_FALSE(url_has_userinfo("https://h/mcp?x=a@b"));
    EXPECT_FALSE(url_has_userinfo("https://h/a@b"));

    EXPECT_TRUE(http_header_syntax_ok("X-Key", "v"));
    EXPECT_FALSE(http_header_syntax_ok("a b", "v"));
    EXPECT_FALSE(http_header_syntax_ok("k", "a\nb"));
    EXPECT_FALSE(http_header_syntax_ok("k", "a\rb"));
    EXPECT_FALSE(http_header_syntax_ok("k", std::string_view("a\0b", 3)));
    EXPECT_FALSE(http_header_syntax_ok("", "v"));
}
