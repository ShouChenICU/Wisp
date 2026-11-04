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
    // afterwards, which to the user is the reload spec 6.3 asks for. Never
    // with the repository's lock held: closing waits on the storage worker,
    // which may itself be waiting for that lock in load().
    std::string reopen;
    auto installed = repo.install(
        zip, [&host](std::span<const SourceFile> files) { return host.check_sources(files); },
        [&](const SceneManifest& m) -> Result<void> {
            if (host.status(m.id).state != SceneState::Running) return Result<void>::ok();
            WISP_TRY(host.close(m.id));
            reopen = m.id;
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
