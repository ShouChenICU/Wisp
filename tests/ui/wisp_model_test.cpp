// WispModel (wisp/ui/wisp_model.hpp): determinism, mood blending, level
// and busy, sparks per panel, the dirty box, the frame rate it asks for,
// the anti-burn-in drift, what each mood's face looks like, the span
// rasterizer for two-colour panels, and the wisp_perf() window.

#include "wisp/ui/wisp_model.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace {

using wisp::hal::PanelKind;
using wisp::ui::WispModel;
using wisp::ui::WispMood;
using wisp::ui::WispParams;
using wisp::ui::WispShape;

// The golden shape: seed 42, size 216, 30 steps of 33 ms, then build,
// hashed with shape_hash(). Any change to the look changes it; say why in
// the commit that updates it.
constexpr uint32_t kGolden = 0xc90710f7u;

uint32_t hash_after(WispModel& m) {
    m.build();
    return wisp::ui::shape_hash(m.shape());
}

int bounds_height(const WispModel& m) { return m.bounds().y2 - m.bounds().y1 + 1; }

bool inside_frame(const wisp::hal::Area& b, const WispModel& m) {
    return b.x1 > 0 && b.y1 > 0 && b.x2 < m.width() - 1 && b.y2 < m.height() - 1;
}

TEST(WispModel, SameSeedSameFrames) {
    WispModel a(216, 42), b(216, 42);
    for (int i = 0; i < 60; ++i) {
        a.step(33);
        b.step(33);
        ASSERT_EQ(hash_after(a), hash_after(b)) << "step " << i;
    }
}

TEST(WispModel, GoldenFrameHash) {
    WispModel m(216, 42);
    for (int i = 0; i < 30; ++i) m.step(33);
    const uint32_t h = hash_after(m);
    EXPECT_EQ(h, kGolden) << std::hex << "0x" << h;
}

TEST(WispModel, DifferentSeedsDiffer) {
    WispModel a(216, 42), b(216, 43);
    for (int i = 0; i < 30; ++i) {
        a.step(33);
        b.step(33);
    }
    EXPECT_NE(hash_after(a), hash_after(b));
}

TEST(WispModel, MoodTransitionInterpolates) {
    const WispParams idle = wisp::ui::mood_params(WispMood::Idle);
    const WispParams listening = wisp::ui::mood_params(WispMood::Listening);
    ASSERT_NE(idle.eye_open, listening.eye_open);

    WispModel m(216, 42);
    m.set_mood(WispMood::Listening);
    EXPECT_EQ(m.params().eye_open, idle.eye_open);
    EXPECT_EQ(m.params().lean, idle.lean);
    m.step(100);  // a step is at most 100 ms
    m.step(50);
    EXPECT_GT(m.params().eye_open, std::min(idle.eye_open, listening.eye_open));
    EXPECT_LT(m.params().eye_open, std::max(idle.eye_open, listening.eye_open));
    m.step(100);
    m.step(50);
    EXPECT_EQ(m.params().eye_open, listening.eye_open);
    EXPECT_EQ(m.params().flame_height, listening.flame_height);
    EXPECT_EQ(m.params().lean, listening.lean);
}

TEST(WispModel, LerpEnds) {
    const WispParams a = wisp::ui::mood_params(WispMood::Idle);
    const WispParams b = wisp::ui::mood_params(WispMood::Error);
    EXPECT_EQ(wisp::ui::lerp(a, b, 0).brightness, a.brightness);
    EXPECT_EQ(wisp::ui::lerp(a, b, 256).brightness, b.brightness);
    EXPECT_EQ(wisp::ui::lerp(a, b, 128).brightness, (a.brightness + b.brightness) / 2);
}

int max_height_with_level(WispMood mood, uint8_t level) {
    WispModel m(216, 42);
    m.set_mood(mood);
    m.set_level(level);
    int h = 0;
    for (int i = 0; i < 15; ++i) {
        m.step(33);
        m.build();
        h = std::max(h, bounds_height(m));
    }
    return h;
}

