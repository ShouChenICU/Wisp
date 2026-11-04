#pragma once

// What SceneView's sources share (scene_view.cpp, scene_view_list.cpp,
// scene_view_canvas.cpp, scene_view_media.cpp): the per-node state and the
// prop value helpers. Private to wisp_scene.

#include <algorithm>
#include <cinttypes>
#include <cmath>
#include <cstdint>
#include <optional>
#include <string_view>
#include <variant>

#include "list_window.hpp"
#include "lvgl.h"
#include "widget_data.hpp"
#include "wisp/os/large.hpp"
#include "wisp/os/os.hpp"
#include "wisp/scene/scene_view.hpp"
#include "wisp/scene/schema.hpp"

namespace wisp::scene::view {

inline constexpr const char* kTag = "scene";

// Numbers further out than this are clamped before any arithmetic, so
// differences of two of them stay finite.
inline constexpr double kNumberLimit = 1e15;

enum class SizeKind : uint8_t { Unset, Px, Fill, Content };

struct Size {
    SizeKind kind = SizeKind::Unset;
    int32_t px = 0;
};

enum class Flow : uint8_t { None, Column, Row };

// A list row's height when the scene gives none: a comfortable touch
// target on the panels Wisp runs on.
inline constexpr int32_t kDefaultRowH = 48;

// What set_prop() leaves for SceneView::update() to redo.
inline constexpr uint32_t kSizeDerived = 1;
inline constexpr uint32_t kPadDerived = 2;
inline constexpr uint32_t kAlignDerived = 4;
inline constexpr uint32_t kRangeDerived = 8;
inline constexpr uint32_t kListDerived = 16;
inline constexpr uint32_t kGridDerived = 32;
inline constexpr uint32_t kAllDerived = 63;

inline const os::LargeString* as_string(const PropValue& v) {
    return std::get_if<os::LargeString>(&v);
}

inline std::optional<bool> as_bool(const PropValue& v) {
    if (const auto* b = std::get_if<bool>(&v)) return *b;
    return std::nullopt;
}

inline double finite(double d) {
    if (std::isnan(d)) return 0;
    return std::clamp(d, -kNumberLimit, kNumberLimit);
}

// A Number prop: an integer or a float, NaN as 0, clamped to
// +-kNumberLimit.
inline std::optional<double> as_number(const PropValue& v) {
    if (const auto* i = std::get_if<int64_t>(&v)) return finite(static_cast<double>(*i));
    if (const auto* d = std::get_if<double>(&v)) return finite(*d);
    return std::nullopt;
}

// An integer prop held to `p`'s range: floats are rounded, NaN is 0.
inline std::optional<int32_t> as_int(const PropValue& v, Prop p) {
    IntRange r = prop_range(p);
    if (const auto* i = std::get_if<int64_t>(&v))
        return static_cast<int32_t>(std::clamp<int64_t>(*i, r.min, r.max));
    if (const auto* d = std::get_if<double>(&v)) {
        double x = std::isnan(*d) ? 0.0 : std::round(*d);
        return static_cast<int32_t>(
            std::clamp(x, static_cast<double>(r.min), static_cast<double>(r.max)));
    }
    return std::nullopt;
}

// A buffer from os::alloc(), freed with the owner.
struct LargeMem {
    void* p = nullptr;
    LargeMem() = default;
    LargeMem(const LargeMem&) = delete;
    LargeMem& operator=(const LargeMem&) = delete;
    LargeMem(LargeMem&& o) noexcept : p(o.p) { o.p = nullptr; }
    LargeMem& operator=(LargeMem&& o) noexcept {
        std::swap(p, o.p);
        return *this;
    }
    ~LargeMem() { os::dealloc(p); }
};

}  // namespace wisp::scene::view

namespace wisp::scene {

// One per node, at a fixed address: the node's object carries it as its
// LVGL user data and its event callbacks' user data. Destroyed only after
// its object is gone, so the buffers below outlive every draw of them.
struct SceneView::Node {
    Node() = default;
    Node(const Node&) = delete;
    Node& operator=(const Node&) = delete;
    // Gives its pixel memory back to the view's budget.
    ~Node() {
        if (view != nullptr) view->media_bytes_ -= canvas_bytes + image_bytes;
    }

    // One built row of a list.
    struct Row {
        uint32_t index = 0;
        os::LargeVector<NodeId> ids;  // ids[0] is the row's root
    };

    SceneView* view = nullptr;
    NodeId id = 0;
    NodeId parent = 0;
    Widget widget = Widget::Screen;
    uint32_t events = 0;
    lv_obj_t* obj = nullptr;
    lv_obj_t* text = nullptr;            // a button's label
    lv_obj_t* filler = nullptr;          // what makes a list's content tall
    view::Flow flow = view::Flow::None;  // how this node lays out its children
    view::Size w, h;
    int32_t grow = -1;  // -1: not given
    bool pad_given = false;
    bool pad_safe = false;
    int32_t pad = 0;
    lv_flex_align_t align = LV_FLEX_ALIGN_START;
    lv_flex_align_t justify = LV_FLEX_ALIGN_START;
    double value = 0, min = 0, max = 100;
    int32_t row_h = view::kDefaultRowH;
    uint32_t rows = 0;
    int32_t cols = 1;
    os::LargeVector<int32_t> grid;  // column templates, then row templates
    bool long_pressed = false;

    // A list: the window last asked for and the rows built in it.
    ListWindow window;
    os::LargeVector<Row> built;
    bool resetting = false;  // inside apply_list_reset(): scroll events wait
    // The root of a list row: its list and index.
    NodeId row_of = 0;
    uint32_t row_index = 0;

    // A canvas: the last commands and the buffer they were drawn into.
    os::LargeVector<uint8_t> draw;
    lv_draw_buf_t canvas_buf{};
    view::LargeMem canvas_mem;
    size_t canvas_bytes = 0;      // of canvas_mem, in the view's budget
    bool canvas_drawn = false;    // draw_canvas() ran since apply_canvas() began
    bool canvas_refused = false;  // too large, and said so

    // A chart.
    SeriesData series;
    bool bar = false;

    // An image: the decoded pixels it shows.
    lv_image_dsc_t image{};
    view::LargeMem image_mem;
    size_t image_bytes = 0;  // of image_mem, in the view's budget
};

}  // namespace wisp::scene
