#include "wisp/media/errors.hpp"

namespace wisp::media {

const char* wire_name(const Error& e) {
    switch (e.code) {
        case ErrorCode::StorageUnavailable:
            return "media_unavailable";
        case ErrorCode::NotFound:
            return "not_found";
        case ErrorCode::InvalidArgument:
            return "invalid_path";
        case ErrorCode::PermissionDenied:
            return "permission_denied";
        case ErrorCode::Unsupported:
            return "unsupported_format";
        case ErrorCode::Corrupt:
            return "decode_error";
        case ErrorCode::Busy:
            return "busy";
        default:
            return "io_error";
    }
}

int http_status(const Error& e) {
    switch (e.code) {
        case ErrorCode::StorageUnavailable:
            return 503;
        case ErrorCode::NotFound:
            return 404;
        case ErrorCode::InvalidArgument:
            return 400;
        case ErrorCode::PermissionDenied:
            return 403;
        case ErrorCode::Unsupported:
            return 415;
        case ErrorCode::Corrupt:
            return 422;
        case ErrorCode::Busy:
            return 503;
        default:
            return 500;
    }
}

}  // namespace wisp::media
