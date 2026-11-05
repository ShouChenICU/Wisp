#pragma once

// DirWatcher: notices edits to scenes in the simulator's --scenes-dir
// working directory (scene-runtime-core spec 6.4), so the scene in front
// can be reloaded after each save.
//
// poll() takes a snapshot of every scene directory under the root: the
// path, size and CRC-32 of each file a package can hold -- scene.json and
// the top-level .lua files (the entry is one of them), lib/** and
// assets/** -- leaving out anything whose name starts with '.' (editor
// swap files). A README, docs/ or data/ (the scene's own store) never
// trigger a reload, and are not read every poll either. The checksum is
// what catches an edit that keeps the size.
//
// A change is reported once the scene's files have held still for one
// whole poll: the poll that first sees a difference only remembers it, and
// the next one reports the scene if nothing moved in between, or waits
// again if something did. Polled every 500 ms, that is a quiet spell of at
// least that long (more than the 300 ms debounce the tool side uses), so a
// file an editor is still writing is never loaded half done. The first
// poll is the baseline and reports nothing. A scene directory that appears
// or disappears is a change like any other.
//
// Reads go through StorageManager, so poll() belongs on the wisp.fs
// worker. Not thread-safe: one caller at a time.

#include <cstdint>
#include <map>
#include <string>
#include <vector>

#include "wisp/core/storage_manager.hpp"

namespace wisp::scene {

class DirWatcher {
public:
    DirWatcher(core::StorageManager& storage, std::string root);

    // The ids of the scenes whose files changed and have since settled
    // (see the file header), sorted.
    std::vector<std::string> poll();

private:
    struct Stamp {
        std::string path;  // relative to the scene directory
        uint64_t size = 0;
        uint32_t crc = 0;
        bool operator==(const Stamp&) const = default;
    };
    using Snapshot = std::vector<Stamp>;  // sorted by path
    struct Tracked {
        Snapshot reported;  // as of the last report (or the baseline)
        Snapshot last;      // as of the last poll
        bool pending = false;
    };

    std::map<std::string, Snapshot> scan();
    Snapshot scan_scene(const std::string& dir);

    core::StorageManager& storage_;
    std::string root_;
    std::map<std::string, Tracked> scenes_;
    bool primed_ = false;
};

}  // namespace wisp::scene
