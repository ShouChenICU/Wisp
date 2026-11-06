// WispModel's rasterizer (wisp/ui/wisp_model.hpp): luminance map, then
// palette. Each part of the wisp is drawn row by row in a normalized
// space where its outline is r = 1, so one table (kBodyLum) gives the
// core, the one-pixel edge and the halo by r^2, with a multiply and a
// lookup per pixel.

#include <algorithm>
#include <cstring>

#include "wisp/ui/wisp_model.hpp"
#include "wisp_tables.hpp"

namespace wisp::ui {

using wisp_detail::isqrt;
using wisp_detail::kBodyLum;
using wisp_detail::kHaloQ8;
using wisp_detail::kSpark;
using wisp_detail::sin16;

namespace {

constexpr uint32_t kBlinkMs = 170;
constexpr int kSparkMax = 215;  // sparks stay pale blue, under the eyes' white

void add_lit(hal::Area& lit, int x1, int x2, int y) {
    if (lit.x2 < lit.x1) {
        lit = {x1, y, x2, y};
        return;
    }
    lit.x1 = std::min<int32_t>(lit.x1, x1);
    lit.x2 = std::max<int32_t>(lit.x2, x2);
    lit.y1 = std::min<int32_t>(lit.y1, y);
    lit.y2 = std::max<int32_t>(lit.y2, y);
}

bool empty(const hal::Area& a) { return a.x2 < a.x1 || a.y2 < a.y1; }

// Palette stops: luminance -> RGB888. Black, a deep blue halo, a blue body
// that turns to pale cyan in the core, and white for eyes and sparks.
struct Stop {
    int lum, r, g, b;
};
constexpr Stop kStops[] = {
    {0, 0, 0, 0},         {16, 1, 5, 22},       {40, 5, 22, 80},      {70, 14, 52, 160},
    {95, 26, 88, 222},    {116, 38, 118, 250},  {150, 64, 165, 255},  {175, 110, 205, 255},
    {190, 150, 228, 255}, {205, 205, 243, 255}, {225, 240, 252, 255}, {255, 255, 255, 255},
};

}  // namespace

void WispModel::build_palette() {
    // Error drains some of the colour: each stop moves towards its grey.
    const int desat = pal_desat_;
    for (int i = 0; i < 256; ++i) {
        size_t s = 0;
        while (s + 2 < std::size(kStops) && kStops[s + 1].lum < i) ++s;
        const Stop& a = kStops[s];
        const Stop& b = kStops[s + 1];
        const int span = std::max<int32_t>(b.lum - a.lum, 1);
        const int t = std::clamp<int32_t>(i - a.lum, 0, span);
        int r = a.r + (b.r - a.r) * t / span;
        int g = a.g + (b.g - a.g) * t / span;
        int bl = a.b + (b.b - a.b) * t / span;
        const int grey = (r * 77 + g * 150 + bl * 29) >> 8;
        r += (grey - r) * desat / 256;
        g += (grey - g) * desat / 256;
        bl += (grey - bl) * desat / 256;
        uint16_t c = static_cast<uint16_t>(((r >> 3) << 11) | ((g >> 2) << 5) | (bl >> 3));
        if (swapped_) c = static_cast<uint16_t>((c >> 8) | (c << 8));
        palette_[static_cast<size_t>(i)] = c;
    }
}

void WispModel::set_swapped_palette(bool swapped) {
    if (swapped == swapped_) return;
    swapped_ = swapped;
    build_palette();
    first_render_ = true;
}

void WispModel::draw_body(uint8_t* lum, hal::Area& lit) const {
    const int32_t above = std::max<int32_t>(r_ * 7 / 10, 256);  // halo scale over the drop's top
    for (int y = 1; y < h_ - 1; ++y) {
        const int32_t dy = y * 256 + 128 - cy_;
        int32_t yn;
        if (dy >= 0) {
            yn = static_cast<int32_t>(static_cast<int64_t>(dy) * 4096 / r_);
        } else if (-dy <= top_) {
            yn = static_cast<int32_t>(static_cast<int64_t>(dy) * 4096 / top_);
        } else {
            yn = -4096 - static_cast<int32_t>(static_cast<int64_t>(-dy - top_) * 4096 / above);
        }
        if (yn <= -8192 || yn >= 8192) continue;
        const int32_t y2 = yn * yn >> 16;  // Q8
        if (y2 >= kHaloQ8) continue;
        const int32_t rowc = cx_ + sway_at(yn);
        const int32_t rw = std::max<int32_t>(half_width_scale(yn), 64);
        const int32_t inv = (1 << 24) / rw;
        const int32_t xmax = static_cast<int32_t>(isqrt(static_cast<uint32_t>(kHaloQ8 - y2) << 16));
        const int32_t xr = static_cast<int32_t>(static_cast<int64_t>(xmax) * rw >> 12);
        const int x0 = std::max<int32_t>(1, (rowc - xr) >> 8);
        const int x1 = std::min<int32_t>(w_ - 2, (rowc + xr) >> 8);
        uint8_t* row = lum + static_cast<size_t>(y) * w_;
        int first = -1, last = -1;
        for (int x = x0; x <= x1; ++x) {
            const int32_t xn = (x * 256 + 128 - rowc) * inv >> 12;
            const int32_t q = (xn * xn >> 16) + y2;
            if (q >= kHaloQ8) continue;
            const uint8_t v = body_lum_[static_cast<size_t>(q >> 2)];
            if (v == 0) continue;
            row[x] = v;
            if (first < 0) first = x;
            last = x;
        }
        if (first >= 0) add_lit(lit, first, last, y);
    }
}

void WispModel::draw_hair(uint8_t* lum, hal::Area& lit) const {
    static constexpr int32_t kRootHw[3] = {12 * 256, 15 * 256, 12 * 256};
    for (size_t i = 0; i < hair_.size(); ++i) {
        const Hair& h = hair_[i];
        const int32_t len = h.ay - h.y;
        if (len < 2 * 256) continue;
        const int32_t root_hw = px(kRootHw[i]) * r_ / std::max<int32_t>(r_base(), 1);
        const int y0 = std::max<int32_t>(1, h.y >> 8);
        const int y1 = std::min<int32_t>(h_ - 2, h.ay >> 8);
        for (int y = y0; y <= y1; ++y) {
            // t: 0 at the tip, 4096 at the root.
            const int32_t t = std::clamp<int32_t>(
                static_cast<int32_t>(static_cast<int64_t>(y * 256 + 128 - h.y) * 4096 / len), 0,
                4096);
            const int32_t u = 4096 - t;
            // The root stays put; the tip swings: a quadratic bend.
            const int32_t c =
                h.ax + static_cast<int32_t>(static_cast<int64_t>(h.x - h.ax) * (u * u >> 12) >> 12);
            // Full near the root, drawn out to a point: t (2 - t).
            const int32_t hw = std::max<int32_t>(root_hw * (t * (8192 - t) >> 12) >> 12, 96);
            const int32_t inv = (1 << 24) / hw;
            const int32_t xr = hw * 3 / 2;
            // Dimmer at the root than the body's core, so tongue and body merge.
            const int32_t scale = 130 + (90 * t >> 12);
            const int x0 = std::max<int32_t>(1, (c - xr) >> 8);
            const int x1 = std::min<int32_t>(w_ - 2, (c + xr) >> 8);
            uint8_t* row = lum + static_cast<size_t>(y) * w_;
            int first = -1, last = -1;
            for (int x = x0; x <= x1; ++x) {
                const int32_t xn = (x * 256 + 128 - c) * inv >> 12;
                const int32_t q = xn * xn >> 16;
                if (q >= kHaloQ8) continue;
                const int v = body_lum_[static_cast<size_t>(q >> 2)] * scale >> 8;
                if (v == 0) continue;
                if (v > row[x]) row[x] = static_cast<uint8_t>(v);
                if (first < 0) first = x;
                last = x;
            }
            if (first >= 0) add_lit(lit, first, last, y);
        }
    }
}

void WispModel::draw_particles(uint8_t* lum, hal::Area& lit) const {
    const int32_t bright = std::clamp<int32_t>(params_.brightness * dim_ / 256, 0, 256);
    const int32_t orbit = params_.particle_orbit;
    for (const Particle& p : particles_) {
        if (p.life_ms == 0) continue;
        int32_t fade = 256;
        if (p.age_ms < 250) fade = p.age_ms * 256 / 250;
        const int32_t tail = p.life_ms * 35 / 100;
        const int32_t left = p.life_ms - p.age_ms;
        if (left < tail) fade = std::min<int32_t>(fade, left * 256 / std::max<int32_t>(tail, 1));
        // On the far side of the orbit a spark is behind the wisp: dimmer.
        const int32_t depth = 196 + (sin16(p.phase) * 60 >> 15);
        const int32_t f = 256 - ((256 - depth) * orbit >> 8);
        // Orbiting sparks are the show in Thinking: brighter.
        const int32_t a =
            p.glow * fade / 256 * f / 256 * bright / 256 * (256 + orbit * 3 / 4) / 256;
        if (a <= 0) continue;
        const int sx = (p.dx >> 8) - 4;
        const int sy = (p.dy >> 8) - 4;
        if (sx < 1 || sy < 1 || sx + 7 > w_ - 2 || sy + 7 > h_ - 2) continue;
        for (int j = 0; j < 8; ++j) {
            uint8_t* row = lum + static_cast<size_t>(sy + j) * w_ + sx;
            for (int i = 0; i < 8; ++i) {
                const int add = kSpark[static_cast<size_t>(j * 8 + i)] * a >> 8;
                if (add == 0) continue;
                const int v = std::min<int32_t>(row[i] + add, kSparkMax);
                if (v > row[i]) row[i] = static_cast<uint8_t>(v);
            }
        }
        add_lit(lit, sx, sx + 7, sy);
        add_lit(lit, sx, sx + 7, sy + 7);
    }
}

// An anti-aliased chevron centred on (cx, cy), Q8 px: its point towards
// `dir` (+1: ">" pointing right), half width hw and half height hh.
void WispModel::draw_chevron(uint8_t* lum, int32_t cx, int32_t cy, int dir, int32_t hw,
                             int32_t hh) const {
    const int32_t th = std::max<int32_t>(r_ * 5 / 100, 200);  // half the stroke
    const int32_t tip_x = cx + dir * hw;
    const int32_t back_x = cx - dir * hw;
    const int32_t seg[2][4] = {{back_x, cy - hh, tip_x, cy}, {back_x, cy + hh, tip_x, cy}};
    const int y0 = std::max<int32_t>(1, (cy - hh - th) / 256 - 1);
    const int y1 = std::min<int32_t>(h_ - 2, (cy + hh + th) / 256 + 1);
    const int x0 = std::max<int32_t>(1, (cx - hw - th) / 256 - 1);
    const int x1 = std::min<int32_t>(w_ - 2, (cx + hw + th) / 256 + 1);
    for (int y = y0; y <= y1; ++y) {
        uint8_t* row = lum + static_cast<size_t>(y) * w_;
        const int32_t py = y * 256 + 128;
        for (int x = x0; x <= x1; ++x) {
            const int32_t px_ = x * 256 + 128;
            int32_t cov = 0;
            for (const auto& s : seg) {
                const int64_t dx = s[2] - s[0], dy = s[3] - s[1];
                const int64_t vx = px_ - s[0], vy = py - s[1];
                const int64_t len2 = dx * dx + dy * dy;
                int64_t t = len2 > 0 ? (vx * dx + vy * dy) * 4096 / len2 : 0;
                t = std::clamp<int64_t>(t, 0, 4096);
                const int64_t ex = vx - dx * t / 4096, ey = vy - dy * t / 4096;
                const auto d = static_cast<int32_t>(
                    isqrt(static_cast<uint32_t>(std::min<int64_t>(ex * ex + ey * ey, 0xffffffff))));
                cov = std::max<int32_t>(cov, std::clamp<int32_t>(th + 128 - d, 0, 256));
            }
            if (cov == 0) continue;
            row[x] = static_cast<uint8_t>(row[x] + ((255 - row[x]) * cov >> 8));
        }
    }
}

void WispModel::draw_eyes(uint8_t* lum) const {
    // Eye openness through a blink: shut and open again over kBlinkMs.
    int32_t open = 256;
    if (blink_ms_ < kBlinkMs) {
        const int32_t t = static_cast<int32_t>(blink_ms_) * 512 / static_cast<int32_t>(kBlinkMs);
        open = std::max<int32_t>(28, std::abs(256 - t));
    }
    const int32_t rx = std::max<int32_t>(r_ * 15 / 100, 192);
    const int32_t ry_full = r_ * 27 / 100 * params_.eye_open / 256;
    const int32_t ry = std::max<int32_t>(ry_full * open / 256, 128);
    const int32_t gx = (params_.gaze_x + glance_x_) * rx / 256 * 9 / 10;
    const int32_t gy = (params_.gaze_y + glance_y_) * (r_ * 27 / 100) / 256 * 12 / 10;
    const int32_t ey = cy_ - r_ * 14 / 100 + gy;
    const int32_t yn_eye =
        static_cast<int32_t>(static_cast<int64_t>(ey - cy_) * 4096 / (ey < cy_ ? top_ : r_));
    const int32_t rowc = cx_ + sway_at(yn_eye);
    const int32_t sep = r_ * 40 / 100;
    if (err_ >= 128) {
        // Error: "> <", eyes screwed shut.
        for (int side = -1; side <= 1; side += 2) {
            draw_chevron(lum, rowc + side * sep + gx, ey, -side, rx * 11 / 10, r_ * 15 / 100);
        }
        return;
    }
    const int32_t inv_rx = (1 << 24) / rx;
    const int32_t inv_ry = (1 << 24) / ry;
    const int32_t rmin = std::min<int32_t>(rx, ry);
    for (int side = -1; side <= 1; side += 2) {
        const int32_t ex = rowc + side * sep + gx;
        const int y0 = std::max<int32_t>(1, (ey - ry) / 256 - 1);
        const int y1 = std::min<int32_t>(h_ - 2, (ey + ry) / 256 + 1);
        const int x0 = std::max<int32_t>(1, (ex - rx) / 256 - 1);
        const int x1 = std::min<int32_t>(w_ - 2, (ex + rx) / 256 + 1);
        for (int y = y0; y <= y1; ++y) {
            const int32_t yq = (y * 256 + 128 - ey) * inv_ry >> 12;
            const int32_t y2 = yq * yq >> 16;
            if (y2 > 512) continue;
            uint8_t* row = lum + static_cast<size_t>(y) * w_;
            for (int x = x0; x <= x1; ++x) {
                const int32_t xq = (x * 256 + 128 - ex) * inv_rx >> 12;
                const int32_t q = (xq * xq >> 16) + y2;
                // Coverage from the distance to the outline, in pixels:
                // about (q - 1) / 2 * radius.
                const int32_t d = (q - 256) * rmin >> 9;  // Q8 px
                const int32_t cov = std::clamp<int32_t>(128 - d, 0, 256);
                if (cov == 0) continue;
                row[x] = static_cast<uint8_t>(row[x] + ((255 - row[x]) * cov >> 8));
            }
        }
    }
}

void WispModel::draw_mouth(uint8_t* lum) const {
    // Speaking: a small dark mouth that opens with the level.
    if (speak_ <= 0) return;
    const int32_t open = level_ * speak_ / 256;  // Q8
    const int32_t rx = std::max<int32_t>(r_ * 13 / 100, 192);
    const int32_t ry =
        std::max<int32_t>((r_ * 2 / 100 + r_ * 13 / 100 * open / 256) * speak_ / 256, 96);
    const int32_t my = cy_ + r_ * 19 / 100;
    const int32_t mx =
        cx_ + sway_at(static_cast<int32_t>(static_cast<int64_t>(my - cy_) * 4096 / r_));
    const int32_t inv_rx = (1 << 24) / rx;
    const int32_t inv_ry = (1 << 24) / ry;
    const int32_t rmin = std::min<int32_t>(rx, ry);
    const int y0 = std::max<int32_t>(1, (my - ry) / 256 - 1);
    const int y1 = std::min<int32_t>(h_ - 2, (my + ry) / 256 + 1);
    const int x0 = std::max<int32_t>(1, (mx - rx) / 256 - 1);
    const int x1 = std::min<int32_t>(w_ - 2, (mx + rx) / 256 + 1);
    constexpr int kMouthLum = 34;
    for (int y = y0; y <= y1; ++y) {
        const int32_t yq = (y * 256 + 128 - my) * inv_ry >> 12;
        const int32_t y2 = yq * yq >> 16;
        if (y2 > 512) continue;
        uint8_t* row = lum + static_cast<size_t>(y) * w_;
        for (int x = x0; x <= x1; ++x) {
            const int32_t xq = (x * 256 + 128 - mx) * inv_rx >> 12;
            const int32_t q = (xq * xq >> 16) + y2;
            const int32_t d = (q - 256) * rmin >> 9;
            const int32_t cov = std::clamp<int32_t>(128 - d, 0, 256) * speak_ / 256;
            if (cov == 0) continue;
            row[x] = static_cast<uint8_t>(row[x] + ((kMouthLum - row[x]) * cov / 256));
        }
    }
}

hal::Area WispModel::render(std::span<uint8_t> lum, std::span<uint16_t> out) {
    const size_t n = static_cast<size_t>(w_) * static_cast<size_t>(h_);
    if (lum.size() < n || out.size() < n) return {0, 0, -1, -1};

    // Brightness with a faint burning flicker.
    int32_t b = params_.brightness * dim_ / 256 + (sin16(phase_[2] * 3 + phase_[0]) * 5 >> 15);
    b = std::clamp<int32_t>(b, 0, 256);
    for (size_t i = 0; i < body_lum_.size(); ++i) {
        body_lum_[i] = static_cast<uint8_t>(kBodyLum[i] * b >> 8);
    }

    // Every lit pixel is in this frame's dirty box, so a new palette needs
    // no full redraw.
    const int32_t desat = err_ * 96 / 256 / 8 * 8;
    if (desat != pal_desat_) {
        pal_desat_ = desat;
        build_palette();
    }

    const hal::Area prev = bounds_;
    if (first_render_) {
        std::memset(lum.data(), 0, n);
    } else if (!empty(prev)) {
        for (int y = prev.y1; y <= prev.y2; ++y) {
            std::memset(lum.data() + static_cast<size_t>(y) * w_ + prev.x1, 0,
                        static_cast<size_t>(prev.x2 - prev.x1 + 1));
        }
    }

    hal::Area lit{0, 0, -1, -1};
    draw_body(lum.data(), lit);
    draw_hair(lum.data(), lit);
    draw_particles(lum.data(), lit);
    draw_eyes(lum.data());
    draw_mouth(lum.data());
    bounds_ = lit;

    hal::Area dirty;
    if (first_render_) {
        dirty = {0, 0, w_ - 1, h_ - 1};
    } else if (empty(prev)) {
        dirty = lit;
    } else if (empty(lit)) {
        dirty = prev;
    } else {
        dirty = {std::min<int32_t>(prev.x1, lit.x1), std::min<int32_t>(prev.y1, lit.y1),
                 std::max<int32_t>(prev.x2, lit.x2), std::max<int32_t>(prev.y2, lit.y2)};
    }
    first_render_ = false;
    if (empty(dirty)) return dirty;

    for (int y = dirty.y1; y <= dirty.y2; ++y) {
        const uint8_t* src = lum.data() + static_cast<size_t>(y) * w_;
        uint16_t* dst = out.data() + static_cast<size_t>(y) * w_;
        for (int x = dirty.x1; x <= dirty.x2; ++x) dst[x] = palette_[src[x]];
    }
    return dirty;
}

}  // namespace wisp::ui