TEST(WispModel, LevelMovesTheFlameWhileListening) {
    EXPECT_GT(max_height_with_level(WispMood::Listening, 100),
              max_height_with_level(WispMood::Listening, 0));

    // Idle ignores the level: the very same frames.
    WispModel a(216, 42), b(216, 42);
    a.set_level(0);
    b.set_level(100);
    for (int i = 0; i < 20; ++i) {
        a.step(33);
        b.step(33);
        ASSERT_EQ(hash_after(a), hash_after(b)) << "step " << i;
    }
}

uint64_t travel_over(bool busy) {
    WispModel m(216, 42);
    m.set_mood(WispMood::Thinking);
    m.set_busy(busy);
    for (int i = 0; i < 30; ++i) m.step(33);  // past the blend and the busy easing
    uint64_t sum = 0;
    for (int i = 0; i < 30; ++i) {
        m.step(33);
        sum += m.particle_travel();
    }
    return sum;
}

TEST(WispModel, BusyThinkingSpeedsParticles) {
    const uint64_t calm = travel_over(false);
    const uint64_t busy = travel_over(true);
    EXPECT_GT(calm, 0u);
    EXPECT_GT(busy, calm * 3 / 2);
}

TEST(WispModel, SparksOnlyOnColourPanels) {
    const WispMood moods[] = {WispMood::Idle, WispMood::Listening, WispMood::Thinking,
                              WispMood::Speaking, WispMood::Error};
    for (PanelKind panel :
         {PanelKind::Amoled, PanelKind::Lcd, PanelKind::Mono, PanelKind::EPaper}) {
        const bool colour = wisp::hal::panel_is_colour(panel);
        WispModel m(216, 7, panel);
        for (int i = 0; i < 300; ++i) {
            if (i % 60 == 0) m.set_mood(moods[(i / 60) % 5]);
            m.set_busy(i % 120 >= 60);
            m.step(33);
            m.build();
            if (colour) {
                ASSERT_GE(m.live_particles(), 1u) << i;
                ASSERT_LE(m.live_particles(), static_cast<size_t>(WispModel::kMaxSparks)) << i;
                ASSERT_LE(m.shape().spark_n, WispModel::kMaxSparks) << i;
            } else {
                ASSERT_EQ(m.live_particles(), 0u) << i;
                ASSERT_EQ(m.shape().spark_n, 0) << i;
            }
        }
    }
}

TEST(WispModel, DirtyBoxCoversLastAndThisFrame) {
    WispModel m(216, 42);
    const wisp::hal::Area first = m.build();
    EXPECT_EQ(first.x1, 0);
    EXPECT_EQ(first.y1, 0);
    EXPECT_EQ(first.x2, m.width() - 1);
    EXPECT_EQ(first.y2, m.height() - 1);
    const WispMood moods[] = {WispMood::Listening, WispMood::Thinking, WispMood::Speaking,
                              WispMood::Error, WispMood::Idle};
    for (int i = 0; i < 100; ++i) {
        if (i % 20 == 0) m.set_mood(moods[i / 20]);
        m.set_level(static_cast<uint8_t>((i * 37) % 101));
        m.step(33);
        const wisp::hal::Area prev = m.bounds();
        const wisp::hal::Area a = m.build();
        const wisp::hal::Area cur = m.bounds();
        ASSERT_GE(a.x1, 0);
        ASSERT_GE(a.y1, 0);
        ASSERT_LT(a.x2, m.width());
        ASSERT_LT(a.y2, m.height());
        for (const wisp::hal::Area& b : {prev, cur}) {
            ASSERT_LE(a.x1, b.x1) << i;
            ASSERT_LE(a.y1, b.y1) << i;
            ASSERT_GE(a.x2, b.x2) << i;
            ASSERT_GE(a.y2, b.y2) << i;
        }
    }
}

