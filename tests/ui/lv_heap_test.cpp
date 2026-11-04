#include "wisp/ui/lv_heap.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <cstddef>
#include <memory>
#include <vector>

#include "lvgl.h"
#include "support/fake_devices.hpp"
#include "support/wait.hpp"
#include "wisp/ui/ui_runtime.hpp"

using wisp::test::FakeDisplay;
using wisp::test::wait_until;

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
