#include "wisp/os/flash_guard.hpp"

#include <cstdio>
#include <cstdlib>

namespace wisp::os {

namespace {

// Whether any thread is currently registered as the flash thread; false
// both before the first set_flash_thread() and after every
// clear_flash_thread() (the single-threaded boot/shutdown phases, where
// on_flash_thread() must allow everything).
std::atomic<bool> g_flash_thread_registered{false};

// Set true only on the thread that called set_flash_thread(), by that
// very call -- every other thread's copy stays false. Cleared on
// clear_flash_thread() too, so a thread that briefly was the flash
// thread (tests reuse OS threads across cases) doesn't misreport itself
// as still being it.
thread_local bool g_is_flash_thread = false;

}  // namespace

void set_flash_thread() {
    g_is_flash_thread = true;
    g_flash_thread_registered.store(true, std::memory_order_release);
}

void clear_flash_thread() {
    g_flash_thread_registered.store(false, std::memory_order_release);
    g_is_flash_thread = false;
}

bool on_flash_thread() {
    return !g_flash_thread_registered.load(std::memory_order_acquire) || g_is_flash_thread;
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

}  // namespace detail

}  // namespace wisp::os
