// SceneHttp: the scenes' HTTP proxy (scene-runtime-core spec 5.3).

#include "wisp/net/scene_http.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <utility>
#include <vector>

#include "support/fake_http.hpp"
#include "support/wait.hpp"
#include "wisp/core/log.hpp"
#include "wisp/port/sim/sim_os.hpp"

namespace {

using wisp::net::AgentHttpLimits;
using wisp::net::HttpPrincipal;
using wisp::net::kAgentHttpId;
using wisp::net::Method;
using wisp::net::SceneHttp;
using wisp::net::SceneHttpLimits;
using wisp::net::SceneHttpRequest;
using wisp::net::SceneHttpResult;
using wisp::net::TransportError;
using wisp::test::wait_until;

const std::vector<std::string> kHosts = {"api.example.com"};

SceneHttpRequest get(std::string url, std::string scene = "s") {
    SceneHttpRequest r;
    r.scene_id = std::move(scene);
    r.instance = 1;
    r.method = Method::Get;
    r.url = std::move(url);
    return r;
}

// The Agent principal's requests do not need a declared scene (agent-tools
// spec 3.2); scene_id is overwritten with kAgentHttpId by submit() anyway.
SceneHttpRequest agent_get(std::string url) {
    SceneHttpRequest r;
    r.principal = HttpPrincipal::Agent;
    r.instance = 1;
    r.method = Method::Get;
    r.url = std::move(url);
    return r;
}

// What done() delivered, collected from whichever thread called it.
struct Results {
    std::mutex mu;
    std::vector<SceneHttpResult> got;

    std::function<void(SceneHttpResult)> sink() {
        return [this](SceneHttpResult r) {
            std::lock_guard<std::mutex> lock(mu);
            got.push_back(std::move(r));
        };
    }
    size_t size() {
        std::lock_guard<std::mutex> lock(mu);
        return got.size();
    }
};

class SceneHttpTest : public ::testing::Test {
protected:
    void SetUp() override {
        wisp::log::reset_for_test();
        wisp::log::init(64 * 1024);
    }

    void TearDown() override {
        client.hold = false;
        http.reset();
        wisp::log::reset_for_test();
    }

    void make(SceneHttpLimits limits = {}) {
        http = std::make_unique<SceneHttp>(client, limits, [this] { return now.load(); });
        ASSERT_TRUE(http->start());
        http->set_hosts("s", kHosts);
    }

    // Submits `req` and waits for its result.
    SceneHttpResult run(SceneHttpRequest req) {
        Results r;
        http->submit(std::move(req), r.sink());
        EXPECT_TRUE(wait_until([&] { return r.size() == 1; }, 5000));
        std::lock_guard<std::mutex> lock(r.mu);
        if (r.got.empty()) return SceneHttpResult{"(no result)"};
        return std::move(r.got.front());
    }

