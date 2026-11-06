#pragma once

// Enforces the single-flash-thread rule (spec 2.1/3.2 of
// docs/superpowers/specs/2026-11-06-flash-thread-design.md): once the
// flash thread (wisp.fs) has registered itself -- set_flash_thread(),
// its first action -- and until it unregisters -- clear_flash_thread(),
// its last action -- only it may touch internal flash: NVS, every
// StorageManager call, the firmware/OTA service, partition reads.
// Outside that window (boot, before wisp.fs starts; shutdown, after it
// stops; and any unit test that never starts a flash thread at all) the
// guard allows everything, since those are the single-threaded phases
// where nothing else can race the flash thread anyway.
//
// Portable: no platform header, built the same way for the host
// simulator and the ESP-IDF target. The registered thread is identified
// with a thread_local flag rather than a platform thread/task handle --
// thread_local is backed on both ports (the simulator's <thread>, and
// FreeRTOS tasks on ESP-IDF via its own per-task TLS slots) without
// wisp_os needing a "current task handle" lookup of its own for this.

#include <atomic>

namespace wisp::os {

// Registers the calling thread as the flash thread.
void set_flash_thread();

// Unregisters the flash thread. Need not be called from the same thread
// as set_flash_thread() (a test may tear down from a different thread
// once the flash thread itself has already exited).
void clear_flash_thread();

// True on the flash thread itself, or whenever no flash thread is
// currently registered (the boot/shutdown phases, and any test that
// never called set_flash_thread()).
bool on_flash_thread();

namespace detail {

// The non-ESP-IDF half of WISP_ASSERT_FLASH_THREAD below: prints `what`
// and aborts if called off the flash thread, otherwise returns. Not for
// direct use -- go through the macro, which also supplies the
// ESP-IDF device behavior.
void flash_guard_abort_if_violated(const char* what);

}  // namespace detail

}  // namespace wisp::os

// Call at every flash entry point (StorageManager's public methods, the
// NVS settings backend, the firmware/OTA service, partition reads) with
// `what` naming the call site, e.g. "StorageManager::write_atomic" (a
// const char*). Off the flash thread:
//   - host builds (the simulator, every ctest): prints the call site and
//     aborts, so code that reaches a flash entry point from the wrong
//     thread fails the very test that exercises it;
//   - ESP-IDF device builds: logs once per call site (the static below
//     is local to each macro expansion, i.e. to each call site, so a
//     tight loop that keeps tripping this doesn't flood the log) via
//     WLOG_E and keeps running. A task whose stack is in PSRAM is
//     already aborted by the hardware before this can even run
//     (AGENTS.md section 5: a flash operation stops the cache, which
//     asserts the stack is internal); a task with an internal stack
//     survives, and this log is the diagnostic trail for why. WLOG_E is
//     not declared here (wisp/core/log.hpp is above wisp_os, which this
//     header must not include -- AGENTS.md section 3); every real call
//     site is in a layer that already includes it.
#if defined(ESP_PLATFORM)
#define WISP_ASSERT_FLASH_THREAD(what)                                                 \
    do {                                                                               \
        if (!::wisp::os::on_flash_thread()) {                                          \
            static std::atomic<bool> wisp_flash_guard_logged_{false};                  \
            if (!wisp_flash_guard_logged_.exchange(true, std::memory_order_relaxed)) { \
                WLOG_E("flash", "flash access off the flash thread: %s", (what));      \
            }                                                                          \
        }                                                                              \
    } while (0)
#else
#define WISP_ASSERT_FLASH_THREAD(what) ::wisp::os::detail::flash_guard_abort_if_violated(what)
#endif
