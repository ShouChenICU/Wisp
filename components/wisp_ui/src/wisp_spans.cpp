// for_each_span() (wisp/ui/wisp_model.hpp): the shape without
// anti-aliasing, as runs of whole pixels. Everything the wisp draws is a
// capsule (stroke segments with round ends, the eyes, the mouth, sparks),
// and a capsule is convex, so each one covers a single run of each row:
// the row's crossing of the segment's side band, and of its two end discs.
// A pixel is in when its centre is.

#include <algorithm>

#include "wisp/ui/wisp_model.hpp"
#include "wisp_tables.hpp"

namespace wisp::ui {

using wisp_detail::isqrt64;

namespace {

struct Interval {
    int64_t lo, hi;  // Q8 x
};

constexpr int kMaxIntervals = 48;

struct Row {
    Interval iv[kMaxIntervals];
    int n = 0;

    // Sorts and merges overlapping intervals in place.
    void compact() {
        std::sort(iv, iv + n, [](const Interval& a, const Interval& b) { return a.lo < b.lo; });
        int m = 0;
        for (int i = 0; i < n; ++i) {
            if (m > 0 && iv[i].lo <= iv[m - 1].hi) {
                iv[m - 1].hi = std::max(iv[m - 1].hi, iv[i].hi);
            } else {
                iv[m++] = iv[i];
            }
        }
        n = m;
    }

    void add(int64_t lo, int64_t hi) {
        if (n == kMaxIntervals) compact();
        if (n == kMaxIntervals) {
            // Still full: widen the nearest one rather than drop coverage.
            iv[n - 1].lo = std::min(iv[n - 1].lo, lo);
            iv[n - 1].hi = std::max(iv[n - 1].hi, hi);
            return;
        }
        iv[n++] = {lo, hi};
    }
};

// The capsule's run on the row whose pixel centres are at y (Q8), if any.
bool capsule_row(const WispShape::Capsule& c, int64_t y, int64_t& lo, int64_t& hi) {
    const int64_t r = c.r;
    const int64_t ax = c.a.x, ay = c.a.y, bx = c.b.x, by = c.b.y;
    if (y < std::min(ay, by) - r || y > std::max(ay, by) + r) return false;
    bool any = false;
    auto take = [&](int64_t x) {
        if (!any) {
            lo = hi = x;
            any = true;
        } else {
            lo = std::min(lo, x);
            hi = std::max(hi, x);
        }
    };
    // The end discs.
    for (int e = 0; e < 2; ++e) {
        const int64_t px = e == 0 ? ax : bx, py = e == 0 ? ay : by;
        const int64_t dy = y - py;
        if (dy < -r || dy > r) continue;
        const auto w = static_cast<int64_t>(isqrt64(static_cast<uint64_t>(r * r - dy * dy)));
        take(px - w);
        take(px + w);
    }
    // The side band: the quad between the segment moved r to either side.
    const int64_t dx = bx - ax, dyy = by - ay;
    const auto len = static_cast<int64_t>(isqrt64(static_cast<uint64_t>(dx * dx + dyy * dyy)));
    if (len > 0) {
        const int64_t nx = -dyy * r / len, ny = dx * r / len;
        const int64_t qx[4] = {ax + nx, bx + nx, bx - nx, ax - nx};
        const int64_t qy[4] = {ay + ny, by + ny, by - ny, ay - ny};
        for (int i = 0; i < 4; ++i) {
            const int j = (i + 1) & 3;
            const int64_t y0 = qy[i], y1 = qy[j];
            if (y0 == y1) {
                if (y == y0) {
                    take(qx[i]);
                    take(qx[j]);
                }
                continue;
            }
            if ((y < y0 && y < y1) || (y > y0 && y > y1)) continue;
            take(qx[i] + (qx[j] - qx[i]) * (y - y0) / (y1 - y0));
        }
    }
    return any;
}

// The pixels whose centres lie in [lo, hi] (Q8): [x1, x2].
void to_pixels(int64_t lo, int64_t hi, int64_t& x1, int64_t& x2) {
    // ceil((lo - 128) / 256) and floor((hi - 128) / 256), for any sign.
    const int64_t a = lo - 128, b = hi - 128;
    x1 = a >= 0 ? (a + 255) / 256 : -((-a) / 256);
    x2 = b >= 0 ? b / 256 : -((-b + 255) / 256);
}

}  // namespace

void for_each_span(const WispShape& s, void* ctx, void (*fn)(void* ctx, int y, int x1, int x2)) {
    for_each_span(s, s.bbox.y1, s.bbox.y2, ctx, fn);
}

void for_each_span(const WispShape& s, int y1, int y2, void* ctx,
                   void (*fn)(void* ctx, int y, int x1, int x2)) {
    const int32_t hs = s.stroke / 2;
    auto each_capsule = [&](auto&& visit) {
        for (int i = 0; i < s.outline_n; ++i) {
            const WispPoint& a = s.outline[static_cast<size_t>(i)];
            const WispPoint& b = s.outline[static_cast<size_t>((i + 1) % s.outline_n)];
            visit(WispShape::Capsule{a, b, hs});
        }
        if (s.chevrons) {
            for (const auto& ch : s.chevron) {
                visit(WispShape::Capsule{ch[0], ch[1], hs});
                visit(WispShape::Capsule{ch[1], ch[2], hs});
            }
        } else {
            for (const auto& e : s.eyes) {
                if (e.r > 0) visit(e);
            }
        }
        if (s.mouth) visit(s.mouth_shape);
        for (int i = 0; i < s.spark_n; ++i) visit(s.sparks[static_cast<size_t>(i)]);
    };

    Row row;
    for (int y = std::max<int>(y1, s.bbox.y1); y <= std::min<int>(y2, s.bbox.y2); ++y) {
        const int64_t yc = int64_t{y} * 256 + 128;
        row.n = 0;
        each_capsule([&](const WispShape::Capsule& c) {
            int64_t lo = 0, hi = 0;
            if (capsule_row(c, yc, lo, hi)) row.add(lo, hi);
        });
        if (row.n == 0) continue;
        row.compact();
        int64_t run_x1 = 0, run_x2 = -2;
        bool have = false;
        for (int i = 0; i < row.n; ++i) {
            int64_t x1 = 0, x2 = 0;
            to_pixels(row.iv[i].lo, row.iv[i].hi, x1, x2);
            x1 = std::max<int64_t>(x1, s.bbox.x1);
            x2 = std::min<int64_t>(x2, s.bbox.x2);
            if (x1 > x2) continue;
            // Runs that touch after rounding become one.
            if (have && x1 <= run_x2 + 1) {
                run_x2 = std::max(run_x2, x2);
                continue;
            }
            if (have) fn(ctx, y, static_cast<int>(run_x1), static_cast<int>(run_x2));
            run_x1 = x1;
            run_x2 = x2;
            have = true;
        }
        if (have) fn(ctx, y, static_cast<int>(run_x1), static_cast<int>(run_x2));
    }
}

}  // namespace wisp::ui
