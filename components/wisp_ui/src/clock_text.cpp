#include "wisp/ui/clock_text.hpp"

#include <cstdio>

namespace wisp::ui {

namespace {

int wrap(int v, int n) {
    v %= n;
    return v < 0 ? v + n : v;
}

}  // namespace

std::string format_clock(int hour, int minute, bool h24, Lang lang) {
    const int h = wrap(hour, 24);
    const int m = wrap(minute, 60);
    // Room for any int: GCC's -Wformat-truncation does not know the ranges.
    char buf[32];
    if (h24) {
        std::snprintf(buf, sizeof(buf), "%02d:%02d", h, m);
        return buf;
    }
    int shown = h % 12;
    if (shown == 0 && lang != Lang::Ja) shown = 12;
    std::snprintf(buf, sizeof(buf), "%d:%02d", shown, m);
    return tr_fill(h < 12 ? StrId::ClockAm : StrId::ClockPm, lang, buf);
}

std::string format_local_clock(int64_t unix_s, int utc_offset_min, bool h24, Lang lang) {
    int64_t local = (unix_s + int64_t{utc_offset_min} * 60) % 86400;
    if (local < 0) local += 86400;
    return format_clock(static_cast<int>(local / 3600), static_cast<int>(local % 3600 / 60), h24,
                        lang);
}

}  // namespace wisp::ui
