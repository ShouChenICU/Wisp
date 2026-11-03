#pragma once

#include <cstddef>
#include <cstdint>
#include <functional>

namespace wisp::net {

// Fills `out` with `len` cryptographically strong random bytes (the
// platform's hardware RNG on the device). Injected so tests can script it.
using RandomFn = std::function<void(uint8_t* out, size_t len)>;

}  // namespace wisp::net
