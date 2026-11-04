#pragma once

// SceneView: the LVGL side of a scene (scene-runtime-core spec 3.1,
// 4.3-4.4). It turns the UiOps the scene task sends (ui_ops.hpp) into
// widgets on its own LVGL screen and reports what the user does to them.
// UI thread only, like every lv_* call (AGENTS.md section 3).
//
// A Build replaces the whole tree. It is checked before anything is
// touched: the root must be a ui.screen with parent 0, every other node a
// known widget under a node built before it that takes children, ids
// unique and non-zero, at most kMaxNodes; a Build that fails any of this
// is dropped whole and the tree stays as it was. A SetProp for a node
// that is gone, a prop the widget does not have, or a value of the wrong
// type is dropped (logged at debug level). Numbers are clamped before
// they reach LVGL: NaN and infinities never get converted, sizes and the
// other integer props are held to the schema's prop_range().
//
// Layout:
//   - The root fills the screen. Its padding is at least the distance
//     from each edge of the screen to hal::safe_rect() (a larger pad is
//     kept, a smaller one or "safe" raised to it), so content always
//     starts inside the safe area (AGENTS.md section 3).
//   - pad = "safe" on any other node adds no padding: the root's padding
//     already keeps everything inside it in the safe area.
//   - screen, column, scroll (direction "v") and list stack children
//     vertically, row and scroll "h" horizontally (LVGL flex). `justify`
//     places children along that direction, `align` across it.
//   - stack lays children over each other; `align` places each one on
//     both axes (start: top left, end: bottom right).
//   - grid fills `cols` equal columns row by row (LVGL grid); `align`
//     places each child in its cell.
//   - w/h "fill" grows along the parent's direction and takes the full
//     extent across it (and stretches across a grid cell). Layout
//     widgets default to "content", scroll and list to "fill", spacer to
//     0 with grow 1.
//
// Numbers for slider, bar and arc: LVGL takes int32 and multiplies a
// value by pixel counts in int32, so a range of whole numbers within
// +-kDirectLimit is passed as it is and anything else (fractions, huge
// ranges) is mapped onto 0..kScaledSteps. A slider's Change event carries
// an integer in the first case and a number in the scene's range in the
// second.
//
// Events (ViewCallbacks::on_event, on the UI thread): Click and LongPress
// for nodes with on_click/on_long_press (a long press is not also a
// click); Change for every input, slider, checkbox and switch the user
// changes, with the new text, value or checked state, so the scene task
// can keep ui.ref():value() current even without on_change; Focus when
// the user focuses an input, which also opens the system keyboard.
// Nothing is reported while an op is being applied, so updates the scene
// sends never come back to it as events.
//
// Lists (virtualized): a ListReset makes the list's content count *
// row_h tall, drops every row it has, holds the scroll position to what
// the new content allows and asks for the rows the viewport needs
// (on_list_range with visible_rows(), list_window.hpp). Scrolling and a
// change of the list's size ask again whenever that window changes, and
// rows that left it are deleted at once (objects and nodes). A ListRows
// row is built only if its index is still in the list's window; one that
// is already there is replaced, so the scene's latest build of a row is
// the one shown. A row's root is placed at index * row_h and sized to the
// list's width and row_h (48 unless given), whatever its own w and h say.
//
// Canvas: a CanvasDraw is drawn into a buffer of the object's size, in
// the display's colour format (lv_display_get_color_format(), so drawing
// the canvas is a plain copy), allocated in Mem::Large. The commands are
// kept, and drawn again when the canvas changes size. A canvas is "fill"
// in both directions unless given a size. One of more than
// kMaxCanvasPixels, or whose buffer would take the view past
// kMaxMediaBytes, has no buffer and shows nothing, reported once
// through on_warning ("ui.canvas: too large") until a size fits again.
//
// Chart: lv_chart, `type` line or bar; series in the theme colours accent,
// ok, warn, danger; values are placed between min and max (default 0 and
// 100).
//
// Image: `src` names a file of the package set_assets() gave, decoded
// once (PNG by LodePNG, JPEG by whatever decoder the platform installed,
// see ui::UiConfig::install_image_decoders) into a buffer in Mem::Large
// that the image shows from then on. LVGL's image cache is off
// (lv_conf.h), so leaving decoding to draw time would decode the file
// again on every redraw. The size in the file's header is checked before
// anything is decoded: a side over kMaxImageSide, more than
// kMaxImagePixels, or pixels that would take the view past
// kMaxMediaBytes are refused. LodePNG allocates through lv_malloc, which
// is Mem::Large with an internal-RAM fallback (lv_mem_wisp.cpp); these
// limits are what bound it. A missing file, one that cannot be decoded
// or one too large leaves the image empty and is reported through
// on_warning ("ui.image: no asset 'assets/x.png'", "ui.image: unsupported
// JPEG 'assets/x.jpg'" for a progressive or damaged JPEG, "ui.image:
// cannot decode 'assets/x.png'", "ui.image: too large 'assets/x.png'").
// Pixels are given back to the budget when the image changes or goes.
//
// Icon: a label showing the LV_SYMBOL_* glyph of `name`.
//
// The screen is not loaded here: the scene host loads it, and must load
// another one before destroying a SceneView whose screen is active.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "lvgl.h"
#include "wisp/base/value.hpp"
#include "wisp/core/scene_repository.hpp"
#include "wisp/hal/display_geometry.hpp"
#include "wisp/os/large.hpp"
#include "wisp/scene/ui_ops.hpp"
#include "wisp/ui/system_layer.hpp"

