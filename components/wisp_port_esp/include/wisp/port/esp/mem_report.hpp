#pragma once

#include <string>

namespace wisp::port::esp {

// Internal / DMA / PSRAM heap figures (free, largest free block, lowest
// free since boot), one line each, then one line per task: stack size
// (from the stack bounds, so a few bytes under the requested size), stack
// high-water mark (bytes never used) and whether the stack sits in
// internal RAM or PSRAM. Backs the firmware's `mem` console command
// (AGENTS.md section 5). Not reentrant: only the console task calls it.
//
// "min" is a lower bound, not a moment that happened: IDF adds up each
// heap region's own lowest point, and internal RAM is several regions
// whose lows can be minutes apart (boot in one, a server start in
// another).
//
// The task rows are best effort. vTaskSuspendAll() only stops the calling
// core, and the walk takes no kernel lock (uxTaskGetSystemState() would,
// but needs CONFIG_FREERTOS_USE_TRACE_FACILITY), so a task deleted on the
// other core during the walk can garble a row or, rarely, crash the
// device. Do not run `mem` while servers or the hotspot are stopping.
std::string mem_report();

}  // namespace wisp::port::esp
