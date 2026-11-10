#pragma once

// Pure, platform-free bookkeeping behind the firmware's light-sleep
// statistics (wisp/port/esp/sleep_stats.hpp, the `sleep` console command):
// the record that lives in RTC memory, how one light sleep is added to it,
// how a session starts and ends, and how a session is printed. No ESP-IDF
// include, so it is unit-tested on the host
// (tests/port_esp/sleep_record_test.cpp).
//
// A session runs from automatic light sleep being enabled (deep standby) to
// it being disabled. The record keeps the running session and the last
// finished one. It sits in RTC_NOINIT memory, which survives the chip reset
// that opening the USB serial port can cause, but not power loss: boot()
// therefore throws away a record whose magic or checksums do not match, and
// closes a session that was still running when the chip reset ("ended by
// a reset"; its length is up to its last light sleep, and its end battery
// is unknown).
//
// note_sleep() runs in ESP-IDF's light-sleep exit callback, in the idle
// task with interrupts off: it only does arithmetic on the record, with no
// allocation, logging or lock. Every other function here runs on a normal
// task while light sleep cannot happen (sleep_stats.cpp holds a
// no-light-sleep PM lock around them), so nothing races with it.
//
// Every type is plain data with no default member initialisers: the record
// is a static in a no-init section, which must not get a constructor.

#include <cstddef>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <optional>
#include <string>

