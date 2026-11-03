#include "wisp/app/format.hpp"

#include <cstdint>
#include <cstdio>
#include <ctime>

#include "wisp/ui/strings.hpp"

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

size_t utf8_sequence(std::string_view s, size_t i) {
    const auto b = [&](size_t k) { return static_cast<uint8_t>(s[k]); };
    const uint8_t c = b(i);
    size_t len = 0;
    uint8_t lo = 0x80, hi = 0xbf;  // allowed range of the second byte
    if (c >= 0xc2 && c <= 0xdf) {
        len = 2;
    } else if (c >= 0xe0 && c <= 0xef) {
        len = 3;
        if (c == 0xe0) lo = 0xa0;
        if (c == 0xed) hi = 0x9f;
    } else if (c >= 0xf0 && c <= 0xf4) {
        len = 4;
        if (c == 0xf0) lo = 0x90;
        if (c == 0xf4) hi = 0x8f;
    } else {
        return 0;
    }
    if (i + len > s.size()) return 0;
    if (b(i + 1) < lo || b(i + 1) > hi) return 0;
    for (size_t k = 2; k < len; ++k) {
        if ((b(i + k) & 0xc0) != 0x80) return 0;
    }
    return len;
}

std::string displayable_ssid(std::string_view ssid) {
    std::string out;
    out.reserve(ssid.size());
    for (size_t i = 0; i < ssid.size();) {
        const auto c = static_cast<uint8_t>(ssid[i]);
        if (c < 0x80) {
            out += c < 0x20 || c == 0x7f ? '?' : static_cast<char>(c);
            ++i;
        } else if (size_t len = utf8_sequence(ssid, i); len != 0) {
            out.append(ssid.substr(i, len));
            i += len;
        } else {
            out += '?';
            ++i;
        }
    }
    return out;
}

const char* state_label(core::DeviceState state) {
    switch (state) {
        case core::DeviceState::Ok:
            return ui::tr(ui::StrId::StateOk);
        case core::DeviceState::Degraded:
            return ui::tr(ui::StrId::StateDegraded);
        case core::DeviceState::Missing:
            return ui::tr(ui::StrId::StateMissing);
        case core::DeviceState::Failed:
            return ui::tr(ui::StrId::StateFailed);
    }
    return ui::tr(ui::StrId::StateFailed);
}

std::string format_time(int64_t unix_s) {
    std::time_t t = static_cast<std::time_t>(unix_s);
    std::tm tm{};
    gmtime_r(&t, &tm);

    // 32 bytes comfortably fits every real calendar date, but GCC's
    // -Wformat-truncation (treated as an error on the ESP-IDF toolchain's
    // default warning flags) conservatively assumes tm_year/tm_mon/etc.
    // could hold any int value and sizes its truncation check off of
    // that, not off of what gmtime_r() actually produces; 80 bytes is
    // large enough that even that worst-case estimate can't trigger it.
    char buf[80];
    std::snprintf(buf, sizeof(buf), "%04d-%02d-%02d %02d:%02d:%02d", tm.tm_year + 1900,
                  tm.tm_mon + 1, tm.tm_mday, tm.tm_hour, tm.tm_min, tm.tm_sec);
    return buf;
}

}  // namespace wisp::app
