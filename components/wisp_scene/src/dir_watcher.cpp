#include "wisp/scene/dir_watcher.hpp"

#include <algorithm>
#include <limits>
#include <string_view>
#include <utility>

#include "wisp/base/crc32.hpp"

namespace wisp::scene {

namespace {

// StorageManager's own depth limit; a working directory deeper than this
// has nothing a package could hold.
constexpr size_t kMaxDepth = 16;

bool ends_with(std::string_view s, std::string_view x) {
    return s.size() >= x.size() && s.substr(s.size() - x.size()) == x;
}

// Names nothing should load: hidden ones, and StorageManager's write
// bookkeeping (which read_file() refuses).
bool skipped(std::string_view name) {
    return name.empty() || name[0] == '.' || ends_with(name, ".tmp") || ends_with(name, ".new");
}

}  // namespace

DirWatcher::DirWatcher(core::StorageManager& storage, std::string root)
    : storage_(storage), root_(std::move(root)) {}

std::vector<std::string> DirWatcher::poll() {
    std::map<std::string, Snapshot> now = scan();
    std::vector<std::string> changed;
    if (!primed_) {
        for (auto& [id, snap] : now) scenes_[id] = Tracked{snap, snap, false};
        primed_ = true;
        return changed;
    }
    for (const auto& [id, snap] : now) scenes_.try_emplace(id);
    for (auto it = scenes_.begin(); it != scenes_.end();) {
        auto found = now.find(it->first);
        const Snapshot current = found == now.end() ? Snapshot{} : found->second;
        Tracked& t = it->second;
        if (current != t.last) {
            // Still moving: remember it and wait for a quiet poll.
            t.last = current;
            t.pending = current != t.reported;
        } else if (t.pending) {
            t.reported = current;
            t.pending = false;
            changed.push_back(it->first);
        }
        // A scene that is gone and has been reported as gone needs no entry.
        if (found == now.end() && t.reported.empty() && !t.pending) {
            it = scenes_.erase(it);
        } else {
            ++it;
        }
    }
    return changed;  // std::map order: sorted
}

std::map<std::string, DirWatcher::Snapshot> DirWatcher::scan() {
    std::map<std::string, Snapshot> out;
    auto entries = storage_.list_dir(root_);
    if (!entries) return out;  // a missing root holds no scenes
    for (const core::DirEntry& e : entries.value()) {
        if (e.kind != core::EntryKind::Dir || skipped(e.name)) continue;
        out.emplace(e.name, scan_scene(root_ + "/" + e.name));
    }
    return out;
}

DirWatcher::Snapshot DirWatcher::scan_scene(const std::string& dir) {
    Snapshot snap;
    // Iterative, like StorageManager's own walks: the worker's stack is 4 KB.
    std::vector<std::pair<std::string, size_t>> todo{{"", 0}};
    while (!todo.empty()) {
        auto [rel, depth] = std::move(todo.back());
        todo.pop_back();
        auto entries = storage_.list_dir(rel.empty() ? dir : dir + "/" + rel);
        if (!entries) continue;
        for (const core::DirEntry& e : entries.value()) {
            if (skipped(e.name)) continue;
            std::string path = rel.empty() ? e.name : rel + "/" + e.name;
            if (e.kind == core::EntryKind::Dir) {
                if (rel.empty() && e.name != "lib" && e.name != "assets") continue;
                if (depth + 1 < kMaxDepth) todo.emplace_back(std::move(path), depth + 1);
                continue;
            }
            if (e.kind != core::EntryKind::File) continue;
            if (rel.empty() && e.name != "scene.json" && !ends_with(e.name, ".lua")) continue;
            Stamp stamp{path, e.size, 0};
            auto body = storage_.read_file_large(dir + "/" + path);
            if (body) {
                stamp.size = body.value().size();
                stamp.crc = crc32(body.value().span());
            } else {
                // Gone or unreadable between the listing and the read: a
                // value no real file has, so the next poll sees a change
                // and waits for the files to settle.
                stamp.size = std::numeric_limits<uint64_t>::max();
            }
            snap.push_back(std::move(stamp));
        }
    }
    std::sort(snap.begin(), snap.end(),
              [](const Stamp& a, const Stamp& b) { return a.path < b.path; });
    return snap;
}

}  // namespace wisp::scene
