#pragma once

// The wisp (spec 6.1) without LVGL: its parameters, its motion, and the
// shape it draws as. WispView (wisp/ui/wisp_view.hpp) draws the shape on
// the screen; tests drive a model directly.
//
// The look is a minimal outline: one closed stroke for the drop-shaped
// body and its three flame tips, two filled eyes, short strokes for the
// "> <" eyes and the mouth, and a few spark dots on colour panels. No
// fill, no glow; moods differ by shape alone, so the same drawing works on
// colour, mono and e-paper panels.
//
// Everything per frame is integer: positions are Q8 pixels, parameters
// Q8.8 (256 is 1.0), angles index a 1024-entry Q15 sine table, and the
// random numbers come from xorshift32 on the model's own seed, so the same
// seed and the same calls give the same frames on every machine. Nothing
// here allocates after construction.

#include <array>
#include <cstddef>
#include <cstdint>

#include "wisp/hal/display.hpp"
#include "wisp/hal/display_geometry.hpp"

namespace wisp::ui {

enum class WispMood : uint8_t { Idle, Listening, Thinking, Speaking, Error };

// One mood's look. Q8.8 unless noted: 256 is 1.0.
struct WispParams {
    int32_t flame_height;    // height of the drop's top and the flame tips
    int32_t sway_speed;      // how fast the outline and tips burn
    int32_t brightness;      // stroke opacity on colour panels
    int32_t particle_rate;   // spark speed
    int32_t particle_orbit;  // 0: sparks rise; 256: they circle up right of the head
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

// What one frame draws, in Q8 pixels of the model's frame (0,0 is its top
// left corner).
struct WispShape {
    static constexpr int kMaxOutline = 104;
    static constexpr int kMaxSparks = 6;

    // A filled stadium: the segment a-b widened by r on every side. Eyes,
    // the mouth and sparks are capsules; so is every stroke segment.
    struct Capsule {
        WispPoint a, b;
        int32_t r = 0;
    };

    // The body and its flame tips: a closed polyline (the last point joins
    // the first), stroked `stroke` wide with round joints.
    std::array<WispPoint, kMaxOutline> outline{};
    int outline_n = 0;
    int32_t stroke = 0;

    // Two filled eyes, or (error) two "> <" strokes of three points each.
    bool chevrons = false;
    std::array<Capsule, 2> eyes{};
    std::array<std::array<WispPoint, 3>, 2> chevron{};

    // Speaking: a filled mouth that is a short line when shut.
    bool mouth = false;
    Capsule mouth_shape{};

    std::array<Capsule, kMaxSparks> sparks{};
    int spark_n = 0;

    // Everything drawn, stroke and anti-aliasing included, in whole pixels.
    hal::Area bbox{0, 0, -1, -1};
    // 0..255: how bright the stroke is on colour panels (the colour mixed
    // towards the background, drawn opaque, so overlapping segment caps do
    // not show).
    uint8_t opacity = 255;
};

class WispModel {
public:
    static constexpr uint32_t kMoodBlendMs = 300, kFastFrameMs = 33, kCalmFrameMs = 66,
                              kMonoFrameMs = 100;
    static constexpr int kMaxSparks = WispShape::kMaxSparks;

    // The frame is size_px wide and size_px * 5 / 4 high. `seed` 0 is
    // taken as 1 (xorshift32 never leaves 0). The panel decides sparks and
    // frame rate (spec 6.1).
    WispModel(int size_px, uint32_t seed, hal::PanelKind panel = hal::PanelKind::Amoled);

    void set_mood(WispMood m);
    WispMood mood() const { return mood_; }
    // 0..100: microphone or speech loudness. Only Listening and Speaking
    // show it.
    void set_level(uint8_t level);
    // Faster sway and sparks while a tool runs.
    void set_busy(bool fast);

    // Advances time by dt_ms (clamped to 100 ms). No allocation, no float.
    void step(uint32_t dt_ms);
    // Finishes the mood blend and any passing effect (blink, shake) at
    // once, for a panel that shows still pictures.
    void settle();

    // Builds this frame's shape. Returns the dirty box: the last frame's
    // bbox joined with this one's, inside the frame. The first build
    // returns the whole frame.
    hal::Area build();
    const WispShape& shape() const { return shape_; }

    // The mood parameters in effect (blended while switching moods).
    const WispParams& params() const { return params_; }
    // Colour panels: 33 while blending and in Listening, Thinking and
    // Error, else 66. Mono: 100. E-paper: 0, no animation.
    uint32_t frame_interval_ms() const;
    hal::PanelKind panel() const { return panel_; }
    int width() const { return w_; }
    int height() const { return h_; }
    size_t live_particles() const { return live_; }

