#include "wisp/app/format.hpp"

#include <cstdio>
#include <ctime>

namespace wisp::app {

std::string format_bytes(uint64_t bytes) {
    if (bytes < 1024) {
        return std::to_string(bytes) + " B";
    }

    static constexpr const char* kUnits[] = {"B", "KB", "MB", "GB", "TB", "PB"};
    double value = static_cast<double>(bytes);
    int unit = 0;
    while (value >= 1024.0 && unit < 5) {
        value /= 1024.0;
        ++unit;
    }

    char buf[32];
    std::snprintf(buf, sizeof(buf), "%.1f %s", value, kUnits[unit]);
    return buf;
}

const char* state_label(core::DeviceState state) {
    switch (state) {
        case core::DeviceState::Ok:
            return "正常";
        case core::DeviceState::Degraded:
            return "降级";
        case core::DeviceState::Missing:
            return "缺失";
        case core::DeviceState::Failed:
            return "失败";
    }
    return "失败";
}

std::string format_time(int64_t unix_s) {
    std::time_t t = static_cast<std::time_t>(unix_s);
    std::tm tm{};
    gmtime_r(&t, &tm);

    char buf[32];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d", tm.tm_year + 1900,
                  tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

}  // namespace wisp::app