// Every span of the shape is inside its bbox: what the bbox does not
// cover is never drawn.
TEST(WispModel, SpansStayInsideTheBbox) {
    WispModel m(216, 3);
    for (int i = 0; i < 200; ++i) {
        if (i % 40 == 0) m.set_mood(static_cast<WispMood>(i / 40));
        m.set_level(static_cast<uint8_t>((i * 23) % 101));
        m.step(33);
        m.build();
        struct Ctx {
            wisp::hal::Area box;
            int bad = 0;
            int n = 0;
            int last_y = -1, last_x2 = -1;
        } ctx{m.bounds()};
        wisp::ui::for_each_span(m.shape(), &ctx, [](void* p, int y, int x1, int x2) {
            auto* c = static_cast<Ctx*>(p);
            ++c->n;
            if (x1 > x2 || y < c->box.y1 || y > c->box.y2 || x1 < c->box.x1 || x2 > c->box.x2) {
                ++c->bad;
            }
            // Rows go down; runs in a row go right without overlapping.
            if (y < c->last_y || (y == c->last_y && x1 <= c->last_x2)) ++c->bad;
            c->last_y = y;
            c->last_x2 = x2;
        });
        ASSERT_GT(ctx.n, 0) << i;
        ASSERT_EQ(ctx.bad, 0) << i;
    }
}

// A single round dot rasterizes to a disc of about the right size.
TEST(WispModel, SpanOfADot) {
    WispShape s;
    s.sparks[0] = {{20 * 256 + 128, 30 * 256 + 128}, {20 * 256 + 128, 30 * 256 + 128}, 5 * 256};
    s.spark_n = 1;
    s.bbox = {0, 0, 63, 63};
    struct Ctx {
        int pixels = 0, rows = 0, widest = 0;
    } ctx;
    wisp::ui::for_each_span(s, &ctx, [](void* p, int, int x1, int x2) {
        auto* c = static_cast<Ctx*>(p);
        c->pixels += x2 - x1 + 1;
        c->widest = std::max(c->widest, x2 - x1 + 1);
        ++c->rows;
    });
    EXPECT_EQ(ctx.rows, 11);
    EXPECT_EQ(ctx.widest, 11);
    EXPECT_NEAR(ctx.pixels, 79, 6);  // pi * 5^2
}

TEST(WispModel, FrameIntervalFollowsActivity) {
    WispModel m(216, 42);
    EXPECT_EQ(m.frame_interval_ms(), 66u);
    m.set_mood(WispMood::Listening);
    EXPECT_EQ(m.frame_interval_ms(), 33u);
    for (int i = 0; i < 12; ++i) m.step(33);
    EXPECT_EQ(m.frame_interval_ms(), 33u);
    m.set_mood(WispMood::Thinking);
    for (int i = 0; i < 12; ++i) m.step(33);
    EXPECT_EQ(m.frame_interval_ms(), 33u);
    m.set_mood(WispMood::Speaking);
    EXPECT_EQ(m.frame_interval_ms(), 33u);  // blending
    for (int i = 0; i < 12; ++i) m.step(33);
    EXPECT_EQ(m.frame_interval_ms(), 66u);
    m.set_mood(WispMood::Error);
    for (int i = 0; i < 12; ++i) m.step(33);
    EXPECT_EQ(m.frame_interval_ms(), 33u);
    m.set_mood(WispMood::Idle);
    EXPECT_EQ(m.frame_interval_ms(), 33u);
    for (int i = 0; i < 12; ++i) m.step(33);
    EXPECT_EQ(m.frame_interval_ms(), 66u);

    WispModel mono(216, 42, PanelKind::Mono);
    EXPECT_EQ(mono.frame_interval_ms(), 100u);
    mono.set_mood(WispMood::Listening);
    EXPECT_EQ(mono.frame_interval_ms(), 100u);
    WispModel paper(216, 42, PanelKind::EPaper);
    EXPECT_EQ(paper.frame_interval_ms(), 0u);
    paper.set_mood(WispMood::Thinking);
    EXPECT_EQ(paper.frame_interval_ms(), 0u);
}

