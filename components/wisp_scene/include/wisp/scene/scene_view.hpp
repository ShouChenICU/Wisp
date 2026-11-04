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
//   - pad = "safe" on any other node is those same distances.
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
// Lists: a ListReset makes the list's content count * row_h tall, so
// ScrollTo can reach row `index`. Building rows (ListRows), canvases
// (CanvasDraw), icons, images and charts are not drawn yet; those nodes
// get an empty object.
//
// The screen is not loaded here: the scene host loads it, and must load
// another one before destroying a SceneView whose screen is active.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <string_view>
#include <unordered_map>
#include <utility>

#include "lvgl.h"
#include "wisp/base/value.hpp"
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
};

class SceneView {
public:
    // Whole numbers within this pass to LVGL unscaled (see above).
    static constexpr int32_t kDirectLimit = 100000;
    static constexpr int32_t kScaledSteps = 1000;

    // Creates the (empty) screen. `system` must outlive the view.
    SceneView(ui::SystemLayer& system, const hal::ScreenShape& shape, ViewCallbacks cb);
    // Deletes the screen and everything on it.
    ~SceneView();

    SceneView(const SceneView&) = delete;
    SceneView& operator=(const SceneView&) = delete;

    lv_obj_t* screen() const { return screen_; }
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
    void apply_set(const SetProp& s);
    void apply_scroll(const ScrollTo& s);
    void apply_focus(const Focus& f);
    void apply_list_reset(const ListReset& r);

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
    void apply_list_height(Node& n);
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
    lv_obj_t* screen_ = nullptr;
    lv_group_t* group_ = nullptr;
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
