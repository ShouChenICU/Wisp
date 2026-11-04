#pragma once

// The internal-RAM budget (AGENTS.md section 5). Internal SRAM is what DMA
// buffers, the stacks of tasks that touch flash and the radio need, and
// running short of it fails late and far from the cause (an httpd that
// cannot get its stack). NetWiring logs the figures each time a server
// starts and warns when they are under the idle floor below. Measured per
// board in docs/hardware/<board>.md; the `mem` console command on the ESP
// firmware prints them with every task's stack.
//
// The figures were set on the one board measured so far
// (waveshare_amoled_216, ESP32-S3 with PSRAM). A board with much less
// internal RAM would warn on every server start; give it its own floors
// (a board cap) rather than loosening these.

#include <cstdint>

#include "wisp/base/value.hpp"

namespace wisp::app {

// Must still be free at the worst moment: station and hotspot both up,
// both httpd instances, a WebSocket log stream, a TLS handshake and a
// firmware upload.
inline constexpr int64_t kWorstInternalFreeMin = 40 * 1024;
// Largest free block at that moment: an httpd restart needs 10 KB at once.
inline constexpr int64_t kWorstInternalLargestMin = 16 * 1024;
// Idle with everything above running except the transient work: the worst
// floor plus the largest dip measured (a TLS handshake, about 20 KB).
inline constexpr int64_t kIdleInternalFreeMin = kWorstInternalFreeMin + 20 * 1024;
inline constexpr int64_t kIdleInternalLargestMin = 20 * 1024;

struct InternalRam {
    int64_t free = -1;     // bytes; -1 unknown
    int64_t largest = -1;  // largest free block, bytes; -1 unknown
};

// "internal_free"/"internal_largest" from Platform::heap_info()'s map.
InternalRam internal_ram(const Value& heap_info);

// Under the idle floor. False when free is unknown (the simulator); a
// missing largest-block figure only skips that half of the check.
bool internal_ram_low(const InternalRam& ram);

}  // namespace wisp::app
