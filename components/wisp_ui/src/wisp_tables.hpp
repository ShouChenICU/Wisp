#pragma once

// Compile-time tables of the wisp renderer (wisp_model.cpp,
// wisp_raster.cpp). The doubles below only ever run in the compiler: the
// tables land in read-only data, so a frame never evaluates a float
// transcendental function.

#include <array>
#include <cstdint>

namespace wisp::ui::wisp_detail {

inline constexpr double kPi = 3.14159265358979323846;

constexpr double c_sin(double x) {
    while (x > kPi) x -= 2 * kPi;
    while (x < -kPi) x += 2 * kPi;
    double term = x;
    double sum = x;
    for (int i = 1; i < 14; ++i) {
        term *= -x * x / ((2.0 * i) * (2.0 * i + 1));
        sum += term;
    }
    return sum;
}

constexpr double c_sqrt(double x) {
    if (x <= 0) return 0;
    double r = x > 1 ? x : 1;
    for (int i = 0; i < 40; ++i) r = 0.5 * (r + x / r);
    return r;
}

constexpr int c_round(double v) { return static_cast<int>(v >= 0 ? v + 0.5 : v - 0.5); }

// sin(2 pi i / 1024) in Q15.
inline constexpr std::array<int16_t, 1024> kSin = [] {
    std::array<int16_t, 1024> t{};
    for (int i = 0; i < 1024; ++i) {
        t[i] = static_cast<int16_t>(c_round(c_sin(2 * kPi * i / 1024) * 32767));
    }
    return t;
}();

// Sine of an angle in 1/65536 turns, Q15.
inline int32_t sin16(uint32_t a) { return kSin[(a >> 6) & 1023]; }
inline int32_t cos16(uint32_t a) { return kSin[((a >> 6) + 256) & 1023]; }

// Body and hair luminance by normalized squared distance from the centre
// line: entry i is q = i / 64 (r = sqrt(q), 1 on the outline). Inside, a
// core that brightens towards the middle; across the outline a one-pixel
// edge; outside, a halo that fades out by r = 1.5. Stays at or below 190 so
// the palette's white is the eyes' and the sparks' alone.
inline constexpr int kHaloQ8 = 576;  // r = 1.5, in q * 256
inline constexpr std::array<uint8_t, 256> kBodyLum = [] {
    std::array<uint8_t, 256> t{};
    for (int i = 0; i < 256; ++i) {
        const double r = c_sqrt(i / 64.0);
        const double core = 116 + 74 * (1 - r * r);  // 190 in the middle, 116 at the outline
        const double g = r < 1 ? 1 : (r >= 1.5 ? 0 : 1 - (r - 1) / 0.5);
        const double halo = 84 * g * g * (0.55 + 0.45 * g);
        const double e = 0.025;  // half the outline's width, in r
        double v;
        if (r <= 1 - e) {
            v = core;
        } else if (r >= 1 + e) {
            v = halo;
        } else {
            const double k = (r - (1 - e)) / (2 * e);
            v = core * (1 - k) + halo * k;
        }
        t[i] = static_cast<uint8_t>(c_round(v));
    }
    return t;
}();

// A spark: 8x8 luminance, 255 in the middle, 0 at the corners.
inline constexpr std::array<uint8_t, 64> kSpark = [] {
    std::array<uint8_t, 64> t{};
    for (int y = 0; y < 8; ++y) {
        for (int x = 0; x < 8; ++x) {
            const double dx = x - 3.5, dy = y - 3.5;
            double f = 1 - (dx * dx + dy * dy) / 16.0;
            f = f < 0 ? 0 : f;
            t[y * 8 + x] = static_cast<uint8_t>(c_round(255 * f * f * f));
        }
    }
    return t;
}();

// floor(sqrt(v)).
inline uint32_t isqrt(uint32_t v) {
    uint32_t r = 0;
    uint32_t bit = 1u << 30;
    while (bit > v) bit >>= 2;
    while (bit != 0) {
        if (v >= r + bit) {
            v -= r + bit;
            r = (r >> 1) + bit;
        } else {
            r >>= 1;
        }
        bit >>= 2;
    }
    return r;
}

}  // namespace wisp::ui::wisp_detail
