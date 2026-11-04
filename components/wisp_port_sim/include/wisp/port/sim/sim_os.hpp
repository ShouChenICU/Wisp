#pragma once

// Test hooks for the simulator's wisp::os (os/os_sim.cpp).

#include "wisp/os/os.hpp"

namespace wisp::port::sim {

// While set, os::alloc() and os::realloc() fail for `kind` the way a
// device does when that memory is missing or exhausted: nullptr, with a
// realloc()'d block left untouched. For tests of fallback paths.
void set_alloc_failure(os::Mem kind, bool fail);

}  // namespace wisp::port::sim
