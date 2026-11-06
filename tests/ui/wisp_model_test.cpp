// WispModel (wisp/ui/wisp_model.hpp): determinism, mood blending, level
// and busy, the particle pool, the dirty box, the frame rate it asks for,
// the anti-burn-in drift and the Error shake; and the wisp_perf() window.

#include "wisp/ui/wisp_model.hpp"

#include <gtest/gtest.h>

#include <algorithm>
#include <cstdint>
#include <cstdlib>
#include <vector>

namespace {

using wisp::ui::WispModel;
using wisp::ui::WispMood;
using wisp::ui::WispParams;

// The golden frame: seed 42, size 216, 30 steps of 33 ms, then render.
// Any change to the look changes it; say why in the commit that updates it.
constexpr uint32_t kGolden = 0x7245bc14u;

struct Frame {
    std::vector<uint8_t> lum;
    std::vector<uint16_t> out;
    explicit Frame(const WispModel& m)
        : lum(static_cast<size_t>(m.width()) * m.height()),
          out(static_cast<size_t>(m.width()) * m.height()) {}
    wisp::hal::Area render(WispModel& m) { return m.render(lum, out); }
    uint32_t hash() const { return wisp::ui::frame_hash(out); }
};

int bounds_height(const WispModel& m) { return m.bounds().y2 - m.bounds().y1 + 1; }

TEST(WispModel, SameSeedSameFrames) {
    WispModel a(216, 42), b(216, 42);
    Frame fa(a), fb(b);
    for (int i = 0; i < 60; ++i) {
        a.step(33);
        b.step(33);
        fa.render(a);
        fb.render(b);
        ASSERT_EQ(fa.hash(), fb.hash()) << "step " << i;
    }
}

TEST(WispModel, GoldenFrameHash) {
    WispModel m(216, 42);
    Frame f(m);
    for (int i = 0; i < 30; ++i) m.step(33);
    f.render(m);
    EXPECT_EQ(f.hash(), kGolden) << std::hex << "0x" << f.hash();
}

TEST(WispModel, DifferentSeedsDiffer) {
    WispModel a(216, 42), b(216, 43);
    Frame fa(a), fb(b);
    for (int i = 0; i < 30; ++i) {
        a.step(33);
        b.step(33);
    }
    fa.render(a);
    fb.render(b);
    EXPECT_NE(fa.hash(), fb.hash());
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

// Heights of the lit bounds over a few steps after setting the level.
int max_height_with_level(WispMood mood, uint8_t level) {
    WispModel m(216, 42);
    Frame f(m);
    m.set_mood(mood);
    m.set_level(level);
    int h = 0;
    for (int i = 0; i < 15; ++i) {
        m.step(33);
        f.render(m);
        h = std::max(h, bounds_height(m));
    }
    return h;
}

TEST(WispModel, LevelMovesTheFlameWhileListening) {
    EXPECT_GT(max_height_with_level(WispMood::Listening, 100),
              max_height_with_level(WispMood::Listening, 0));

    // Idle ignores the level: the very same frames.
    WispModel a(216, 42), b(216, 42);
    Frame fa(a), fb(b);
    a.set_level(0);
    b.set_level(100);
    for (int i = 0; i < 20; ++i) {
        a.step(33);
        b.step(33);
        fa.render(a);
        fb.render(b);
        ASSERT_EQ(fa.hash(), fb.hash()) << "step " << i;
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

TEST(WispModel, ParticleCountWithinPool) {
    WispModel m(216, 7);
    const WispMood moods[] = {WispMood::Idle, WispMood::Listening, WispMood::Thinking,
                              WispMood::Speaking, WispMood::Error};
    EXPECT_GE(m.live_particles(), static_cast<size_t>(WispModel::kMinParticles));
    for (int i = 0; i < 600; ++i) {
        if (i % 60 == 0) m.set_mood(moods[(i / 60) % 5]);
        m.set_busy(i % 120 >= 60);
        m.step(33);
        ASSERT_GE(m.live_particles(), static_cast<size_t>(WispModel::kMinParticles)) << i;
        ASSERT_LE(m.live_particles(), static_cast<size_t>(WispModel::kMaxParticles)) << i;
    }
}

TEST(WispModel, DirtyBoxInsideBounds) {
    WispModel m(216, 42);
    Frame f(m);
    const wisp::hal::Area first = f.render(m);
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
        const std::vector<uint16_t> before = f.out;
        const wisp::hal::Area a = f.render(m);
        ASSERT_GE(a.x1, 0);
        ASSERT_GE(a.y1, 0);
        ASSERT_LT(a.x2, m.width());
        ASSERT_LT(a.y2, m.height());
        ASSERT_LE(a.x1, a.x2);
        ASSERT_LE(a.y1, a.y2);
        for (int y = 0; y < m.height(); ++y) {
            for (int x = 0; x < m.width(); ++x) {
                if (x >= a.x1 && x <= a.x2 && y >= a.y1 && y <= a.y2) continue;
                const size_t k = static_cast<size_t>(y) * m.width() + x;
                ASSERT_EQ(f.out[k], before[k]) << "step " << i << " at " << x << "," << y;
                ASSERT_EQ(f.out[k], 0) << "step " << i << " at " << x << "," << y;
            }
        }
    }
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
}

TEST(WispModel, IdleDriftsOverTime) {
    WispModel m(216, 42);
    Frame f(m);
    int32_t min_x = INT32_MAX, max_x = INT32_MIN, min_y = INT32_MAX, max_y = INT32_MIN;
    for (int t = 0; t < 60000; t += 66) {
        m.step(66);
        const wisp::ui::WispPoint c = m.centre();
        min_x = std::min(min_x, c.x);
        max_x = std::max(max_x, c.x);
        min_y = std::min(min_y, c.y);
        max_y = std::max(max_y, c.y);
        if ((t / 66) % 10 == 0) {
            f.render(m);
            // Nothing lit reaches the frame's edge, so nothing is cut off.
            const wisp::hal::Area b = m.bounds();
            ASSERT_GT(b.x1, 0) << t;
            ASSERT_GT(b.y1, 0) << t;
            ASSERT_LT(b.x2, m.width() - 1) << t;
            ASSERT_LT(b.y2, m.height() - 1) << t;
        }
    }
    const int32_t span = std::max(max_x - min_x, max_y - min_y);
    EXPECT_GE(span, 2 * 256);
}

// The widest and tallest the wisp gets (level jumping between 0 and 100,
// busy, every mood) still leaves the frame's edge dark: nothing is cut off.
TEST(WispModel, NothingClipsInAnyMood) {
    for (int size : {216, 134, 149}) {
        WispModel m(size, 9);
        Frame f(m);
        for (int i = 0; i < 5 * 120; ++i) {
            if (i % 120 == 0) m.set_mood(static_cast<WispMood>(i / 120));
            m.set_level((i / 8) % 2 == 0 ? 100 : 0);
            m.set_busy(true);
            m.step(33);
            f.render(m);
            const wisp::hal::Area b = m.bounds();
            ASSERT_GT(b.x1, 0) << size << " step " << i;
            ASSERT_GT(b.y1, 0) << size << " step " << i;
            ASSERT_LT(b.x2, m.width() - 1) << size << " step " << i;
            ASSERT_LT(b.y2, m.height() - 1) << size << " step " << i;
        }
    }
}

// The body is most of the frame's width (spec 6.2's 45% view is the
// character, not a margin around it).
TEST(WispModel, BodyFillsTheFrame) {
    WispModel m(216, 42);
    Frame f(m);
    m.step(33);
    f.render(m);
    // The body's own width: the row through its centre, above the halo.
    const int y = m.centre().y >> 8;
    int first = -1, last = -1;
    for (int x = 0; x < m.width(); ++x) {
        if (f.lum[static_cast<size_t>(y) * m.width() + x] >= 100) {
            if (first < 0) first = x;
            last = x;
        }
    }
    const int body = last - first + 1;
    EXPECT_GE(body * 100, m.width() * 55) << body;
    EXPECT_LE(body * 100, m.width() * 72) << body;
}

TEST(WispModel, ErrorDimsThenSettles) {
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

TEST(WispModel, MoodsLookDifferent) {
    std::vector<uint32_t> hashes;
    for (WispMood mood : {WispMood::Idle, WispMood::Listening, WispMood::Thinking,
                          WispMood::Speaking, WispMood::Error}) {
        WispModel m(216, 42);
        Frame f(m);
        m.set_mood(mood);
        for (int i = 0; i < 20; ++i) m.step(33);
        f.render(m);
        hashes.push_back(f.hash());
    }
    std::sort(hashes.begin(), hashes.end());
    EXPECT_EQ(std::unique(hashes.begin(), hashes.end()), hashes.end());
}

TEST(WispModel, TooSmallBuffersDrawNothing) {
    WispModel m(216, 42);
    std::vector<uint8_t> lum(10);
    std::vector<uint16_t> out(10);
    const wisp::hal::Area a = m.render(lum, out);
    EXPECT_LT(a.x2, a.x1);
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
