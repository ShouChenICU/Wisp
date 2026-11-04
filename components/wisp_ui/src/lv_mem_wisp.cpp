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

// Every block starts with a header holding the size LVGL asked for, so
// free and realloc can keep the byte count exact. 16 bytes rather than
// sizeof(size_t) so the pointer handed to LVGL keeps the base allocation's
// alignment on every platform.
constexpr size_t kHeader = 16;
static_assert(sizeof(size_t) <= kHeader);

std::atomic<size_t> g_bytes{0};

void* block_of(void* user) { return static_cast<uint8_t*>(user) - kHeader; }

size_t size_of(void* block) {
    size_t s = 0;
    std::memcpy(&s, block, sizeof(s));
    return s;
}

void* finish(void* block, size_t size) {
    std::memcpy(block, &size, sizeof(size));
    return static_cast<uint8_t*>(block) + kHeader;
}

// PSRAM first. A board without PSRAM, or one that has run out of it, still
// gets a working UI from internal RAM rather than none at all (devices
// degrade, AGENTS.md section 3).
void* alloc_block(size_t bytes) {
    void* p = wisp::os::alloc(bytes, wisp::os::Mem::Large);
    return p != nullptr ? p : wisp::os::alloc(bytes, wisp::os::Mem::Fast);
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

void* lv_malloc_core(size_t size) {
    if (size > SIZE_MAX - kHeader) {
        return nullptr;
    }
    void* block = alloc_block(size + kHeader);
    if (block == nullptr) {
        return nullptr;
    }
    g_bytes.fetch_add(size, std::memory_order_relaxed);
    return finish(block, size);
}

void* lv_realloc_core(void* p, size_t new_size) {
    if (p == nullptr) {
        return lv_malloc_core(new_size);
    }
    if (new_size > SIZE_MAX - kHeader) {
        return nullptr;
    }
    void* block = block_of(p);
    const size_t old_size = size_of(block);
    void* moved = wisp::os::realloc(block, new_size + kHeader, wisp::os::Mem::Large);
    if (moved == nullptr) {
        // Same fallback as alloc_block(): copy into internal RAM by hand,
        // since os::realloc() can only target one kind of memory.
        moved = wisp::os::alloc(new_size + kHeader, wisp::os::Mem::Fast);
        if (moved == nullptr) {
            return nullptr;
        }
        std::memcpy(moved, block, kHeader + (old_size < new_size ? old_size : new_size));
        wisp::os::dealloc(block);
    }
    if (new_size >= old_size) {
        g_bytes.fetch_add(new_size - old_size, std::memory_order_relaxed);
    } else {
        g_bytes.fetch_sub(old_size - new_size, std::memory_order_relaxed);
    }
    return finish(moved, new_size);
}

void lv_free_core(void* p) {
    if (p == nullptr) {
        return;
    }
    void* block = block_of(p);
    g_bytes.fetch_sub(size_of(block), std::memory_order_relaxed);
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

}  // namespace wisp::ui
