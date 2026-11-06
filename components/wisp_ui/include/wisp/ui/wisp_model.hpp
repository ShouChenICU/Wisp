#pragma once

// The wisp (spec 6.1, foundation spec 1.9) without LVGL: its parameters,
// its motion and its rasterizer. WispView (wisp/ui/wisp_view.hpp) puts a
// model on the screen; tests drive one directly.
//
// Everything per frame is integer: positions are Q8 pixels, parameters
// Q8.8 (256 is 1.0), angles index a 1024-entry Q15 sine table, and the
// random numbers come from xorshift32 on the model's own seed, so the same
// seed and the same calls give the same frames on every machine. step()
// and render() never allocate; the caller owns the luminance map and the
// RGB565 frame.
//
// Drawing: an 8-bit luminance map (body with its halo from one lookup
// table, three hair tongues, particles, eyes) is turned into RGB565
// through a 256-entry palette. 0 is black; the body stays below the
// eyes' white, which owns the top of the palette.

#include <array>
#include <cstddef>
#include <cstdint>
#include <span>

#include "wisp/hal/display.hpp"

namespace wisp::ui {

enum class WispMood : uint8_t { Idle, Listening, Thinking, Speaking, Error };

// One mood's look. Q8.8 unless noted: 256 is 1.0.
struct WispParams {
    int32_t flame_height;    // height of the drop's top and the hair
    int32_t sway_speed;      // how fast the outline and hair burn
    int32_t brightness;      // luminance scale
    int32_t particle_rate;   // particle speed
    int32_t particle_orbit;  // 0: particles rise; 256: they circle the body
    int32_t eye_open;        // eye height
    int32_t gaze_x;          // eyes' offset, 256 = one eye width
    int32_t gaze_y;          // eyes' offset, 256 = one eye height; negative looks up
    int32_t lean;            // top's sideways lean, 256 = one body radius
    int32_t jitter;          // shake amplitude in Q8 pixels at size 216 (decays, see step())
};

WispParams mood_params(WispMood m);
// a + (b - a) * t, t in 0..256.
WispParams lerp(const WispParams& a, const WispParams& b, uint32_t t_q8);

// A point in Q8 pixels of the model's frame.
struct WispPoint {
    int32_t x = 0;
    int32_t y = 0;
};

class WispModel {
public:
    static constexpr uint32_t kMoodBlendMs = 300, kFastFrameMs = 33, kCalmFrameMs = 66;
    static constexpr int kMinParticles = 20, kMaxParticles = 40;

    // The frame is size_px wide and size_px * 5 / 4 high. `seed` 0 is
    // taken as 1 (xorshift32 never leaves 0).
    WispModel(int size_px, uint32_t seed);

    void set_mood(WispMood m);
    WispMood mood() const { return mood_; }
    // 0..100: microphone or speech loudness. Only Listening and Speaking
    // show it.
    void set_level(uint8_t level);
    // Faster particles while a tool runs.
    void set_busy(bool fast);

    // Advances time by dt_ms (clamped to 100 ms). No allocation, no float.
    void step(uint32_t dt_ms);

    // Draws the current state: luminance into `lum`, then the palette into
    // `out` (both width() * height(), row-major). Returns the dirty box:
    // the last frame's lit bounds joined with this one's, inside the frame.
    // Pixels of `out` outside it are left as they were and are black. The
    // first render after construction covers the whole frame. Spans that
    // are too small draw nothing and return an empty box (x2 < x1).
    hal::Area render(std::span<uint8_t> lum, std::span<uint16_t> out);

    // The mood parameters in effect (blended while switching moods).
    const WispParams& params() const { return params_; }
    // 33 while blending and in Listening, Thinking and Error; else 66.
    uint32_t frame_interval_ms() const;
    int width() const { return w_; }
    int height() const { return h_; }
    size_t live_particles() const { return live_; }

    // Observation for tests and the demo. The last render's lit bounds.
    hal::Area bounds() const { return bounds_; }
    // The body's centre (the middle of the drop's round bottom), Q8 px.
    WispPoint centre() const { return {cx_, cy_}; }
    // Distance the live particles moved in the last step, Q8 px.
    uint32_t particle_travel() const { return travel_; }

    // Stores the palette in the panel's byte order: true for displays that
    // take each RGB565 pixel high byte first (caps "rgb565_be"). The next
    // render() redraws the whole frame.
    void set_swapped_palette(bool swapped);

private:
    struct Particle {
        int32_t x = 0, y = 0;    // rising position, Q8 px
        int32_t vx = 0, vy = 0;  // Q8 px per second
        int32_t dx = 0, dy = 0;  // drawn position (rising or orbiting), Q8 px
        uint32_t phase = 0;      // orbit angle, 1/65536 turn
        uint16_t age_ms = 0;
        uint16_t life_ms = 0;  // 0: the slot is free
        uint8_t glow = 0;      // brightness, 0..255
        int8_t orbit_pct = 0;  // orbit radius offset, percent
    };
    struct Hair {
        int32_t x = 0, y = 0;    // tip, Q8 px
        int32_t vx = 0, vy = 0;  // Q8 px per second
        int32_t ax = 0, ay = 0;  // root, Q8 px
    };
    // Body row geometry at normalized height yn (Q12, 0 at the centre,
    // -4096 at the top of the drop, 4096 at the bottom).
    int32_t sway_at(int32_t yn) const;           // centre line offset from cx_, Q8 px
    int32_t half_width_scale(int32_t yn) const;  // outline's x scale, Q8 px

