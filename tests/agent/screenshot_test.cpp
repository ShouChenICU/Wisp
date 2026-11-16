// ui.screenshot (spec 3.6): what the panel shows, the invisible parts black,
// cropped to the visible pixels' bounding box, scaled to 240 px wide (never
// up), as a JPEG for a vision model.

#include "screenshot.hpp"

#include <gtest/gtest.h>

#include <cstdint>
#include <memory>
#include <random>
#include <string>
#include <vector>

#include "lvgl.h"
#include "support/fake_devices.hpp"
#include "support/jpeg_decode.hpp"
#include "wisp/agent/io_runner.hpp"
#include "wisp/agent/limits.hpp"
#include "wisp/agent/tool_registry.hpp"
#include "wisp/agent/tools.hpp"
#include "wisp/base/image.hpp"
#include "wisp/base/json.hpp"
#include "wisp/hal/display_geometry.hpp"
#include "wisp/ui/ui_runtime.hpp"

using wisp::Value;
using wisp::agent::make_screenshot;
using wisp::agent::screenshot_area;
using wisp::agent::ScreenshotOptions;
using wisp::agent::ToolCtx;
using wisp::agent::ToolImage;
using wisp::hal::ScreenShape;
using wisp::hal::ScreenShapeKind;
using wisp::test::decode_jpeg;
using wisp::test::DecodedJpeg;

namespace {

ScreenShape rect(int32_t w, int32_t h) { return {ScreenShapeKind::Rect, w, h, 0}; }
ScreenShape rounded(int32_t side, int32_t r) {
    return {ScreenShapeKind::RoundedRect, side, side, r};
}
ScreenShape circle(int32_t side) { return {ScreenShapeKind::Circle, side, side, 0}; }

std::vector<uint8_t> frame(const ScreenShape& s, uint8_t value) {
    return std::vector<uint8_t>(static_cast<size_t>(s.width) * s.height * 3, value);
}

std::shared_ptr<ToolImage> shot(const ScreenShape& s, std::vector<uint8_t> rgb,
                                const ScreenshotOptions& opts = {}) {
    auto r = make_screenshot(std::move(rgb), s, opts);
    EXPECT_TRUE(r) << (r ? "" : r.error().message);
    return r ? r.value() : nullptr;
}

DecodedJpeg decoded(const ToolImage& img) {
    auto d = decode_jpeg({img.bytes.data(), img.bytes.size()});
    EXPECT_TRUE(d.has_value());
    return d ? *d : DecodedJpeg{};
}

bool black(const DecodedJpeg& d, uint32_t x, uint32_t y) {
    const uint8_t* p = d.at(x, y);
    return p[0] < 16 && p[1] < 16 && p[2] < 16;
}

bool white(const DecodedJpeg& d, uint32_t x, uint32_t y) {
    const uint8_t* p = d.at(x, y);
    return p[0] > 239 && p[1] > 239 && p[2] > 239;
}

}  // namespace

TEST(Screenshot, AreaIsTheVisibleBoundingBox) {
    for (const ScreenShape& s : {rect(320, 240), rounded(480, 40), circle(466)}) {
        const wisp::hal::Area a = screenshot_area(s);
        EXPECT_EQ(a.x1, 0);
        EXPECT_EQ(a.y1, 0);
        EXPECT_EQ(a.x2, s.width - 1);
        EXPECT_EQ(a.y2, s.height - 1);
    }
}

