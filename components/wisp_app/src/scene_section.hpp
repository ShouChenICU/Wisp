#pragma once

// The self-check screen's "场景" card (scene-runtime-core spec 8): one row
// per installed scene, its name and its state -- a green "运行中" chip
// while it runs, a red "出错" chip after an error (spec 3.6: it stays
// until the scene opens again), its version otherwise -- or "还没有安装
// 场景". Tapping a row opens the scene. Subproject 8's shell replaces this
// card.
//
// The list comes from SceneRepository::list() on the storage worker
// (SelfCheck asks for it), the states from SceneRuntime::status(), an
// in-memory read. Both are compared before anything is touched: an
// unchanged list rebuilds no row, an unchanged state restyles nothing, so
// a static screen stays at zero redraws.
//
// UI thread only.

#include <functional>
#include <string>
#include <vector>

#include "lvgl.h"
#include "scene_wiring.hpp"
#include "wisp/core/scene_host.hpp"

namespace wisp::app {

class SceneSection {
public:
    // The state of a scene by id; empty: every scene is stopped (no
    // runtime).
    using StatusFn = std::function<core::SceneStatus(const std::string& id)>;
    using OpenFn = std::function<void(const std::string& id)>;

    // Appends the card to `col` (which owns the widgets) with the rows of
    // the last listing; call again after the column was rebuilt.
    void build(lv_obj_t* col, StatusFn status, OpenFn on_open);

    // A new listing. Rebuilds the rows only if it differs from the last.
    void set_entries(std::vector<SceneEntry> entries);

    // Re-reads every row's state; restyles only the rows whose state moved.
    void update_states();

    // Whether a listing has arrived since construction.
    bool listed() const { return listed_; }

private:
    struct Row {
        SceneEntry entry;
        core::SceneState state = core::SceneState::Stopped;
        bool rendered = false;
        lv_obj_t* chip = nullptr;
        lv_obj_t* status = nullptr;
    };

    void build_rows();
    void render(Row& row);
    static void row_cb(lv_event_t* e);

    StatusFn status_;
    OpenFn on_open_;
    lv_obj_t* card_ = nullptr;
    std::vector<SceneEntry> entries_;
    std::vector<Row> rows_;
    bool listed_ = false;
};

}  // namespace wisp::app
