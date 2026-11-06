#pragma once

// UiInspector: what the agent sees of the screen (agent-core spec 5.1,
// 5.2): the control tree of the scene in front with the system layer's
// pieces over it, and a wait for the screen to settle after an install.
//
// tree() collects everything in one job on the UI thread (posted with
// UiRuntime::post(); the only place it touches LVGL) and formats and folds
// it on the caller's thread. Each widget is one line,
//   type#ref "text" [x,y w×h] flags
// indented two spaces per level, coordinates absolute on the screen; a
// node without a ref gets "n<id>" ("label#n12"), which tree(ref) also
// takes. Flags: hidden, disabled, checked, scroll=<pos>/<max> for a scroll
// or list. The scene's lines are wrapped with scene_data() (spec 6.3);
// the system layer's pieces follow outside the wrapper as
// "system <piece> [x,y w×h]". Past kTreeMaxChars the scene's part is
// folded by fold_tree().
//
// wait_stable() watches the UI's frame counter (UiPerf::frames) from the
// caller's thread: the screen is stable once no frame was drawn for
// `quiet_ms`.
//
// Neither may run on the UI thread's behalf while it waits (Busy there);
// both may wait, so not on the scene task or the storage worker either.

#include <cstddef>
#include <cstdint>
#include <span>
#include <string>
#include <string_view>

#include "wisp/base/result.hpp"
#include "wisp/hal/display_geometry.hpp"
#include "wisp/os/large.hpp"
#include "wisp/scene/scene_runtime.hpp"
#include "wisp/ui/system_layer.hpp"
#include "wisp/ui/ui_runtime.hpp"

namespace wisp::scene {

// The screen counts as settled after this long without a redraw, waiting
// at most kSettleMaxMs.
inline constexpr uint32_t kSettleQuietMs = 300;
inline constexpr uint32_t kSettleMaxMs = 2000;
// tree() folds past this many bytes, keeping this many children at each
// end of a folded container.
inline constexpr size_t kTreeMaxChars = 2000;
inline constexpr size_t kTreeKeepEnds = 5;
// How long tree() waits for the UI thread.
inline constexpr uint32_t kTreeUiWaitMs = 1000;

inline constexpr uint32_t kNoParent = UINT32_MAX;

// One line of a tree, without its indentation. Lines come in pre-order:
// a line's parent_index is the index of an earlier line, or kNoParent.
// The text is in Mem::Large: a tree may have kMaxNodes lines.
struct TreeLine {
    uint16_t depth = 0;
    os::LargeString text;
    uint32_t parent_index = kNoParent;
};

// The lines, indented two spaces per depth and joined with '\n', in at
// most `max_chars` bytes:
//   1. as they are when they fit;
//   2. else the container with the most children first, then the next,
//      each with more than 2 * kTreeKeepEnds children keeping the first
//      and last kTreeKeepEnds and showing "…还有 N 行" for the N between,
//      until it fits;
//   3. else, deepest first, whole subtrees become a suffix
//      " …（N 个子控件）" (N descendants) of their root's line, until it
//      fits;
//   4. else cut after the last line that fits, ending with
//      "…已截断，用 ui.tree(ref) 查看子树".
std::string fold_tree(std::span<const TreeLine> lines, size_t max_chars);

struct InspectorDeps {
    ui::UiRuntime& ui;
    ui::SystemLayer& layer;
    SceneRuntime& scenes;
    hal::ScreenShape shape;
};

class UiInspector {
public:
    explicit UiInspector(InspectorDeps deps);

    // The tree (file header), or the subtree of the node with that ref.
    // Errors, by message: "no_scene" (NotFound) with no scene view in
    // front; "scene_error: <text>" (InvalidArgument; the error text
    // wrapped with scene_data()) while the scene in front shows its error; "not_found: ..."
    // (NotFound) for an unknown ref; Timeout if the UI thread did not answer in kTreeUiWaitMs; Busy
    // on the UI thread.
    Result<std::string> tree(std::string_view ref = {});

    // True once nothing was drawn for quiet_ms, false after max_ms. Busy on
    // the UI thread.
    Result<bool> wait_stable(uint32_t quiet_ms = kSettleQuietMs, uint32_t max_ms = kSettleMaxMs);

private:
    InspectorDeps deps_;
};

}  // namespace wisp::scene