    wisp::test::FakeHttpClient client;
    std::atomic<uint64_t> now{1000};
    std::unique_ptr<SceneHttp> http;
};

TEST(SceneHttpRules, ExactHostMatch) {
    EXPECT_EQ(SceneHttp::check_url("https://api.example.com/x", kHosts), "");
    EXPECT_EQ(SceneHttp::check_url("https://API.example.com", kHosts), "");
    EXPECT_EQ(SceneHttp::check_url("https://api.example.com:8443/x?q=1#f", kHosts), "");
    EXPECT_EQ(SceneHttp::check_url("https://api.example.com?q=1", kHosts), "");
    for (const char* bad : {
             "https://x.api.example.com/",
             "https://api.example.com.evil.com/",
             "https://example.com/",
             "https://api.example.com./",
             "https://user@api.example.com/",
             "https://api.example.com@evil.com/",
             "https://evil.com\\@api.example.com/",
             "https://api.example.com%2eevil.com/",
             "https:///x",
             "https://api.example.com:/x",
             "https://api.example.com:99999/x",
             "https://api.example.com:8a/x",
             "https://api.example.com/x y",
             "https://api.example.com/\r\nX: y",
         })
        EXPECT_EQ(SceneHttp::check_url(bad, kHosts), "permission_denied") << bad;
    EXPECT_EQ(SceneHttp::check_url("https://api.example.com/", {}), "permission_denied");
}

TEST(SceneHttpRules, RejectsNonHttps) {
    for (const char* url : {"http://api.example.com/x", "ftp://api.example.com/", "api.example.com",
                            "", "https:/api.example.com", "wss://api.example.com/"})
        EXPECT_EQ(SceneHttp::check_url(url, kHosts), "not_https") << url;
    EXPECT_EQ(SceneHttp::check_url("HTTPS://api.example.com/", kHosts), "");
}

TEST(SceneHttpRules, ForbiddenHeaders) {
    for (const char* name :
         {"Cookie", "cookie2", "Proxy-Authorization", "Host", "Connection", "Content-Length",
          "Transfer-Encoding", "Upgrade", "TE", "Trailer", "Keep-Alive", "Expect", "X-Wisp",
          "Proxy-Anything", "Sec-Fetch-Mode", "", "Bad Name", "a:b", "\xe4\xb8\xad"})
        EXPECT_FALSE(SceneHttp::header_allowed(name, "v")) << name;
    for (const char* name : {"Authorization", "X-Api-Key", "Accept", "Content-Type", "User-Agent"})
        EXPECT_TRUE(SceneHttp::header_allowed(name, "v")) << name;
    EXPECT_FALSE(SceneHttp::header_allowed("X-A", "a\r\nX-B: c"));
    EXPECT_FALSE(SceneHttp::header_allowed("X-A", "a\nb"));
    EXPECT_FALSE(SceneHttp::header_allowed("X-A", "a\rb"));
    EXPECT_FALSE(SceneHttp::header_allowed("X-A", std::string_view("a\0b", 3)));
    EXPECT_TRUE(SceneHttp::header_allowed("X-A", ""));
}

TEST_F(SceneHttpTest, ExactHostMatch) {
    make();
    client.next = {.status = 200, .body = "ok"};
    auto r = run(get("https://API.example.com/x"));
    EXPECT_EQ(r.error, "");
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(std::string(reinterpret_cast<const char*>(r.body.data()), r.body.size()), "ok");
    ASSERT_EQ(client.calls(), 1u);
    EXPECT_EQ(client.last()->url, "https://API.example.com/x");

    EXPECT_EQ(run(get("https://x.api.example.com/")).error, "permission_denied");
    EXPECT_EQ(run(get("https://api.example.com.evil.com/")).error, "permission_denied");
    // Another scene's hosts are not this one's.
    EXPECT_EQ(run(get("https://api.example.com/", "other")).error, "permission_denied");
    http->set_hosts("other", {"b.example.com"});
    EXPECT_EQ(run(get("https://api.example.com/", "other")).error, "permission_denied");
    EXPECT_EQ(run(get("https://b.example.com/", "other")).error, "");
    EXPECT_EQ(client.calls(), 2u);
}

TEST_F(SceneHttpTest, RejectsNonHttps) {
    make();
    EXPECT_EQ(run(get("http://api.example.com/x")).error, "not_https");
    EXPECT_EQ(client.calls(), 0u);
}

TEST_F(SceneHttpTest, ResponseLimit256KB) {
    make();
    client.next = {.status = 200, .body = std::string(262144, 'x')};
    auto r = run(get("https://api.example.com/big"));
    EXPECT_EQ(r.error, "");
    EXPECT_EQ(r.body.size(), 262144u);
    ASSERT_TRUE(client.last());
    EXPECT_EQ(client.last()->max_response_bytes, 262144u);
    EXPECT_TRUE(static_cast<bool>(client.last()->on_body));

    client.next = {.status = 200, .body = std::string(262145, 'x')};
    r = run(get("https://api.example.com/big"));
    EXPECT_EQ(r.error, "too_large");
    EXPECT_EQ(r.status, 0);
    EXPECT_EQ(r.body.size(), 0u);

    // A client that ignored the cap is still held to it.
    client.ignore_limit = true;
    r = run(get("https://api.example.com/big"));
    EXPECT_EQ(r.error, "too_large");
    EXPECT_EQ(r.body.size(), 0u);
}

TEST_F(SceneHttpTest, RequestSizeLimits) {
    make();
    auto big_body = get("https://api.example.com/");
    big_body.method = Method::Post;
    big_body.body.assign(SceneHttp::kMaxRequestBody + 1, 'b');
    EXPECT_EQ(run(std::move(big_body)).error, "too_large");
    std::string long_url = "https://api.example.com/";
    long_url.append(SceneHttp::kMaxUrlBytes, 'p');
    EXPECT_EQ(run(get(long_url)).error, "too_large");
    auto big_header = get("https://api.example.com/");
    big_header.headers.emplace_back("X-Big", std::string(SceneHttp::kMaxHeaderBytes, 'h'));
    EXPECT_EQ(run(std::move(big_header)).error, "too_large");
    auto many_big = get("https://api.example.com/");
    for (int i = 0; i < 5; ++i)
        many_big.headers.emplace_back("X-" + std::to_string(i), std::string(900, 'h'));
    EXPECT_EQ(run(std::move(many_big)).error, "too_large");
    EXPECT_EQ(client.calls(), 0u);

    auto fits = get("https://api.example.com/");
    fits.method = Method::Post;
    fits.body.assign(SceneHttp::kMaxRequestBody, 'b');
    EXPECT_EQ(run(std::move(fits)).error, "");
    ASSERT_EQ(client.calls(), 1u);
    EXPECT_EQ(client.last()->method, Method::Post);
    EXPECT_EQ(wisp::test::FakeHttpClient::body_of(*client.last()).size(),
              SceneHttp::kMaxRequestBody);
}

TEST_F(SceneHttpTest, ThirtyPerMinute) {
    make();
    for (int i = 0; i < 30; ++i) {
        now += 1000;
        ASSERT_EQ(run(get("https://api.example.com/")).error, "") << i;
    }
    EXPECT_EQ(run(get("https://api.example.com/")).error, "rate_limited");
    EXPECT_EQ(client.calls(), 30u);
    // Rejected requests do not count; other scenes have their own window.
    http->set_hosts("t", kHosts);
    EXPECT_EQ(run(get("https://api.example.com/", "t")).error, "");
    // The first request falls out of the window 60 s after it was sent.
    now += 60000 - 30000;
    EXPECT_EQ(run(get("https://api.example.com/")).error, "rate_limited");
    now += 1000;
    EXPECT_EQ(run(get("https://api.example.com/")).error, "");
    EXPECT_EQ(run(get("https://api.example.com/")).error, "rate_limited");
    now += 60000;
    EXPECT_EQ(run(get("https://api.example.com/")).error, "");
}

TEST_F(SceneHttpTest, FullQueueIsRateLimited) {
    make();
    client.hold = true;
    Results r;
    http->submit(get("https://api.example.com/0"), r.sink());
    ASSERT_TRUE(wait_until([&] { return client.held.load() == 1; }, 2000));
    for (int i = 0; i < 8; ++i) http->submit(get("https://api.example.com/q"), r.sink());
    EXPECT_EQ(r.size(), 0u);
    http->submit(get("https://api.example.com/9"), r.sink());
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(r.got[0].error, "rate_limited");
    client.hold = false;
    ASSERT_TRUE(wait_until([&] { return r.size() == 10; }, 5000));
    EXPECT_EQ(client.calls(), 9u);
}

TEST_F(SceneHttpTest, TimeoutIs15s) {
    make();
    run(get("https://api.example.com/"));
    ASSERT_TRUE(client.last());
    EXPECT_EQ(client.last()->timeout_ms, 15000u);
}

// Every read on its own is well inside 15 s; the request as a whole is
// not, so the deadline ends it.
TEST_F(SceneHttpTest, TimeoutIsOneDeadlineForTheWholeRequest) {
    make();
    client.next = {.status = 200, .body = std::string(8 * 4096, 'x')};
    client.before_piece = [this] { now += 5000; };
    auto r = run(get("https://api.example.com/trickle"));
    EXPECT_EQ(r.error, "timeout");
    EXPECT_EQ(r.status, 0);
    EXPECT_EQ(r.body.size(), 0u);
    // Pieces at +5, +10 s; the one at +15 s is refused.
    EXPECT_EQ(client.pieces.load(), 2);
}

// A call blocked inside the client (a server that trickles bytes into
// one library read, which polls nothing) is ended at the deadline by the
// watch shutting its socket down.
TEST_F(SceneHttpTest, BlockedCallEndsAtTheDeadline) {
    SceneHttpLimits limits;
    limits.timeout_ms = 300;
    make(limits);
    client.hold = true;
    Results r;
    const uint64_t t0 = wisp::os::now_ms();
    http->submit(get("https://api.example.com/stall"), r.sink());
    ASSERT_TRUE(wait_until([&] { return r.size() == 1; }, 3000));
    EXPECT_GE(wisp::os::now_ms() - t0, 250u);
    EXPECT_EQ(r.got[0].error, "timeout");
    EXPECT_TRUE(client.saw_cancel.load());
    // The worker is free again.
    client.hold = false;
    EXPECT_EQ(run(get("https://api.example.com/next")).error, "");
}

TEST_F(SceneHttpTest, TransportErrors) {
    make();
    for (auto [t, code] :
         {std::pair{TransportError::Dns, "dns"}, std::pair{TransportError::Connect, "connect"},
          std::pair{TransportError::Tls, "tls"}, std::pair{TransportError::Timeout, "timeout"},
          std::pair{TransportError::Other, "other"}}) {
        client.next = {.transport = t};
        auto r = run(get("https://api.example.com/"));
        EXPECT_EQ(r.error, code);
        EXPECT_EQ(r.status, 0);
    }
    // An HTTP error status is a response, headers and all.
    client.next = {.status = 302,
                   .body = "moved",
                   .headers = {{"location", "https://elsewhere.example.com/"}}};
    auto r = run(get("https://api.example.com/"));
    EXPECT_EQ(r.error, "");
    EXPECT_EQ(r.status, 302);
    ASSERT_EQ(r.headers.size(), 1u);
    EXPECT_EQ(r.headers[0].first, "location");
}

TEST_F(SceneHttpTest, ForbiddenHeaders) {
    make();
    for (const char* name : {"Cookie", "Proxy-Authorization", "Host"}) {
        auto req = get("https://api.example.com/");
        req.headers.emplace_back(name, "v");
        EXPECT_EQ(run(std::move(req)).error, "permission_denied") << name;
    }
    auto crlf = get("https://api.example.com/");
    crlf.headers.emplace_back("X-A", "a\r\nCookie: b");
    EXPECT_EQ(run(std::move(crlf)).error, "permission_denied");
    auto many = get("https://api.example.com/");
    for (int i = 0; i < 17; ++i) many.headers.emplace_back("X-H" + std::to_string(i), "v");
    EXPECT_EQ(run(std::move(many)).error, "permission_denied");
    EXPECT_EQ(client.calls(), 0u);

    auto ok = get("https://api.example.com/");
    ok.headers = {{"Authorization", "Bearer k"}, {"X-Api-Key", "k2"}};
    for (int i = 0; i < 14; ++i) ok.headers.emplace_back("X-H" + std::to_string(i), "v");
    EXPECT_EQ(run(std::move(ok)).error, "");
    ASSERT_EQ(client.calls(), 1u);
    const auto sent = client.last()->headers;
    ASSERT_EQ(sent.size(), 16u);
    EXPECT_EQ(sent[0], (std::pair<std::string, std::string>{"Authorization", "Bearer k"}));
    EXPECT_EQ(sent[1], (std::pair<std::string, std::string>{"X-Api-Key", "k2"}));
}

TEST_F(SceneHttpTest, CancelSceneSuppressesCallbackAndAborts) {
    make();
    http->set_hosts("t", kHosts);
    client.hold = true;
    Results s;
    Results t;
    http->submit(get("https://api.example.com/inflight"), s.sink());
    ASSERT_TRUE(wait_until([&] { return client.held.load() == 1; }, 2000));
    http->submit(get("https://api.example.com/queued"), s.sink());
    http->submit(get("https://api.example.com/other", "t"), t.sink());

    http->cancel_scene("s");
    // The request in flight was told to stop; the worker moves on to the
    // other scene's request, which is held in turn.
    ASSERT_TRUE(wait_until([&] { return client.saw_cancel.load(); }, 2000));
    ASSERT_TRUE(wait_until([&] { return client.calls() == 2 && client.held.load() == 1; }, 2000));
    EXPECT_EQ(client.last()->url, "https://api.example.com/other");
    client.hold = false;
    ASSERT_TRUE(wait_until([&] { return t.size() == 1; }, 2000));
    wisp::os::sleep_ms(50);
    EXPECT_EQ(s.size(), 0u);
    EXPECT_EQ(client.calls(), 2u);

    // The scene can send again (its window still counts what it sent).
    EXPECT_EQ(run(get("https://api.example.com/again")).error, "");
}

// cancel_scene() runs on the UI thread: it only asks the watch, and the
// socket shutdown, however slow, happens elsewhere.
TEST_F(SceneHttpTest, CancelSceneDoesNotWaitForTheShutdown) {
    make();
    client.hold = true;
    client.shutdown_ms = 500;
    Results r;
    http->submit(get("https://api.example.com/inflight"), r.sink());
    ASSERT_TRUE(wait_until([&] { return client.held.load() == 1; }, 2000));
    const uint64_t t0 = wisp::os::now_ms();
    http->cancel_scene("s");
    EXPECT_LT(wisp::os::now_ms() - t0, 100u);
    ASSERT_TRUE(wait_until([&] { return client.saw_cancel.load(); }, 3000));
    client.hold = false;
    client.shutdown_ms = 0;
    EXPECT_EQ(run(get("https://api.example.com/next")).error, "");
    EXPECT_EQ(r.size(), 0u);
}

TEST_F(SceneHttpTest, StopDropsEverythingWithoutCallbacks) {
    make();
    client.hold = true;
    Results r;
    http->submit(get("https://api.example.com/inflight"), r.sink());
    ASSERT_TRUE(wait_until([&] { return client.held.load() == 1; }, 2000));
    http->submit(get("https://api.example.com/queued"), r.sink());
    http->stop();
    EXPECT_TRUE(client.saw_cancel.load());
    EXPECT_EQ(r.size(), 0u);
    // A stopped proxy answers at once.
    http->submit(get("https://api.example.com/late"), r.sink());
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(r.got[0].error, "other");
}

// Without its watch timer the proxy does not run: requests fail at once.
TEST(SceneHttpStart, NoWatchTimerLeavesItDisabled) {
    wisp::test::FakeHttpClient client;
    SceneHttp http(client, SceneHttpLimits{}, [] { return wisp::os::now_ms(); });
    wisp::port::sim::set_timer_failure(true);
    auto started = http.start();
    wisp::port::sim::set_timer_failure(false);
    ASSERT_FALSE(started.is_ok());
    EXPECT_EQ(started.error().code, wisp::ErrorCode::NoMemory);
    http.set_hosts("s", kHosts);
    Results r;
    http.submit(get("https://api.example.com/"), r.sink());
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(r.got[0].error, "other");
    EXPECT_EQ(client.calls(), 0u);
}

TEST_F(SceneHttpTest, AuthorizationNeverLogged) {
    make();
    auto secret = [](const char* url) {
        auto req = get(url);
        req.method = Method::Post;
        req.headers = {{"Authorization", "Bearer TOPSECRET-1"}, {"X-Api-Key", "KEYSECRET-2"}};
        req.body = "BODYSECRET-3";
        return req;
    };
    client.next = {.status = 200, .body = "fine"};
    EXPECT_EQ(run(secret("https://api.example.com/p?key=QUERYSECRET-4")).error, "");
    client.next = {.transport = TransportError::Tls};
    EXPECT_EQ(run(secret("https://api.example.com/p?key=QUERYSECRET-4")).error, "tls");
    EXPECT_EQ(run(secret("https://evil.example.com/p?key=QUERYSECRET-4")).error,
              "permission_denied");
    EXPECT_EQ(run(secret("http://api.example.com/p?key=QUERYSECRET-4")).error, "not_https");
    auto cookie = secret("https://api.example.com/p?key=QUERYSECRET-4");
    cookie.headers.emplace_back("Cookie", "COOKIESECRET-5");
    EXPECT_EQ(run(std::move(cookie)).error, "permission_denied");
    for (int i = 0; i < 40; ++i) run(secret("https://api.example.com/p?key=QUERYSECRET-4"));

    const std::string log = wisp::log::ring_snapshot();
    EXPECT_NE(log.find("evil.example.com"), std::string::npos) << log;
    for (const char* s :
         {"TOPSECRET", "KEYSECRET", "BODYSECRET", "QUERYSECRET", "COOKIESECRET", "/p"})
        EXPECT_EQ(log.find(s), std::string::npos) << s << " in:\n" << log;
}

// Deep standby (spec 4.4): accepted requests wait in a per-scene list of
// at most four instead of going out.
TEST_F(SceneHttpTest, DeferringQueuesUpToFourPerScene) {
    make();
    http->set_hosts("t", kHosts);
    http->set_deferring(true);
    Results s;
    Results t;
    for (int i = 0; i < 4; ++i)
        http->submit(get("https://api.example.com/" + std::to_string(i)), s.sink());
    EXPECT_EQ(s.size(), 0u);
    EXPECT_EQ(http->deferred_count(), 4u);
    EXPECT_FALSE(http->idle());

    http->submit(get("https://api.example.com/4"), s.sink());
    ASSERT_EQ(s.size(), 1u);
    EXPECT_EQ(s.got[0].error, "deferred_queue_full");
    EXPECT_FALSE(s.got[0].deferred);

    // Another scene has a list of its own.
    http->submit(get("https://api.example.com/t", "t"), t.sink());
    EXPECT_EQ(t.size(), 0u);
    EXPECT_EQ(http->deferred_count(), 5u);

    wisp::os::sleep_ms(50);
    EXPECT_EQ(client.calls(), 0u);
}

// Resuming sends the waiting requests in the order they were submitted,
// a run queue's worth at a time, each result marked deferred.
TEST_F(SceneHttpTest, ResumeSendsInOrderMarkedDeferred) {
    SceneHttpLimits limits;
    limits.queue = 2;
    make(limits);
    http->set_hosts("t", kHosts);
    http->set_deferring(true);
    Results r;
    http->submit(get("https://api.example.com/0"), r.sink());
    http->submit(get("https://api.example.com/1", "t"), r.sink());
    http->submit(get("https://api.example.com/2"), r.sink());
    http->submit(get("https://api.example.com/3", "t"), r.sink());
    http->submit(get("https://api.example.com/4"), r.sink());
    EXPECT_EQ(r.size(), 0u);

    http->set_deferring(false);
    ASSERT_TRUE(wait_until([&] { return r.size() == 5; }, 5000));
    EXPECT_EQ(client.urls(),
              (std::vector<std::string>{"https://api.example.com/0", "https://api.example.com/1",
                                        "https://api.example.com/2", "https://api.example.com/3",
                                        "https://api.example.com/4"}));
    {
        std::lock_guard<std::mutex> lock(r.mu);
        for (const auto& res : r.got) {
            EXPECT_EQ(res.error, "");
            EXPECT_TRUE(res.deferred);
        }
    }
    EXPECT_EQ(http->deferred_count(), 0u);
    EXPECT_TRUE(wait_until([&] { return http->idle(); }, 2000));

    // With deferring off a request goes straight out, unmarked.
    auto now_res = run(get("https://api.example.com/now"));
    EXPECT_EQ(now_res.error, "");
    EXPECT_FALSE(now_res.deferred);
}

// Deferring starts while requests sit in the run queue (a window closing,
// Wi-Fi dropping): the ones not started yet go back to wait, in order,
// instead of running on a radio that is going off.
TEST_F(SceneHttpTest, QueuedRequestsGoBackWhenDeferringStarts) {
    make();
    client.hold = true;
    Results r;
    http->submit(get("https://api.example.com/0"), r.sink());
    ASSERT_TRUE(wait_until([&] { return client.calls() == 1; }, 3000));
    http->submit(get("https://api.example.com/1"), r.sink());
    http->submit(get("https://api.example.com/2"), r.sink());

    http->set_deferring(true);
    EXPECT_EQ(http->deferred_count(), 2u);
    http->submit(get("https://api.example.com/3"), r.sink());
    EXPECT_EQ(http->deferred_count(), 3u);

    // The one already in flight finishes as it is.
    client.hold = false;
    ASSERT_TRUE(wait_until([&] { return r.size() == 1; }, 3000));
    wisp::os::sleep_ms(50);
    EXPECT_EQ(client.calls(), 1u);
    {
        std::lock_guard<std::mutex> lock(r.mu);
        EXPECT_FALSE(r.got[0].deferred);
    }

    http->set_deferring(false);
    ASSERT_TRUE(wait_until([&] { return r.size() == 4; }, 3000));
    EXPECT_EQ(client.urls(),
              (std::vector<std::string>{"https://api.example.com/0", "https://api.example.com/1",
                                        "https://api.example.com/2", "https://api.example.com/3"}));
    std::lock_guard<std::mutex> lock(r.mu);
    for (size_t i = 1; i < 4; ++i) EXPECT_TRUE(r.got[i].deferred);
}

// While the waiting requests drain into a full run queue, a new one lines
// up behind them (in order, per scene capped) rather than being refused.
TEST_F(SceneHttpTest, NewRequestsQueueBehindTheBacklog) {
    SceneHttpLimits limits;
    limits.queue = 2;
    make(limits);
    http->set_hosts("t", kHosts);
    http->set_deferring(true);
    Results r;
    http->submit(get("https://api.example.com/0"), r.sink());
    http->submit(get("https://api.example.com/1", "t"), r.sink());
    http->submit(get("https://api.example.com/2"), r.sink());
    http->submit(get("https://api.example.com/3", "t"), r.sink());
    http->submit(get("https://api.example.com/4"), r.sink());

    client.hold = true;
    http->set_deferring(false);
    ASSERT_TRUE(wait_until([&] { return client.calls() == 1; }, 3000));
    // In flight: 0. Run queue: 1, 2. Waiting: 3, 4.
    http->submit(get("https://api.example.com/n0"), r.sink());
    http->submit(get("https://api.example.com/n1"), r.sink());
    EXPECT_EQ(r.size(), 0u);
    // Scene "s" now has 4, n0 n1 among them, behind the backlog: full.
    http->submit(get("https://api.example.com/n2"), r.sink());
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(r.got[0].error, "rate_limited");

    client.hold = false;
    ASSERT_TRUE(wait_until([&] { return r.size() == 8; }, 3000));
    EXPECT_EQ(client.urls(),
              (std::vector<std::string>{"https://api.example.com/0", "https://api.example.com/1",
                                        "https://api.example.com/2", "https://api.example.com/3",
                                        "https://api.example.com/4", "https://api.example.com/n0",
                                        "https://api.example.com/n1"}));
    std::lock_guard<std::mutex> lock(r.mu);
    for (size_t i = 1; i < 6; ++i) EXPECT_TRUE(r.got[i].deferred) << i;
    EXPECT_FALSE(r.got[6].deferred);
    EXPECT_FALSE(r.got[7].deferred);
}

TEST_F(SceneHttpTest, DeferredTimeoutAfterOneHour) {
    SceneHttpLimits limits;
    limits.deferred_check_ms = 10;
    make(limits);
    http->set_deferring(true);
    Results r;
    http->submit(get("https://api.example.com/late"), r.sink());
    now += 3600000;
    wisp::os::sleep_ms(100);
    EXPECT_EQ(r.size(), 0u);
    EXPECT_EQ(http->deferred_count(), 1u);

    now += 1;
    ASSERT_TRUE(wait_until([&] { return r.size() == 1; }, 3000));
    EXPECT_EQ(r.got[0].error, "deferred_timeout");
    EXPECT_TRUE(r.got[0].deferred);
    EXPECT_EQ(http->deferred_count(), 0u);
    EXPECT_TRUE(http->idle());
    EXPECT_EQ(client.calls(), 0u);
}

// A deferred request counts against the scene's 30 per minute when it is
// submitted.
TEST_F(SceneHttpTest, DeferredStillRateLimited) {
    SceneHttpLimits limits;
    limits.max_deferred_per_scene = 64;
    make(limits);
    http->set_deferring(true);
    Results r;
    for (int i = 0; i < 30; ++i) http->submit(get("https://api.example.com/"), r.sink());
    EXPECT_EQ(r.size(), 0u);
    http->submit(get("https://api.example.com/"), r.sink());
    ASSERT_EQ(r.size(), 1u);
    EXPECT_EQ(r.got[0].error, "rate_limited");
    EXPECT_EQ(http->deferred_count(), 30u);
}

TEST_F(SceneHttpTest, CancelSceneDropsDeferred) {
    make();
    http->set_hosts("t", kHosts);
    http->set_deferring(true);
    Results s;
    Results t;
    http->submit(get("https://api.example.com/s0"), s.sink());
    http->submit(get("https://api.example.com/t0", "t"), t.sink());
    http->submit(get("https://api.example.com/s1"), s.sink());
    http->cancel_scene("s");
    EXPECT_EQ(http->deferred_count(), 1u);

    http->set_deferring(false);
    ASSERT_TRUE(wait_until([&] { return t.size() == 1; }, 3000));
    wisp::os::sleep_ms(50);
    EXPECT_EQ(s.size(), 0u);
    EXPECT_EQ(client.urls(), (std::vector<std::string>{"https://api.example.com/t0"}));
}

TEST_F(SceneHttpTest, StopDropsDeferredWithoutCallbacks) {
    make();
    http->set_deferring(true);
    Results r;
    http->submit(get("https://api.example.com/"), r.sink());
    http->stop();
    EXPECT_EQ(r.size(), 0u);
    EXPECT_EQ(http->deferred_count(), 0u);
}

// agent-tools spec 3.2: the Agent principal skips the https-only rule, the
// host allowlist and the per-scene rate limit; a Scene request to the same
// host is still held to all three.
TEST_F(SceneHttpTest, AgentPrincipalSkipsHttpsHostAndRate) {
    make();
    client.next = {.status = 200, .body = "ok"};
    for (int i = 0; i < 40; ++i) EXPECT_EQ(run(agent_get("http://lan.local/x")).error, "") << i;
    EXPECT_EQ(client.calls(), 40u);

    EXPECT_EQ(run(get("http://lan.local/x")).error, "not_https");
    EXPECT_EQ(run(get("https://lan.local/x")).error, "permission_denied");
    for (int i = 0; i < 30; ++i) {
        now += 1000;
        EXPECT_EQ(run(get("https://api.example.com/")).error, "") << i;
    }
    EXPECT_EQ(run(get("https://api.example.com/")).error, "rate_limited");
}

TEST_F(SceneHttpTest, AgentPrincipalLimits) {
    make();
    client.next = {.status = 200, .body = "ok"};

    std::string url = "https://x.example.com/";
    url.append(AgentHttpLimits::kMaxUrlBytes - url.size(), 'p');
    ASSERT_EQ(url.size(), AgentHttpLimits::kMaxUrlBytes);
    EXPECT_EQ(run(agent_get(url)).error, "");
    url.push_back('p');
    EXPECT_EQ(run(agent_get(url)).error, "too_large");

    auto many = agent_get("https://x.example.com/");
    for (int i = 0; i < 33; ++i) many.headers.emplace_back("X-H" + std::to_string(i), "v");
    EXPECT_EQ(run(std::move(many)).error, "too_large");

    auto big_body = agent_get("https://x.example.com/");
    big_body.method = Method::Post;
    big_body.body.assign(AgentHttpLimits::kMaxRequestBody, 'b');
    EXPECT_EQ(run(std::move(big_body)).error, "");
    auto too_big_body = agent_get("https://x.example.com/");
    too_big_body.method = Method::Post;
    too_big_body.body.assign(AgentHttpLimits::kMaxRequestBody + 1, 'b');
    EXPECT_EQ(run(std::move(too_big_body)).error, "too_large");

    EXPECT_EQ(run(agent_get("ftp://x.example.com/")).error, "bad_url");
    EXPECT_EQ(run(agent_get("https:///x")).error, "bad_url");
    // No IPv6 literals (host characters outside [a-z0-9.-]).
    EXPECT_EQ(run(agent_get("https://[::1]/x")).error, "bad_url");
}

// The Agent principal skips header_allowed()'s policy (the Scene
// denylist), but not its syntax half: a malformed name or a CRLF-carrying
// value is still refused outright, since a header injection is still a
// header injection even from trusted code.
TEST_F(SceneHttpTest, AgentPrincipalBadHeaderSyntax) {
    make();
    client.next = {.status = 200, .body = "ok"};
    auto crlf = agent_get("https://x.example.com/");
    crlf.headers = {{"X-A", "a\r\nX-B: c"}};
    EXPECT_EQ(run(std::move(crlf)).error, "bad_header");

    auto bad_name = agent_get("https://x.example.com/");
    bad_name.headers = {{"Bad Name", "v"}};
    EXPECT_EQ(run(std::move(bad_name)).error, "bad_header");

    // A header the Scene denylist would block (sec-*) is fine here: only
    // the syntax check applies.
    auto sec = agent_get("https://x.example.com/");
    sec.headers = {{"Sec-Fake", "v"}};
    EXPECT_EQ(run(std::move(sec)).error, "");
    ASSERT_TRUE(client.last());
    ASSERT_EQ(client.last()->headers.size(), 1u);
    EXPECT_EQ(client.last()->headers[0], (std::pair<std::string, std::string>{"Sec-Fake", "v"}));
}

TEST_F(SceneHttpTest, AgentPrincipalHopByHopHeadersIgnored) {
    make();
    client.next = {.status = 200, .body = "ok"};
    auto req = agent_get("https://x.example.com/");
    req.headers = {{"Host", "evil.example.com"},
                   {"content-length", "999"},
                   {"Transfer-Encoding", "chunked"},
                   {"CONNECTION", "keep-alive"},
                   {"X-Api-Key", "k1"}};
    EXPECT_EQ(run(std::move(req)).error, "");
    ASSERT_TRUE(client.last());
    ASSERT_EQ(client.last()->headers.size(), 1u);
    EXPECT_EQ(client.last()->headers[0], (std::pair<std::string, std::string>{"X-Api-Key", "k1"}));
}

TEST_F(SceneHttpTest, AgentPrincipalNeverDeferred) {
    make();
    http->set_deferring(true);
    EXPECT_EQ(run(agent_get("https://x.example.com/")).error, "offline");
    EXPECT_EQ(client.calls(), 0u);
    EXPECT_EQ(http->deferred_count(), 0u);

    // A Scene request still waits, as before.
    Results r;
    http->submit(get("https://api.example.com/"), r.sink());
    EXPECT_EQ(r.size(), 0u);
    EXPECT_EQ(http->deferred_count(), 1u);
    http->set_deferring(false);
    ASSERT_TRUE(wait_until([&] { return r.size() == 1; }, 3000));
    EXPECT_EQ(r.got[0].error, "");
}

// The Agent principal also bypasses limits_.queue (file header): a full
// run queue still accepts it rather than answering "rate_limited".
TEST_F(SceneHttpTest, AgentPrincipalBypassesQueueCap) {
    SceneHttpLimits limits;
    limits.queue = 1;
    make(limits);
    client.hold = true;
    Results r;
    http->submit(get("https://api.example.com/0"), r.sink());
    ASSERT_TRUE(wait_until([&] { return client.held.load() == 1; }, 2000));
    http->submit(get("https://api.example.com/1"), r.sink());  // fills queue_ (capacity 1)
    wisp::os::sleep_ms(20);
    EXPECT_EQ(r.size(), 0u);
    EXPECT_EQ(run(get("https://api.example.com/2")).error, "rate_limited");

    Results ar;
    http->submit(agent_get("https://x.example.com/"), ar.sink());
    wisp::os::sleep_ms(20);
    EXPECT_EQ(ar.size(), 0u);  // accepted, just waiting behind the others

    client.hold = false;
    ASSERT_TRUE(wait_until([&] { return ar.size() == 1; }, 3000));
    EXPECT_EQ(ar.got[0].error, "");
}

// The fake client mimics the real ones (esp_http_client, curl): over the
// cap without truncate_at_limit fails the request; SceneHttp sets it only
// for the Agent principal's plain (non-download) requests, so this goes
// through the same path a real client does, not a test-only shortcut.
TEST_F(SceneHttpTest, AgentPrincipalResponseCapTruncates) {
    make();
    client.next = {.status = 200,
                   .body = std::string(AgentHttpLimits::kMaxResponse + 10, 'x'),
                   .headers = {{"x-thing", "v"}}};
    auto r = run(agent_get("https://x.example.com/big"));
    EXPECT_EQ(r.error, "");
    EXPECT_TRUE(r.body_truncated);
    EXPECT_EQ(r.body.size(), AgentHttpLimits::kMaxResponse);
    EXPECT_EQ(r.status, 200);
    ASSERT_EQ(r.headers.size(), 1u);
    ASSERT_TRUE(client.last());
    EXPECT_TRUE(client.last()->truncate_at_limit);

    // The Scene principal is still held to its own (smaller) cap, as an
    // error, not a truncation; it never asks the client to truncate.
    client.next = {.status = 200, .body = std::string(262145, 'x')};
    auto r2 = run(get("https://api.example.com/big"));
    EXPECT_EQ(r2.error, "too_large");
    EXPECT_FALSE(r2.body_truncated);
    ASSERT_TRUE(client.last());
    EXPECT_FALSE(client.last()->truncate_at_limit);
}

// A huge 3xx body is truncated, not followed: it comes back as the final
// answer (file header), not merged into the redirect chain.
TEST_F(SceneHttpTest, AgentPrincipalTruncated3xxIsNotFollowed) {
    make();
    int call = 0;
    client.responder = [&](const wisp::net::HttpClientRequest&) -> wisp::net::HttpClientResponse {
        ++call;
        return {.status = 302,
                .body = std::string(AgentHttpLimits::kMaxResponse + 10, 'x'),
                .headers = {{"location", "https://two.example.com/"}}};
    };
    auto req = agent_get("https://one.example.com/");
    req.max_redirects = 5;
    auto r = run(std::move(req));
    EXPECT_EQ(r.error, "");
    EXPECT_EQ(r.status, 302);
    EXPECT_TRUE(r.body_truncated);
    EXPECT_EQ(call, 1);
    EXPECT_EQ(r.final_url, "https://one.example.com/");
    client.responder = nullptr;
}

TEST_F(SceneHttpTest, AgentPrincipalRedirects) {
    make();
    int call = 0;
    client.responder = [&](const wisp::net::HttpClientRequest&) -> wisp::net::HttpClientResponse {
        ++call;
        if (call == 1)
            return {.status = 301, .headers = {{"location", "https://two.example.com/"}}};
        if (call == 2)
            return {.status = 301, .headers = {{"location", "https://three.example.com/"}}};
        return {.status = 200, .body = "done"};
    };
    auto req = agent_get("https://one.example.com/");
    req.max_redirects = 5;
    auto r = run(std::move(req));
    EXPECT_EQ(r.error, "");
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.final_url, "https://three.example.com/");
    EXPECT_EQ(call, 3);