    uint32_t rand();
    int32_t rand_range(int32_t lo, int32_t hi);  // [lo, hi]
    void spawn(Particle& p, bool initial);
    void step_particles(uint32_t dt_ms);
    void step_hair(uint32_t dt_ms);
    void place_hair(bool snap);
    void update_geometry();
    void build_palette();
    // A length given in Q8 px for size 216, scaled to this model's size.
    // The drop's bottom radius before breathing: 0.29 of the width, so the
    // body fills most of the frame and the halo, hair and sparks the rest.
    int32_t r_base() const { return px(63 * 256); }
    int32_t px(int32_t q8_at_216) const {
        return static_cast<int32_t>(static_cast<int64_t>(q8_at_216) * w_ / 216);
    }
    void draw_body(uint8_t* lum, hal::Area& lit) const;
    void draw_hair(uint8_t* lum, hal::Area& lit) const;
    void draw_particles(uint8_t* lum, hal::Area& lit) const;
    void draw_eyes(uint8_t* lum) const;
    void draw_chevron(uint8_t* lum, int32_t cx, int32_t cy, int dir, int32_t hw, int32_t hh) const;
    void draw_mouth(uint8_t* lum) const;

    int w_, h_;
    uint32_t rng_;
    WispMood mood_ = WispMood::Idle;
    WispParams from_{}, to_{}, params_{};
    // How much Listening and Speaking show the level, Q8, blended like params_.
    int32_t listen_from_ = 0, listen_to_ = 0, listen_ = 0;
    int32_t speak_from_ = 0, speak_to_ = 0, speak_ = 0;
    // How much of Error's face and colour shows, Q8, blended the same way.
    int32_t err_from_ = 0, err_to_ = 0, err_ = 0;
    uint32_t blend_ms_ = kMoodBlendMs;
    int32_t level_target_ = 0;  // Q8
    int32_t level_ = 0;         // smoothed, Q8
    bool busy_ = false;
    int32_t busy_q8_ = 0;  // eased towards busy_

    uint32_t time_ms_ = 0;
    uint32_t phase_[3] = {0, 0, 0};  // outline burn, 1/65536 turn
    uint32_t hair_phase_ = 0;
    uint32_t error_ms_ = 0;  // time in Error
    uint32_t next_blink_ms_ = 0;
    uint32_t blink_ms_ = 0;  // time into the blink; >= kBlinkMs: open
    uint32_t next_glance_ms_ = 0;
    int32_t glance_tx_ = 0, glance_ty_ = 0;  // where Idle glances, Q8 of an eye
    int32_t glance_x_ = 0, glance_y_ = 0;    // eased
    int32_t bounce_ = 0, bounce_v_ = 0;      // speaking stretch, Q12 and per second

    // Geometry for the next render, from update_geometry().
    int32_t cx_ = 0, cy_ = 0;  // Q8 px
    int32_t r_ = 0;            // bottom's radius, Q8 px
    int32_t top_ = 0;          // drop's height above the centre, Q8 px
    int32_t sway_amp_ = 0;     // Q8 px at the top
    int32_t dim_ = 256;        // Error's passing dip, Q8

    std::array<Particle, kMaxParticles> particles_{};
    size_t live_ = 0;
    uint32_t travel_ = 0;
    std::array<Hair, 3> hair_{};

    std::array<uint16_t, 256> palette_{};
    std::array<uint8_t, 256> body_lum_{};  // kBodyLum scaled by brightness
    bool swapped_ = false;
    int32_t pal_desat_ = 0;  // the palette's current desaturation, Q8
    bool first_render_ = true;
    hal::Area bounds_{0, 0, -1, -1};
};

// FNV-1a over the pixels, low byte first.
uint32_t frame_hash(std::span<const uint16_t> px);

// The wisps' frame rate and mean time per frame (model step plus render)
// over the last 5 s, summed over every view. 0, 0 when none drew a frame.
struct WispPerf {
    double fps = 0, ms = 0;
};
WispPerf wisp_perf(uint64_t now_ms);
// Records one frame that took frame_us; WispView calls it per frame.
void wisp_perf_record(uint64_t now_ms, uint32_t frame_us);
// Forgets every recorded frame (tests).
void wisp_perf_reset();

}  // namespace wisp::ui
