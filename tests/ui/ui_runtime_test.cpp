#include "wisp/ui/ui_runtime.hpp"

#include <gtest/gtest.h>

#include <atomic>
#include <memory>

#include "support/fake_devices.hpp"
#include "support/wait.hpp"
#include "wisp/os/os.hpp"

using wisp::test::FakeDisplay;
using wisp::test::wait_until;

TEST(UiRuntime, RendersPostedContentWithAlignedFlushes) {
    auto disp = std::make_shared<FakeDisplay>("display.main", 480, 480, /*area_align=*/2);
    wisp::ui::UiRuntime ui(disp, nullptr);
    ASSERT_TRUE(ui.start());

    std::atomic<bool> on_ui{false};
    ui.post([&] {
        on_ui = ui.on_ui_thread();
        lv_obj_t* l = lv_label_create(lv_screen_active());
        lv_label_set_text(l, "Wisp 你好");
    });

    ASSERT_TRUE(wait_until([&] { return disp->flush_count() > 0; }, 2000));
    EXPECT_TRUE(on_ui);
    EXPECT_FALSE(ui.on_ui_thread());

    for (auto& a : disp->areas()) {
        EXPECT_EQ(a.x1 % 2, 0);
        EXPECT_EQ(a.y1 % 2, 0);
        EXPECT_EQ((a.x2 + 1) % 2, 0);
    }

    auto shot = ui.snapshot_rgb888();
    ASSERT_TRUE(shot);
    EXPECT_EQ(shot.value().size(), 480u * 480u * 3u);

    ui.stop();
}

TEST(UiRuntime, PerfCountsFramesUnderAnimation) {
    auto disp = std::make_shared<FakeDisplay>("display.main", 480, 480, 2);
    wisp::ui::UiRuntime ui(disp, nullptr);
    ASSERT_TRUE(ui.start());

    ui.post([] {
        lv_obj_t* o = lv_obj_create(lv_screen_active());
        lv_anim_t a;
        lv_anim_init(&a);
        lv_anim_set_var(&a, o);
        lv_anim_set_values(&a, 0, 300);
        lv_anim_set_duration(&a, 1000);
        lv_anim_set_repeat_count(&a, LV_ANIM_REPEAT_INFINITE);
        lv_anim_set_exec_cb(&a,
                            [](void* v, int32_t x) { lv_obj_set_x(static_cast<lv_obj_t*>(v), x); });
        lv_anim_start(&a);
    });

    wisp::os::sleep_ms(1500);
    auto p = ui.perf();
    EXPECT_GT(p.fps, 5.0f);
    EXPECT_GT(p.frames, 5u);
    EXPECT_LE(p.cpu_percent, 100);

    ui.stop();
}
