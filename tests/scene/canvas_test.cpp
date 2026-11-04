// ui.canvas: draw(g) runs on the scene task, its commands travel as a
// CanvasDraw and are drawn into a buffer in the display's colour format
// (scene-runtime-core spec 4.3).

#include <gtest/gtest.h>

#include <cstring>
#include <span>
#include <string>
#include <variant>
#include <vector>

#include "support/scene_harness.hpp"
#include "widget_data.hpp"

namespace {

using wisp::scene::CanvasDraw;
using wisp::scene::draw_command_count;
using wisp::scene::draw_text;
using wisp::scene::DrawCommand;
using wisp::scene::DrawOp;
using wisp::scene::read_draw_command;
using wisp::test::describe;

class Canvas : public wisp::test::SceneHarness {
protected:
    static constexpr wisp::scene::NodeId kCanvas = 2;  // screen 1, canvas 2

    // Every CanvasDraw applied so far.
    std::vector<std::vector<uint8_t>> draws() {
        std::vector<std::vector<uint8_t>> out;
        for (const auto& op : applied)
            if (const auto* d = std::get_if<CanvasDraw>(&op))
                out.emplace_back(d->commands.begin(), d->commands.end());
        return out;
    }

    static std::vector<DrawCommand> commands(const std::vector<uint8_t>& bytes) {
        std::vector<DrawCommand> out;
        std::span<const uint8_t> s(bytes);
        for (size_t i = 0; i < draw_command_count(s); ++i) {
            DrawCommand c;
            EXPECT_TRUE(read_draw_command(s, i, c));
            out.push_back(c);
        }
        return out;
    }

