#pragma once

// SceneRecents: the order the scene list shows the scenes in, most recently
// opened first (MVP spec 6.4). Kept in <System>/shell/recents.json as
//   {"version":1,"recent":{"<id>":<seq>, ...}}
// where a larger seq is more recent. The App calls opened() on every
// scene.state "running" and forget() on "deleted"; a factory reset removes
// <System>/shell/ (and calls forget_all() first, so a save still pending
// cannot write it back).
//
// Saving: a change arms a one-shot kSaveDelayMs timer, whose callback hands
// the write to the storage worker through `submit_io`, so however many
// scenes open within that second there is one write. load() and flush()
// touch flash themselves: on the worker, or before it starts.
//
// Threads: every method may be called from any thread (one mutex); the
// file work runs where the header above says.

#include <cstdint>
#include <functional>
#include <map>
#include <memory>
#include <span>
#include <string>
#include <vector>

#include "wisp/base/result.hpp"
#include "wisp/core/storage_manager.hpp"
#include "wisp/os/os.hpp"

namespace wisp::shell {

class SceneRecents {
public:
    // Changes within this long make one write.
    static constexpr uint32_t kSaveDelayMs = 1000;

    SceneRecents(core::StorageManager& storage, std::string path,
                 std::function<bool(std::function<void()>)> submit_io);
    // Stops the timer; a change not saved yet is lost (flush() first).
    ~SceneRecents();

    SceneRecents(const SceneRecents&) = delete;
    SceneRecents& operator=(const SceneRecents&) = delete;

    // Reads the file; a missing one is an empty list, a malformed one is
    // ignored with a warning (also Ok). Flash.
    Result<void> load();
    // Writes a change still waiting for the timer. Flash.
    Result<void> flush();

    // Scene `id` came to the front (scene.state running).
    void opened(const std::string& id);
    // Scene `id` was deleted (scene.state deleted).
    void forget(const std::string& id);
    // Drops everything and saves nothing from now on (factory reset).
    void forget_all();

    // `ids` most recently opened first; those never opened last, by id.
    std::vector<std::string> order(std::span<const std::string> ids) const;

private:
    // What a save job queued on the worker shares with this object, so it
    // stays valid however long the job waits.
    struct State {
        os::Mutex mutex;
        std::map<std::string, uint64_t> seq;
        uint64_t next = 1;
        bool dirty = false;
        bool frozen = false;
    };
    // Writes the state if it changed since the last write. Flash.
    static Result<void> save(core::StorageManager& storage, const std::string& path, State& st);
    void changed();

    core::StorageManager& storage_;
    std::string path_;
    std::function<bool(std::function<void()>)> submit_io_;
    std::shared_ptr<State> st_ = std::make_shared<State>();
    std::unique_ptr<os::Timer> timer_;
};

}  // namespace wisp::shell
