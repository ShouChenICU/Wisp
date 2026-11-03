#include "wisp/port/sim/sim_http_client.hpp"

#include <curl/curl.h>
#include <gtest/gtest.h>

#include <string>

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
