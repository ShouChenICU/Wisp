#include "wisp/net/http.hpp"

#include <gtest/gtest.h>

#include "wisp/base/error.hpp"
#include "wisp/base/json.hpp"
#include "wisp/base/value.hpp"

using wisp::ErrorCode;
using wisp::net::http_status_for;

TEST(Http, HeaderAndCookieLookupIsCaseInsensitiveAndMultiValued) {
    wisp::net::HttpRequest r;
    r.headers["cookie"] = "a=1; wisp_token=abc ; b=2";
    EXPECT_EQ(r.cookie("wisp_token"), "abc");
    EXPECT_EQ(r.cookie("a"), "1");
    EXPECT_EQ(r.cookie("b"), "2");
    EXPECT_EQ(r.cookie("c"), "");
    EXPECT_EQ(r.cookie("wisp"), "");
}

TEST(Http, HeaderLookupMissingReturnsEmpty) {
    wisp::net::HttpRequest r;
    r.headers["content-type"] = "application/json";
    EXPECT_EQ(r.header("content-type"), "application/json");
    EXPECT_EQ(r.header("Content-Type"), "application/json");
    EXPECT_EQ(r.header("host"), "");
    EXPECT_EQ(r.cookie("wisp_token"), "");
}

TEST(Http, StatusMapping) {
    EXPECT_EQ(http_status_for(ErrorCode::RateLimited), 429);
    EXPECT_EQ(http_status_for(ErrorCode::PermissionDenied), 403);
    EXPECT_EQ(http_status_for(ErrorCode::InvalidArgument), 400);
    EXPECT_EQ(http_status_for(ErrorCode::NotFound), 404);
    EXPECT_EQ(http_status_for(ErrorCode::AlreadyExists), 409);
    EXPECT_EQ(http_status_for(ErrorCode::Busy), 409);
    EXPECT_EQ(http_status_for(ErrorCode::QuotaExceeded), 409);
    EXPECT_EQ(http_status_for(ErrorCode::Unsupported), 501);
    EXPECT_EQ(http_status_for(ErrorCode::Timeout), 504);
    EXPECT_EQ(http_status_for(ErrorCode::StorageUnavailable), 503);
    EXPECT_EQ(http_status_for(ErrorCode::NoMemory), 503);
    EXPECT_EQ(http_status_for(ErrorCode::IoError), 500);
    EXPECT_EQ(http_status_for(ErrorCode::DeviceFailure), 500);
    EXPECT_EQ(http_status_for(ErrorCode::Ok), 500);
}

TEST(Http, JsonAndErrorResponses) {
    auto ok = wisp::net::HttpResponse::json(201, wisp::Value(wisp::Map{{"id", wisp::Value(7)}}));
    EXPECT_EQ(ok.status, 201);
    EXPECT_EQ(ok.content_type, "application/json");
    EXPECT_EQ(ok.body, R"({"id":7})");
    EXPECT_TRUE(ok.static_body.empty());

    auto err = wisp::net::HttpResponse::error(404, "not_found", "no such \"thing\"");
    EXPECT_EQ(err.status, 404);
    EXPECT_EQ(err.content_type, "application/json");
    auto parsed = wisp::from_json(err.body);
    ASSERT_TRUE(parsed);
    EXPECT_EQ(err.body, R"({"error":{"code":"not_found","message":"no such \"thing\""}})");
}
