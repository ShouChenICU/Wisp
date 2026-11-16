#pragma once

// The JsonAlloc that puts a parse_json_large() document in PSRAM. On the
// device an ordinary small malloc lands in internal SRAM (AGENTS.md
// section 8), and a parsed server response can be tens of KB in many
// small blocks; wisp_base cannot reach os::alloc itself.

#include <cstddef>

#include "wisp/base/json.hpp"
#include "wisp/os/os.hpp"

namespace wisp::net {

inline const JsonAlloc& large_json_alloc() {
    static const JsonAlloc alloc{
        [](size_t n) { return os::alloc(n, os::Mem::Large); },
        [](void* p) { os::dealloc(p); },
        [](void* p, size_t n) { return os::realloc(p, n, os::Mem::Large); },
    };
    return alloc;
}

}  // namespace wisp::net
