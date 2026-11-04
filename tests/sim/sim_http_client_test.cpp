#include "wisp/port/sim/sim_http_client.hpp"

#include <curl/curl.h>
#include <gtest/gtest.h>

#include <atomic>
#include <cstdint>
#include <span>
#include <string>
#include <thread>
#include <utility>
#include <vector>

#include "wisp/net/http.hpp"
#include "wisp/os/os.hpp"
#include "wisp/port/sim/sim_http_server.hpp"

using wisp::net::HttpClientRequest;
using wisp::net::HttpRequest;
using wisp::net::HttpResponse;
using wisp::net::Method;
using wisp::net::TransportError;
using wisp::port::sim::SimHttpClient;
using wisp::port::sim::SimHttpServer;

namespace {

std::string url(uint16_t port, const char* path) {
    return "http://127.0.0.1:" + std::to_string(port) + path;
}

}  // namespace

TEST(SimHttpClient, RoundTripsAgainstLoopbackServer) {
    SimHttpServer server;
    ASSERT_TRUE(server.start(0,
                             [](const HttpRequest& req) {
                                 HttpResponse res;
                                 res.status = req.method == Method::Put ? 201 : 500;
                                 res.content_type = "text/plain";
                                 res.body = req.path + "|" + req.header("x-probe") + "|" + req.body;
                                 return res;
                             },
                             {}));

    SimHttpClient client;
    HttpClientRequest req;
    req.method = Method::Put;
    req.url = url(server.port(), "/v1/thing");
    req.headers = {{"X-Probe", "yes"}, {"Content-Type", "application/json"}};
    req.body = "{\"a\":1}";
    auto res = client.request(req);
    EXPECT_EQ(res.transport, TransportError::None);
    EXPECT_EQ(res.status, 201);
    EXPECT_EQ(res.body, "/v1/thing|yes|{\"a\":1}");

    // An HTTP error status is still a completed transport.
    SimHttpServer missing;
    ASSERT_TRUE(missing.start(
        0, [](const HttpRequest&) { return HttpResponse::error(404, "not_found", "nope"); }, {}));
    HttpClientRequest get;
    get.url = url(missing.port(), "/models");
    auto res404 = client.request(get);
    EXPECT_EQ(res404.transport, TransportError::None);
    EXPECT_EQ(res404.status, 404);
}

TEST(SimHttpClient, ClosedPortIsAConnectError) {
    SimHttpServer server;
    ASSERT_TRUE(server.start(0, [](const HttpRequest&) { return HttpResponse{}; }, {}));
    uint16_t port = server.port();
    server.stop();

    SimHttpClient client;
    HttpClientRequest req;
    req.url = url(port, "/");
    auto res = client.request(req);
    EXPECT_EQ(res.transport, TransportError::Connect);
    EXPECT_EQ(res.status, 0);
}

TEST(SimHttpClient, SlowServerTimesOut) {
    SimHttpServer server;
    ASSERT_TRUE(server.start(0,
                             [](const HttpRequest&) {
                                 wisp::os::sleep_ms(1500);
                                 return HttpResponse{};
                             },
                             {}));

    SimHttpClient client;
    HttpClientRequest req;
    req.url = url(server.port(), "/slow");
    req.timeout_ms = 200;
    uint64_t t0 = wisp::os::now_ms();
    auto res = client.request(req);
    EXPECT_EQ(res.transport, TransportError::Timeout);
    EXPECT_LT(wisp::os::now_ms() - t0, 1400u);
}

TEST(SimHttpClient, OversizedResponseIsRefused) {
    // Outlives every response that points into it.
    static const std::vector<uint8_t> big(SimHttpClient::kMaxResponseBytes + 4096, 'x');
    static const std::vector<uint8_t> fits(SimHttpClient::kMaxResponseBytes, 'y');
    SimHttpServer server;
    ASSERT_TRUE(server.start(0,
                             [](const HttpRequest& req) {
                                 HttpResponse res;
                                 res.content_type = "text/plain";
                                 res.static_body = req.path == "/big"
                                                       ? std::span<const uint8_t>(big)
                                                       : std::span<const uint8_t>(fits);
                                 return res;
                             },
                             {}));

    SimHttpClient client;
    HttpClientRequest req;
    req.url = url(server.port(), "/big");
    auto res = client.request(req);
    EXPECT_EQ(res.transport, TransportError::TooLarge);
    EXPECT_EQ(res.status, 0);
    EXPECT_TRUE(res.body.empty());

    req.url = url(server.port(), "/fits");
    res = client.request(req);
    EXPECT_EQ(res.transport, TransportError::None);
    EXPECT_EQ(res.status, 200);
    EXPECT_EQ(res.body.size(), SimHttpClient::kMaxResponseBytes);
}

