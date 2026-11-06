#pragma once

#include <cstddef>

namespace wisp::port::esp {

// Whether a task that declared `flash_access` and `hot_stack`
// (os::TaskConfig, EspHttpServerConfig) may have its stack in PSRAM,
// before asking whether PSRAM has room. On every target this port builds
// for, a flash-touching task keeps an internal stack: without XIP from
// PSRAM a flash read, write or erase stops the cache and asserts that
// the stack is internal, and even with XIP mapping flash
// (esp_partition_mmap, the OTA end-of-upload check) still does. A future
// target where neither holds decides differently here, gated on its IDF
// target, and no caller changes. hot_stack keeps a task's stack internal
// for speed rather than for flash safety (currently only the UI task,
// AGENTS.md section 5); either reason alone is enough to keep it
// internal.
constexpr bool stack_may_use_psram(bool flash_access, bool hot_stack) {
    return !flash_access && !hot_stack;
}

// True when a new task's stack of `bytes` can go to PSRAM right now: the
// build lets FreeRTOS take an external stack
// (CONFIG_FREERTOS_TASK_CREATE_ALLOW_EXT_MEM; without it task creation
// asserts on a PSRAM stack instead of failing) and PSRAM has a block that
// large. Otherwise the caller uses an internal stack.
bool external_stack_available(size_t bytes);

// The placement for a task about to be created: PSRAM only when both of
// the above allow it, else internal RAM.
inline bool place_stack_in_psram(bool flash_access, bool hot_stack, size_t bytes) {
    return stack_may_use_psram(flash_access, hot_stack) && external_stack_available(bytes);
}

}  // namespace wisp::port::esp
