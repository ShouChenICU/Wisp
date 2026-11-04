#pragma once

#include <cstddef>
#include <cstdint>
#include <span>

#include "wisp/base/result.hpp"

namespace wisp {

// Decompresses a raw DEFLATE stream (RFC 1951: no zlib or gzip wrapper)
// from `in` into `out` and returns the number of bytes written. The whole
// output lives in `out`, so back-references read straight from it and no
// separate 32 KB window is kept. Decoding stops at the end of the final
// block; any bytes after it are ignored.
//
// Errors (all ErrorCode::InvalidArgument): a malformed or truncated
// stream, a back-reference before the start of the output, or output that
// does not fit in `out`.
Result<size_t> inflate_raw(std::span<const uint8_t> in, std::span<uint8_t> out);

}  // namespace wisp