    // Six hops in a row, one more than the limit: the sixth 3xx, not
    // followed, comes back as it is.
    call = 0;
    client.responder = [&](const wisp::net::HttpClientRequest&) -> wisp::net::HttpClientResponse {
        ++call;
        return {.status = 302, .headers = {{"location", "/next"}}};
    };
    auto req6 = agent_get("https://one.example.com/");
    req6.max_redirects = 5;
    auto r6 = run(std::move(req6));
    EXPECT_EQ(r6.error, "");
    EXPECT_EQ(r6.status, 302);
    EXPECT_EQ(call, 6);
    EXPECT_EQ(r6.final_url, "https://one.example.com/next");

    // max_redirects left at 0: the first 3xx comes back unfollowed.
    call = 0;
    client.responder = [&](const wisp::net::HttpClientRequest&) -> wisp::net::HttpClientResponse {
        ++call;
        return {.status = 302, .headers = {{"location", "https://elsewhere.example.com/"}}};
    };
    auto r0 = run(agent_get("https://one.example.com/"));
    EXPECT_EQ(r0.error, "");
    EXPECT_EQ(r0.status, 302);
    EXPECT_EQ(call, 1);
    EXPECT_EQ(r0.final_url, "https://one.example.com/");

    // Scene requests ignore max_redirects outright: a 3xx always comes
    // back as it is, even if a caller set it.
    call = 0;
    auto scene_req = get("https://api.example.com/");
    scene_req.max_redirects = 5;
    auto scene_r = run(std::move(scene_req));
    EXPECT_EQ(scene_r.error, "");
    EXPECT_EQ(scene_r.status, 302);
    EXPECT_EQ(call, 1);

