#pragma once

// Sets the system wall clock (settimeofday). The one setter on ESP: the
// board's boot path sets it from the RTC and Platform::set_wall_clock sets
// it after a network sync (connectivity spec 5.6). Touches no flash.

#include <sys/time.h>

#include <cerrno>
#include <cstdint>
#include <ctime>
#include <string>

#include "wisp/base/error.hpp"
#include "wisp/base/result.hpp"

namespace wisp::port::esp {

inline Result<void> set_wall_clock(int64_t unix_s) {
    timeval tv = {};
    tv.tv_sec = static_cast<time_t>(unix_s);
    if (settimeofday(&tv, nullptr) != 0) {
        const int err = errno;
        return Result<void>::err(ErrorCode::IoError, "settimeofday: errno " + std::to_string(err));
    }
    return Result<void>::ok();
}

}  // namespace wisp::port::esp