namespace wisp::scene {

enum class UiEvent { Click, LongPress, Change, Focus };

struct ViewCallbacks {
    // Called on the UI thread; forwards to the scene task.
    std::function<void(NodeId node, UiEvent ev, Value value)> on_event;
    std::function<void(NodeId list, uint32_t first, uint32_t count)> on_list_range;
    // A problem the scene should hear about (a missing image file); the
    // scene host logs it to the scene's log.
    std::function<void(NodeId node, std::string_view message)> on_warning;
};

class SceneView {
public:
    // Whole numbers within this pass to LVGL unscaled (see above).
    static constexpr int32_t kDirectLimit = 100000;
    static constexpr int32_t kScaledSteps = 1000;
    // Pixel memory limits (spec 5.6): an image's sides and pixel count,
    // a canvas's pixel count, and the decoded images and canvas buffers
    // of one view together.
    static constexpr uint32_t kMaxImageSide = 1024;
    static constexpr uint32_t kMaxImagePixels = 512 * 1024;
    static constexpr uint32_t kMaxCanvasPixels = 512 * 1024;
    static constexpr size_t kMaxMediaBytes = 4 * 1024 * 1024;

    // Creates the (empty) screen. `system` must outlive the view.
    SceneView(ui::SystemLayer& system, const hal::ScreenShape& shape, ViewCallbacks cb);
    // Deletes the screen and everything on it.
    ~SceneView();

    SceneView(const SceneView&) = delete;
    SceneView& operator=(const SceneView&) = delete;

    lv_obj_t* screen() const { return screen_; }
    // The package images read their files from. Kept until the next call
    // or the view's destruction; call it before the Build that uses it.
    void set_assets(std::shared_ptr<const core::ScenePackage> package);
    void apply(const UiOp& op);
    // The object of `node` (the screen for the root), or nullptr.
    lv_obj_t* object(NodeId node) const;
    // The node built with ref = `ref`.
    std::optional<NodeId> find_ref(std::string_view ref) const;
    // Nodes in the tree.
    size_t object_count() const { return nodes_.size(); }

private:
    // One per node, at a fixed address: the node's object carries it as
    // its LVGL user data and its event callbacks' user data.
    struct Node;
    struct Insets {
        int32_t left = 0, top = 0, right = 0, bottom = 0;
    };