    client.responder = nullptr;
}

// Location forms beyond a plain absolute URL or absolute path (finding:
// scheme-relative, query-only, relative-path merge with ".."/".",  and a
// base with a query but no path building a sane origin).
TEST_F(SceneHttpTest, AgentPrincipalRedirectLocationForms) {
    make();
    auto follow_once = [&](const std::string& original, const std::string& location) {
        int call = 0;
        client.responder =
            [&](const wisp::net::HttpClientRequest&) -> wisp::net::HttpClientResponse {
            ++call;
            if (call == 1) return {.status = 302, .headers = {{"location", location}}};
            return {.status = 200, .body = "ok"};
        };
        auto req = agent_get(original);
        req.max_redirects = 1;
        auto r = run(std::move(req));
        client.responder = nullptr;
        EXPECT_EQ(r.error, "") << original << " -> " << location;
        EXPECT_EQ(r.status, 200) << original << " -> " << location;
        return r.final_url;
    };

    EXPECT_EQ(follow_once("https://one.example.com/a/b", "//two.example.com/p"),
              "https://two.example.com/p");
    EXPECT_EQ(follow_once("https://one.example.com/a/b?x=1", "?page=2"),
              "https://one.example.com/a/b?page=2");
    EXPECT_EQ(follow_once("https://one.example.com/a/b", "c"), "https://one.example.com/a/c");
    EXPECT_EQ(follow_once("https://one.example.com/a/b/c", "../x"), "https://one.example.com/a/x");
    EXPECT_EQ(follow_once("https://one.example.com?x=1", "/next"), "https://one.example.com/next");

    // Dot-segment removal applies to the path only: a query value that
    // itself looks like a URL or a path with ".." must survive untouched
    // (finding: normalize_dot_segments was rewriting past the '?').
    EXPECT_EQ(follow_once("https://one.example.com/x", "/cb?u=https://a.com/x"),
              "https://one.example.com/cb?u=https://a.com/x");
    EXPECT_EQ(follow_once("https://one.example.com/x", "/login?r=/x/../y"),
              "https://one.example.com/login?r=/x/../y");
    // Empty path segments are kept, not collapsed.
    EXPECT_EQ(follow_once("https://one.example.com/x", "/a//b"), "https://one.example.com/a//b");
    // A trailing ".." leaves a trailing "/" (RFC 3986 5.2.4's own example).
    EXPECT_EQ(follow_once("https://one.example.com/x", "/a/b/.."), "https://one.example.com/a/");
}

