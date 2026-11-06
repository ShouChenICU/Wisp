#pragma once

// CachedSettingsBackend: Settings served from memory, persisted on the
// flash thread (spec 3.4 of
// docs/superpowers/specs/2026-11-06-flash-thread-design.md).
//
// load() reads every key of the platform backend (NVS or the simulator's
// JSON file) once. From then on get() only reads memory, so any thread may
// call it, the UI and the scene task included. set()/erase()/erase_all()
// change memory at once, remember which keys are dirty and, unless a save
// is already queued, queue one Normal job on the StorageWorker. That job
// hands every dirty key to the platform's write_batch() in one go, so a
// key set many times before the job runs is written once, with its last
// value.
//
// A failed save keeps the new values in memory and the keys dirty; it is
// logged and retried on the next change or flush(). On the third failure
// in a row `on_persist_failed` is called once (App posts the "设置未能保存"
// system notice); a successful save starts the count again.
//
// Before the worker starts and after it stops (the single-threaded boot
// and shutdown phases) StorageWorker::run_sync() runs the job inline, so
// the cache works without a running worker too: every change is then
// written at once, on the calling thread.
//
// The keys and values sit in Mem::Large: the cache lives for the whole
// run and every entry is small, so plain malloc would put them all in
// internal RAM (AGENTS.md section 8). The worker's jobs point at this
// object, so it must outlive every job it queued: destroy it only after
// the worker has stopped or drained.

#include <cstdint>
#include <functional>
#include <map>
#include <set>
#include <span>
#include <string>
#include <string_view>
#include <vector>

#include "wisp/base/result.hpp"
#include "wisp/base/value.hpp"
#include "wisp/core/settings.hpp"
#include "wisp/core/storage_worker.hpp"
#include "wisp/os/large.hpp"
#include "wisp/os/os.hpp"

namespace wisp::core {

class CachedSettingsBackend final : public SettingsBackend {
public:
    // `on_persist_failed` runs on whichever thread ran the failed save (the
    // worker, or the caller in the boot/shutdown phases), without this
    // object's lock held. It may be empty.
    CachedSettingsBackend(SettingsBackend& platform, StorageWorker& worker,
                          std::function<void()> on_persist_failed);

    CachedSettingsBackend(const CachedSettingsBackend&) = delete;
    CachedSettingsBackend& operator=(const CachedSettingsBackend&) = delete;

    // Replaces the cache with platform.load_all(), run on the worker (or
    // inline before it starts). Call once, before the first get(); on
    // failure the cache stays as it was and the platform's error is
    // returned.
    Result<void> load();

    // Waits up to `timeout_ms` (or os::kForever) for every dirty key to be
    // written; Ok at once when nothing is dirty. Timeout if the worker did
    // not get to it in time (the save still happens later), Busy if its
    // queue was full, or the platform's error if the save failed. For the
    // factory reset, a restart and deep standby.
    Result<void> flush(uint32_t timeout_ms);

    Result<Value> get(const std::string& ns, const std::string& key) override;
    Result<void> set(const std::string& ns, const std::string& key, const Value& value) override;
    Result<void> erase(const std::string& ns, const std::string& key) override;
    // Clears the cache; the queued save erases every namespace of the
    // platform before it writes any key set after this call.
    Result<void> erase_all() override;
    // The cache's contents (no flash).
    Result<std::vector<SettingsEntry>> load_all() override;
    // Applies `changes` to the cache and queues one save, as the same
    // set()/erase() calls would.
    Result<void> write_batch(std::span<const SettingsChange> changes) override;

private:
    struct Name {
        os::LargeString ns;
        os::LargeString key;
    };
    struct NameView {
        std::string_view ns;
        std::string_view key;
    };
    struct NameLess {
        using is_transparent = void;
        static NameView view(const Name& n) {
            return {std::string_view(n.ns.data(), n.ns.size()),
                    std::string_view(n.key.data(), n.key.size())};
        }
        static NameView view(const NameView& n) { return n; }
        template <typename A, typename B>
        bool operator()(const A& a, const B& b) const {
            const NameView x = view(a);
            const NameView y = view(b);
            return x.ns != y.ns ? x.ns < y.ns : x.key < y.key;
        }
    };
    // A Value kept in Mem::Large: scalars as they are, a string's text, a
    // list or map as its JSON.
    struct Slot {
        Value::Type type = Value::Type::Null;
        bool b = false;
        int64_t i = 0;
        double d = 0;
        os::LargeString text;
    };
    using Cache = std::map<Name, Slot, NameLess, os::LargeAllocator<std::pair<const Name, Slot>>>;
    using Dirty = std::set<Name, NameLess, os::LargeAllocator<Name>>;

    static Slot to_slot(const Value& value);
    static Value from_slot(const Slot& slot);
    static Name make_name(std::string_view ns, std::string_view key);

    // Called with mutex_ held: stores `value` (erases the key when null)
    // and marks the key dirty. True when the caller must schedule() a
    // save, none being queued; it is then counted as queued.
    bool put_locked(std::string_view ns, std::string_view key, const Value* value);
    bool claim_save_locked();
    // Queues the save claimed by put_locked()/claim_save_locked().
    void schedule();
    // The save: on the worker, or inline outside its run.
    Result<void> persist();

    SettingsBackend& platform_;
    StorageWorker& worker_;
    std::function<void()> on_persist_failed_;

    os::Mutex mutex_;
    Cache cache_;
    Dirty dirty_;
    // erase_all() was called and the platform has not been erased since.
    bool erase_pending_ = false;
    // A save is queued and has not started yet.
    bool queued_ = false;
    // A save has taken the dirty keys and is writing them.
    bool saving_ = false;
    // Saves that failed in a row.
    int failures_ = 0;
};

}  // namespace wisp::core
