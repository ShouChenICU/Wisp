// ui.tree (agent-core spec 5.2): a thin adapter over scene::UiInspector.
// It runs on the agent's thread: the inspector posts its one job to the UI
// thread and waits for it there.
//
// ui.screenshot (spec 3.6): runs on wisp.tool.io. The snapshot itself is
// taken on the UI thread (UiRuntime::snapshot_rgb888() posts it and waits);
// blacking out, scaling and encoding happen here, off the UI thread.

#include <string>
#include <string_view>

#include "screenshot.hpp"
#include "wisp/agent/limits.hpp"
#include "wisp/agent/tools.hpp"

namespace wisp::agent {

Result<void> add_ui_tools(ToolRegistry& reg, scene::UiInspector& inspector) {
    return reg.add({
        .name = "ui.tree",
        .description = "The widgets on screen, one per line: type#ref \"text\" [x,y w×h] and "
                       "state. With `ref`, only that widget's subtree.",
        .schema_json = R"({"type":"object","properties":{"ref":{"type":"string","maxLength":64}}})",
        .thread = ToolThread::Agent,
        .run =
            [&inspector](const Value& a, ToolCtx&) {
                const Value* ref = a.find("ref");
                auto r = inspector.tree(ref != nullptr && ref->type() == Value::Type::String
                                            ? std::string_view(ref->as_string())
                                            : std::string_view());
                // The message starts with the code: "no_scene", "scene_error:
                // ..." (the scene's text already wrapped), "not_found: ...".
                if (!r) return tool_error(error_code(r.error()), r.error().message);
                ToolOutput out;
                out.result = Value(Map{{"ok", Value(true)}, {"tree", Value(r.value())}});
                return out;
            },
    });
}

Result<void> add_screenshot_tool(ToolRegistry& reg, ScreenshotToolDeps deps) {
    ui::UiRuntime* ui = &deps.ui;
    const hal::ScreenShape shape = deps.shape;
    return reg.add({
        .name = "ui.screenshot",
        .description = "Takes a picture of what the screen shows now (the scene and any dialog "
                       "over it; parts the panel cannot show are black), 240 px wide, and shows "
                       "it to you as an image. For checking how a scene looks; ui.tree gives text "
                       "and positions.",
        .risk = ToolRisk::Normal,
        .vision_only = true,
        .thread = ToolThread::Io,
        .timeout_ms = kScreenshotToolMs,
        .run =
            [ui, shape](const Value&, ToolCtx&) {
                auto snap = ui->snapshot_rgb888(kSnapshotWaitMs);
                if (!snap) {
                    switch (snap.error().code) {
                        case ErrorCode::Busy:
                            return tool_error("busy", snap.error().message);
                        case ErrorCode::Timeout:
                            return tool_error("timeout", snap.error().message);
                        default:
                            return tool_error("snapshot_failed", snap.error().message);
                    }
                }
                auto img = make_screenshot(std::move(snap.value()), shape);
                if (!img) {
                    switch (img.error().code) {
                        case ErrorCode::QuotaExceeded:
                            return tool_error("too_large", img.error().message);
                        case ErrorCode::NoMemory:
                            return tool_error("no_memory", img.error().message);
                        default:
                            return tool_error("snapshot_failed", img.error().message);
                    }
                }
                ToolOutput out;
                out.result_json = R"({"ok":true,"image":)";
                append_image_json(out.result_json, *img.value());
                out.result_json.push_back('}');
                out.image = std::move(img.value());
                return out;
            },
    });
}

}  // namespace wisp::agent
