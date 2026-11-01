#include "wisp/base/error.hpp"

namespace wisp {

const char* to_string(ErrorCode code) {
    switch (code) {
        case ErrorCode::Ok:
            return "ok";
        case ErrorCode::InvalidArgument:
            return "invalid_argument";
        case ErrorCode::NotFound:
            return "not_found";
        case ErrorCode::AlreadyExists:
            return "already_exists";
        case ErrorCode::Timeout:
            return "timeout";
        case ErrorCode::IoError:
            return "io_error";
        case ErrorCode::NoMemory:
            return "no_memory";
        case ErrorCode::Unsupported:
            return "unsupported";
        case ErrorCode::StorageUnavailable:
            return "storage_unavailable";
        case ErrorCode::QuotaExceeded:
            return "quota_exceeded";
        case ErrorCode::DeviceFailure:
            return "device_failure";
        case ErrorCode::Busy:
            return "busy";
    }
    return "unknown";
}

}  // namespace wisp
