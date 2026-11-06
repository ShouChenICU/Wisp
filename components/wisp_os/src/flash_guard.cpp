#include "wisp/os/flash_guard.hpp"

#include <cstdint>
#include <cstdio>
#include <cstdlib>

namespace wisp::os {

namespace {

// The id of the registered flash thread, 0 when none is (before the first
// set_flash_thread() and after the owner's clear_flash_thread(): the
// single-threaded boot/shutdown phases, where on_flash_thread() must
// allow everything).
std::atomic<uint32_t> g_owner{0};

// Never-reused per-thread ids, handed out on a thread's first use of the
// guard. A counter rather than a thread_local's address: a thread that
// exits while still registered must not be impersonated by a later one
// that happens to get the same TLS slot.
std::atomic<uint32_t> g_next_id{1};
thread_local uint32_t t_id = 0;

uint32_t this_thread_id() {
    if (t_id == 0) {
        t_id = g_next_id.fetch_add(1, std::memory_order_relaxed);
    }
    return t_id;
}

// Installed by set_flash_violation_reporter(); null until a higher layer
// installs one at boot. A plain function pointer (not std::function), so
// the atomic itself can be lock-free and the hook adds no allocation on
// the violation path.
std::atomic<FlashViolationReporter> g_flash_violation_reporter{nullptr};

}  // namespace

void set_flash_thread() { g_owner.store(this_thread_id(), std::memory_order_release); }

void clear_flash_thread() {
    uint32_t self = this_thread_id();
    g_owner.compare_exchange_strong(self, 0, std::memory_order_acq_rel);
}

bool on_flash_thread() {
    uint32_t owner = g_owner.load(std::memory_order_acquire);
    return owner == 0 || owner == this_thread_id();
}

void set_flash_violation_reporter(FlashViolationReporter reporter) {
    g_flash_violation_reporter.store(reporter, std::memory_order_release);
}

namespace detail {

void flash_guard_abort_if_violated(const char* what) {
    if (on_flash_thread()) {
        return;
    }
    std::fprintf(stderr, "wisp::os: flash access off the flash thread: %s\n",
                 what != nullptr ? what : "(unknown)");
    std::fflush(stderr);
    std::abort();
}

void flash_guard_report(const char* what) {
    FlashViolationReporter reporter = g_flash_violation_reporter.load(std::memory_order_acquire);
    if (reporter != nullptr) {
        reporter(what);
    }
}

}  // namespace detail

}  // namespace wisp::os
