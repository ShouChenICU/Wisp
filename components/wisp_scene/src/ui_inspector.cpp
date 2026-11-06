#include "wisp/scene/ui_inspector.hpp"

#include <cstdio>
#include <memory>
#include <mutex>
#include <string>
#include <utility>

#include "lvgl.h"
#include "wisp/os/os.hpp"
#include "wisp/scene/scene_data.hpp"
#include "wisp/scene/scene_view.hpp"
#include "wisp/scene/schema.hpp"

namespace wisp::scene {

namespace {

// How much of a widget's text a line shows, in code points.
constexpr size_t kTextChars = 60;
// How often wait_stable() looks at the frame counter.
constexpr uint32_t kPollMs = 20;

// The text a widget shows, if it is one that shows text. UI thread.
const char* widget_text(Widget w, lv_obj_t* obj) {
    switch (w) {
        case Widget::Label:
            return lv_label_get_text(obj);
        case Widget::Button:
            for (uint32_t i = 0; i < lv_obj_get_child_count(obj); ++i) {
                lv_obj_t* c = lv_obj_get_child(obj, static_cast<int32_t>(i));
                if (lv_obj_check_type(c, &lv_label_class)) return lv_label_get_text(c);
            }
            return nullptr;
        case Widget::Input:
            return lv_textarea_get_text(obj);
        case Widget::Checkbox:
            return lv_checkbox_get_text(obj);
        default:
            return nullptr;
    }
}

// `"text"`, escaped, cut to kTextChars code points.
void append_quoted(os::LargeString& out, const char* text) {
    out += '"';
    size_t chars = 0;
    for (const char* p = text; *p != 0; ++p) {
        const unsigned char c = static_cast<unsigned char>(*p);
        if ((c & 0xC0) != 0x80 && chars++ == kTextChars) {
            out += "…";
            break;
        }
        if (c == '"' || c == '\\') {
            out += '\\';
            out += static_cast<char>(c);
        } else if (c == '\n') {
            out += "\\n";
        } else if (c < 0x20) {
            out += ' ';
        } else {
            out += static_cast<char>(c);
        }
    }
    out += '"';
}

void append_box(os::LargeString& out, lv_obj_t* obj) {
    lv_area_t a;
    lv_obj_get_coords(obj, &a);
    char buf[64];
    std::snprintf(buf, sizeof(buf), "[%d,%d %d×%d]", static_cast<int>(a.x1), static_cast<int>(a.y1),
                  static_cast<int>(lv_area_get_width(&a)),
                  static_cast<int>(lv_area_get_height(&a)));
    out += buf;
}

// One line of the tree (ui_inspector.hpp). UI thread.
os::LargeString node_line(const NodeRef& n) {
    os::LargeString out(widget_spec(n.widget).name);
    out += '#';
    if (n.ref.empty()) {
        out += 'n';
        out += std::to_string(n.id).c_str();
    } else {
        out.append(n.ref.data(), n.ref.size());
    }
    out += ' ';
    if (const char* text = widget_text(n.widget, n.obj); text != nullptr) {
        append_quoted(out, text);
        out += ' ';
    }
    append_box(out, n.obj);
    if (lv_obj_has_flag(n.obj, LV_OBJ_FLAG_HIDDEN)) out += " hidden";
    if (lv_obj_has_state(n.obj, LV_STATE_DISABLED)) out += " disabled";
    if (lv_obj_has_state(n.obj, LV_STATE_CHECKED)) out += " checked";
    if (n.widget == Widget::Scroll || n.widget == Widget::List) {
        const int32_t y = lv_obj_get_scroll_y(n.obj);
        const int32_t ymax = y + lv_obj_get_scroll_bottom(n.obj);
        const int32_t x = lv_obj_get_scroll_x(n.obj);
        const int32_t xmax = x + lv_obj_get_scroll_right(n.obj);
        const bool horizontal = xmax > 0 && ymax <= 0;
        char buf[48];
        std::snprintf(buf, sizeof(buf), " scroll=%d/%d", static_cast<int>(horizontal ? x : y),
                      static_cast<int>(horizontal ? xmax : ymax));
        out += buf;
    }
    return out;
}

// "n12" names node 12 when it has no ref of its own.
bool matches(const NodeRef& n, std::string_view ref) {
    if (n.ref == ref) return true;
    if (!n.ref.empty() || ref.size() < 2 || ref[0] != 'n') return false;
    return ref.substr(1) == std::to_string(n.id);
}

// What the UI thread hands back to tree().
struct Collected {
    os::Semaphore done{1, 0};
    std::mutex mu;
    bool answered = false;
    bool has_view = false;
    bool has_ref = true;
    os::LargeVector<TreeLine> lines;
    std::string system;  // the "system ..." lines, each ending in '\n'
};

}  // namespace

UiInspector::UiInspector(InspectorDeps deps) : deps_(std::move(deps)) {}

Result<std::string> UiInspector::tree(std::string_view ref) {
    using R = Result<std::string>;
    if (deps_.ui.on_ui_thread()) return R::err(ErrorCode::Busy, "cannot wait for the UI here");
    const std::optional<std::string> front = deps_.scenes.foreground();
    if (!front) return R::err(ErrorCode::NotFound, "no_scene");
    const core::SceneStatus status = deps_.scenes.status(*front);
    if (status.state == core::SceneState::Error)
        return R::err(ErrorCode::InvalidArgument,
                      "scene_error: " + scene_data(*front, status.error));

    auto c = std::make_shared<Collected>();
    SceneRuntime& scenes = deps_.scenes;
    ui::SystemLayer& layer = deps_.layer;
    const std::string wanted(ref);
    const bool posted = deps_.ui.post([c, &scenes, &layer, wanted] {
        std::lock_guard<std::mutex> lock(c->mu);
        const SceneView* view = scenes.ui_current_view();
        // A view whose first Build has not been applied yet is not up.
        if (view != nullptr && view->object_count() == 0) view = nullptr;
        c->has_view = view != nullptr;
        if (view != nullptr) {
            lv_obj_update_layout(view->screen());
            // The path from the root to the node being visited: its ids,
            // depths and lines (kNoParent for a node not listed).
            struct Step {
                NodeId id;
                uint16_t depth;
                uint32_t line;
            };
            os::LargeVector<Step> path;
            // With a ref: the depth of the node it names once found, and
            // whether its subtree is over.
            int top_depth = -1;
            bool over = false;
            c->has_ref = wanted.empty();
            view->for_each_node([&](const NodeRef& n) {
                if (over) return;
                while (!path.empty() && path.back().id != n.parent) path.pop_back();
                const uint16_t depth =
                    path.empty() ? 0 : static_cast<uint16_t>(path.back().depth + 1);
                const uint32_t parent_line = path.empty() ? kNoParent : path.back().line;
                bool listed = wanted.empty();
                if (!wanted.empty()) {
                    if (top_depth < 0 && matches(n, wanted)) {
                        top_depth = depth;
                        c->has_ref = true;
                    } else if (top_depth >= 0 && depth <= top_depth) {
                        over = true;
                        return;
                    }
                    listed = top_depth >= 0;
                }
                uint32_t line_index = kNoParent;
                if (listed) {
                    TreeLine line;
                    line.depth = static_cast<uint16_t>(depth - (top_depth >= 0 ? top_depth : 0));
                    line.text = node_line(n);
                    line.parent_index = parent_line;
                    line_index = static_cast<uint32_t>(c->lines.size());
                    c->lines.push_back(std::move(line));
                }
                path.push_back({n.id, depth, line_index});
            });
            if (wanted.empty()) {
                for (const auto& [name, obj] : layer.visible_pieces()) {
                    os::LargeString line("system ");
                    line += name;
                    line += ' ';
                    append_box(line, obj);
                    c->system.append(line.data(), line.size());
                    c->system.push_back('\n');
                }
            }
        }
        c->answered = true;
        c->done.give();
    });
    if (!posted) return R::err(ErrorCode::Busy, "UI queue full");
    if (!c->done.take(kTreeUiWaitMs))
        return R::err(ErrorCode::Timeout, "the UI thread did not answer");
    std::lock_guard<std::mutex> lock(c->mu);
    if (!c->has_view) return R::err(ErrorCode::NotFound, "no_scene");
    if (!c->has_ref)
        return R::err(ErrorCode::NotFound, "not_found: no widget with ref '" + wanted + "'");
    const size_t wrapper = scene_data(*front, "").size();
    const size_t fixed = wrapper + (c->system.empty() ? 0 : 1 + c->system.size() - 1);
    const size_t budget = kTreeMaxChars > fixed ? kTreeMaxChars - fixed : 0;
    std::string out = scene_data(*front, fold_tree(c->lines, budget));
    if (!c->system.empty()) {
        out.push_back('\n');
        out.append(c->system, 0, c->system.size() - 1);
    }
    return R::ok(std::move(out));
}

Result<bool> UiInspector::wait_stable(uint32_t quiet_ms, uint32_t max_ms) {
    if (deps_.ui.on_ui_thread()) return Result<bool>::err(ErrorCode::Busy, "cannot wait here");
    const uint64_t start = os::now_ms();
    uint32_t frames = deps_.ui.perf().frames;
    uint64_t last_change = start;
    for (;;) {
        os::sleep_ms(kPollMs);
        const uint64_t now = os::now_ms();
        const uint32_t f = deps_.ui.perf().frames;
        if (f != frames) {
            frames = f;
            last_change = now;
        }
        if (now - last_change >= quiet_ms) return Result<bool>::ok(true);
        if (now - start >= max_ms) return Result<bool>::ok(false);
    }
}

}  // namespace wisp::scene