// Every hop's resolved target is re-checked exactly like a fresh request's
// URL: an invalid one (here, an unsupported scheme) ends the chain with
// the 3xx that pointed at it, not followed, rather than being sent as is.
TEST_F(SceneHttpTest, AgentPrincipalRedirectTargetIsRevalidated) {
    make();
    int call = 0;
    client.responder = [&](const wisp::net::HttpClientRequest&) -> wisp::net::HttpClientResponse {
        ++call;
        return {.status = 302, .headers = {{"location", "ftp://bad.example.com/"}}};
    };
    auto req = agent_get("https://one.example.com/");
    req.max_redirects = 5;
    auto r = run(std::move(req));
    EXPECT_EQ(r.error, "");
    EXPECT_EQ(r.status, 302);
    EXPECT_EQ(call, 1);
    EXPECT_EQ(r.final_url, "https://one.example.com/");
    client.responder = nullptr;
}

// A hop that crosses to a different scheme, host or port drops
// Authorization (and, by the same rule, Cookie/Proxy-Authorization) from
// then on, so a credential for the first host never reaches another one.
TEST_F(SceneHttpTest, AgentPrincipalDropsCredentialsAcrossOrigin) {
    make();
    int call = 0;
    bool call1_has_creds = false;
    bool call2_has_creds = false;
    bool call3_has_creds = false;
    auto has_creds = [](const wisp::net::HttpClientRequest& r) {
        for (const auto& [n, v] : r.headers)
            if (n == "Authorization" || n == "Cookie" || n == "Proxy-Authorization") return true;
        return false;
    };
    client.responder = [&](const wisp::net::HttpClientRequest& r) -> wisp::net::HttpClientResponse {
        ++call;
        if (call == 1) {
            call1_has_creds = has_creds(r);
            return {.status = 302, .headers = {{"location", "https://two.example.com/"}}};
        }
        if (call == 2) {
            call2_has_creds = has_creds(r);
            return {.status = 302, .headers = {{"location", "/p3"}}};
        }
        call3_has_creds = has_creds(r);
        return {.status = 200, .body = "ok"};
    };
    auto req = agent_get("https://one.example.com/");
    req.max_redirects = 5;
    req.headers = {{"Authorization", "Bearer secret"},
                   {"Cookie", "s=1"},
                   {"Proxy-Authorization", "Basic x"},
                   {"X-Keep", "v"}};
    auto r = run(std::move(req));
    EXPECT_EQ(r.error, "");
    EXPECT_EQ(call, 3);
    EXPECT_TRUE(call1_has_creds);
    EXPECT_FALSE(call2_has_creds);
    EXPECT_FALSE(call3_has_creds);
    client.responder = nullptr;
}