TEST(Screenshot, SizePerShape) {
    struct Case {
        ScreenShape shape;
        uint32_t w, h;
    };
    const Case cases[] = {
        {rect(320, 240), 240, 180},
        {rounded(480, 40), 240, 240},
        {circle(466), 240, 240},
        {rect(200, 100), 200, 100},  // never scaled up
    };
    for (const Case& c : cases) {
        auto img = shot(c.shape, frame(c.shape, 255));
        ASSERT_NE(img, nullptr);
        EXPECT_STREQ(img->mime, "image/jpeg");
        EXPECT_EQ(img->width, c.w);
        EXPECT_EQ(img->height, c.h);
        const auto info = wisp::sniff_image({img->bytes.data(), img->bytes.size()});
        ASSERT_TRUE(info.has_value());
        EXPECT_STREQ(info->mime, "image/jpeg");
        EXPECT_EQ(info->width, c.w);
        EXPECT_EQ(info->height, c.h);
        const size_t n = img->bytes.size();
        ASSERT_GT(n, 4u);
        EXPECT_EQ(img->bytes.data()[0], 0xFF);
        EXPECT_EQ(img->bytes.data()[1], 0xD8);
        EXPECT_EQ(img->bytes.data()[n - 2], 0xFF);
        EXPECT_EQ(img->bytes.data()[n - 1], 0xD9);
    }
}

TEST(Screenshot, CircleCornersAreBlack) {
    auto img = shot(circle(466), frame(circle(466), 255));
    ASSERT_NE(img, nullptr);
    const DecodedJpeg d = decoded(*img);
    ASSERT_EQ(d.width, 240u);
    EXPECT_TRUE(black(d, 2, 2));
    EXPECT_TRUE(black(d, 237, 2));
    EXPECT_TRUE(white(d, 120, 120));
}

TEST(Screenshot, RoundedCornersAreBlackAndEdgesKept) {
    auto img = shot(rounded(480, 40), frame(rounded(480, 40), 255));
    ASSERT_NE(img, nullptr);
    const DecodedJpeg d = decoded(*img);
    ASSERT_EQ(d.width, 240u);
    EXPECT_TRUE(black(d, 0, 0));
    // The visible top edge is kept; a crop to safe_rect() would cut it.
    EXPECT_TRUE(white(d, 120, 0));
}

TEST(Screenshot, ChannelsKeepTheirOrder) {
    const ScreenShape s = rect(64, 64);
    std::vector<uint8_t> rgb = frame(s, 0);
    for (int32_t y = 0; y < 64; ++y) {
        for (int32_t x = 0; x < 64; ++x) {
            uint8_t* p = &rgb[(static_cast<size_t>(y) * 64 + x) * 3];
            if (x < 32) {
                p[0] = 255;
            } else {
                p[2] = 255;
            }
        }
    }
    auto img = shot(s, std::move(rgb));
    ASSERT_NE(img, nullptr);
    const DecodedJpeg d = decoded(*img);
    ASSERT_EQ(d.width, 64u);
    EXPECT_GT(d.at(8, 32)[0], 200);
    EXPECT_LT(d.at(8, 32)[2], 60);
    EXPECT_GT(d.at(56, 32)[2], 200);
    EXPECT_LT(d.at(56, 32)[0], 60);
}

TEST(Screenshot, TooLargeIsQuotaExceeded) {
    const ScreenShape s = rect(240, 240);
    std::vector<uint8_t> rgb = frame(s, 0);
    std::mt19937 gen(7);
    for (uint8_t& b : rgb) b = static_cast<uint8_t>(gen());
    ScreenshotOptions opts;
    opts.max_bytes = 512;
    auto r = make_screenshot(std::move(rgb), s, opts);
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, wisp::ErrorCode::QuotaExceeded);
}

TEST(Screenshot, WrongSnapshotSizeIsRefused) {
    const ScreenShape s = rect(64, 64);
    std::vector<uint8_t> rgb(static_cast<size_t>(64) * 63 * 3, 255);
    auto r = make_screenshot(std::move(rgb), s);
    ASSERT_FALSE(r);
    EXPECT_EQ(r.error().code, wisp::ErrorCode::InvalidArgument);
}

namespace {

// A real UiRuntime on a round 466 px panel whose screen is white, with the
// tool registered on an IoRunner.
struct ToolHarness {
    std::shared_ptr<wisp::test::FakeDisplay> disp =
        std::make_shared<wisp::test::FakeDisplay>("display.main", 466, 466, 1, "circle");
    wisp::ui::UiRuntime ui{disp, nullptr};
    wisp::agent::IoRunner io;
    std::unique_ptr<wisp::agent::ToolRegistry> reg;