    // The canvas pixel at (x, y) as stored, 16 bits.
    uint16_t pixel(int32_t x, int32_t y) {
        uint16_t px = 0;
        on_ui([&] {
            lv_draw_buf_t* buf = lv_canvas_get_draw_buf(view->object(kCanvas));
            ASSERT_NE(buf, nullptr);
            std::memcpy(&px, buf->data + y * buf->header.stride + x * 2, 2);
        });
        return px;
    }
};

TEST_F(Canvas, CommandsRoundTripAndLimit512) {
    start("rect");
    auto err = open(R"(
        return ui.screen {
          ui.canvas { w = 100, h = 60, draw = function(g)
            g:fill("bg")
            g:line(1, 2, 30, 40, { color = "#102030", width = 3 })
            g:circle(50, 30, 20, { fill = true, color = "accent" })
            g:arc(50, 30, 25, 270, 90, { width = 4 })
            g:rect(5, 6, 70, 40, { radius = 8, color = "ok" })
            g:text(10, 12, "你好", { font = "lg", color = "warn" })
            for i = 1, 506 do g:line(0, 0, i, i) end
          end },
        }
    )");
    ASSERT_FALSE(err) << describe(err);
    auto all = draws();
    ASSERT_EQ(all.size(), 1u);
    auto cmds = commands(all[0]);
    ASSERT_EQ(cmds.size(), 512u);
    EXPECT_EQ(cmds[0].op, DrawOp::Fill);
    EXPECT_EQ(cmds[0].rgb, 0x000000u);
    EXPECT_EQ(cmds[1].op, DrawOp::Line);
    EXPECT_EQ(cmds[1].rgb, 0x102030u);
    EXPECT_EQ(cmds[1].width, 3);
    EXPECT_EQ((std::vector<int32_t>(cmds[1].v, cmds[1].v + 4)),
              (std::vector<int32_t>{1, 2, 30, 40}));
    EXPECT_EQ(cmds[2].op, DrawOp::Circle);
    EXPECT_EQ(cmds[2].fill, 1);
    EXPECT_EQ(cmds[2].rgb, 0x4F9DFFu);
    EXPECT_EQ(cmds[2].v[2], 20);
    EXPECT_EQ(cmds[3].op, DrawOp::Arc);
    EXPECT_EQ(cmds[3].v[3], 270);
    EXPECT_EQ(cmds[3].v[4], 90);
    EXPECT_EQ(cmds[3].width, 4);
    EXPECT_EQ(cmds[3].rgb, 0xFFFFFFu);  // the text colour by default
    EXPECT_EQ(cmds[4].op, DrawOp::Rect);
    EXPECT_EQ(cmds[4].radius, 8);
    EXPECT_EQ(cmds[4].fill, 0);
    EXPECT_EQ(cmds[5].op, DrawOp::Text);
    EXPECT_EQ(cmds[5].font, 3);  // lg
    EXPECT_EQ(draw_text(std::span<const uint8_t>(all[0]), cmds[5]), "你好");
    EXPECT_EQ(cmds[511].v[2], 506);

    // The view drew them: the filled circle's centre has the accent colour.
    EXPECT_EQ(pixel(50, 30), lv_color_to_u16(lv_color_hex(0x4F9DFF)));

    vm.reset();
    batch.clear();
    err = open(R"(
        return ui.screen {
          ui.canvas { draw = function(g)
            for i = 1, 513 do
              g:line(0, 0, i, i)
            end
          end },
        }
    )");
    ASSERT_TRUE(err);
    EXPECT_EQ(err->text(), "main.lua:5: canvas: more than 512 draw commands");

    // Bad arguments are the scene's errors, at its line.
    vm.reset();
    batch.clear();
    err = open(R"(
        return ui.screen {
          ui.canvas { draw = function(g)
            g:line(0, 0, "x", 1)
          end },
        }
    )");
    ASSERT_TRUE(err);
    EXPECT_EQ(err->text(), "main.lua:4: canvas: g:line expects numbers (argument 3 is string)");

    vm.reset();
    batch.clear();
    err = open(R"(
        return ui.screen {
          ui.canvas { draw = function(g)
            g:rect(0, 0, 4, 4, { color = "pink" })
          end },
        }
    )");
    ASSERT_TRUE(err);
    EXPECT_EQ(err->text(),
              "main.lua:4: canvas: color expects \"#RRGGBB\" or one of accent, text, muted, "
              "surface, bg, danger, ok, warn, got 'pink'");

    // g only draws while draw(g) runs.
    vm.reset();
    batch.clear();
    err = open(R"(
        local kept
        _G.later = state(0)
        return ui.screen {
          ui.canvas { draw = function(g)
            if _G.later:get() == 0 then kept = g else kept:fill("bg") end
          end },
        }
    )");
    ASSERT_FALSE(err) << describe(err);
    err = message("_G.later:set(1)");
    ASSERT_TRUE(err);
    EXPECT_EQ(err->text(), "main.lua:6: canvas: g is only valid while draw(g) runs");
}

TEST_F(Canvas, RedrawsOnDependencyChange) {
    start("rect");
    auto err = open(R"(
        _G.r = state(10)
        _G.other = state(0)
        return ui.screen {
          ui.canvas { w = 100, h = 100, draw = function(g)
            g:fill("#FF0000")
            g:circle(50, 50, _G.r:get(), { fill = true, color = "#0000FF" })
          end },
        }
    )");
    ASSERT_FALSE(err) << describe(err);
    ASSERT_EQ(draws().size(), 1u);
    EXPECT_EQ(commands(draws()[0])[1].v[2], 10);
    const uint16_t red = lv_color_to_u16(lv_color_hex(0xFF0000));
    const uint16_t blue = lv_color_to_u16(lv_color_hex(0x0000FF));
    EXPECT_EQ(pixel(50, 50), blue);
    EXPECT_EQ(pixel(50, 35), red);

    err = message("_G.other:set(1)");
    ASSERT_FALSE(err) << describe(err);
    EXPECT_EQ(draws().size(), 1u);

    err = message("_G.r:set(20)");
    ASSERT_FALSE(err) << describe(err);
    ASSERT_EQ(draws().size(), 2u);
    EXPECT_EQ(commands(draws()[1])[1].v[2], 20);
    EXPECT_EQ(pixel(50, 35), blue);

    // A refresh sends the drawing again.
    err = message("local W = ... W.refresh_all()");
    ASSERT_FALSE(err) << describe(err);
    EXPECT_EQ(draws().size(), 3u);
}

TEST_F(Canvas, BufferUsesDisplayColorFormat) {
    for (bool swapped : {false, true}) {
        SCOPED_TRACE(swapped ? "RGB565_SWAPPED" : "RGB565");
        start("rect", swapped);
        auto err = open(R"(
            return ui.screen {
              ui.canvas { w = 101, h = 60, draw = function(g) g:fill("#FF0000") end },
            }
        )");
        ASSERT_FALSE(err) << describe(err);
        const lv_color_format_t want =
            swapped ? LV_COLOR_FORMAT_RGB565_SWAPPED : LV_COLOR_FORMAT_RGB565;
        on_ui([&] {
            ASSERT_EQ(lv_display_get_color_format(nullptr), want);
            lv_draw_buf_t* buf = lv_canvas_get_draw_buf(view->object(kCanvas));
            ASSERT_NE(buf, nullptr);
            EXPECT_EQ(buf->header.cf, want);
            EXPECT_EQ(buf->header.w, 101);
            EXPECT_EQ(buf->header.h, 60);
            EXPECT_EQ(buf->data_size, lv_draw_buf_width_to_stride(101, want) * 60);
        });
        uint16_t red = lv_color_to_u16(lv_color_hex(0xFF0000));
        if (swapped) red = static_cast<uint16_t>((red >> 8) | (red << 8));
        EXPECT_EQ(pixel(3, 3), red);
        TearDown();
        ui.reset();
        vm.reset();
        batch.clear();
        applied.clear();
        next_id = 1;
    }
}

// A canvas that changes size is drawn again into a buffer of the new size.
TEST_F(Canvas, ResizeRedrawsIntoANewBuffer) {
    start("rect");
    auto err = open(R"(
        _G.w = state(40)
        return ui.screen {
          ui.canvas { w = _G.w, h = 30, draw = function(g) g:fill("#00FF00") end },
        }
    )");
    ASSERT_FALSE(err) << describe(err);
    err = message("_G.w:set(80)");
    ASSERT_FALSE(err) << describe(err);
    on_ui([&] {
        lv_obj_update_layout(view->screen());
        lv_draw_buf_t* buf = lv_canvas_get_draw_buf(view->object(kCanvas));
        ASSERT_NE(buf, nullptr);
        EXPECT_EQ(buf->header.w, 80);
    });
    EXPECT_EQ(pixel(70, 20), lv_color_to_u16(lv_color_hex(0x00FF00)));
}

}  // namespace
