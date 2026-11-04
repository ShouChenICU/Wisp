// LVGL 9.5's LV_STDLIB_CUSTOM memory hooks (see LVGL's src/stdlib/lv_mem.h),
// backed by os::alloc(Mem::Large). See wisp/ui/lv_heap.hpp for why.
//
// Built into a different target per platform: on ESP-IDF it is one of
// wisp_ui's SRCS (kept in the link by "-u lv_malloc_core", see
// components/wisp_ui/CMakeLists.txt); on
// the host simulator/CMakeLists.txt compiles it into the static `lvgl`
// target itself, because lvgl -> wisp_ui -> lvgl would be a cycle between
// static libraries.
//
// lv_malloc()/lv_realloc() in LVGL's lv_mem.c already handle size 0 (they
// hand out a sentinel and never reach these hooks with it) and log
// failures, so the hooks below only allocate and count.

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>

#include "lvgl.h"
#include "wisp/os/os.hpp"
#include "wisp/ui/lv_heap.hpp"

#ifndef WISP_LV_CONF
#error "wisp lv_conf.h not in use"
#endif
static_assert(LV_USE_STDLIB_MALLOC == LV_STDLIB_CUSTOM,
              "lv_mem_wisp.cpp provides the LV_STDLIB_CUSTOM hooks only");

namespace {

using wisp::os::Mem;

// Every block starts with a header holding the size LVGL asked for and
// whether the block fell back to internal RAM, so free and realloc keep
// both counters exact. 16 bytes rather than sizeof(Header) so the pointer
// handed to LVGL keeps the base allocation's alignment on every platform.
struct Header {
    size_t size;
    size_t internal;  // 1 when the block is in Mem::Fast
};
constexpr size_t kHeader = 16;
static_assert(sizeof(Header) <= kHeader);

std::atomic<size_t> g_bytes{0};
std::atomic<size_t> g_internal{0};

void* block_of(void* user) { return static_cast<uint8_t*>(user) - kHeader; }

Header header_of(void* block) {
    Header h{};
    std::memcpy(&h, block, sizeof(h));
    return h;
}

void* finish(void* block, size_t size, bool internal) {
    const Header h{size, internal ? 1u : 0u};
    std::memcpy(block, &h, sizeof(h));
    g_bytes.fetch_add(size, std::memory_order_relaxed);
    if (internal) {
        g_internal.fetch_add(size, std::memory_order_relaxed);
    }
    return static_cast<uint8_t*>(block) + kHeader;
}

void uncount(const Header& h) {
    g_bytes.fetch_sub(h.size, std::memory_order_relaxed);
    if (h.internal != 0) {
        g_internal.fetch_sub(h.size, std::memory_order_relaxed);
    }
}

}  // namespace

extern "C" {

void lv_mem_init(void) {}

void lv_mem_deinit(void) {}

lv_mem_pool_t lv_mem_add_pool(void* mem, size_t bytes) {
    // Pools only exist for LV_STDLIB_BUILTIN's TLSF heap.
    LV_UNUSED(mem);
    LV_UNUSED(bytes);
    return nullptr;
}

void lv_mem_remove_pool(lv_mem_pool_t pool) { LV_UNUSED(pool); }

// PSRAM first. A board without PSRAM, or one that has run out of it, still
// gets a working UI from internal RAM rather than none at all (devices
// degrade, AGENTS.md section 3); wisp/ui/lv_heap.hpp says what that costs.
void* lv_malloc_core(size_t size) {
    if (size > SIZE_MAX - kHeader) {
        return nullptr;
    }
    bool internal = false;
    void* block = wisp::os::alloc(size + kHeader, Mem::Large);
    if (block == nullptr) {
        internal = true;
        block = wisp::os::alloc(size + kHeader, Mem::Fast);
    }
    if (block == nullptr) {
        return nullptr;
    }
    return finish(block, size, internal);
}

void* lv_realloc_core(void* p, size_t new_size) {
    if (p == nullptr) {
        return lv_malloc_core(new_size);
    }
    if (new_size > SIZE_MAX - kHeader) {
        return nullptr;
    }
    void* block = block_of(p);
    const Header old = header_of(block);
    // A block that fell back to internal RAM moves to PSRAM here as soon
    // as PSRAM has room again.
    bool internal = false;
    void* moved = wisp::os::realloc(block, new_size + kHeader, Mem::Large);
    if (moved == nullptr) {
        // Same fallback as lv_malloc_core(), copied by hand since
        // os::realloc() can only target one kind of memory.
        internal = true;
        moved = wisp::os::alloc(new_size + kHeader, Mem::Fast);
        if (moved == nullptr) {
            return nullptr;
        }
        std::memcpy(moved, block, kHeader + (old.size < new_size ? old.size : new_size));
        wisp::os::dealloc(block);
    }
    uncount(old);
    return finish(moved, new_size, internal);
}

void lv_free_core(void* p) {
    if (p == nullptr) {
        return;
    }
    void* block = block_of(p);
    uncount(header_of(block));
    wisp::os::dealloc(block);
}

void lv_mem_monitor_core(lv_mem_monitor_t* mon_p) {
    // The platform heap owns free-space and fragmentation figures; LVGL's
    // own share is wisp::ui::lv_heap_bytes(). lv_mem_monitor() has already
    // zeroed *mon_p.
    LV_UNUSED(mon_p);
}

lv_result_t lv_mem_test_core(void) { return LV_RESULT_OK; }

}  // extern "C"

namespace wisp::ui {

size_t lv_heap_bytes() { return g_bytes.load(std::memory_order_relaxed); }

size_t lv_heap_internal_bytes() { return g_internal.load(std::memory_order_relaxed); }

}  // namespace wisp::ui