TEST(SimHttpClient, StreamsBodyHeadersAndCancels) {
    static const std::vector<uint8_t> body(100000, 'z');
    SimHttpServer server;
    ASSERT_TRUE(server.start(0,
                             [](const HttpRequest& req) {
                                 HttpResponse res;
                                 if (req.path == "/slow") wisp::os::sleep_ms(3000);
                                 res.status = 203;
                                 res.content_type = "application/json; charset=utf-8";
                                 res.headers = {{"X-Thing", "  v1 "}, {"X-Other", "v2"}};
                                 res.static_body = std::span<const uint8_t>(body);
                                 return res;
                             },
                             {}));
    SimHttpClient client;

    HttpClientRequest req;
    req.url = url(server.port(), "/body");
    size_t pieces = 0;
    std::string got;
    req.on_body = [&](std::span<const uint8_t> chunk) {
        ++pieces;
        got.append(reinterpret_cast<const char*>(chunk.data()), chunk.size());
        return true;
    };
    req.max_response_bytes = body.size();
    auto res = client.request(req);
    EXPECT_EQ(res.transport, TransportError::None);
    EXPECT_EQ(res.status, 203);
    EXPECT_TRUE(res.body.empty());
    EXPECT_EQ(got, std::string(body.begin(), body.end()));
    EXPECT_GT(pieces, 1u);
    auto header = [&](const char* name) {
        for (const auto& [n, v] : res.headers)
            if (n == name) return v;
        return std::string("(missing)");
    };
    EXPECT_EQ(header("content-type"), "application/json; charset=utf-8");
    EXPECT_EQ(header("x-thing"), "v1");
    EXPECT_EQ(header("x-other"), "v2");
    for (const auto& [n, v] : res.headers)
        for (char c : n) EXPECT_FALSE(c >= 'A' && c <= 'Z') << n;

    // The body goes to `body` without on_body.
    HttpClientRequest plain;
    plain.url = url(server.port(), "/body");
    res = client.request(plain);
    EXPECT_EQ(res.body.size(), body.size());

    // One byte over the cap.
    req.max_response_bytes = body.size() - 1;
    got.clear();
    res = client.request(req);
    EXPECT_EQ(res.transport, TransportError::TooLarge);
    EXPECT_EQ(res.status, 0);
    EXPECT_TRUE(res.headers.empty());

    // on_body refusing a piece stops the transfer.
    req.max_response_bytes = 0;
    req.on_body = [](std::span<const uint8_t>) { return false; };
    res = client.request(req);
    EXPECT_EQ(res.transport, TransportError::Cancelled);

    // Cancelled while waiting for a slow server.
    std::atomic<bool> stop{false};
    HttpClientRequest slow;
    slow.url = url(server.port(), "/slow");
    slow.cancelled = [&] { return stop.load(); };
    const uint64_t t0 = wisp::os::now_ms();
    std::thread canceller([&] {
        wisp::os::sleep_ms(200);
        stop = true;
    });
    res = client.request(slow);
    canceller.join();
    EXPECT_EQ(res.transport, TransportError::Cancelled);
    EXPECT_LT(wisp::os::now_ms() - t0, 2500u);

    // The body comes from body_ref when it is set.
    SimHttpServer echo;
    ASSERT_TRUE(echo.start(0,
                           [](const HttpRequest& r) {
                               HttpResponse out;
                               out.content_type = "text/plain";
                               out.body = r.body;
                               return out;
                           },
                           {}));
    static const std::string ref = "from the reference";
    HttpClientRequest post;
    post.method = Method::Post;
    post.url = url(echo.port(), "/echo");
    post.body = "ignored";
    post.body_ref = {reinterpret_cast<const uint8_t*>(ref.data()), ref.size()};
    res = client.request(post);
    EXPECT_EQ(res.body, ref);
}

TEST(SimHttpClient, MapsCurlErrors) {
    EXPECT_EQ(SimHttpClient::transport_error_for(CURLE_OK), TransportError::None);
    EXPECT_EQ(SimHttpClient::transport_error_for(CURLE_COULDNT_RESOLVE_HOST), TransportError::Dns);
    EXPECT_EQ(SimHttpClient::transport_error_for(CURLE_COULDNT_CONNECT), TransportError::Connect);
    EXPECT_EQ(SimHttpClient::transport_error_for(CURLE_OPERATION_TIMEDOUT),
              TransportError::Timeout);
    EXPECT_EQ(SimHttpClient::transport_error_for(CURLE_SSL_CONNECT_ERROR), TransportError::Tls);
    EXPECT_EQ(SimHttpClient::transport_error_for(CURLE_PEER_FAILED_VERIFICATION),
              TransportError::Tls);
    EXPECT_EQ(SimHttpClient::transport_error_for(CURLE_SSL_CACERT_BADFILE), TransportError::Tls);
    EXPECT_EQ(SimHttpClient::transport_error_for(CURLE_SSL_CERTPROBLEM), TransportError::Tls);
    EXPECT_EQ(SimHttpClient::transport_error_for(CURLE_UNSUPPORTED_PROTOCOL),
              TransportError::Other);
}
