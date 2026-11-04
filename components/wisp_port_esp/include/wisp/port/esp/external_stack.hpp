#pragma once

#include <cstddef>

namespace wisp::port::esp {

// True when a new task's stack of `bytes` can go to PSRAM right now: the
// build lets FreeRTOS take an external stack
// (CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM; without it task creation
// asserts on a PSRAM stack instead of failing) and PSRAM has a block that
// large. Otherwise the caller uses an internal stack. Backs
// os::TaskConfig::external_stack and EspHttpServerConfig::external_stack.
bool external_stack_available(size_t bytes);

}  // namespace wisp::port::esp
