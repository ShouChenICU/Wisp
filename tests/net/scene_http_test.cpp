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

TEST(SceneHttpTask, Config) {
    const auto t = SceneHttp::task_config();
    EXPECT_STREQ(t.name, "wisp.shttp");
    EXPECT_EQ(t.stack_bytes, 8192u);
    EXPECT_EQ(t.priority, 3);
    EXPECT_EQ(t.core, -1);
    EXPECT_FALSE(t.flash_access);
}

}  // namespace