// One deadline for the whole chain (finding: hops 2+ must stay watched
// and bounded by the same deadline, not a fresh one each).
TEST_F(SceneHttpTest, AgentPrincipalRedirectChainSharesOneDeadline) {
    make();
    int call = 0;
    client.responder = [&](const wisp::net::HttpClientRequest&) -> wisp::net::HttpClientResponse {
        ++call;
        now += 20000;
        return {.status = 302, .headers = {{"location", "/next"}}};
    };
    auto req = agent_get("https://one.example.com/");
    req.max_redirects = 5;
    req.timeout_ms = 50000;
    auto r = run(std::move(req));
    EXPECT_EQ(r.error, "timeout");
    // Hop 1 is sent at +0s, hop 2 at +20s (both still under the 50s
    // deadline); the chain's one deadline is past by +40s, when hop 3
    // would otherwise start.
    EXPECT_EQ(call, 3);
    client.responder = nullptr;
}

// The watch is re-armed before every later hop (not left disarmed after
// hop 1), so a cancel during hop 2 still reaches it.
TEST_F(SceneHttpTest, AgentPrincipalCancelDuringSecondHop) {
    make();
    int call = 0;
    client.responder = [&](const wisp::net::HttpClientRequest&) -> wisp::net::HttpClientResponse {
        ++call;
        if (call == 1)
            return {.status = 302, .headers = {{"location", "https://two.example.com/"}}};
        client.hold = true;
        return {};
    };
    Results r;
    auto req = agent_get("https://one.example.com/");
    req.max_redirects = 5;
    http->submit(std::move(req), r.sink());
    ASSERT_TRUE(wait_until([&] { return client.held.load() == 1; }, 2000));
    EXPECT_EQ(call, 2);
    http->cancel_scene(std::string(kAgentHttpId));
    ASSERT_TRUE(wait_until([&] { return client.saw_cancel.load(); }, 2000));
    client.hold = false;
    // cancel_scene() suppresses done() entirely (file header, same as the
    // Scene principal): the point of this test is that the abort actually
    // reached hop 2, not that a result was delivered.
    wisp::os::sleep_ms(50);
    EXPECT_EQ(r.size(), 0u);
    EXPECT_EQ(call, 2);
    client.responder = nullptr;
}

