#include "wisp/ui/lv_heap.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <memory>
#include <vector>

#include "lvgl.h"
#include "support/fake_devices.hpp"
#include "support/wait.hpp"
#include "wisp/os/os.hpp"
#include "wisp/port/sim/sim_os.hpp"
#include "wisp/ui/ui_runtime.hpp"

using wisp::os::Mem;
using wisp::port::sim::set_alloc_failure;
using wisp::test::FakeDisplay;
using wisp::test::wait_until;

namespace {

// Runs `fn` on a started UiRuntime's UI thread, the only thread that may
// call LVGL, and waits for it.
template <class Fn>
void on_ui_thread(Fn fn) {
    auto disp = std::make_shared<FakeDisplay>("display.main", 480, 480, 2);
    wisp::ui::UiRuntime ui(disp, nullptr);
    ASSERT_TRUE(ui.start());
    std::atomic<bool> done{false};
    ui.post([&] {
        fn();
        done = true;
    });
    ASSERT_TRUE(wait_until([&] { return done.load(); }, 5000));
    ui.stop();
}

bool filled(const void* p, size_t n, uint8_t v) {
    const auto* b = static_cast<const uint8_t*>(p);
    for (size_t i = 0; i < n; ++i) {
        if (b[i] != v) return false;
    }
    return true;
}

}  // namespace

TEST(LvHeap, ObjectsAreCountedAndReleased) {
    auto disp = std::make_shared<FakeDisplay>("display.main", 480, 480, 2);
    wisp::ui::UiRuntime ui(disp, nullptr);
    ASSERT_TRUE(ui.start());

    std::atomic<bool> done{false};
    size_t before = 0;
    size_t with_objects = 0;
    size_t after = 0;
    ui.post([&] {
        before = wisp::ui::lv_heap_bytes();
        std::vector<lv_obj_t*> objs;
        for (int i = 0; i < 100; ++i) {
            objs.push_back(lv_obj_create(lv_screen_active()));
        }
        with_objects = wisp::ui::lv_heap_bytes();
        for (lv_obj_t* o : objs) {
            lv_obj_delete(o);
        }
        after = wisp::ui::lv_heap_bytes();
        done = true;
    });
    ASSERT_TRUE(wait_until([&] { return done.load(); }, 5000));

    EXPECT_GT(before, 0u);
    EXPECT_GT(with_objects, before);
    EXPECT_LE(after, before + 1024);

    ui.stop();
}

TEST(LvHeap, FallsBackToInternalRamWithoutPsram) {
    on_ui_thread([] {
        const size_t total = wisp::ui::lv_heap_bytes();
        EXPECT_EQ(wisp::ui::lv_heap_internal_bytes(), 0u);

        // No PSRAM: LVGL still gets memory, from internal RAM, and it is
        // counted as such.
        set_alloc_failure(Mem::Large, true);
        auto* p = static_cast<uint8_t*>(lv_malloc(1000));
        ASSERT_NE(p, nullptr);
        std::memset(p, 0x5a, 1000);
        EXPECT_EQ(wisp::ui::lv_heap_internal_bytes(), 1000u);

        // Growing it while PSRAM is still out copies it within internal RAM.
        p = static_cast<uint8_t*>(lv_realloc(p, 2000));
        ASSERT_NE(p, nullptr);
        EXPECT_TRUE(filled(p, 1000, 0x5a));
        EXPECT_EQ(wisp::ui::lv_heap_internal_bytes(), 2000u);
        std::memset(p, 0x5a, 2000);

        // Once PSRAM has room again, the next resize moves the block there.
        set_alloc_failure(Mem::Large, false);
        p = static_cast<uint8_t*>(lv_realloc(p, 3000));
        ASSERT_NE(p, nullptr);
        EXPECT_TRUE(filled(p, 2000, 0x5a));
        EXPECT_EQ(wisp::ui::lv_heap_internal_bytes(), 0u);
        EXPECT_EQ(wisp::ui::lv_heap_bytes(), total + 3000);

        // Both memories gone: the resize fails and the block survives.
        set_alloc_failure(Mem::Large, true);
        set_alloc_failure(Mem::Fast, true);
        EXPECT_EQ(lv_realloc(p, 8000), nullptr);
        set_alloc_failure(Mem::Large, false);
        set_alloc_failure(Mem::Fast, false);
        EXPECT_TRUE(filled(p, 2000, 0x5a));
        EXPECT_EQ(wisp::ui::lv_heap_bytes(), total + 3000);

        lv_free(p);
        EXPECT_EQ(wisp::ui::lv_heap_bytes(), total);
        EXPECT_EQ(wisp::ui::lv_heap_internal_bytes(), 0u);
    });
}
