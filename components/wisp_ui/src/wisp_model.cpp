// WispModel's parameters and motion (wisp/ui/wisp_model.hpp); the shape
// it draws as is built in wisp_shape.cpp, and rasterized for two-colour
// panels in wisp_spans.cpp.

#include "wisp/ui/wisp_model.hpp"

#include <algorithm>
#include <cstdlib>

#include "wisp/os/os.hpp"
#include "wisp_tables.hpp"

namespace wisp::ui {

using wisp_detail::cos16;
using wisp_detail::isqrt;
using wisp_detail::sin16;

namespace {

constexpr uint32_t kBlinkMs = 170;
constexpr uint32_t kErrorShakeMs = 600;
constexpr uint32_t kMaxStepMs = 100;

// A phase in 1/65536 turns advanced by `mhz` (millihertz) over dt_ms,
// times speed (Q8).
uint32_t advance(uint32_t phase, uint32_t dt_ms, uint32_t mhz, int32_t speed_q8) {
    const uint64_t inc = static_cast<uint64_t>(dt_ms) * mhz * 65536u / 1000000u *
                         static_cast<uint64_t>(std::max<int32_t>(speed_q8, 0)) / 256u;
    return phase + static_cast<uint32_t>(inc);
}

// t^2 (3 - 2t), t and result in 0..256.
uint32_t smooth_q8(uint32_t t) { return t * t * (768 - 2 * t) >> 16; }

// Where a periodic motion of `period_ms` is at time t, in 1/65536 turns.
uint32_t cycle(uint32_t t_ms, uint32_t period_ms) {
    return static_cast<uint32_t>(static_cast<uint64_t>(t_ms % period_ms) * 65536u / period_ms);
}

int32_t lerp1(int32_t a, int32_t b, uint32_t t) {
    return a + (b - a) * static_cast<int32_t>(t) / 256;
}

int32_t ease_to(int32_t cur, int32_t target, uint32_t dt_ms, uint32_t tau_ms) {
    const int32_t k = static_cast<int32_t>(std::min<uint32_t>(256, dt_ms * 256 / tau_ms));
    return cur + (target - cur) * k / 256;
}

// Flame tips: where each hangs on the drop (normalized height, and the
// fraction of the half width to the side, Q8), and where its tip rests
// relative to that point (Q8 px at size 216).
struct HairSpec {
    int32_t yn, side, rest_x, rest_y;
};
constexpr uint32_t kSpringStepMs = 25;

constexpr HairSpec kHair[3] = {
    {-2900, -170, -3 * 256, -24 * 256},
    {-3500, 0, 1 * 256, -30 * 256},
    {-2800, 170, 4 * 256, -21 * 256},
};

}  // namespace

WispParams mood_params(WispMood m) {
    switch (m) {
        case WispMood::Idle:
            return {256, 256, 230, 256, 0, 256, 0, 0, 0, 0};
        case WispMood::Listening:
            return {276, 380, 256, 330, 0, 320, 0, 40, 56, 0};
        case WispMood::Thinking:
            return {256, 300, 240, 300, 256, 210, 120, -256, -20, 0};
        case WispMood::Speaking:
            return {268, 330, 250, 290, 0, 256, 0, 0, 0, 0};
        case WispMood::Error:
            return {200, 180, 170, 140, 0, 120, 0, 70, 0, 4 * 256};
    }
    return mood_params(WispMood::Idle);
}

WispParams lerp(const WispParams& a, const WispParams& b, uint32_t t) {
    t = std::min<uint32_t>(t, 256);
    return {lerp1(a.flame_height, b.flame_height, t),
            lerp1(a.sway_speed, b.sway_speed, t),
            lerp1(a.brightness, b.brightness, t),
            lerp1(a.particle_rate, b.particle_rate, t),
            lerp1(a.particle_orbit, b.particle_orbit, t),
            lerp1(a.eye_open, b.eye_open, t),
            lerp1(a.gaze_x, b.gaze_x, t),
            lerp1(a.gaze_y, b.gaze_y, t),
            lerp1(a.lean, b.lean, t),
            lerp1(a.jitter, b.jitter, t)};
}

WispModel::WispModel(int size_px, uint32_t seed, hal::PanelKind panel)
    : w_(std::max<int32_t>(size_px, 16)),
      h_(std::max<int32_t>(size_px, 16) * 5 / 4),
      rng_(seed == 0 ? 1 : seed),
      panel_(panel),
      sparks_on_(hal::panel_is_colour(panel)) {
    params_ = from_ = to_ = mood_params(WispMood::Idle);
    blink_ms_ = kBlinkMs;
    next_blink_ms_ = static_cast<uint32_t>(rand_range(1500, 4000));
    next_glance_ms_ = static_cast<uint32_t>(rand_range(2000, 4000));
    for (uint32_t& p : phase_) p = rand();
    hair_phase_ = rand();
    update_geometry();
    place_hair(true);
    if (sparks_on_) {
        for (size_t i = 0; i < 4; ++i) {
            spawn(particles_[i], true);
            ++live_;
        }
    }
}

uint32_t WispModel::rand() {
    uint32_t x = rng_;
    x ^= x << 13;
    x ^= x >> 17;
    x ^= x << 5;
    rng_ = x;
    return x;
}

int32_t WispModel::rand_range(int32_t lo, int32_t hi) {
    if (hi <= lo) return lo;
    return lo + static_cast<int32_t>(rand() % static_cast<uint32_t>(hi - lo + 1));
}

void WispModel::set_mood(WispMood m) {
    if (m == mood_) return;
    from_ = params_;
    to_ = mood_params(m);
    listen_from_ = listen_;
    listen_to_ = m == WispMood::Listening ? 256 : 0;
    speak_from_ = speak_;
    speak_to_ = m == WispMood::Speaking ? 256 : 0;
    err_from_ = err_;
    err_to_ = m == WispMood::Error ? 256 : 0;
    blend_ms_ = 0;
    if (m == WispMood::Error) error_ms_ = 0;
    mood_ = m;
}

void WispModel::set_level(uint8_t level) {
    level_target_ = std::min<int32_t>(level, 100) * 256 / 100;
}

void WispModel::set_busy(bool fast) { busy_ = fast; }

uint32_t WispModel::frame_interval_ms() const {
    if (panel_ == hal::PanelKind::EPaper) return 0;
    if (panel_ == hal::PanelKind::Mono) return kMonoFrameMs;
    if (blend_ms_ < kMoodBlendMs || mood_ == WispMood::Listening || mood_ == WispMood::Thinking ||
        mood_ == WispMood::Error) {
        return kFastFrameMs;
    }
    return kCalmFrameMs;
}

void WispModel::step(uint32_t dt_ms) {
    dt_ms = std::min(dt_ms, kMaxStepMs);
    time_ms_ += dt_ms;

    if (blend_ms_ < kMoodBlendMs) {
        blend_ms_ = std::min(blend_ms_ + dt_ms, kMoodBlendMs);
        const uint32_t t = smooth_q8(blend_ms_ * 256 / kMoodBlendMs);
        params_ = lerp(from_, to_, t);
        listen_ = lerp1(listen_from_, listen_to_, t);
        speak_ = lerp1(speak_from_, speak_to_, t);
        err_ = lerp1(err_from_, err_to_, t);
    }

    // The level jumps up quickly and falls back slowly, like a VU meter.
    level_ = ease_to(level_, level_target_, dt_ms, level_target_ > level_ ? 60 : 180);
    busy_q8_ = ease_to(busy_q8_, busy_ ? 256 : 0, dt_ms, 250);

    // Busy (a tool runs): everything burns faster.
    const int32_t sway = params_.sway_speed * (256 + busy_q8_ * 3 / 4) / 256;
    phase_[0] = advance(phase_[0], dt_ms, 1100, sway);
    phase_[1] = advance(phase_[1], dt_ms, 1700, sway);
    phase_[2] = advance(phase_[2], dt_ms, 2900, sway);
    hair_phase_ = advance(hair_phase_, dt_ms, 1300, sway);

    if (mood_ == WispMood::Error) error_ms_ = std::min<uint32_t>(error_ms_ + dt_ms, 60000);

    // Blinks, in every mood.
    if (blink_ms_ < kBlinkMs) blink_ms_ = std::min(blink_ms_ + dt_ms, kBlinkMs);
    if (time_ms_ >= next_blink_ms_) {
        blink_ms_ = 0;
        // Now and then a quick double blink.
        next_blink_ms_ = time_ms_ + static_cast<uint32_t>(rand() % 5 == 0 ? rand_range(260, 320)
                                                                          : rand_range(2400, 5600));
    }

    // Idle looks around now and then; other moods look where they look.
    if (mood_ == WispMood::Idle) {
        if (time_ms_ >= next_glance_ms_) {
            if (glance_tx_ != 0 || glance_ty_ != 0) {
                glance_tx_ = glance_ty_ = 0;
                next_glance_ms_ = time_ms_ + static_cast<uint32_t>(rand_range(1800, 4200));
            } else {
                glance_tx_ = rand_range(-230, 230);
                glance_ty_ = rand_range(-140, 90);
                next_glance_ms_ = time_ms_ + static_cast<uint32_t>(rand_range(900, 1900));
            }
        }
    } else {
        glance_tx_ = glance_ty_ = 0;
    }
    glance_x_ = ease_to(glance_x_, glance_tx_, dt_ms, 110);
    glance_y_ = ease_to(glance_y_, glance_ty_, dt_ms, 110);

    // Speaking: a spring pulled by the level stretches the drop.
    // Springs are stepped at most kSpringStepMs at a time: a slow panel's
    // 100 ms frames would otherwise make them unstable.
    {
        const int64_t target = static_cast<int64_t>(level_) * speak_ / 256 * 640 / 256;  // Q12
        for (uint32_t left = dt_ms; left > 0;) {
            const uint32_t d = std::min(left, kSpringStepMs);
            left -= d;
            const int64_t a = 700 * (target - bounce_) - 13 * static_cast<int64_t>(bounce_v_);
            bounce_v_ += static_cast<int32_t>(a * d / 1000);
            bounce_ += static_cast<int32_t>(static_cast<int64_t>(bounce_v_) * d / 1000);
            bounce_ = std::clamp<int32_t>(bounce_, -900, 900);
        }
    }

    update_geometry();
    step_hair(dt_ms);
    step_particles(dt_ms);
}

void WispModel::settle() {
    blend_ms_ = kMoodBlendMs;
    params_ = to_;
    listen_ = listen_to_;
    speak_ = speak_to_;
    err_ = err_to_;
    level_ = level_target_;
    busy_q8_ = busy_ ? 256 : 0;
    error_ms_ = std::max(error_ms_, kErrorShakeMs);
    blink_ms_ = kBlinkMs;
    glance_x_ = glance_tx_ = 0;
    glance_y_ = glance_ty_ = 0;
    bounce_ = static_cast<int32_t>(static_cast<int64_t>(level_) * speak_ / 256 * 640 / 256);
    bounce_v_ = 0;
    update_geometry();
    place_hair(true);
}

void WispModel::update_geometry() {
    const int32_t rb = r_base();
    // Breathing: 3.6 s in and out; the whole wisp floats up and down with it.
    const uint32_t breath = cycle(time_ms_, 3600);
    const int32_t b = sin16(breath);
    r_ = rb + (rb * b >> 15) * 6 / 256;
    const int32_t listen_level = level_ * listen_ / 256;  // Q8
    const int32_t flame = params_.flame_height * (256 + listen_level * 38 / 256) / 256;
    top_ = static_cast<int32_t>(static_cast<int64_t>(r_) * 3 / 2 * flame / 256);
    top_ += (top_ * b >> 15) * 10 / 256;
    top_ = static_cast<int32_t>(static_cast<int64_t>(top_) * (4096 + bounce_) / 4096);
    r_ = static_cast<int32_t>(static_cast<int64_t>(r_) * (4096 - bounce_ / 3) / 4096);
    sway_amp_ = r_ * 18 / 256 * (256 + listen_level / 2) / 256;

    // Anti-burn-in drift: two slow, unrelated periods, so the wisp never
    // sits on the same pixels for long.
    const int32_t drift_x = px(5 * 256 + 154) * sin16(cycle(time_ms_, 23000)) >> 15;
    const int32_t drift_y = px(3 * 256 + 230) * sin16(cycle(time_ms_, 31000) + 10923) >> 15;
    const int32_t bob = px(2 * 256 + 154) * sin16(breath + 16384) >> 15;

    int32_t shake_x = 0, shake_y = 0;
    if (mood_ == WispMood::Error) {
        if (error_ms_ < kErrorShakeMs) {
            const int32_t amp = static_cast<int32_t>(static_cast<int64_t>(px(to_.jitter)) *
                                                     (kErrorShakeMs - error_ms_) / kErrorShakeMs);
            shake_x = amp * sin16(error_ms_ * 65536u * 13u / 1000u) >> 15;
            shake_y = (amp / 3) * cos16(error_ms_ * 65536u * 9u / 1000u) >> 15;
        }
    }

    cx_ = w_ * 128 + drift_x + shake_x;
    cy_ = h_ * 256 * 645 / 1000 + drift_y + bob + shake_y - rb * bounce_ / 4096 / 2;
}

int32_t WispModel::sway_at(int32_t yn) const {
    const int32_t up = yn < 0 ? std::min<int32_t>(-yn, 4096) : 0;
    // A quarter of the movement at the bottom, all of it at the top.
    const int32_t amp = sway_amp_ * (1024 + 3 * up / 4) / 4096;
    // Low spatial frequency only (well under a wave over the height): the
    // body bends as a whole and its edge stays a smooth curve.
    const int32_t s = (3 * sin16(phase_[0] + static_cast<uint32_t>(yn * 4)) +
                       2 * sin16(phase_[1] - static_cast<uint32_t>(yn * 7)) +
                       sin16(phase_[2] + static_cast<uint32_t>(yn * 2))) /
                      6;
    int32_t off = amp * s >> 15;
    off += static_cast<int32_t>(static_cast<int64_t>(params_.lean * r_ / 256) * (up * up >> 12) /
                                4096);
    return off;
}

int32_t WispModel::half_width_scale(int32_t yn) const {
    if (yn >= 0) return r_;
    const int32_t up = std::min<int32_t>(-yn, 4096);
    // Quadratic, so the width has no kink at the equator.
    const int32_t taper = 256 - 150 * (up * up >> 12) / 4096;
    return r_ * taper / 256;
}

void WispModel::place_hair(bool snap) {
    const int32_t listen_level = level_ * listen_ / 256;
    const int32_t flame = params_.flame_height * (256 + listen_level * 64 / 256) / 256;
    const int32_t scale = r_ * 256 / std::max<int32_t>(r_base(), 1);  // Q8, follows breathing
    for (size_t i = 0; i < hair_.size(); ++i) {
        const HairSpec& s = kHair[i];
        Hair& h = hair_[i];
        const int32_t dy = s.yn * top_ / 4096;
        // The outline's half width at the root: x scale * sqrt(1 - yn^2).
        const int32_t y2 = std::min<int32_t>(s.yn * s.yn >> 12, 4096);
        const int32_t hw = half_width_scale(s.yn) *
                           static_cast<int32_t>(isqrt(static_cast<uint32_t>(4096 - y2) << 12)) /
                           4096;
        h.ax = cx_ + sway_at(s.yn) + hw * s.side / 256;
        h.ay = cy_ + dy;
        if (snap) {
            h.x = h.ax + px(s.rest_x) * flame / 256;
            h.y = h.ay + static_cast<int32_t>(static_cast<int64_t>(px(s.rest_y)) * flame / 256 *
                                              scale / 256);
            h.vx = h.vy = 0;
        }
    }
}

void WispModel::step_hair(uint32_t dt_ms) {
    place_hair(false);
    const int32_t listen_level = level_ * listen_ / 256;
    const int32_t flame = params_.flame_height * (256 + listen_level * 64 / 256) / 256;
    const int32_t scale = r_ * 256 / std::max<int32_t>(r_base(), 1);
    for (size_t i = 0; i < hair_.size(); ++i) {
        const HairSpec& s = kHair[i];
        Hair& h = hair_[i];
        const uint32_t ph = hair_phase_ + static_cast<uint32_t>(i) * 21845u;
        const int32_t tx = h.ax + px(s.rest_x) * flame / 256 + px(params_.lean * 20) / 256 +
                           (px(4 * 256) * sin16(ph) >> 15);
        const int32_t ty =
            h.ay +
            static_cast<int32_t>(static_cast<int64_t>(px(s.rest_y)) * flame / 256 * scale / 256) +
            (px(3 * 256) * cos16(ph * 2 + 9000) >> 15);
        // Spring towards the rest point, lightly damped: about 2 Hz, so
        // the tips trail the body's movement and overshoot a little.
        for (uint32_t left = dt_ms; left > 0;) {
            const uint32_t d = std::min(left, kSpringStepMs);
            left -= d;
            const int64_t ax =
                170 * static_cast<int64_t>(tx - h.x) - 15 * static_cast<int64_t>(h.vx) / 2;
            const int64_t ay =
                170 * static_cast<int64_t>(ty - h.y) - 15 * static_cast<int64_t>(h.vy) / 2;
            h.vx += static_cast<int32_t>(ax * d / 1000);
            h.vy += static_cast<int32_t>(ay * d / 1000);
            h.x += static_cast<int32_t>(static_cast<int64_t>(h.vx) * d / 1000);
            h.y += static_cast<int32_t>(static_cast<int64_t>(h.vy) * d / 1000);
        }
        // Never below its root: a tongue hangs up, not down.
        h.y = std::min<int32_t>(h.y, h.ay - px(4 * 256));
    }
}

void WispModel::spawn(Particle& p, bool initial) {
    p.life_ms = static_cast<uint16_t>(rand_range(1600, 3400));
    p.age_ms = initial ? static_cast<uint16_t>(rand_range(0, p.life_ms - 1)) : 0;
    // Around the upper outline, where the flame would shed them.
    const int32_t side = rand_range(r_ * 3 / 10, r_ * 11 / 10);
    p.x = cx_ + ((rand() & 1) != 0 ? side : -side);
    p.y = cy_ - rand_range(r_ * 3 / 10, r_ * 15 / 10);
    p.vx = rand_range(-px(6 * 256), px(6 * 256));
    p.vy = -rand_range(px(14 * 256), px(28 * 256));
    p.phase = rand();
    p.orbit_pct = static_cast<int8_t>(rand_range(-10, 10));
    if (initial) {
        p.x += p.vx * p.age_ms / 1000;
        p.y += p.vy * p.age_ms / 1000;
        // Spread over the frame's height without running off its top.
        p.y = std::max<int32_t>(p.y, px(14 * 256));
    }
    p.dx = p.x;
    p.dy = p.y;
}

void WispModel::step_particles(uint32_t dt_ms) {
    const int32_t speed = params_.particle_rate * (256 + busy_q8_ * 150 / 256) / 256;
    // Four sparks when calm, up to six when lively; none on two-colour panels.
    const size_t target =
        sparks_on_ ? static_cast<size_t>(
                         4 + std::clamp<int32_t>((params_.particle_rate - 256) * 2 / 64, 0, 2))
                   : 0;
    const int32_t orbit = params_.particle_orbit;
    // The orbit: a small ring up and to the right of the head, where a
    // thinking wisp looks, clear of the face.
    const int32_t ocx = cx_ + r_ * 75 / 100, ocy = cy_ - top_ * 90 / 100;
    const int32_t margin = 6 * 256;
    travel_ = 0;
    for (Particle& p : particles_) {
        if (p.life_ms == 0) continue;
        if (static_cast<uint32_t>(p.age_ms) + dt_ms >= p.life_ms) {
            p.life_ms = 0;
            --live_;
            continue;
        }
        const bool fresh = p.age_ms == 0;
        p.age_ms = static_cast<uint16_t>(p.age_ms + dt_ms);
        p.x += static_cast<int32_t>(static_cast<int64_t>(p.vx) * speed / 256 * dt_ms / 1000);
        p.y += static_cast<int32_t>(static_cast<int64_t>(p.vy) * speed / 256 * dt_ms / 1000);
        p.phase += dt_ms * 21u * static_cast<uint32_t>(std::max<int32_t>(speed, 0)) / 256u;
        const int32_t rise_x = p.x + (px(3 * 256) * sin16(p.phase / 3 + p.age_ms * 44u) >> 15);
        const int32_t rx = r_ * 50 / 100 * (100 + p.orbit_pct) / 100;
        const int32_t ry = r_ * 20 / 100 * (100 + p.orbit_pct) / 100;
        const int32_t ox = ocx + (rx * cos16(p.phase) >> 15);
        const int32_t oy = ocy + (ry * sin16(p.phase) >> 15) - (ox - ocx) * 14 / 100;
        const int32_t nx = rise_x + (ox - rise_x) * orbit / 256;
        const int32_t ny = p.y + (oy - p.y) * orbit / 256;
        // A new spark's first step places it; that jump is not travel.
        if (!fresh) travel_ += static_cast<uint32_t>(std::abs(nx - p.dx) + std::abs(ny - p.dy));
        p.dx = nx;
        p.dy = ny;
        if (nx < margin || nx > w_ * 256 - margin || ny < margin + px(6 * 256) ||
            ny > h_ * 256 - margin) {
            p.life_ms = 0;
            --live_;
        }
    }
    for (Particle& p : particles_) {
        if (live_ >= target) break;
        if (p.life_ms != 0) continue;
        spawn(p, false);
        ++live_;
    }
}

namespace {

// 100 ms buckets over the last 5 s.
constexpr uint64_t kBucketMs = 100;
constexpr size_t kBuckets = 50;

struct PerfWindow {
    os::Mutex mutex;
    uint64_t index[kBuckets] = {};  // bucket number + 1; 0: empty
    uint32_t frames[kBuckets] = {};
    uint64_t us[kBuckets] = {};
};

PerfWindow& perf_window() {
    static PerfWindow w;
    return w;
}

}  // namespace

void wisp_perf_record(uint64_t now_ms, uint32_t frame_us) {
    PerfWindow& w = perf_window();
    const uint64_t n = now_ms / kBucketMs;
    const size_t i = static_cast<size_t>(n % kBuckets);
    w.mutex.lock();
    if (w.index[i] != n + 1) {
        w.index[i] = n + 1;
        w.frames[i] = 0;
        w.us[i] = 0;
    }
    ++w.frames[i];
    w.us[i] += frame_us;
    w.mutex.unlock();
}

WispPerf wisp_perf(uint64_t now_ms) {
    PerfWindow& w = perf_window();
    const uint64_t n = now_ms / kBucketMs;
    uint64_t frames = 0, us = 0;
    w.mutex.lock();
    for (size_t i = 0; i < kBuckets; ++i) {
        const uint64_t idx = w.index[i];
        if (idx == 0 || idx - 1 > n || n - (idx - 1) >= kBuckets) continue;
        frames += w.frames[i];
        us += w.us[i];
    }
    w.mutex.unlock();
    WispPerf p;
    if (frames != 0) {
        p.fps = static_cast<double>(frames) * 1000.0 / static_cast<double>(kBucketMs * kBuckets);
        p.ms = static_cast<double>(us) / 1000.0 / static_cast<double>(frames);
    }
    return p;
}

void wisp_perf_reset() {
    PerfWindow& w = perf_window();
    w.mutex.lock();
    for (size_t i = 0; i < kBuckets; ++i) {
        w.index[i] = 0;
        w.frames[i] = 0;
        w.us[i] = 0;
    }
    w.mutex.unlock();
}

}  // namespace wisp::ui