TEST_F(SceneHttpTest, AgentPrincipalPerRequestTimeout) {
    make();
    client.next = {.status = 200, .body = "ok"};
    auto req = agent_get("https://x.example.com/");
    req.timeout_ms = 50000;
    EXPECT_EQ(run(std::move(req)).error, "");
    ASSERT_TRUE(client.last());
    EXPECT_EQ(client.last()->timeout_ms, 50000u);

    auto over = agent_get("https://x.example.com/");
    over.timeout_ms = 150000;
    EXPECT_EQ(run(std::move(over)).error, "");
    ASSERT_TRUE(client.last());
    EXPECT_EQ(client.last()->timeout_ms, AgentHttpLimits::kMaxTimeoutMs);
}

// The 50 s is an actual deadline, not just a number handed to the client:
// it fires on schedule (checked between pieces with the fake clock, as
// TimeoutIsOneDeadlineForTheWholeRequest does for the Scene principal's
// own timeout), neither sooner (the Scene default of 15 s) nor never.
TEST_F(SceneHttpTest, AgentPrincipalPerRequestTimeoutFiresOnSchedule) {
    make();
    client.next = {.status = 200, .body = std::string(4 * 4096, 'x')};
    client.before_piece = [this] { now += 20000; };
    auto req = agent_get("https://x.example.com/trickle");
    req.timeout_ms = 50000;
    auto r = run(std::move(req));
    EXPECT_EQ(r.error, "timeout");
    // Pieces at +20 s, +40 s; the one at +60 s is past the 50 s deadline.
    EXPECT_EQ(client.pieces.load(), 2);
}

