#pragma once

#include "sdkconfig.h"
#include "wisp/port/esp/target_facts.hpp"

namespace wisp::port::esp {

static_assert(find_target_facts(CONFIG_IDF_TARGET) != nullptr,
              "add this target to kTargetFacts in wisp/port/esp/target_facts.hpp");

// The facts of the target this firmware is built for.
inline constexpr const TargetFacts& kTarget = *find_target_facts(CONFIG_IDF_TARGET);

}  // namespace wisp::port::esp