TEST(WispModel, SettleFinishesTheMood) {
    WispModel m(216, 42, PanelKind::EPaper);
    m.set_mood(WispMood::Error);
    m.settle();
    EXPECT_EQ(m.params().eye_open, wisp::ui::mood_params(WispMood::Error).eye_open);
    m.build();
    EXPECT_TRUE(m.shape().chevrons);
    // No shake frozen into the picture: the same centre as a settled Idle.
    WispModel idle(216, 42, PanelKind::EPaper);
    idle.settle();
    EXPECT_EQ(m.centre().x, idle.centre().x);
}

TEST(WispModel, IdleDriftsOverTime) {
    WispModel m(216, 42);
    int32_t min_x = INT32_MAX, max_x = INT32_MIN, min_y = INT32_MAX, max_y = INT32_MIN;
    for (int t = 0; t < 60000; t += 66) {
        m.step(66);
        const wisp::ui::WispPoint c = m.centre();
        min_x = std::min(min_x, c.x);
        max_x = std::max(max_x, c.x);
        min_y = std::min(min_y, c.y);
        max_y = std::max(max_y, c.y);
        if ((t / 66) % 10 == 0) {
            m.build();
            ASSERT_TRUE(inside_frame(m.bounds(), m)) << t;
        }
    }
    const int32_t span = std::max(max_x - min_x, max_y - min_y);
    EXPECT_GE(span, 2 * 256);
}

// The widest and tallest the wisp gets (level jumping between 0 and 100,
// busy, every mood) stays off the frame's edge: nothing is cut off.
TEST(WispModel, NothingClipsInAnyMood) {
    for (int size : {216, 134, 149}) {
        WispModel m(size, 9);
        for (int i = 0; i < 5 * 120; ++i) {
            if (i % 120 == 0) m.set_mood(static_cast<WispMood>(i / 120));
            m.set_level((i / 8) % 2 == 0 ? 100 : 0);
            m.set_busy(true);
            m.step(33);
            m.build();
            ASSERT_TRUE(inside_frame(m.bounds(), m)) << size << " step " << i;
        }
    }
}

// The outline is most of the frame's width (spec 6.2's 45% view is the
// character, not a margin around it).
TEST(WispModel, BodyFillsTheFrame) {
    WispModel m(216, 42);
    m.step(33);
    m.build();
    int32_t lo = INT32_MAX, hi = INT32_MIN;
    const WispShape& s = m.shape();
    for (int i = 0; i < s.outline_n; ++i) {
        lo = std::min(lo, s.outline[static_cast<size_t>(i)].x);
        hi = std::max(hi, s.outline[static_cast<size_t>(i)].x);
    }
    const int body = (hi - lo + s.stroke) / 256;
    EXPECT_GE(body * 100, m.width() * 55) << body;
    EXPECT_LE(body * 100, m.width() * 75) << body;
    // Stroke: about 1/40 of the height, at least 2 px.
    EXPECT_NEAR(s.stroke, m.height() * 256 / 40, 256);
    WispModel tiny(40, 1);
    tiny.build();
    EXPECT_GE(tiny.shape().stroke, 2 * 256);
}

TEST(WispModel, ErrorShakesThenSettles) {
    const int32_t idle = wisp::ui::mood_params(WispMood::Idle).brightness;
    WispModel m(216, 42);
    for (int i = 0; i < 10; ++i) m.step(33);
    m.set_mood(WispMood::Error);
    int32_t max_jump = 0;
    wisp::ui::WispPoint last = m.centre();
    for (int t = 33; t <= 400; t += 33) {
        m.step(33);
        if (t <= 300) EXPECT_LT(m.params().brightness, idle) << t;
        const wisp::ui::WispPoint c = m.centre();
        max_jump = std::max({max_jump, std::abs(c.x - last.x), std::abs(c.y - last.y)});
        last = c;
    }
    EXPECT_GE(max_jump, 256) << "no shake";
    for (int t = 0; t < 1500; t += 33) m.step(33);
    last = m.centre();
    max_jump = 0;
    for (int i = 0; i < 10; ++i) {
        m.step(33);
        const wisp::ui::WispPoint c = m.centre();
        max_jump = std::max({max_jump, std::abs(c.x - last.x), std::abs(c.y - last.y)});
        last = c;
    }
    EXPECT_LT(max_jump, 128) << "still shaking";
}

