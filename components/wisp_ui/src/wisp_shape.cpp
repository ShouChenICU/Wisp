// WispModel's shape (wisp/ui/wisp_model.hpp): the outline, the face and
// the sparks for the current state, in Q8 pixels.

#include <algorithm>
#include <cstdlib>

#include "wisp/ui/wisp_model.hpp"
#include "wisp_tables.hpp"

namespace wisp::ui {

using wisp_detail::cos16;
using wisp_detail::sin16;

namespace {

constexpr uint32_t kBlinkMs = 170;
// Where the flame tips leave the body: this far round from the top, each
// side (1/65536 turn).
constexpr uint32_t kRootAngle = 8900;
constexpr int kBodyPoints = 64;
constexpr int kEdgePoints = 8;  // per crown edge

// Quadratic Bezier p0-c-p1 at t / kEdgePoints.
WispPoint bezier(WispPoint p0, WispPoint c, WispPoint p1, int t) {
    const int64_t u = kEdgePoints - t;
    const int64_t n = static_cast<int64_t>(kEdgePoints) * kEdgePoints;
    return {static_cast<int32_t>((u * u * p0.x + 2 * u * t * c.x + int64_t{t} * t * p1.x) / n),
            static_cast<int32_t>((u * u * p0.y + 2 * u * t * c.y + int64_t{t} * t * p1.y) / n)};
}

// A capsule for a filled ellipse-ish blob of half width rx and half height
// ry centred on c: a stadium along its longer axis.
WispShape::Capsule blob(WispPoint c, int32_t rx, int32_t ry) {
    if (ry >= rx) return {{c.x, c.y - (ry - rx)}, {c.x, c.y + (ry - rx)}, rx};
    return {{c.x - (rx - ry), c.y}, {c.x + (rx - ry), c.y}, ry};
}

void grow(hal::Area& box, int32_t x1, int32_t y1, int32_t x2, int32_t y2) {
    // Q8 extent to whole pixels.
    const int32_t px1 = x1 >> 8, py1 = y1 >> 8, px2 = (x2 + 255) >> 8, py2 = (y2 + 255) >> 8;
    if (box.x2 < box.x1) {
        box = {px1, py1, px2, py2};
        return;
    }
    box.x1 = std::min<int32_t>(box.x1, px1);
    box.y1 = std::min<int32_t>(box.y1, py1);
    box.x2 = std::max<int32_t>(box.x2, px2);
    box.y2 = std::max<int32_t>(box.y2, py2);
}

void grow(hal::Area& box, const WispShape::Capsule& c, int32_t pad) {
    const int32_t r = c.r + pad;
    grow(box, std::min<int32_t>(c.a.x, c.b.x) - r, std::min<int32_t>(c.a.y, c.b.y) - r,
         std::max<int32_t>(c.a.x, c.b.x) + r, std::max<int32_t>(c.a.y, c.b.y) + r);
}

}  // namespace

WispPoint WispModel::body_point(uint32_t a) const {
    // On the unit circle in the drop's normalized space, then out to the
    // drop: the top half stretched up to top_ and narrowed by
    // half_width_scale(), the whole swayed sideways by height.
    const int32_t xs = sin16(a);   // Q15
    const int32_t ys = -cos16(a);  // Q15, negative is up
    const int32_t yn = ys >> 3;    // Q12
    const int32_t dy = static_cast<int32_t>(static_cast<int64_t>(yn) * (yn < 0 ? top_ : r_) / 4096);
    const int32_t dx = static_cast<int32_t>(static_cast<int64_t>(xs) * half_width_scale(yn) >> 15);
    return {cx_ + dx + sway_at(yn), cy_ + dy};
}

void WispModel::build_outline() {
    WispShape& s = shape_;
    int n = 0;
    // The body from the right root clockwise round the bottom to the left
    // root.
    const uint32_t span = 65536u - 2 * kRootAngle;
    for (int k = 0; k < kBodyPoints; ++k) {
        const uint32_t a =
            kRootAngle + static_cast<uint32_t>(static_cast<uint64_t>(span) * k / (kBodyPoints - 1));
        s.outline[static_cast<size_t>(n++)] = body_point(a);
    }
    const WispPoint root_l = s.outline[static_cast<size_t>(n - 1)];
    const WispPoint root_r = s.outline[0];
    const WispPoint tip_l{hair_[0].x, hair_[0].y};
    const WispPoint tip_c{hair_[1].x, hair_[1].y};
    const WispPoint tip_r{hair_[2].x, hair_[2].y};
    // The valleys between the tips: a little below the lower of the two
    // tips' roots, between them.
    const int32_t valley_drop = r_ * 22 / 100;
    const WispPoint valley_1{
        (hair_[0].ax + hair_[1].ax) / 2,
        std::max<int32_t>(hair_[0].ay, hair_[1].ay) - r_ * 16 / 100 + valley_drop / 4};
    const WispPoint valley_2{
        (hair_[1].ax + hair_[2].ax) / 2,
        std::max<int32_t>(hair_[1].ay, hair_[2].ay) - r_ * 16 / 100 + valley_drop / 4};

    // Each edge is a quadratic from a base to a tip that leaves the base
    // going up (the control point above it), so every tip is a convex
    // tongue with a sharp point; out of a root the edge instead carries on
    // along the body, so the side tips grow out of it without a kink.
    struct Edge {
        WispPoint from, to;
        bool rising;  // from a base up to a tip
    };
    const Edge edges[6] = {{root_l, tip_l, true},   {tip_l, valley_1, false},
                           {valley_1, tip_c, true}, {tip_c, valley_2, false},
                           {valley_2, tip_r, true}, {tip_r, root_r, false}};
    for (int e = 0; e < 6; ++e) {
        const Edge& ed = edges[e];
        const WispPoint base = ed.rising ? ed.from : ed.to;
        const WispPoint tip = ed.rising ? ed.to : ed.from;
        WispPoint c{base.x + (tip.x - base.x) / 6, base.y + (tip.y - base.y) * 6 / 10};
        if (e == 0 || e == 5) {
            const WispPoint prev = s.outline[e == 0 ? kBodyPoints - 2 : 1];
            const int64_t tx = base.x - prev.x, ty = base.y - prev.y;
            const auto seg = static_cast<int64_t>(
                wisp_detail::isqrt64(static_cast<uint64_t>(tx * tx + ty * ty)));
            const int64_t ex = tip.x - base.x, ey = tip.y - base.y;
            const auto reach = static_cast<int64_t>(
                wisp_detail::isqrt64(static_cast<uint64_t>(ex * ex + ey * ey)));
            if (seg > 0) {
                c = {static_cast<int32_t>(base.x + tx * reach / 2 / seg),
                     static_cast<int32_t>(base.y + ty * reach / 2 / seg)};
            }
        }
        const int last = e == 5 ? kEdgePoints - 1 : kEdgePoints;  // the right root is point 0
        for (int t = 1; t <= last; ++t) {
            s.outline[static_cast<size_t>(n++)] = bezier(ed.from, c, ed.to, t);
        }
    }
    s.outline_n = n;
}

void WispModel::build_face() {
    WispShape& s = shape_;
    // Eye openness through a blink: shut and open again over kBlinkMs.
    int32_t open = 256;
    if (blink_ms_ < kBlinkMs) {
        const int32_t t = static_cast<int32_t>(blink_ms_) * 512 / static_cast<int32_t>(kBlinkMs);
        open = std::abs(256 - t);
    }
    // Speaking has no mouth: the eyes squash with the level, riding the
    // same spring as the body's bounce, so they spring back (and overshoot
    // a little) when the level drops.
    const int32_t squash = bounce_ * 2;  // Q12
    const int32_t half_stroke = s.stroke / 2;
    const int32_t rx = std::max<int32_t>(
        static_cast<int32_t>(static_cast<int64_t>(r_ * 19 / 100) * (4096 + squash / 4) / 4096),
        half_stroke);
    const int32_t ry_nominal = r_ * 34 / 100;
    const int32_t ry =
        std::max<int32_t>(static_cast<int32_t>(static_cast<int64_t>(ry_nominal * params_.eye_open /
                                                                    256 * open / 256) *
                                               (4096 - squash) / 4096),
                          half_stroke);
    const int32_t gx = (params_.gaze_x + glance_x_) * rx / 256 * 9 / 10;
    const int32_t gy = (params_.gaze_y + glance_y_) * ry_nominal / 256;
    const int32_t ey = cy_ - r_ * 10 / 100 + gy;
    const int32_t yn_eye =
        static_cast<int32_t>(static_cast<int64_t>(ey - cy_) * 4096 / (ey < cy_ ? top_ : r_));
    const int32_t rowc = cx_ + sway_at(yn_eye);
    const int32_t sep = r_ * 42 / 100;

    s.chevrons = err_ >= 128;
    for (int side = 0; side < 2; ++side) {
        const int32_t ex = rowc + (side == 0 ? -sep : sep) + gx;
        s.eyes[static_cast<size_t>(side)] = blob({ex, ey}, rx, ry);
        // "> <": each points at the middle.
        const int32_t hw = r_ * 15 / 100, hh = r_ * 16 / 100;
        const int32_t dir = side == 0 ? 1 : -1;
        const int32_t cey = cy_ - r_ * 8 / 100;
        const int32_t cex = rowc + (side == 0 ? -sep : sep);
        s.chevron[static_cast<size_t>(side)] = {WispPoint{cex - dir * hw, cey - hh},
                                                WispPoint{cex + dir * hw, cey},
                                                WispPoint{cex - dir * hw, cey + hh}};
    }
}

void WispModel::build_sparks() {
    WispShape& s = shape_;
    s.spark_n = 0;
    if (!sparks_on_) return;
    const int32_t r_full = std::max<int32_t>(s.stroke * 55 / 100, 384);
    for (const Particle& p : particles_) {
        if (p.life_ms == 0 || s.spark_n >= WispShape::kMaxSparks) continue;
        // Grow in, shrink out.
        int32_t fade = 256;
        if (p.age_ms < 250) fade = p.age_ms * 256 / 250;
        const int32_t tail = p.life_ms * 35 / 100;
        const int32_t left = p.life_ms - p.age_ms;
        if (left < tail) fade = std::min<int32_t>(fade, left * 256 / std::max<int32_t>(tail, 1));
        const int32_t r = r_full * fade / 256;
        if (r < 160) continue;
        s.sparks[static_cast<size_t>(s.spark_n++)] = {{p.dx, p.dy}, {p.dx, p.dy}, r};
    }
}

hal::Area WispModel::build() {
    WispShape& s = shape_;
    const hal::Area prev = s.bbox;
    s.stroke = std::max<int32_t>(h_ * 256 / 40, 2 * 256);
    s.opacity = static_cast<uint8_t>(std::clamp<int32_t>(params_.brightness, 0, 255));
    build_outline();
    build_face();
    build_sparks();

    // One pixel of room around everything for anti-aliasing.
    constexpr int32_t kPad = 256;
    hal::Area box{0, 0, -1, -1};
    for (int i = 0; i < s.outline_n; ++i) {
        const WispPoint& p = s.outline[static_cast<size_t>(i)];
        grow(box, WispShape::Capsule{p, p, s.stroke / 2}, kPad);
    }
    if (s.chevrons) {
        for (const auto& ch : s.chevron) {
            for (const WispPoint& p : ch) grow(box, WispShape::Capsule{p, p, s.stroke / 2}, kPad);
        }
    } else {
        for (const auto& e : s.eyes) grow(box, e, kPad);
    }
    for (int i = 0; i < s.spark_n; ++i) grow(box, s.sparks[static_cast<size_t>(i)], kPad);
    box.x1 = std::max<int32_t>(box.x1, 0);
    box.y1 = std::max<int32_t>(box.y1, 0);
    box.x2 = std::min<int32_t>(box.x2, w_ - 1);
    box.y2 = std::min<int32_t>(box.y2, h_ - 1);
    s.bbox = box;

    hal::Area dirty;
    if (first_build_ || prev.x2 < prev.x1) {
        dirty = {0, 0, w_ - 1, h_ - 1};
    } else {
        dirty = {std::min<int32_t>(prev.x1, box.x1), std::min<int32_t>(prev.y1, box.y1),
                 std::max<int32_t>(prev.x2, box.x2), std::max<int32_t>(prev.y2, box.y2)};
    }
    first_build_ = false;
    return dirty;
}

uint32_t shape_hash(const WispShape& s) {
    uint32_t h = 2166136261u;
    auto mix = [&h](int32_t v) {
        const auto u = static_cast<uint32_t>(v);
        for (int i = 0; i < 4; ++i) h = (h ^ ((u >> (8 * i)) & 0xffu)) * 16777619u;
    };
    auto mix_capsule = [&mix](const WispShape::Capsule& c) {
        mix(c.a.x);
        mix(c.a.y);
        mix(c.b.x);
        mix(c.b.y);
        mix(c.r);
    };
    mix(s.outline_n);
    for (int i = 0; i < s.outline_n; ++i) {
        mix(s.outline[static_cast<size_t>(i)].x);
        mix(s.outline[static_cast<size_t>(i)].y);
    }
    mix(s.stroke);
    mix(s.chevrons ? 1 : 0);
    if (s.chevrons) {
        for (const auto& ch : s.chevron) {
            for (const WispPoint& p : ch) {
                mix(p.x);
                mix(p.y);
            }
        }
    } else {
        for (const auto& e : s.eyes) mix_capsule(e);
    }
    mix(s.spark_n);
    for (int i = 0; i < s.spark_n; ++i) mix_capsule(s.sparks[static_cast<size_t>(i)]);
    mix(s.bbox.x1);
    mix(s.bbox.y1);
    mix(s.bbox.x2);
    mix(s.bbox.y2);
    mix(s.opacity);
    return h;
}

}  // namespace wisp::ui
