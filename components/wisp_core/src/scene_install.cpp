#include "wisp/core/scene_install.hpp"

#include <string>

#include "wisp/core/log.hpp"

namespace wisp::core {

Result<SceneInfo> install_scene(SceneRepository& repo, SceneHost& host,
                                std::span<const uint8_t> zip) {
    // A running scene's store keeps flushing into its data/ directory, which
    // the replace moves; a flush landing mid-move is lost, or leaves a stray
    // data/ that blocks the replace for good. So the scene closes first --
    // its close returns once the final flush is on disk -- and opens again
    // afterwards, which to the user is the reload spec 6.3 asks for.
    //
    // The close is asked for whatever the state: a scene in the error state
    // may still have its last flush queued, and an open that began before
    // the install (repo.installing() refuses those that come after) has not
    // reached Running yet; closing runs behind either on the scene task.
    // Only a scene that was running is opened again.
    //
    // Never with the repository's lock held: closing waits on the storage
    // worker, which may itself be waiting for that lock in load().
    std::string reopen;
    auto installed = repo.install(
        zip, [&host](std::span<const SourceFile> files) { return host.check_sources(files); },
        [&](const SceneManifest& m) -> Result<void> {
            const bool was_running = host.status(m.id).state == SceneState::Running;
            Result<void> closed = host.close(m.id);
            if (!closed) {
                // Nothing has been replaced. A close that timed out is still
                // queued and happens later; the scene then stays closed on
                // its old package. Any other failure closed nothing.
                const std::string tag = "scene." + m.id;
                WLOG_W(tag.c_str(), "install stopped: the scene did not close: %s",
                       closed.error().message.c_str());
                const bool timed_out = closed.error().code == ErrorCode::Timeout;
                return Result<void>::err(
                    closed.error().code,
                    "scene " + m.id +
                        (timed_out ? " did not close in time (" : " could not be closed (") +
                        closed.error().message + (timed_out ? "; it will still close)" : ")") +
                        "; the installed package was left unchanged");
            }
            if (was_running) reopen = m.id;
            return Result<void>::ok();
        });
    if (!reopen.empty()) {
        // Whichever package is in place now: the new one, or the old one if
        // the replace failed.
        Result<void> opened = host.open(reopen);
        if (!opened) {
            const std::string tag = "scene." + reopen;
            WLOG_W(tag.c_str(), "reopen after install failed: %s", opened.error().message.c_str());
        }
    } else if (installed) {
        host.on_installed(installed.value().manifest.id);
    }
    return installed;
}

}  // namespace wisp::core
