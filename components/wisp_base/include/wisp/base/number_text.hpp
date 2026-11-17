#pragma once

// Text for doubles in JSON, without std::to_chars/from_chars for floating
// point: their tables add about 140 KB to the firmware, while snprintf and
// strtod are linked anyway. Both depend on the C locale's decimal point;
// nothing in Wisp calls setlocale, so it is always ".".

#include <cstddef>
#include <string_view>

namespace wisp {

// Room for the longest text format_double() writes ("-1.2345678901234567e-308"
// and the like, or a fixed form no longer than that).
inline constexpr size_t kDoubleTextMax = 32;

// Writes finite `d` as std::to_chars(first, last, d) does and returns the
// length: the fewest significant digits that read back as `d`, in fixed or
// scientific form, whichever is shorter (fixed on a tie), an integral value
// in fixed form with its exact digits. One exception: an integral value of
// 2^64 or more in fixed form comes from the libc's "%.0f", which may pad
// after 17 significant digits with zeros (picolibc does); the text has the
// same length and reads back as `d`. The scientific exponent has at least
// two digits ("1e+05"). Not NUL-terminated. Infinity and NaN write nothing
// and return 0.
size_t format_double(double d, char (&out)[kDoubleTextMax]);

// Reads `text`, which must be exactly one JSON number (RFC 8259: an optional
// '-', no leading zeros, no '+', blanks, hex, "inf" or "nan"), into `out` as
// the nearest double. False, `out` untouched, when it is not one, or when it
// lies outside double's range: it would overflow to infinity, or a nonzero
// value would underflow to 0 (subnormals are read).
bool parse_double(std::string_view text, double& out);

}  // namespace wisp