    using NodeMap =
        std::unordered_map<NodeId, os::LargeUnique<Node>, std::hash<NodeId>, std::equal_to<NodeId>,
                           os::LargeAllocator<std::pair<const NodeId, os::LargeUnique<Node>>>>;
    using RefMap = std::map<os::LargeString, NodeId, std::less<>,
                            os::LargeAllocator<std::pair<const os::LargeString, NodeId>>>;

    void apply_build(const Build& b);
    // Creates the object of one node (of a Build or a row), sets its
    // props and events; the node must already be in nodes_.
    void init_node(Node& n, const NodeDesc& d, Node* parent);
    void apply_set(const SetProp& s);
    void apply_scroll(const ScrollTo& s);
    void apply_focus(const Focus& f);
    void apply_list_reset(const ListReset& r);
    void apply_list_rows(const ListRows& r);
    void apply_canvas(const CanvasDraw& d);

    // Lists (scene_view_list.cpp).
    // `old`: the ids of the row it replaces, which it may reuse.
    bool row_ok(const Node& list, const os::LargeVector<NodeDesc>& row,
                const os::LargeVector<NodeId>* old) const;
    void build_row(Node& list, uint32_t index, const os::LargeVector<NodeDesc>& row);
    void drop_row(Node& list, size_t slot);
    void drop_rows(Node& list);
    // Recomputes the list's window; `force` asks for it even unchanged.
    void update_window(Node& list, bool force);
    void place_row(Node& root);
    static void list_event_cb(lv_event_t* e);

    // Canvas, chart, image and icon (scene_view_canvas.cpp,
    // scene_view_media.cpp).
    void draw_canvas(Node& n);
    void release_canvas(Node& n);
    static void canvas_event_cb(lv_event_t* e);
    void set_series(Node& n, const PropValue& v);
    void apply_chart(Node& n);
    void set_image(Node& n, const PropValue& v);
    void set_icon(Node& n, const PropValue& v);
    void warn(Node& n, std::string_view message);

    bool build_ok(const Build& b) const;
    void clear_tree();
    void reset_screen_style();
    Node* find(NodeId id) const;
    lv_obj_t* create_object(Node& n, lv_obj_t* parent);
    void attach_events(Node& n);
    // Applies what it can at once; returns the derived state update()
    // must redo.
    uint32_t set_prop(Node& n, uint16_t prop, const PropValue& v);
    void update(Node& n, uint32_t what);
    void apply_size(Node& n);
    void apply_pad(Node& n);
    void apply_flex_align(Node& n);
    void apply_range(Node& n);
    void apply_list_height(Node& n);  // and re-places the rows
    static lv_align_t stack_align(const Node& stack);
    void place_in_stack(Node& stack);
    void layout_grid(Node& grid);
    void set_text(Node& n, const PropValue& v);
    void report(Node& n, UiEvent ev, Value value);
    void report_change(Node& n);
    static bool direct_range(const Node& n);

    static void event_cb(lv_event_t* e);

    ui::SystemLayer& system_;
    hal::ScreenShape shape_;
    Insets safe_;
    ViewCallbacks cb_;
    std::shared_ptr<const core::ScenePackage> assets_;
    lv_obj_t* screen_ = nullptr;
    lv_group_t* group_ = nullptr;
    // The decoded images and canvas buffers of every node, against
    // kMaxMediaBytes. Declared before nodes_: a Node gives its bytes back
    // when it is destroyed.
    size_t media_bytes_ = 0;
    NodeMap nodes_;
    RefMap refs_;
    NodeId root_ = 0;
    // > 0 while an op is being applied: events LVGL raises meanwhile are
    // the scene's own updates, not the user's.
    int applying_ = 0;
    // While a Build runs: grids are laid out once at its end.
    bool building_ = false;
};

}  // namespace wisp::scene