    ToolHarness() {
        wisp::agent::ToolExecutors ex;
        ex.io = [this](std::function<void()> fn) { return io.post(std::move(fn)); };
        reg = std::make_unique<wisp::agent::ToolRegistry>(std::move(ex));
        EXPECT_TRUE(reg->add(wisp::agent::ToolDef{
            .name = "ui.tree",
            .description = "a tool every model gets",
            .run = [](const Value&, ToolCtx&) { return wisp::agent::ToolOutput{}; },
        }));
        EXPECT_TRUE(wisp::agent::add_screenshot_tool(*reg, {.ui = ui, .shape = circle(466)}));
        EXPECT_TRUE(ui.start());
        ui.run_blocking([] {
            lv_obj_t* scr = lv_screen_active();
            lv_obj_set_style_bg_color(scr, lv_color_white(), 0);
            lv_obj_set_style_bg_opa(scr, LV_OPA_COVER, 0);
        });
    }
    ~ToolHarness() {
        io.join_all();
        ui.stop();
    }

    wisp::agent::ToolOutput call() {
        ToolCtx c;
        c.cancelled = [] { return false; };
        c.vision = true;
        return reg->call("ui_screenshot", "", c);
    }
};

std::string error_of(const wisp::agent::ToolOutput& out) {
    const Value* e = out.result.find("error");
    return e != nullptr ? e->as_string() : std::string("(none)");
}

}  // namespace

TEST(ScreenshotTool, ShowsTheScreen) {
    ToolHarness h;
    const wisp::agent::ToolOutput out = h.call();
    ASSERT_TRUE(out.ok) << error_of(out);
    ASSERT_NE(out.image, nullptr);
    auto parsed = wisp::from_json(std::string_view(out.result_json.data(), out.result_json.size()));
    ASSERT_TRUE(parsed);
    EXPECT_TRUE(parsed.value().find("ok")->as_bool());
    const Value* image = parsed.value().find("image");
    ASSERT_NE(image, nullptr);
    EXPECT_EQ(image->find("mime")->as_string(), "image/jpeg");
    EXPECT_EQ(image->find("bytes")->as_int(), static_cast<int64_t>(out.image->bytes.size()));
    EXPECT_EQ(image->find("width")->as_int(), 240);
    EXPECT_EQ(image->find("height")->as_int(), 240);
    const DecodedJpeg d = decoded(*out.image);
    ASSERT_EQ(d.width, 240u);
    EXPECT_TRUE(white(d, 120, 120));
    EXPECT_TRUE(black(d, 2, 2));
}

TEST(ScreenshotTool, IsVisionOnlyIoAndLast) {
    ToolHarness h;
    const wisp::agent::ToolDef* def = h.reg->find("ui.screenshot");
    ASSERT_NE(def, nullptr);
    EXPECT_TRUE(def->vision_only);
    EXPECT_EQ(def->thread, wisp::agent::ToolThread::Io);
    EXPECT_EQ(def->timeout_ms, 10000u);
    EXPECT_EQ(def->risk, wisp::agent::ToolRisk::Normal);
    const std::string without(h.reg->tools_json(false).data(), h.reg->tools_json(false).size());
    EXPECT_EQ(without.find("ui_screenshot"), std::string::npos);
    auto parsed = wisp::from_json(
        std::string_view(h.reg->tools_json(true).data(), h.reg->tools_json(true).size()));
    ASSERT_TRUE(parsed);
    const auto& list = parsed.value().as_list();
    ASSERT_FALSE(list.empty());
    EXPECT_EQ(list.back().find("function")->find("name")->as_string(), "ui_screenshot");
}

TEST(ScreenshotTool, StoppedUiIsAnError) {
    ToolHarness h;
    h.ui.stop();
    const wisp::agent::ToolOutput out = h.call();
    EXPECT_FALSE(out.ok);
    const std::string e = error_of(out);
    EXPECT_TRUE(e == "busy" || e == "timeout" || e == "snapshot_failed") << e;
    EXPECT_EQ(out.image, nullptr);
}
