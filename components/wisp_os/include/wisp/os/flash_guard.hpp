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
// A second role, the SD I/O thread, does SD-card file I/O so that slow
// card access does not hold up internal flash work. It registers the
// same way (set_sd_io_thread()/clear_sd_io_thread(), same take-over and
// stale-clear rules) and the two rules are:
//   - flash (WISP_ASSERT_FLASH_THREAD): the flash thread only, or anyone
//     while no flash thread is registered -- but never the SD I/O thread,
//     which must not touch internal flash in any phase;
//   - SD (WISP_ASSERT_SD_IO_THREAD): the SD I/O thread, or anyone the
//     flash rule allows (the flash thread can still reach the card, e.g.
//     before an SD I/O thread exists).
//
// Portable: no platform header, built the same way for the host
// simulator and the ESP-IDF target. The registered thread is identified
// by a thread_local id rather than a platform thread/task handle --
// thread_local is backed on both ports (the simulator's <thread>, and
// FreeRTOS tasks on ESP-IDF via its own per-task TLS slots) without
// wisp_os needing a "current task handle" lookup of its own for this.

#include <atomic>

namespace wisp::os {

// Registers the calling thread as the flash thread.
void set_flash_thread();

// Unregisters the calling thread if it is the registered flash thread;
// a no-op on any other thread. A later set_flash_thread() on another
// thread takes over, and the earlier thread's clear then leaves the newer
// registration alone -- so two workers alive at once (in a test) cannot
// switch the guard off for each other.
void clear_flash_thread();

// True on the flash thread itself, or whenever no flash thread is
// currently registered (the boot/shutdown phases, and any test that
// never called set_flash_thread()). Always false on the registered SD
// I/O thread.
bool on_flash_thread();

// Registers the calling thread as the SD I/O thread.
void set_sd_io_thread();

// Unregisters the calling thread if it is the registered SD I/O thread;
// a no-op on any other thread, with the same take-over rule as
// clear_flash_thread().
void clear_sd_io_thread();

// True only on the registered SD I/O thread; false everywhere when none
// is registered.
bool on_sd_io_thread();

// Whether SD-card file I/O is allowed here: on the SD I/O thread, or
// wherever on_flash_thread() is true.
bool sd_io_allowed();

// Called, at most once per call site, on an ESP-IDF device build's
// violation (see WISP_ASSERT_FLASH_THREAD below). wisp_os cannot call
// WLOG_E itself (wisp/core/log.hpp sits above it -- AGENTS.md section 3),
// so it reports through this hook instead: a higher layer installs one at
// boot (wisp_core's logging setup installs one that calls WLOG_E) via
// set_flash_violation_reporter(). Unlike the earlier approach of simply
// naming WLOG_E inside the macro and relying on every call site to have
// included wisp/core/log.hpp first, this compiles independently of what
// the call site itself includes. Null (the default, and every build that
// never installs one, including every host/sim build, which never
// reaches the ESP-IDF branch below at all): a violation is just not
// reported through this path.
using FlashViolationReporter = void (*)(const char* what);
void set_flash_violation_reporter(FlashViolationReporter reporter);

namespace detail {

// The non-ESP-IDF half of WISP_ASSERT_FLASH_THREAD below: prints `what`
// and aborts if called off the flash thread, otherwise returns. Not for
// direct use -- go through the macro, which also supplies the
// ESP-IDF device behavior.
void flash_guard_abort_if_violated(const char* what);

// The non-ESP-IDF half of WISP_ASSERT_SD_IO_THREAD below: prints `what`
// and aborts unless sd_io_allowed(). Go through the macro.
void sd_guard_abort_if_violated(const char* what);

// Calls the reporter installed by set_flash_violation_reporter(), if any;
// a silent no-op otherwise. Used by WISP_ASSERT_FLASH_THREAD's ESP-IDF
// device branch below; exposed here (rather than kept private to
// flash_guard.cpp) so the hook itself can be unit-tested on the host
// without needing an ESP-IDF build.
void flash_guard_report(const char* what);

}  // namespace detail

}  // namespace wisp::os

// Call at every flash entry point (StorageManager's public methods, the
// NVS settings backend, the firmware/OTA service, partition reads) with
// `what` naming the call site, e.g. "StorageManager::write_atomic" (a
// const char*). Off the flash thread:
//   - host builds (the simulator, every ctest): prints the call site and
//     aborts, so code that reaches a flash entry point from the wrong
//     thread fails the very test that exercises it;
//   - ESP-IDF device builds: reports once per call site (the static
//     below is local to each macro expansion, i.e. to each call site, so
//     a tight loop that keeps tripping this doesn't flood the log)
//     through the reporter hook above, and keeps running. A task whose
//     stack is in PSRAM is already aborted by the hardware before this
//     can even run (AGENTS.md section 5: a flash operation stops the
//     cache, which asserts the stack is internal); a task with an
//     internal stack survives, and the report is the diagnostic trail
//     for why.
#if defined(ESP_PLATFORM)
#define WISP_ASSERT_FLASH_THREAD(what)                                                 \
    do {                                                                               \
        if (!::wisp::os::on_flash_thread()) {                                          \
            static std::atomic<bool> wisp_flash_guard_logged_{false};                  \
            if (!wisp_flash_guard_logged_.exchange(true, std::memory_order_relaxed)) { \
                ::wisp::os::detail::flash_guard_report(what);                          \
            }                                                                          \
        }                                                                              \
    } while (0)
#else
#define WISP_ASSERT_FLASH_THREAD(what) ::wisp::os::detail::flash_guard_abort_if_violated(what)
#endif

// Call at every SD-card file I/O entry point, with `what` naming the call
// site. Off both the SD I/O thread and the flash thread (sd_io_allowed()
// false): host builds print the call site and abort; ESP-IDF device
// builds report once per call site through the same reporter hook as
// WISP_ASSERT_FLASH_THREAD and keep running.
#if defined(ESP_PLATFORM)
#define WISP_ASSERT_SD_IO_THREAD(what)                                              \
    do {                                                                            \
        if (!::wisp::os::sd_io_allowed()) {                                         \
            static std::atomic<bool> wisp_sd_guard_logged_{false};                  \
            if (!wisp_sd_guard_logged_.exchange(true, std::memory_order_relaxed)) { \
                ::wisp::os::detail::flash_guard_report(what);                       \
            }                                                                       \
        }                                                                           \
    } while (0)
#else
#define WISP_ASSERT_SD_IO_THREAD(what) ::wisp::os::detail::sd_guard_abort_if_violated(what)
#endif

// For code that serves both media (e.g. a storage area that may sit on
// internal flash or on the SD card): `on_sd` (a bool) picks the SD rule
// when true, else the flash rule.
#define WISP_ASSERT_MEDIUM_THREAD(on_sd, what) \
    do {                                       \
        if (on_sd) {                           \
            WISP_ASSERT_SD_IO_THREAD(what);    \
        } else {                               \
            WISP_ASSERT_FLASH_THREAD(what);    \
        }                                      \
    } while (0)