    // Observation for tests and the demo. The last build's bbox.
    hal::Area bounds() const { return shape_.bbox; }
    // The body's centre (the middle of the drop's round bottom), Q8 px.
    WispPoint centre() const { return {cx_, cy_}; }
    // Distance the live sparks moved in the last step, Q8 px.
    uint32_t particle_travel() const { return travel_; }

private:
    struct Particle {
        int32_t x = 0, y = 0;    // rising position, Q8 px
        int32_t vx = 0, vy = 0;  // Q8 px per second
        int32_t dx = 0, dy = 0;  // drawn position (rising or orbiting), Q8 px
        uint32_t phase = 0;      // orbit angle, 1/65536 turn
        uint16_t age_ms = 0;
        uint16_t life_ms = 0;  // 0: the slot is free
        int8_t orbit_pct = 0;  // orbit radius offset, percent
    };
    struct Hair {
        int32_t x = 0, y = 0;    // tip, Q8 px
        int32_t vx = 0, vy = 0;  // Q8 px per second
        int32_t ax = 0, ay = 0;  // rest anchor, Q8 px
    };
    // Body row geometry at normalized height yn (Q12, 0 at the centre,
    // -4096 at the top of the drop, 4096 at the bottom).
    int32_t sway_at(int32_t yn) const;           // centre line offset from cx_, Q8 px
    int32_t half_width_scale(int32_t yn) const;  // outline's x scale, Q8 px
    // The body outline's point at angle `a` (1/65536 turn, 0 at the top,
    // clockwise).
    WispPoint body_point(uint32_t a) const;

    uint32_t rand();
    int32_t rand_range(int32_t lo, int32_t hi);  // [lo, hi]
    void spawn(Particle& p, bool initial);
    void step_particles(uint32_t dt_ms);
    void step_hair(uint32_t dt_ms);
    void place_hair(bool snap);
    void update_geometry();
    void build_outline();
    void build_face();
    void build_sparks();
    // The drop's bottom radius before breathing: 0.29 of the width, so the
    // body fills most of the frame and the flame tips and sparks the rest.
    int32_t r_base() const { return px(63 * 256); }
    // A length given in Q8 px for size 216, scaled to this model's size.
    int32_t px(int32_t q8_at_216) const {
        return static_cast<int32_t>(static_cast<int64_t>(q8_at_216) * w_ / 216);
    }

    int w_, h_;
    uint32_t rng_;
    hal::PanelKind panel_;
    bool sparks_on_;
    WispMood mood_ = WispMood::Idle;
    WispParams from_{}, to_{}, params_{};
    // How much Listening, Speaking and Error show, Q8, blended like params_.
    int32_t listen_from_ = 0, listen_to_ = 0, listen_ = 0;
    int32_t speak_from_ = 0, speak_to_ = 0, speak_ = 0;
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

    // Geometry for the next build, from update_geometry().
    int32_t cx_ = 0, cy_ = 0;  // Q8 px
    int32_t r_ = 0;            // bottom's radius, Q8 px
    int32_t top_ = 0;          // drop's height above the centre, Q8 px
    int32_t sway_amp_ = 0;     // Q8 px at the top

    std::array<Particle, kMaxSparks> particles_{};
    size_t live_ = 0;
    uint32_t travel_ = 0;
    std::array<Hair, 3> hair_{};

    WispShape shape_{};
    bool first_build_ = true;
};

// FNV-1a over everything a shape draws.
uint32_t shape_hash(const WispShape& s);

// Calls fn(ctx, y, x1, x2) for every run of whole pixels the shape covers,
// row by row from the top, x1 <= x2, in the shape's own coordinates: the
// shape drawn without anti-aliasing, for two-colour panels. Each row's runs
// are sorted and do not overlap. No allocation.
void for_each_span(const WispShape& s, void* ctx, void (*fn)(void* ctx, int y, int x1, int x2));
// The same for rows y1..y2 only (a draw call's clip).
void for_each_span(const WispShape& s, int y1, int y2, void* ctx,
                   void (*fn)(void* ctx, int y, int x1, int x2));

// The wisps' frame rate and mean time per frame (model step, build and
// drawing calls) over the last 5 s, summed over every view. 0, 0 when none
// drew a frame.
struct WispPerf {
    double fps = 0, ms = 0;
};
WispPerf wisp_perf(uint64_t now_ms);
// Records one frame that took frame_us; WispView calls it per frame.
void wisp_perf_record(uint64_t now_ms, uint32_t frame_us);
// Forgets every recorded frame (tests).
void wisp_perf_reset();

}  // namespace wisp::ui
