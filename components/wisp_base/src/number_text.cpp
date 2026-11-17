#include "wisp/base/number_text.hpp"

#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <new>

namespace wisp {

namespace {

bool digit(char c) { return c >= '0' && c <= '9'; }

// A decimal: digits[0..n) with the point after the first, times 10^exp.
struct Decimal {
    char digits[20];
    int n = 0;
    int exp = 0;
};

bool reads_back(const Decimal& c, double d) {
    char text[kDoubleTextMax];
    std::snprintf(text, sizeof text, "%c.%.*se%d", c.digits[0], c.n - 1, c.digits + 1, c.exp);
    return std::strtod(text, nullptr) == d;
}

// A `prec`-digit decimal that reads back as `d`, if there is one. The
// nearest does whenever any does, except just above a power of two: the
// gap below it is half the gap above, so the decimal one step up can read
// back when the nearest, below, does not.
bool shortest_at(double d, int prec, Decimal& c) {
    char sci[kDoubleTextMax];
    std::snprintf(sci, sizeof sci, "%.*e", prec - 1, d);
    c.n = 0;
    const char* p = sci;
    for (; *p != 'e'; ++p) {
        if (digit(*p)) c.digits[c.n++] = *p;
    }
    c.exp = std::atoi(p + 1);
    if (reads_back(c, d)) return true;
    int bin_exp = 0;
    if (std::frexp(d, &bin_exp) != 0.5) return false;
    int i = c.n - 1;
    while (i >= 0 && c.digits[i] == '9') c.digits[i--] = '0';
    if (i < 0) {
        c.digits[0] = '1';
        ++c.exp;
    } else {
        ++c.digits[i];
    }
    return reads_back(c, d);
}

}  // namespace

size_t format_double(double d, char (&out)[kDoubleTextMax]) {
    // "inf" and "nan" have no exponent for shortest_at() to stop at.
    if (!std::isfinite(d)) return 0;
    size_t len = 0;
    if (std::signbit(d)) {
        out[len++] = '-';
        d = -d;
    }

    // The fewest significant digits that read back as `d`: 17 always do,
    // and if some count does, every larger one does too.
    // `hi` only moves to a count that read back, so when the search ends
    // `c` already holds that count's digits unless it never moved.
    Decimal c;
    Decimal probe;
    int lo = 1;
    int hi = 17;
    bool found = false;
    while (lo < hi) {
        const int mid = (lo + hi) / 2;
        if (shortest_at(d, mid, probe)) {
            hi = mid;
            c = probe;
            found = true;
        } else {
            lo = mid + 1;
        }
    }
    if (!found) shortest_at(d, lo, c);
    const char* digits = c.digits;
    int n = c.n;
    const int exp = c.exp;
    while (n > 1 && digits[n - 1] == '0') --n;

    // Lengths of both forms, sign left out.
    const int abs_exp = exp < 0 ? -exp : exp;
    const int sci_len = n + (n > 1 ? 1 : 0) + 2 + (abs_exp >= 100 ? 3 : 2);
    int fixed_len = 0;
    if (exp >= n - 1) {
        fixed_len = exp + 1;
    } else if (exp >= 0) {
        fixed_len = n + 1;
    } else {
        fixed_len = n + 1 - exp;
    }

    char* o = out + len;
    if (fixed_len <= sci_len) {
        if (exp >= n - 1) {
            // Integral: its exact digits, like to_chars (the shortest digits
            // padded with zeros would read back too, but are not the value).
            // From 2^64 up this relies on "%.0f"; picolibc prints at most
            // 17 significant digits and pads the rest with zeros, which has
            // the same length and still reads back, but is not exact.
            if (d < 18446744073709551616.0) {
                o = std::to_chars(o, out + kDoubleTextMax, static_cast<uint64_t>(d)).ptr;
            } else {
                o += std::snprintf(o, static_cast<size_t>(out + kDoubleTextMax - o), "%.0f", d);
            }
        } else if (exp >= 0) {
            std::memcpy(o, digits, static_cast<size_t>(exp) + 1);
            o += exp + 1;
            *o++ = '.';
            std::memcpy(o, digits + exp + 1, static_cast<size_t>(n - exp - 1));
            o += n - exp - 1;
        } else {
            *o++ = '0';
            *o++ = '.';
            for (int i = 0; i < -exp - 1; ++i) *o++ = '0';
            std::memcpy(o, digits, static_cast<size_t>(n));
            o += n;
        }
    } else {
        *o++ = digits[0];
        if (n > 1) {
            *o++ = '.';
            std::memcpy(o, digits + 1, static_cast<size_t>(n - 1));
            o += n - 1;
        }
        *o++ = 'e';
        *o++ = exp < 0 ? '-' : '+';
        if (abs_exp < 10) *o++ = '0';
        o = std::to_chars(o, out + kDoubleTextMax, abs_exp).ptr;
    }
    return static_cast<size_t>(o - out);
}

bool parse_double(std::string_view text, double& out) {
    // JSON's grammar first: strtod alone would take '+', blanks, hex and
    // the inf/nan words.
    const char* p = text.data();
    const char* end = p + text.size();
    bool nonzero = false;
    if (p < end && *p == '-') ++p;
    if (p >= end || !digit(*p)) return false;
    if (*p == '0') {
        ++p;
    } else {
        for (; p < end && digit(*p); ++p) nonzero = true;
    }
    if (p < end && *p == '.') {
        ++p;
        if (p >= end || !digit(*p)) return false;
        for (; p < end && digit(*p); ++p) nonzero = nonzero || *p != '0';
    }
    if (p < end && (*p == 'e' || *p == 'E')) {
        ++p;
        if (p < end && (*p == '+' || *p == '-')) ++p;
        if (p >= end || !digit(*p)) return false;
        while (p < end && digit(*p)) ++p;
    }
    if (p != end) return false;

    // strtod needs a terminated string, and the view may run on into more
    // digits. Numbers are short; a long one goes to the heap briefly.
    char small[64];
    char* buf = small;
    if (text.size() >= sizeof small) {
        buf = new (std::nothrow) char[text.size() + 1];
        if (buf == nullptr) return false;
    }
    std::memcpy(buf, text.data(), text.size());
    buf[text.size()] = '\0';
    const double v = std::strtod(buf, nullptr);
    if (buf != small) delete[] buf;

    if (std::isinf(v) || (v == 0 && nonzero)) return false;
    out = v;
    return true;
}

}  // namespace wisp
