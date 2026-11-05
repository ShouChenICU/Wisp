#include "wisp/hal/display_geometry.hpp"

#include <algorithm>
#include <cmath>

namespace wisp::hal {

namespace {

// Beyond the exact visible boundary, safe_rect()'s RoundedRect/Circle
// corners keep this much extra straight-line room (see its header
// comment): panel corners are measured by eye against a few candidate
// radii (docs/porting-guide.md), not read off to the pixel, so content
// flush against the exact boundary risks clipping on the real part.
constexpr int32_t kSafeCornerMargin = 4;

// std::map::find()-based accessor for an int-valued caps key -- Properties
// is wisp::Map (std::map<std::string, Value>), not a Value itself, so this
// is a plain map lookup rather than Value::find().
const Value* find_cap(const Properties& caps, const char* key) {
    auto it = caps.find(key);
    return it == caps.end() ? nullptr : &it->second;
}

}  // namespace

Result<ScreenShape> screen_shape_from_caps(const Properties& caps) {
    const Value* width = find_cap(caps, "width");
    const Value* height = find_cap(caps, "height");
    if (width == nullptr || width->as_int() <= 0 || height == nullptr || height->as_int() <= 0) {
        return Result<ScreenShape>::err(ErrorCode::InvalidArgument,
                                        "display caps: width/height missing or not positive");
    }

    ScreenShape shape;
    shape.width = static_cast<int32_t>(width->as_int());
    shape.height = static_cast<int32_t>(height->as_int());

    const Value* shape_cap = find_cap(caps, "shape");
    std::string shape_str = shape_cap == nullptr ? std::string("rect") : shape_cap->as_string();

    if (shape_str == "rect") {
        shape.kind = ScreenShapeKind::Rect;
    } else if (shape_str == "circle") {
        shape.kind = ScreenShapeKind::Circle;
    } else if (shape_str == "rounded_rect") {
        shape.kind = ScreenShapeKind::RoundedRect;
        const Value* radius = find_cap(caps, "corner_radius");
        if (radius == nullptr || radius->as_int() < 0) {
            return Result<ScreenShape>::err(
                ErrorCode::InvalidArgument,
                "display caps: rounded_rect requires a non-negative corner_radius");
        }
        int32_t max_radius = std::min(shape.width, shape.height) / 2;
        shape.corner_radius = static_cast<int32_t>(std::min<int64_t>(radius->as_int(), max_radius));
    } else {
        return Result<ScreenShape>::err(ErrorCode::InvalidArgument,
                                        "display caps: unknown shape \"" + shape_str + "\"");
    }

    return Result<ScreenShape>::ok(shape);
}

bool visible(const ScreenShape& shape, int32_t x, int32_t y) {
    if (x < 0 || x >= shape.width || y < 0 || y >= shape.height) {
        return false;
    }

    switch (shape.kind) {
        case ScreenShapeKind::Rect:
            return true;

        case ScreenShapeKind::Circle: {
            double cx = shape.width / 2.0;
            double cy = shape.height / 2.0;
            double radius = std::min(shape.width, shape.height) / 2.0;
            double dx = (x + 0.5) - cx;
            double dy = (y + 0.5) - cy;
            return dx * dx + dy * dy <= radius * radius;
        }

        case ScreenShapeKind::RoundedRect: {
            int32_t r = shape.corner_radius;
            bool in_left = x < r;
            bool in_right = x >= shape.width - r;
            bool in_top = y < r;
            bool in_bottom = y >= shape.height - r;
            if (!((in_left || in_right) && (in_top || in_bottom))) {
                // Not inside any of the four r x r corner squares -- the
                // ordinary (unclipped) part of the rectangle.
                return true;
            }
            double cx = in_left ? r : shape.width - r;
            double cy = in_top ? r : shape.height - r;
            double dx = (x + 0.5) - cx;
            double dy = (y + 0.5) - cy;
            return dx * dx + dy * dy <= static_cast<double>(r) * r;
        }
    }
    return false;
}

Area safe_rect(const ScreenShape& shape) {
    switch (shape.kind) {
        case ScreenShapeKind::Rect:
            return Area{0, 0, shape.width - 1, shape.height - 1};

        case ScreenShapeKind::RoundedRect: {
            double r = shape.corner_radius;
            double inner_radius = r - kSafeCornerMargin;
            int32_t margin = static_cast<int32_t>(std::ceil(r - inner_radius / std::sqrt(2.0)));
            return Area{margin, margin, shape.width - 1 - margin, shape.height - 1 - margin};
        }

        case ScreenShapeKind::Circle: {
            double radius = std::min(shape.width, shape.height) / 2.0;
            double inner_radius = radius - kSafeCornerMargin;
            int32_t margin_x =
                static_cast<int32_t>(std::ceil(shape.width / 2.0 - inner_radius / std::sqrt(2.0)));
            int32_t margin_y =
                static_cast<int32_t>(std::ceil(shape.height / 2.0 - inner_radius / std::sqrt(2.0)));
            return Area{margin_x, margin_y, shape.width - 1 - margin_x,
                        shape.height - 1 - margin_y};
        }
    }
    return Area{0, 0, shape.width - 1, shape.height - 1};
}

Span row_span(const ScreenShape& shape, int32_t y) {
    if (y < 0 || y >= shape.height) {
        return Span{};
    }
    if (shape.kind == ScreenShapeKind::Rect) {
        return Span{0, shape.width - 1};
    }

    // RoundedRect and Circle are both convex, so each row's visible
    // pixels form a single contiguous range (if any): scanning from both
    // ends for the first visible() pixel is simplest, and -- by
    // construction -- always exactly consistent with visible() itself
    // (row_span() is only used for one-time layout, not a per-frame hot
    // path, so the O(width) scan costs nothing that matters).
    int32_t x1 = -1;
    for (int32_t x = 0; x < shape.width; ++x) {
        if (visible(shape, x, y)) {
            x1 = x;
            break;
        }
    }
    if (x1 < 0) {
        return Span{};
    }
    int32_t x2 = x1;
    for (int32_t x = shape.width - 1; x > x2; --x) {
        if (visible(shape, x, y)) {
            x2 = x;
            break;
        }
    }
    return Span{x1, x2};
}

}  // namespace wisp::hal
