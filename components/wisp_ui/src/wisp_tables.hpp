#pragma once

// Compile-time tables and integer helpers of the wisp renderer
// (wisp_model.cpp, wisp_shape.cpp, wisp_spans.cpp). The doubles below only ever run in the
// compiler: the tables land in read-only data, so a frame never evaluates a float transcendental
// function.

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

// floor(sqrt(v)) for 64-bit v.
inline uint64_t isqrt64(uint64_t v) {
    uint64_t r = 0;
    uint64_t bit = uint64_t{1} << 62;
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
