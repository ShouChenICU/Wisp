#pragma once

// SceneWorkspace: the agent's scene drafts and their installs (agent-core
// spec 4.1, 5.1).
//
// A draft is <drafts_root>/<id>/ (on the device /int/scenes/.drafts/<id>,
// which SceneRepository skips like every directory starting with '.'),
// laid out like a package and kept across restarts. The first change to
// an installed scene copies the installed package into the draft; a new id
// starts with an empty one. Reads prefer the draft and fall back to the
// installed package. Paths follow core::valid_package_path(), and a draft
// may hold at most the repository's package limit (512 KiB).
//
// install() (only from the agent's own thread, never the UI thread, the
// scene task or the storage worker, since it waits on all three):
//   1. reads the draft on the flash thread;
//   2. checks scene.json (it must name the id, and the entry and icon
//      files must be there);
//   3. asks `confirm` before the scene gets an http host the installed
//      version does not have (a refusal is "denied");
//   4. compiles every .lua file (SceneRuntime::check_sources());
//   5. builds the tree (SceneRuntime::check_build());
//   6. zips the draft and installs it with core::install_scene(), keeping
//      the installed version as the previous one;
//   7. opens the scene in front;
//   8. drops the draft;
// and then checks it (spec 5.1): waits for the screen to settle
// (UiInspector::wait_stable()) and reports the scene's Lua error, if it
// faulted, its new log lines (W and E, at most kCheckLogsPerLevel each,
// from step 6 on) and UiInspector::tree(). A failure in steps 1-6 returns
// "errors" ([{file, line, message, suggestion}]) and leaves the installed
// version and the draft as they were.
//
// rollback() installs the previous version (SceneRepository::prev_zip())
// the same way from step 6, which makes the current one the previous one.
//
// Threads: every draft method does its file work on the flash thread
// (core::call_on_worker(), inline when already there), so the tools that
// call them may run there (ToolThread::Fs). install() and rollback() touch
// flash only through SceneRepository and the worker, and the UI only
// through UiInspector, which posts to it.
//
// Errors carry their tool error code in front of the message ("bad_path:
// ...", see error_code()).

#include <cstddef>
#include <cstdint>
#include <functional>
#include <string>
#include <string_view>

#include "wisp/agent/tool_registry.hpp"
#include "wisp/base/result.hpp"
#include "wisp/base/value.hpp"
#include "wisp/core/scene_repository.hpp"
#include "wisp/core/storage_manager.hpp"
#include "wisp/core/storage_worker.hpp"
#include "wisp/os/large.hpp"
#include "wisp/scene/scene_runtime.hpp"
#include "wisp/scene/ui_inspector.hpp"

namespace wisp::agent {

struct SceneWorkspaceDeps {
    core::SceneRepository& repo;
    scene::SceneRuntime& runtime;
    core::StorageManager& storage;
    core::StorageWorker& worker;
    // Empty: every new host is refused.
    ConfirmFn confirm;
    std::string drafts_root;  // <data_root>/.drafts
    // The check after install() and rollback(); null: no tree, and
    // "stable" is false.
    scene::UiInspector* inspector = nullptr;
    // Called with the installed manifest after install() or rollback().
    std::function<void(const core::SceneManifest&)> on_tools_changed{};
};

struct InstallError {
    std::string file;  // "main.lua", "scene.json"; empty if unknown
    int line = 0;
    std::string message, suggestion;
};

Value to_value(const InstallError& e);

// The tool error code an error of SceneWorkspace carries: the text before
// the first ": " when it is a snake_case word, else to_string(code).
std::string error_code(const Error& e);

class SceneWorkspace {
public:
    explicit SceneWorkspace(SceneWorkspaceDeps deps);

    // [{id, name, version, state, bytes, draft}] for every installed scene
    // and every draft of one that is not (state "draft" then).
    Value list();
    // {"draft": bool, "files": [{path, bytes}]}: the draft if any, else
    // the installed package. NotFound for neither.
    Result<Value> list_files(const std::string& id);
    // The draft's file if there is a draft, else the installed one.
    // "too_large" past kReadMaxBytes.
    Result<os::LargeString> read_file(const std::string& id, std::string_view path);
    // Whether read_file() would read the draft.
    bool has_draft(const std::string& id);
    Result<void> write_file(const std::string& id, std::string_view path, std::string_view content);
    // Replaces the only occurrence of old_text (bytes, not overlapping).
    // NotFound "not_found" for none, AlreadyExists "not_unique" for more;
    // *count is set either way.
    Result<void> edit_file(const std::string& id, std::string_view path, std::string_view old_text,
                           std::string_view new_text, size_t* count);
    Result<void> delete_file(const std::string& id, std::string_view path);
    // Drops the draft; NotFound without one. *name (if given) is the
    // scene's name for the note.
    Result<void> discard(const std::string& id, std::string* name = nullptr);
    ToolOutput install(const std::string& id, ToolCtx& ctx);
    ToolOutput rollback(const std::string& id, ToolCtx& ctx);

private:
    std::string draft_dir(const std::string& id) const;
    // These run on the flash thread.
    bool draft_exists_here(const std::string& id);
    // Copies the installed package into a draft that does not exist yet.
    Result<void> ensure_draft_here(const std::string& id);
    Result<uint64_t> draft_bytes_here(const std::string& id);
    // The scene's name for a note: the draft's or the installed manifest's,
    // else the id.
    std::string name_here(const std::string& id);
    // install()'s steps 6-8 and the check, for a package already checked.
    ToolOutput install_zip(const std::string& id, std::span<const uint8_t> zip, bool drop_draft,
                           const std::string& note, ToolCtx& ctx);

    SceneWorkspaceDeps deps_;
};

}  // namespace wisp::agent
