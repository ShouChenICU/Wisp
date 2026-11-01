#pragma once

#include <string>

namespace wisp {

// Error taxonomy shared by every portable Wisp component. Keep this list
// small and generic; component-specific detail belongs in Error::message.
enum class ErrorCode {
    Ok,
    InvalidArgument,
    NotFound,
    AlreadyExists,
    Timeout,
    IoError,
    NoMemory,
    Unsupported,
    StorageUnavailable,
    QuotaExceeded,
    DeviceFailure,
    Busy,
};

// Returns the snake_case name of `code`, e.g. ErrorCode::InvalidArgument ->
// "invalid_argument". Used for logging and diagnostics.
const char* to_string(ErrorCode code);

struct Error {
    ErrorCode code = ErrorCode::Ok;
    std::string message;
};

}  // namespace wisp