namespace wisp::port::esp {

// One power.main reading. `valid` is 0 when there was none (no device, a
// failed read, or a session ended by a reset).
struct SleepBattery {
    int16_t percent;  // -1 if the gauge does not know
    uint16_t mv;
    uint8_t charging;
    uint8_t usb_present;  // VBUS: on USB power, the battery figures mean nothing
    uint8_t valid;
    uint8_t reserved;
};

inline constexpr int kSleepBuckets = 6;
// One counter per bit of esp_sleep_get_wakeup_causes() (a bit per
// esp_sleep_wakeup_cause_t value).
inline constexpr int kSleepCauses = 32;

struct SleepSession {
    uint32_t flags;      // sleep_record::kActive and friends
    uint32_t sleeps;     // light sleeps that actually slept (slept_us > 0)
    int64_t start_us;    // boot clock (esp_timer) at the start; this boot only
    int64_t elapsed_us;  // start to end, or to the latest sleep while running
    int64_t asleep_us;
    int64_t longest_us;
    int64_t shortest_us;  // meaningless while sleeps == 0
    uint32_t buckets[kSleepBuckets];
    uint32_t causes[kSleepCauses];
    SleepBattery start_battery;
    SleepBattery end_battery;
    uint32_t checksum;  // over every byte before this field
};
static_assert(offsetof(SleepSession, checksum) == 216, "SleepSession must have no padding");

struct SleepRecord {
    uint32_t magic;
    uint32_t version;
    SleepSession current;
    SleepSession last;
};

namespace sleep_record {

inline constexpr uint32_t kMagic = 0x57534c50;  // "WSLP"
inline constexpr uint32_t kVersion = 1;

inline constexpr uint32_t kActive = 1u << 0;
inline constexpr uint32_t kUsjAtStart = 1u << 1;  // the USB debug port was connected
inline constexpr uint32_t kEndedByReset = 1u << 2;

// Upper bounds of the histogram buckets, in microseconds; the last bucket
// is everything from 5 s up.
inline constexpr int64_t kBucketUpperUs[kSleepBuckets - 1] = {10000, 50000, 200000, 1000000,
                                                              5000000};

inline int bucket(int64_t slept_us) {
    for (int i = 0; i < kSleepBuckets - 1; ++i) {
        if (slept_us < kBucketUpperUs[i]) return i;
    }
    return kSleepBuckets - 1;
}

// FNV-1a over the session's bytes up to its checksum field. The structs
// have no padding before that field (8-byte members first, then 4-byte
// ones, then the 8-byte battery pairs).
inline uint32_t checksum(const SleepSession& s) {
    const auto* p = reinterpret_cast<const uint8_t*>(&s);
    uint32_t h = 2166136261u;
    for (size_t i = 0; i < offsetof(SleepSession, checksum); ++i) {
        h = (h ^ p[i]) * 16777619u;
    }
    return h;
}

inline void seal(SleepSession& s) { s.checksum = checksum(s); }

inline void clear(SleepSession& s) {
    std::memset(&s, 0, sizeof(s));
    seal(s);
}

inline bool valid(const SleepRecord& r) {
    return r.magic == kMagic && r.version == kVersion &&
           r.current.checksum == checksum(r.current) && r.last.checksum == checksum(r.last);
}

inline void init(SleepRecord& r) {
    std::memset(&r, 0, sizeof(r));
    r.magic = kMagic;
    r.version = kVersion;
    seal(r.current);
    seal(r.last);
}

// At boot: keeps a valid record, closing a session the reset interrupted;
// anything else (power-on garbage, a torn write) starts over.
inline void boot(SleepRecord& r) {
    if (!valid(r)) {
        init(r);
        return;
    }
    if (r.current.flags & kActive) {
        r.last = r.current;
        r.last.flags = (r.last.flags & ~kActive) | kEndedByReset;
        std::memset(&r.last.end_battery, 0, sizeof(r.last.end_battery));
        seal(r.last);
        clear(r.current);
    }
}

// Light-sleep exit: `slept_us` is what ESP-IDF measured (0 when the idle
// period was too short to try), `causes` esp_sleep_get_wakeup_causes().
inline void note_sleep(SleepSession& s, int64_t now_us, int64_t slept_us, uint32_t causes) {
    if (!(s.flags & kActive) || slept_us <= 0) return;
    if (s.sleeps == 0 || slept_us < s.shortest_us) s.shortest_us = slept_us;
    if (slept_us > s.longest_us) s.longest_us = slept_us;
    ++s.sleeps;
    s.asleep_us += slept_us;
    ++s.buckets[bucket(slept_us)];
    for (int bit = 0; bit < kSleepCauses; ++bit) {
        if (causes & (1u << bit)) ++s.causes[bit];
    }
    s.elapsed_us = now_us - s.start_us;
    seal(s);
}

// Ends the running session into `last`; false when none was running.
inline bool end(SleepRecord& r, int64_t now_us, const SleepBattery& battery) {
    if (!(r.current.flags & kActive)) return false;
    r.last = r.current;
    r.last.flags &= ~kActive;
    r.last.elapsed_us = now_us - r.last.start_us;
    r.last.end_battery = battery;
    seal(r.last);
    clear(r.current);
    return true;
}

// Starts a session, ending a running one first (with no end battery).
inline void begin(SleepRecord& r, int64_t now_us, const SleepBattery& battery, bool usj) {
    end(r, now_us, SleepBattery{});
    std::memset(&r.current, 0, sizeof(r.current));
    r.current.flags = kActive | (usj ? kUsjAtStart : 0u);
    r.current.start_us = now_us;
    r.current.start_battery = battery;
    seal(r.current);
}

// Battery percent lost per hour, when both ends have a known percent and
// the session has a length.
inline std::optional<double> drain_per_hour(const SleepSession& s) {
    const SleepBattery& a = s.start_battery;
    const SleepBattery& b = s.end_battery;
    if (!a.valid || !b.valid || a.percent < 0 || b.percent < 0 || s.elapsed_us <= 0) {
        return std::nullopt;
    }
    return static_cast<double>(a.percent - b.percent) * 3600e6 / static_cast<double>(s.elapsed_us);
}

// The bucket holding the median sleep, or -1 with no sleeps.
inline int median_bucket(const SleepSession& s) {
    if (s.sleeps == 0) return -1;
    uint64_t seen = 0;
    for (int i = 0; i < kSleepBuckets; ++i) {
        seen += s.buckets[i];
        if (seen * 2 >= s.sleeps) return i;
    }
    return kSleepBuckets - 1;
}

// "0s", "250 ms", "45s", "12m05s", "7h12m".
inline std::string format_duration(int64_t us) {
    char buf[32];
    if (us <= 0) return "0s";
    if (us < 1000000) {
        std::snprintf(buf, sizeof(buf), "%lld ms", static_cast<long long>(us / 1000));
        return buf;
    }
    const long long s = us / 1000000;
    if (s < 60) {
        std::snprintf(buf, sizeof(buf), "%llds", s);
    } else if (s < 3600) {
        std::snprintf(buf, sizeof(buf), "%lldm%02llds", s / 60, s % 60);
    } else {
        std::snprintf(buf, sizeof(buf), "%lldh%02lldm", s / 3600, (s % 3600) / 60);
    }
    return buf;
}

// Names one bit of the wake-cause bitmap; nullptr for "cause<bit>".
using CauseName = const char* (*)(int bit);

namespace detail {

inline constexpr const char* kBucketLabels[kSleepBuckets] = {"<10ms",    "10-50ms", "50-200ms",
                                                             "200ms-1s", "1-5s",    ">=5s"};
inline constexpr const char* kMedianLabels[kSleepBuckets] = {"<10 ms",     "10-50 ms", "50-200 ms",
                                                             "200 ms-1 s", "1-5 s",    ">=5 s"};

inline void append(std::string& out, const char* fmt, auto... args) {
    char buf[96];
    std::snprintf(buf, sizeof(buf), fmt, args...);
    out += buf;
}

inline std::string length(int64_t us) {
    char buf[32];
    if (us < 1000000) {
        std::snprintf(buf, sizeof(buf), "%.1f ms", static_cast<double>(us) / 1000.0);
    } else {
        std::snprintf(buf, sizeof(buf), "%.2f s", static_cast<double>(us) / 1e6);
    }
    return buf;
}

inline std::string wakes(const SleepSession& s, CauseName name) {
    std::string out;
    for (int bit = 0; bit < kSleepCauses; ++bit) {
        if (s.causes[bit] == 0) continue;
        if (!out.empty()) out += ' ';
        const char* n = name != nullptr ? name(bit) : nullptr;
        if (n != nullptr) {
            append(out, "%s %u", n, static_cast<unsigned>(s.causes[bit]));
        } else {
            append(out, "cause%d %u", bit, static_cast<unsigned>(s.causes[bit]));
        }
    }
    return out.empty() ? "none" : out;
}

inline int asleep_percent(const SleepSession& s) {
    if (s.elapsed_us <= 0) return 0;
    return static_cast<int>(s.asleep_us * 100 / s.elapsed_us);
}

inline std::string battery(const SleepBattery& b) {
    if (!b.valid || b.percent < 0) return "?";
    std::string out;
    append(out, "%d%%", static_cast<int>(b.percent));
    return out;
}

}  // namespace detail

// One line for the log when a session ends.
inline std::string summary(const SleepSession& s, CauseName name) {
    std::string out = "light sleep session: " + format_duration(s.elapsed_us);
    detail::append(out, ", asleep %d%%, %u sleeps", detail::asleep_percent(s),
                   static_cast<unsigned>(s.sleeps));
    const int median = median_bucket(s);
    if (median >= 0) {
        out += ", median ";
        out += detail::kMedianLabels[median];
    }
    out += ", wakes: " + detail::wakes(s, name);
    out +=
        ", battery " + detail::battery(s.start_battery) + " -> " + detail::battery(s.end_battery);
    if (s.start_battery.usb_present || (s.flags & kUsjAtStart)) {
        out += " (USB connected: not a battery figure)";
    } else if (auto rate = drain_per_hour(s)) {
        detail::append(out, " (%.1f%%/h)", *rate);
    }
    return out;
}

// The multi-line form the console prints, each line ending in '\n'.
inline std::string report(const SleepSession& s, const char* title, CauseName name) {
    std::string out = title;
    const bool running = (s.flags & kActive) != 0;
    if (!running && s.start_us == 0 && s.elapsed_us == 0 && s.sleeps == 0 &&
        !s.start_battery.valid) {
        return out + ": none\n";
    }
    if (running) {
        out += " (running)";
    } else if (s.flags & kEndedByReset) {
        out += " (ended by a reset, up to the last wake)";
    }
    out += ": " + format_duration(s.elapsed_us) + "\n";
    const double share =
        s.elapsed_us > 0 ? static_cast<double>(s.asleep_us) * 100.0 / s.elapsed_us : 0.0;
    out += "  asleep " + format_duration(s.asleep_us);
    detail::append(out, " (%.1f%%), %u sleeps", share, static_cast<unsigned>(s.sleeps));
    if (s.sleeps > 0) {
        out += ", shortest " + detail::length(s.shortest_us) + ", longest " +
               detail::length(s.longest_us);
    }
    out += "\n  lengths:";
    for (int i = 0; i < kSleepBuckets; ++i) {
        detail::append(out, "%s %s %u", i == 0 ? "" : ",", detail::kBucketLabels[i],
                       static_cast<unsigned>(s.buckets[i]));
    }
    const int median = median_bucket(s);
    if (median >= 0) {
        out += " (median ";
        out += detail::kMedianLabels[median];
        out += ")";
    }
    out += "\n  wakes: " + detail::wakes(s, name) + "\n";
    auto battery_line = [](const SleepBattery& b) {
        if (!b.valid) return std::string("?");
        std::string text = detail::battery(b);
        detail::append(text, " %u mV", static_cast<unsigned>(b.mv));
        if (b.charging) text += " charging";
        return text;
    };
    out += "  battery: start " + battery_line(s.start_battery) + ", end " +
           battery_line(s.end_battery);
    if (auto rate = drain_per_hour(s)) {
        detail::append(out, ", %.1f%%/h", *rate);
    }
    out += "\n";
    if (s.flags & kUsjAtStart) {
        out +=
            "  note: USB debug port connected at start: it holds light sleep off while "
            "connected\n";
    }
    if (s.start_battery.usb_present) {
        out += "  note: USB power present at start: the battery figures mean nothing\n";
    }
    return out;
}

}  // namespace sleep_record

}  // namespace wisp::port::esp