const WispShape& settled(WispModel& m, WispMood mood, uint8_t level = 0) {
    m.set_mood(mood);
    m.set_level(level);
    m.settle();
    for (int i = 0; i < 12; ++i) m.step(33);
    m.settle();
    m.build();
    return m.shape();
}

int32_t eye_height(const WispShape::Capsule& c) { return std::abs(c.b.y - c.a.y) + 2 * c.r; }

// Moods differ by shape alone (spec 6.1): bigger eyes listening, eyes up
// and to the right thinking, a mouth speaking, "> <" in error.
TEST(WispModel, MoodsDifferByShape) {
    WispModel idle_m(216, 42), listen_m(216, 42), think_m(216, 42), speak_m(216, 42),
        error_m(216, 42);
    const WispShape idle = settled(idle_m, WispMood::Idle);
    const WispShape listen = settled(listen_m, WispMood::Listening);
    const WispShape think = settled(think_m, WispMood::Thinking);
    const WispShape speak = settled(speak_m, WispMood::Speaking, 80);
    const WispShape error = settled(error_m, WispMood::Error);

    EXPECT_FALSE(idle.chevrons);
    EXPECT_FALSE(idle.mouth);
    EXPECT_GT(eye_height(listen.eyes[0]), eye_height(idle.eyes[0]) * 11 / 10);
    // Thinking: eyes clearly higher (relative to the body) and to the right.
    const int32_t idle_rel = idle.eyes[0].a.y - idle_m.centre().y;
    const int32_t think_rel = think.eyes[0].a.y - think_m.centre().y;
    EXPECT_LT(think_rel, idle_rel - eye_height(idle.eyes[0]) / 4);
    EXPECT_GT(think.eyes[0].a.x - think_m.centre().x, idle.eyes[0].a.x - idle_m.centre().x);
    EXPECT_TRUE(speak.mouth);
    EXPECT_TRUE(error.chevrons);
    EXPECT_FALSE(error.mouth);
}

TEST(WispModel, MouthOpensWithLevel) {
    WispModel shut(216, 42), open(216, 42);
    const WispShape a = settled(shut, WispMood::Speaking, 0);
    const WispShape b = settled(open, WispMood::Speaking, 100);
    ASSERT_TRUE(a.mouth);
    ASSERT_TRUE(b.mouth);
    EXPECT_GT(eye_height(b.mouth_shape), eye_height(a.mouth_shape) * 2);
    // Shut, it is a line: as tall as the stroke.
    EXPECT_LE(eye_height(a.mouth_shape), a.stroke + 256);
}

TEST(WispPerf, RollingFiveSecondWindow) {
    wisp::ui::wisp_perf_reset();
    EXPECT_EQ(wisp::ui::wisp_perf(1000).fps, 0);
    for (int k = 1; k <= 75; ++k) {
        wisp::ui::wisp_perf_record(static_cast<uint64_t>(k) * 5000 / 75 + 100000, 4000);
    }
    const wisp::ui::WispPerf p = wisp::ui::wisp_perf(105000);
    EXPECT_NEAR(p.fps, 15.0, 0.5);
    EXPECT_NEAR(p.ms, 4.0, 0.01);
    const wisp::ui::WispPerf idle = wisp::ui::wisp_perf(110100);
    EXPECT_EQ(idle.fps, 0);
    EXPECT_EQ(idle.ms, 0);
}

}  // namespace