TEST_F(SceneHttpTest, AgentPrincipalLogPrivacy) {
    make();
    client.next = {.status = 200, .body = "ok"};
    auto req = agent_get("https://x.example.com/p?key=QUERYSECRET-9");
    req.method = Method::Post;
    req.headers = {{"Authorization", "Bearer AGENTSECRET-1"}};
    req.body = "AGENTBODYSECRET-2";
    EXPECT_EQ(run(std::move(req)).error, "");
    client.next = {.transport = TransportError::Tls};
    EXPECT_EQ(run(agent_get("https://x.example.com/p?key=QUERYSECRET-9")).error, "tls");

    const std::string log = wisp::log::ring_snapshot();
    for (const char* s : {"AGENTSECRET", "AGENTBODYSECRET", "QUERYSECRET", "/p"})
        EXPECT_EQ(log.find(s), std::string::npos) << s << " in:\n" << log;
    // agent-tools spec 3.2's one log line per request (method, host,
    // status, duration) is there, not just silence.
    EXPECT_NE(log.find("x.example.com"), std::string::npos) << log;
    EXPECT_NE(log.find("POST"), std::string::npos) << log;
}

TEST(SceneHttpTask, Config) {
    const auto t = SceneHttp::task_config();
    EXPECT_STREQ(t.name, "wisp.shttp");
    EXPECT_EQ(t.stack_bytes, 8192u);
    EXPECT_EQ(t.priority, 3);
    EXPECT_EQ(t.core, -1);
    EXPECT_FALSE(t.flash_access);
}

}  // namespace

namespace {

// A download (on_body set): no 256 KB response cap, the body goes to
// on_body piece by piece and the result carries none of it.
TEST_F(SceneHttpTest, DownloadStreamsWithoutResponseCap) {
    make();
    const std::string body(1024 * 1024, 'd');
    client.next = {.status = 200, .body = body};
    std::mutex mu;
    std::string got;
    SceneHttpRequest req = get("https://api.example.com/big");
    req.download = true;
    req.on_body = [&](std::span<const uint8_t> chunk) {
        std::lock_guard<std::mutex> lock(mu);
        got.append(reinterpret_cast<const char*>(chunk.data()), chunk.size());
        return true;
    };
    SceneHttpResult r = run(std::move(req));
    EXPECT_EQ(r.error, "");
    EXPECT_EQ(r.status, 200);
    EXPECT_EQ(r.body.size(), 0u);
    {
        std::lock_guard<std::mutex> lock(mu);
        EXPECT_EQ(got.size(), body.size());
        EXPECT_EQ(got, body);
    }
    ASSERT_TRUE(client.last());
    EXPECT_EQ(client.last()->timeout_ms, SceneHttpRequest::kDownloadTimeoutMs);
    EXPECT_GT(client.last()->max_response_bytes, body.size());

    // on_body refusing stops the transfer.
    SceneHttpRequest stop = get("https://api.example.com/big");
    stop.download = true;
    stop.on_body = [](std::span<const uint8_t>) { return false; };
    EXPECT_EQ(run(std::move(stop)).error, "other");

    // max_body caps it like max_response does an ordinary request.
    SceneHttpRequest capped = get("https://api.example.com/big");
    capped.download = true;
    capped.max_body = 1000;
    capped.on_body = [](std::span<const uint8_t>) { return true; };
    EXPECT_EQ(run(std::move(capped)).error, "too_large");
}

}  // namespace
