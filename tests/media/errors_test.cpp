#include "wisp/media/errors.hpp"

#include <gtest/gtest.h>

#include <string>

using namespace wisp;

TEST(MediaErrors, WireNamesAndStatus) {
    struct Row {
        ErrorCode code;
        const char* name;
        int status;
    };
    const Row rows[] = {
        {ErrorCode::StorageUnavailable, "media_unavailable", 503},
        {ErrorCode::NotFound, "not_found", 404},
        {ErrorCode::InvalidArgument, "invalid_path", 400},
        {ErrorCode::PermissionDenied, "permission_denied", 403},
        {ErrorCode::Unsupported, "unsupported_format", 415},
        {ErrorCode::Corrupt, "decode_error", 422},
        {ErrorCode::Busy, "busy", 503},
        {ErrorCode::IoError, "io_error", 500},
        {ErrorCode::NoMemory, "io_error", 500},
    };
    for (const auto& row : rows) {
        const Error e{row.code, "x"};
        EXPECT_STREQ(media::wire_name(e), row.name) << to_string(row.code);
        EXPECT_EQ(media::http_status(e), row.status) << to_string(row.code);
    }
    EXPECT_STREQ(to_string(ErrorCode::Corrupt), "corrupt");
}
