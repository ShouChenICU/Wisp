// Unit tests for wisp::hal::draw_buf_policy(): how the Display caps
// "draw_buf_lines", "draw_buf_mem" and "render_mode" become UiRuntime's
// draw buffers, and that a display declaring none of them gets exactly
// the buffers UiRuntime always used.

#include "wisp/hal/draw_buf_policy.hpp"

#include <gtest/gtest.h>

#include "wisp/base/value.hpp"

using wisp::Properties;
using wisp::Value;
using wisp::hal::draw_buf_policy;
using wisp::hal::DrawBufMem;
using wisp::hal::DrawBufPolicy;

namespace {

Properties base_caps() { return Properties{{"width", 480}, {"height", 480}}; }

}  // namespace

TEST(DrawBufPolicy, DefaultsWithoutAnyDeclaration) {
    DrawBufPolicy p = draw_buf_policy(base_caps());
    EXPECT_EQ(p.lines, 24u);
    EXPECT_EQ(p.lines, wisp::hal::kDefaultDrawBufLines);
    EXPECT_EQ(p.mem, DrawBufMem::Dma);
    EXPECT_TRUE(p.warnings.empty());
}

TEST(DrawBufPolicy, ExplicitDefaultsAreAcceptedSilently) {
    Properties caps = base_caps();
    caps["draw_buf_lines"] = 24;
    caps["draw_buf_mem"] = Value("dma");
    caps["render_mode"] = Value("partial");
    DrawBufPolicy p = draw_buf_policy(caps);
    EXPECT_EQ(p.lines, 24u);
    EXPECT_EQ(p.mem, DrawBufMem::Dma);
    EXPECT_TRUE(p.warnings.empty());
}

TEST(DrawBufPolicy, HonoursDeclaredLines) {
    Properties caps = base_caps();
    caps["draw_buf_lines"] = 40;
    DrawBufPolicy p = draw_buf_policy(caps);
    EXPECT_EQ(p.lines, 40u);
    EXPECT_TRUE(p.warnings.empty());
}

TEST(DrawBufPolicy, ClampsLinesToTheDisplayHeight) {
    Properties caps = base_caps();
    caps["draw_buf_lines"] = 1000;
    DrawBufPolicy p = draw_buf_policy(caps);
    EXPECT_EQ(p.lines, 480u);
    EXPECT_EQ(p.warnings.size(), 1u);
}

TEST(DrawBufPolicy, RejectsUnusableLines) {
    for (Value bad : {Value(0), Value(-3), Value("many"), Value(2.5)}) {
        Properties caps = base_caps();
        caps["draw_buf_lines"] = bad;
        DrawBufPolicy p = draw_buf_policy(caps);
        EXPECT_EQ(p.lines, 24u);
        EXPECT_EQ(p.warnings.size(), 1u);
    }
}

// A short display never gets a buffer taller than itself, even by default.
TEST(DrawBufPolicy, DefaultLinesFitAShortDisplay) {
    DrawBufPolicy p = draw_buf_policy(Properties{{"width", 160}, {"height", 16}});
    EXPECT_EQ(p.lines, 16u);
    EXPECT_TRUE(p.warnings.empty());
}

TEST(DrawBufPolicy, LargeMeansDmaCapablePsram) {
    Properties caps = base_caps();
    caps["draw_buf_mem"] = Value("large");
    DrawBufPolicy p = draw_buf_policy(caps);
    EXPECT_EQ(p.mem, DrawBufMem::Large);
    EXPECT_TRUE(p.warnings.empty());
}

TEST(DrawBufPolicy, UnknownMemFallsBackToDma) {
    for (Value bad : {Value("psram"), Value(1), Value(true)}) {
        Properties caps = base_caps();
        caps["draw_buf_mem"] = bad;
        DrawBufPolicy p = draw_buf_policy(caps);
        EXPECT_EQ(p.mem, DrawBufMem::Dma);
        EXPECT_EQ(p.warnings.size(), 1u);
    }
}

// "full" and "direct" are reserved names; only partial rendering exists
// yet, so they degrade to it with a warning, as does anything else.
TEST(DrawBufPolicy, OnlyPartialRenderModeIsImplemented) {
    for (const char* mode : {"full", "direct", "tiled"}) {
        Properties caps = base_caps();
        caps["render_mode"] = Value(mode);
        DrawBufPolicy p = draw_buf_policy(caps);
        EXPECT_EQ(p.lines, 24u) << mode;
        EXPECT_EQ(p.mem, DrawBufMem::Dma) << mode;
        EXPECT_EQ(p.warnings.size(), 1u) << mode;
    }
}

TEST(DrawBufPolicy, WarningsAccumulate) {
    Properties caps = base_caps();
    caps["draw_buf_lines"] = 0;
    caps["draw_buf_mem"] = Value("sram");
    caps["render_mode"] = Value("full");
    EXPECT_EQ(draw_buf_policy(caps).warnings.size(), 3u);
}
