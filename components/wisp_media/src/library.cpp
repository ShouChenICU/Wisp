#include "wisp/media/library.hpp"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <optional>
#include <string_view>
#include <utility>

#include "index_store.hpp"
#include "wisp/core/log.hpp"
#include "wisp/media/path.hpp"
#include "wisp/media/tags.hpp"
#include "wisp/os/flash_guard.hpp"

namespace wisp::media {

namespace {

constexpr const char* kTag = "media.lib";

// Bytes read from the start of a track for its ID3v2 tag and stream
// header (media spec 3.3).
constexpr size_t kHeadBytes = 64 * 1024;
// Bytes read after an ID3v2 tag too large for the head read, enough to
// reach the first MPEG frame and its Xing/VBRI header.
constexpr size_t kInfoBytes = 16 * 1024;
// The least of the stream after the tag the head must hold to be parsed
// from it.
constexpr size_t kInfoMinBytes = 4096;
constexpr size_t kId3v1Bytes = 128;
// Unchanged files a scan job compares before it yields the worker.
constexpr size_t kComparesPerJob = 64;

constexpr std::string_view kPartSuffix = ".part";
constexpr std::string_view kDoneSuffix = ".done";

template <class T>
Result<T> fail(ErrorCode code, std::string msg) {
    return Result<T>::err(code, std::move(msg));
}

template <class T, class U>
Result<T> forward(const Result<U>& r) {
    return Result<T>::err(r.error().code, r.error().message);
}

bool ends_with(std::string_view s, std::string_view suffix) {
    return s.size() >= suffix.size() && s.substr(s.size() - suffix.size()) == suffix;
}

bool ends_with_ci(std::string_view s, std::string_view suffix) {
    if (s.size() < suffix.size()) return false;
    s = s.substr(s.size() - suffix.size());
    for (size_t i = 0; i < s.size(); ++i) {
        char c = s[i];
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        if (c != suffix[i]) return false;
    }
    return true;
}

// StorageManager's own write bookkeeping beside a file.
bool is_bookkeeping(std::string_view name) {
    return ends_with(name, ".tmp") || ends_with(name, ".new");
}

bool is_music(std::string_view rel) { return rel.substr(0, 6) == "music/"; }

bool contains_ci(std::string_view hay, std::string_view needle) {
    if (needle.empty()) return true;
    auto lower = [](char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; };
    auto it = std::search(hay.begin(), hay.end(), needle.begin(), needle.end(),
                          [&](char a, char b) { return lower(a) == lower(b); });
    return it != hay.end();
}

std::string_view sv(const os::LargeString& s) { return {s.data(), s.size()}; }
os::LargeString large(std::string_view s) { return os::LargeString(s.data(), s.size()); }

Track to_track(const IndexEntry& e) {
    return Track{std::string(sv(e.id)),
                 std::string(sv(e.title)),
                 std::string(sv(e.artist)),
                 std::string(sv(e.album)),
                 e.duration_s,
                 e.size,
                 e.mtime};
}

std::pair<std::string, std::string> split_parent(const std::string& rel) {
    size_t slash = rel.find_last_of('/');
    if (slash == std::string::npos) return {"", rel};
    return {rel.substr(0, slash), rel.substr(slash + 1)};
}

}  // namespace

// Everything here except the members marked otherwise is guarded by `mu`,
// which is held only for memory work. The scan state, the touched list and
// the save flag are used only by storage jobs, which run one at a time.
struct MediaLibrary::Impl {
    MediaLibrary& self;
    core::StorageManager& storage;
    core::StorageRouter* router;
    std::string root;

    mutable os::Mutex mu;
    // Bumped by every card change; a job that started under another value
    // belongs to a card that is gone.
    uint32_t gen = 0;
    bool available = false;
    Index index;
    // Target paths (relative) of the writers alive now.
    os::LargeVector<os::LargeString> writing;
    std::function<void(const std::string&)> on_removed;

    core::CellSubscription sd_sub;
    bool started = false;
    bool have_changes = false;
    uint32_t last_changes = 0;  // only on the bus callback

    // Shared with every queued job: the job's last act (finish_one()) may
    // be what lets stop() return and the library be destroyed, so what it
    // touches then must not live in the library.
    struct Jobs {
        std::atomic<bool> stopping{false};
        std::atomic<int> inflight{0};
        os::Semaphore idle{1, 0};
    };
    std::shared_ptr<Jobs> jobs = std::allocate_shared<Jobs>(os::LargeAllocator<Jobs>());
    std::atomic<bool>& stopping = jobs->stopping;

    // Storage jobs only.
    struct Pending {
        os::LargeString rel;
        uint64_t size;
        int64_t mtime;
    };
    struct Scan {
        uint32_t id = 0;
        uint32_t gen = 0;
        os::LargeVector<os::LargeString> dirs;
        os::LargeVector<Pending> files;
        Index found;
    };
    os::LargeUnique<Scan> scan;
    uint32_t scan_ids = 0;
    bool rescan_again = false;
    // Ids written or removed while a scan runs: the scan's view of them may
    // be older than the live index's.
    os::LargeVector<os::LargeString> touched;
    // The index on the card is older than the one in memory.
    bool index_dirty = false;
    bool save_queued = false;

    Impl(MediaLibrary& s, const MediaLibraryDeps& deps)
        : self(s), storage(deps.storage), router(deps.router), root(deps.root) {}

    std::string abs(std::string_view rel) const {
        return rel.empty() ? root : root + "/" + std::string(rel);
    }

    // ---- jobs ------------------------------------------------------------

    static void finish_one(Jobs& j) {
        if (j.inflight.fetch_sub(1) == 1 && j.stopping.load()) j.idle.give();
    }

    // Queues `job` on the card's worker. False when the library is
    // stopping or the lane is full; the job is then dropped.
    bool post(core::IoLane lane, std::function<void()> job) {
        jobs->inflight.fetch_add(1);
        if (stopping.load()) {
            finish_one(*jobs);
            return false;
        }
        if (router == nullptr) {
            job();
            finish_one(*jobs);
            return true;
        }
        if (!router->submit(
                core::Medium::Sd,
                [j = jobs, job = std::move(job)]() mutable {
                    job();
                    // Captures (a caller's completion among them) go before
                    // stop() may return.
                    job = nullptr;
                    finish_one(*j);
                },
                lane)) {
            finish_one(*jobs);
            return false;
        }
        return true;
    }

    // post() for a call with a completion: a refusal completes at once.
    template <class T>
    void call(Done<T> done, std::function<Result<T>()> fn) {
        auto shared = std::make_shared<Done<T>>(std::move(done));
        bool queued = post(core::IoLane::Normal, [shared, fn = std::move(fn)] { (*shared)(fn()); });
        if (!queued) {
            (*shared)(stopping.load()
                          ? fail<T>(ErrorCode::StorageUnavailable, "media library stopped")
                          : fail<T>(ErrorCode::Busy, "card queue full"));
        }
    }

    // ---- state -----------------------------------------------------------

    template <class F>
    void update_state_locked(F&& f) {
        self.state_.update(std::forward<F>(f));
    }

    void index_changed_locked() {
        update_state_locked([&](MediaLibraryState& s) {
            s.track_count = static_cast<uint32_t>(index.size());
            s.revision = s.revision + 1;
        });
    }

    Result<void> check_available() const {
        std::lock_guard<os::Mutex> lock(mu);
        if (!available) return Result<void>::err(ErrorCode::StorageUnavailable, "no card");
        return Result<void>::ok();
    }

    uint32_t current_gen() const {
        std::lock_guard<os::Mutex> lock(mu);
        return gen;
    }

    // ---- card follow (bus thread) ---------------------------------------

    void on_card(const core::SdStatus& sd) {
        if (have_changes && sd.changes == last_changes) return;
        have_changes = true;
        last_changes = sd.changes;
        uint32_t g = 0;
        {
            std::lock_guard<os::Mutex> lock(mu);
            g = ++gen;
            const bool had = available || !index.empty();
            available = false;
            Index().swap(index);
            update_state_locked([&](MediaLibraryState& s) {
                s.available = false;
                s.scanning = false;
                s.track_count = 0;
                if (had) s.revision = s.revision + 1;
            });
        }
        if (!sd.online) {
            WLOG_I(kTag, "card removed: index cleared");
            return;
        }
        if (!post(core::IoLane::Normal, [this, g] { mount(g); }))
            WLOG_W(kTag, "card queue full: media library not mounted (rescan retries)");
    }

    // ---- mount and scan (storage jobs) ----------------------------------

    void mount(uint32_t g) {
        if (current_gen() != g) return;
        for (std::string_view folder : kFolders) {
            if (auto r = storage.make_dirs(abs(folder)); !r)
                WLOG_W(kTag, "creating media/%.*s: %s", static_cast<int>(folder.size()),
                       folder.data(), r.error().message.c_str());
        }
        Index loaded;
        bool stale = true;
        Result<os::LargeBuffer> raw = storage.read_file_large(abs(kIndexFile));
        if (raw) {
            const auto& b = raw.value();
            auto decoded =
                decode_index(std::string_view(reinterpret_cast<const char*>(b.data()), b.size()));
            if (decoded) {
                loaded = std::move(decoded.value());
                stale = false;
            } else {
                WLOG_W(kTag, "index discarded: %s", decoded.error().message.c_str());
            }
        } else if (raw.error().code != ErrorCode::NotFound) {
            WLOG_W(kTag, "index unreadable: %s", raw.error().message.c_str());
        }
        {
            std::lock_guard<os::Mutex> lock(mu);
            if (gen != g) return;
            available = true;
            index = std::move(loaded);
            update_state_locked([&](MediaLibraryState& s) {
                s.available = true;
                s.track_count = static_cast<uint32_t>(index.size());
                s.revision = s.revision + 1;
            });
        }
        index_dirty = stale;
        start_scan(g);
    }

    void start_scan(uint32_t g) {
        if (scan && scan->gen == g) {
            rescan_again = true;
            return;
        }
        scan = os::make_large<Scan>();
        if (!scan) {
            WLOG_W(kTag, "no memory to scan music/");
            std::lock_guard<os::Mutex> lock(mu);
            // A rescan queued behind a finished one left `scanning` set.
            if (gen == g) update_state_locked([](MediaLibraryState& st) { st.scanning = false; });
            return;
        }
        scan->id = ++scan_ids;
        scan->gen = g;
        scan->dirs.push_back(large("music"));
        touched.clear();
        rescan_again = false;
        {
            std::lock_guard<os::Mutex> lock(mu);
            if (gen != g) {
                scan.reset();
                return;
            }
            update_state_locked([](MediaLibraryState& s) { s.scanning = true; });
        }
        queue_step(scan->id);
    }

    void queue_step(uint32_t id) {
        if (router == nullptr) {
            while (scan_step(id)) {
            }
            return;
        }
        if (!post(core::IoLane::Bulk, [this, id] {
                if (scan_step(id)) queue_step(id);
            })) {
            abandon_scan(id, "card queue full or stopping");
        }
    }

    void abandon_scan(uint32_t id, const char* why) {
        if (!scan || scan->id != id) return;
        const uint32_t g = scan->gen;
        scan.reset();
        std::lock_guard<os::Mutex> lock(mu);
        if (gen == g) {
            WLOG_W(kTag, "scan of music/ stopped: %s", why);
            update_state_locked([](MediaLibraryState& s) { s.scanning = false; });
        }
    }

    // One unit of a scan: a directory listing, one file's tag read, or up to
    // kComparesPerJob unchanged files. True when there is more to do.
    bool scan_step(uint32_t id) {
        if (!scan || scan->id != id) return false;
        if (current_gen() != scan->gen || stopping.load()) {
            scan.reset();
            return false;
        }
        Scan& s = *scan;
        if (!s.files.empty()) {
            for (size_t n = 0; n < kComparesPerJob && !s.files.empty(); ++n) {
                Pending p = std::move(s.files.back());
                s.files.pop_back();
                std::optional<IndexEntry> known = lookup(sv(p.rel));
                if (known && known->size == p.size && known->mtime == p.mtime) {
                    s.found.push_back(std::move(*known));
                    continue;
                }
                // Gone since its folder was visited: no entry.
                if (auto e = read_track(sv(p.rel), p.size, p.mtime))
                    s.found.push_back(std::move(*e));
                break;  // one tag read per job
            }
            return true;
        }
        if (!s.dirs.empty()) {
            std::string dir(sv(s.dirs.back()));
            s.dirs.pop_back();
            Result<void> r = walk_dir(dir, [&](const core::DirEntry& e) {
                std::string rel = dir + "/" + e.name;
                if (rel.size() > kMaxPathBytes) return;
                if (e.kind == core::EntryKind::Dir) {
                    s.dirs.push_back(large(rel));
                } else if (e.kind == core::EntryKind::File && is_track_name(e.name)) {
                    s.files.push_back(Pending{large(rel), e.size, e.mtime});
                }
            });
            if (!r) {
                if (r.error().code == ErrorCode::StorageUnavailable) {
                    scan.reset();
                    return false;
                }
                WLOG_W(kTag, "scan skips %s: %s", dir.c_str(), r.error().message.c_str());
            }
            return true;
        }
        finish_scan();
        return false;
    }

    void finish_scan() {
        Scan& s = *scan;
        sort_index(s.found);
        const uint32_t g = s.gen;
        bool changed = false;
        {
            std::lock_guard<os::Mutex> lock(mu);
            if (gen != g) {
                scan.reset();
                return;
            }
            for (const os::LargeString& id : touched) {
                auto it = std::lower_bound(
                    s.found.begin(), s.found.end(), id,
                    [](const IndexEntry& e, const os::LargeString& k) { return e.id < k; });
                if (it != s.found.end() && it->id == id) s.found.erase(it);
                if (auto live = find_locked(sv(id)); live != index.end()) s.found.push_back(*live);
            }
            if (!touched.empty()) sort_index(s.found);
            changed = s.found != index;
            if (changed) {
                index = std::move(s.found);
                index_changed_locked();
            }
            if (!rescan_again)
                update_state_locked([](MediaLibraryState& st) { st.scanning = false; });
        }
        touched.clear();
        scan.reset();
        if (changed) index_dirty = true;
        if (index_dirty) save_index();
        if (rescan_again) start_scan(g);
    }

    // Reads a track's tags and duration; a file that cannot be read or
    // parsed still becomes a track, titled after its name. None when the
    // file no longer exists.
    std::optional<IndexEntry> read_track(std::string_view rel, uint64_t size, int64_t mtime) {
        self.tag_reads_.fetch_add(1, std::memory_order_relaxed);
        IndexEntry e;
        e.id = large(rel);
        e.size = size;
        e.mtime = mtime;
        TrackTags tags;
        uint32_t duration_ms = 0;
        const std::string path = abs(rel);
        Result<os::LargeBuffer> head = storage.read_range(path, 0, kHeadBytes);
        if (head) {
            std::span<const uint8_t> h = head.value().span();
            if (ends_with_ci(rel, ".wav")) {
                if (auto w = parse_wav(h, size); w) duration_ms = w.value().duration_ms;
            } else {
                const size_t id3 = id3v2_size(h);
                if (id3 > 0) tags = parse_id3v2(h.first(std::min(id3, h.size())));
                bool has_v1 = false;
                if (size >= id3 + kId3v1Bytes) {
                    auto tail = storage.read_range(path, size - kId3v1Bytes, kId3v1Bytes);
                    if (tail && tail.value().size() == kId3v1Bytes &&
                        std::memcmp(tail.value().data(), "TAG", 3) == 0) {
                        has_v1 = true;
                        TrackTags v1 = parse_id3v1(tail.value().span());
                        if (tags.title.empty()) tags.title = std::move(v1.title);
                        if (tags.artist.empty()) tags.artist = std::move(v1.artist);
                        if (tags.album.empty()) tags.album = std::move(v1.album);
                    }
                }
                // A trailing ID3v1 tag is not audio: leaving it out keeps the
                // constant-bit-rate estimate from counting it.
                const uint64_t audio_end = has_v1 ? size - kId3v1Bytes : size;
                Result<Mp3Info> info = fail<Mp3Info>(ErrorCode::Corrupt, "no stream");
                if (id3 < h.size() && (h.size() - id3 >= kInfoMinBytes || h.size() == size)) {
                    info = parse_mp3_info(h.subspan(id3), audio_end, id3);
                } else if (id3 < size) {
                    if (auto more = storage.read_range(path, id3, kInfoBytes); more)
                        info = parse_mp3_info(more.value().span(), audio_end, id3);
                }
                if (info) duration_ms = info.value().duration_ms;
            }
        } else if (head.error().code == ErrorCode::NotFound) {
            return std::nullopt;
        } else {
            WLOG_W(kTag, "reading %.*s: %s", static_cast<int>(rel.size()), rel.data(),
                   head.error().message.c_str());
        }
        e.title = large(tags.title.empty() ? title_from_file_name(rel) : tags.title);
        e.artist = large(tags.artist);
        e.album = large(tags.album);
        e.duration_s = (duration_ms + 500) / 1000;
        return e;
    }

    void save_index() {
        os::LargeString text;
        {
            std::lock_guard<os::Mutex> lock(mu);
            if (!available) return;
            text = encode_index(index);
        }
        auto r = storage.write_atomic(
            abs(kIndexFile),
            std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(text.data()), text.size()));
        if (r) {
            index_dirty = false;
        } else {
            WLOG_W(kTag, "saving the index: %s", r.error().message.c_str());
        }
    }

    // A save after single-entry changes, coalesced: many removals in a row
    // write the index once or a few times, not once each.
    void save_soon() {
        index_dirty = true;
        if (save_queued || scan) return;  // a running scan saves when it ends
        save_queued = true;
        if (!post(core::IoLane::Bulk, [this] {
                save_queued = false;
                if (index_dirty && !scan) save_index();
            })) {
            save_queued = false;
        }
    }

    // ---- index lookups ---------------------------------------------------

    Index::iterator find_locked(std::string_view id) {
        auto it =
            std::lower_bound(index.begin(), index.end(), id,
                             [](const IndexEntry& e, std::string_view k) { return sv(e.id) < k; });
        return it != index.end() && sv(it->id) == id ? it : index.end();
    }

    std::optional<IndexEntry> lookup(std::string_view id) {
        std::lock_guard<os::Mutex> lock(mu);
        auto it = find_locked(id);
        if (it == index.end()) return std::nullopt;
        return *it;
    }

    void note_touched(std::string_view rel) {
        if (scan) touched.push_back(large(rel));
    }

    // After a file at `rel` was written: (re)reads it when it is a track.
    void after_write(std::string_view rel, uint32_t g) {
        if (!is_music(rel) || !is_track_name(rel)) return;
        auto [parent, name] = split_parent(std::string(rel));
        Result<core::DirEntry> e = entry_of(parent, name);
        if (!e || e.value().kind != core::EntryKind::File) return;
        std::optional<IndexEntry> read = read_track(rel, e.value().size, e.value().mtime);
        if (!read) return;
        IndexEntry entry = std::move(*read);
        {
            std::lock_guard<os::Mutex> lock(mu);
            if (gen != g) return;
            auto it = find_locked(rel);
            if (it != index.end()) {
                *it = std::move(entry);
            } else {
                auto at = std::lower_bound(
                    index.begin(), index.end(), entry.id,
                    [](const IndexEntry& a, const os::LargeString& k) { return a.id < k; });
                index.insert(at, std::move(entry));
            }
            index_changed_locked();
        }
        note_touched(rel);
        save_soon();
    }

    // After `rel` (a file, or a folder: everything under it) was removed.
    void after_remove(const std::string& rel, uint32_t g) {
        bool changed = false;
        {
            std::lock_guard<os::Mutex> lock(mu);
            if (gen != g) return;
            const std::string prefix = rel + "/";
            auto gone = std::remove_if(index.begin(), index.end(), [&](const IndexEntry& e) {
                std::string_view id = sv(e.id);
                return id == rel || id.substr(0, prefix.size()) == prefix;
            });
            changed = gone != index.end();
            if (changed) {
                index.erase(gone, index.end());
                index_changed_locked();
            }
        }
        // Also when the index had no entry: a running scan may have the file
        // queued from its folder's visit, and must not add it back.
        note_touched(rel);
        if (changed) save_soon();
        std::function<void(const std::string&)> cb;
        {
            std::lock_guard<os::Mutex> lock(mu);
            cb = on_removed;
        }
        if (cb) cb(rel);
    }

    // ---- directory helpers (storage jobs) -------------------------------

    bool is_writing(std::string_view rel) const {
        std::lock_guard<os::Mutex> lock(mu);
        return std::any_of(writing.begin(), writing.end(),
                           [&](const os::LargeString& w) { return sv(w) == rel; });
    }

    // Calls `visible` with each entry of `dir` a caller may see: no hidden
    // names, no write bookkeeping, no special files; in directory order.
    // Visited, not listed, so a folder of thousands of files builds no
    // listing in internal RAM. On the way, a writer's ".done" whose final
    // rename was interrupted is promoted when its target is missing
    // (dropped when it is not), and a ".part" no live writer owns is
    // removed (AGENTS.md section 8).
    Result<void> walk_dir(const std::string& dir,
                          const std::function<void(const core::DirEntry&)>& visible) {
        // visit_dir() holds the card's I/O lock: the leftovers are dealt
        // with after it returns.
        std::vector<std::string> leftovers;
        WISP_TRY(storage.visit_dir(abs(dir), [&](const core::DirEntry& e) {
            const std::string_view n = e.name;
            if (n.empty() || n[0] == '.') {
                if (e.kind == core::EntryKind::File &&
                    (ends_with(n, kDoneSuffix) || ends_with(n, kPartSuffix)))
                    leftovers.push_back(e.name);
                return true;
            }
            if (e.kind != core::EntryKind::Other && !is_bookkeeping(n)) visible(e);
            return true;
        }));
        for (const std::string& n : leftovers) {
            if (auto promoted = recover(dir, n)) visible(*promoted);
        }
        return Result<void>::ok();
    }

    // A writer's leftover `name` in `dir` (see walk_dir()); the promoted
    // target's entry when it became visible.
    std::optional<core::DirEntry> recover(const std::string& dir, const std::string& name) {
        const bool done = ends_with(name, kDoneSuffix);
        const size_t suffix = (done ? kDoneSuffix : kPartSuffix).size();
        if (name.size() <= 1 + suffix) return std::nullopt;
        const std::string target = name.substr(1, name.size() - 1 - suffix);
        const std::string rel = dir.empty() ? target : dir + "/" + target;
        if (is_writing(rel)) return std::nullopt;
        const std::string from = abs(dir) + "/" + name;
        if (!done || entry_of(dir, target)) {
            (void)storage.remove_tree(from);
            return std::nullopt;
        }
        if (!storage.rename(from, abs(rel))) return std::nullopt;
        WLOG_I(kTag, "recovered %s", rel.c_str());
        auto e = entry_of(dir, target);
        return e ? std::optional<core::DirEntry>(std::move(e.value())) : std::nullopt;
    }

    // Whether a regular file is at `path`: a zero-length read is a stat
    // that does not walk the folder.
    bool file_exists(const std::string& path) {
        return static_cast<bool>(storage.read_range(path, 0, 0));
    }

    // walk_dir()'s recovery for the one name `rel`, for the calls that name
    // a file without listing its folder (AGENTS.md section 8: recover on
    // read): a ".done" is promoted when the target is missing and dropped
    // when it is not, so it can never bring back a file removed later.
    // With `drop_part`, an unowned ".part" goes too (remove()).
    void resolve_leftovers(const std::string& rel, bool drop_part = false) {
        if (is_writing(rel)) return;
        auto [parent, name] = split_parent(rel);
        const std::string dir = abs(parent);
        const std::string done = dir + "/." + name + std::string(kDoneSuffix);
        if (drop_part) (void)storage.remove_tree(dir + "/." + name + std::string(kPartSuffix));
        if (!file_exists(done)) return;
        const std::string target = abs(rel);
        Result<os::LargeBuffer> probe = storage.read_range(target, 0, 0);
        if (!probe && probe.error().code == ErrorCode::NotFound) {
            if (storage.rename(done, target)) WLOG_I(kTag, "recovered %s", rel.c_str());
        } else {
            (void)storage.remove_tree(done);
        }
    }

    // One entry of `dir`, visited rather than listed: a folder may hold
    // thousands of files.
    Result<core::DirEntry> entry_of(const std::string& dir, const std::string& name) {
        std::optional<core::DirEntry> found;
        Result<void> r = storage.visit_dir(abs(dir), [&](const core::DirEntry& e) {
            if (e.name != name) return true;
            found = e;
            return false;
        });
        if (!r) return forward<core::DirEntry>(r);
        if (!found || found->kind == core::EntryKind::Other)
            return fail<core::DirEntry>(ErrorCode::NotFound, "not found");
        return Result<core::DirEntry>::ok(std::move(*found));
    }
};

// ---- MediaLibrary ---------------------------------------------------------

MediaLibrary::MediaLibrary(MediaLibraryDeps deps)
    : storage_(deps.storage), state_(deps.bus, MediaLibraryState{}, deps.mirror) {
    impl_ = os::make_large<Impl>(*this, deps);
    if (!impl_) std::abort();  // constructed once at boot; nothing works without it
}

MediaLibrary::~MediaLibrary() { stop(); }

void MediaLibrary::start() {
    Impl& m = *impl_;
    if (m.started || m.stopping.load()) return;
    m.started = true;
    if (!storage_.has_sd()) {
        WLOG_I(kTag, "no removable medium: media library off");
        return;
    }
    m.sd_sub = storage_.sd_cell().subscribe([&m](const core::SdStatus& sd) { m.on_card(sd); });
}

void MediaLibrary::stop() {
    Impl& m = *impl_;
    // The subscription first: its callback queues jobs.
    m.sd_sub.reset();
    m.stopping.store(true);
    while (m.jobs->inflight.load() > 0) m.jobs->idle.take(50);
    std::lock_guard<os::Mutex> lock(m.mu);
    m.available = false;
    m.update_state_locked([](MediaLibraryState& s) {
        s.available = false;
        s.scanning = false;
    });
}

void MediaLibrary::list(std::string dir, Done<std::vector<MediaEntry>> done) {
    using R = Result<std::vector<MediaEntry>>;
    Impl& m = *impl_;
    Result<std::string> rel = normalize_dir(dir);
    if (!rel) return done(forward<std::vector<MediaEntry>>(rel));
    if (auto a = m.check_available(); !a) return done(forward<std::vector<MediaEntry>>(a));
    m.call<std::vector<MediaEntry>>(std::move(done), [&m, rel = std::move(rel.value())]() -> R {
        WISP_TRY(m.check_available());
        std::vector<MediaEntry> out;
        WISP_TRY(m.walk_dir(rel, [&](const core::DirEntry& e) {
            out.push_back(MediaEntry{e.name, e.kind == core::EntryKind::Dir, e.size, e.mtime});
        }));
        std::sort(out.begin(), out.end(),
                  [](const MediaEntry& a, const MediaEntry& b) { return a.name < b.name; });
        return R::ok(std::move(out));
    });
}

void MediaLibrary::stat(std::string path, Done<MediaEntry> done) {
    using R = Result<MediaEntry>;
    Impl& m = *impl_;
    Result<std::string> rel = normalize(path);
    if (!rel) return done(forward<MediaEntry>(rel));
    if (auto a = m.check_available(); !a) return done(forward<MediaEntry>(a));
    m.call<MediaEntry>(std::move(done),
                       [this, rel = std::move(rel.value())]() -> R { return stat_now(rel); });
}

Result<MediaEntry> MediaLibrary::stat_now(const std::string& path) {
    using R = Result<MediaEntry>;
    WISP_ASSERT_SD_IO_THREAD("MediaLibrary::stat_now");
    Impl& m = *impl_;
    Result<std::string> rel = normalize(path);
    if (!rel) return forward<MediaEntry>(rel);
    WISP_TRY(m.check_available());
    auto [parent, name] = split_parent(rel.value());
    auto e = m.entry_of(parent, name);
    if (!e && e.error().code == ErrorCode::NotFound) {
        m.resolve_leftovers(rel.value());
        e = m.entry_of(parent, name);
    }
    if (!e) return forward<MediaEntry>(e);
    const core::DirEntry& d = e.value();
    return R::ok(MediaEntry{d.name, d.kind == core::EntryKind::Dir, d.size, d.mtime});
}

void MediaLibrary::read(std::string path, uint64_t off, size_t len, Done<os::LargeBuffer> done) {
    Impl& m = *impl_;
    Result<std::string> rel = normalize(path);
    if (!rel) return done(forward<os::LargeBuffer>(rel));
    if (auto a = m.check_available(); !a) return done(forward<os::LargeBuffer>(a));
    m.call<os::LargeBuffer>(std::move(done), [this, rel = std::move(rel.value()), off, len] {
        return read_now(rel, off, len);
    });
}

Result<os::LargeBuffer> MediaLibrary::read_now(const std::string& path, uint64_t off, size_t len) {
    WISP_ASSERT_SD_IO_THREAD("MediaLibrary::read_now");
    Impl& m = *impl_;
    Result<std::string> rel = normalize(path);
    if (!rel) return forward<os::LargeBuffer>(rel);
    WISP_TRY(m.check_available());
    const std::string path_abs = m.abs(rel.value());
    Result<os::LargeBuffer> r = storage_.read_range(path_abs, off, std::min(len, kMaxReadBytes));
    if (r || r.error().code != ErrorCode::NotFound) return r;
    // Only a missing file pays for the check: the player reads here often.
    m.resolve_leftovers(rel.value());
    return storage_.read_range(path_abs, off, std::min(len, kMaxReadBytes));
}

void MediaLibrary::write(std::string path, os::LargeBuffer data, Done<void> done) {
    Impl& m = *impl_;
    Result<std::string> rel = normalize_for_write(path);
    if (!rel) return done(forward<void>(rel));
    if (auto a = m.check_available(); !a) return done(a);
    auto buf = std::make_shared<os::LargeBuffer>(std::move(data));
    m.call<void>(std::move(done), [&m, rel = std::move(rel.value()), buf]() -> Result<void> {
        const uint32_t g = m.current_gen();
        WISP_TRY(m.check_available());
        if (m.is_writing(rel)) return Result<void>::err(ErrorCode::Busy, "being written");
        m.resolve_leftovers(rel);
        WISP_TRY(m.storage.write_atomic(m.abs(rel), buf->span()));
        m.after_write(rel, g);
        return Result<void>::ok();
    });
}

void MediaLibrary::remove(std::string path, Done<void> done) {
    Impl& m = *impl_;
    Result<std::string> rel = normalize_for_write(path);
    if (!rel) return done(forward<void>(rel));
    if (auto a = m.check_available(); !a) return done(a);
    m.call<void>(std::move(done), [&m, rel = std::move(rel.value())]() -> Result<void> {
        const uint32_t g = m.current_gen();
        WISP_TRY(m.check_available());
        if (m.is_writing(rel)) return Result<void>::err(ErrorCode::Busy, "being written");
        // First, so a ".done" of the file being removed cannot be promoted
        // after it is gone.
        m.resolve_leftovers(rel, /*drop_part=*/true);
        auto [parent, name] = split_parent(rel);
        auto e = m.entry_of(parent, name);
        if (!e) return forward<void>(e);
        if (e.value().kind == core::EntryKind::Dir) {
            bool empty = true;
            WISP_TRY(m.storage.visit_dir(m.abs(rel), [&](const core::DirEntry&) {
                empty = false;
                return false;
            }));
            if (!empty) return Result<void>::err(ErrorCode::InvalidArgument, "folder not empty");
        }
        WISP_TRY(m.storage.remove_tree(m.abs(rel)));
        m.after_remove(rel, g);
        return Result<void>::ok();
    });
}

void MediaLibrary::mkdir(std::string path, Done<void> done) {
    Impl& m = *impl_;
    Result<std::string> rel = normalize_for_write(path);
    if (!rel) return done(forward<void>(rel));
    if (auto a = m.check_available(); !a) return done(a);
    m.call<void>(std::move(done), [&m, rel = std::move(rel.value())]() -> Result<void> {
        WISP_TRY(m.check_available());
        return m.storage.make_dirs(m.abs(rel));
    });
}

void MediaLibrary::tracks(TrackQuery query, Done<TrackPage> done) {
    Impl& m = *impl_;
    TrackPage page;
    {
        std::lock_guard<os::Mutex> lock(m.mu);
        if (!m.available) return done(fail<TrackPage>(ErrorCode::StorageUnavailable, "no card"));
        const uint32_t limit = std::min(query.limit, kMaxTrackPage);
        uint32_t matched = 0;
        for (const IndexEntry& e : m.index) {
            if (!query.artist.empty() && sv(e.artist) != query.artist) continue;
            if (!query.album.empty() && sv(e.album) != query.album) continue;
            if (!query.q.empty() && !contains_ci(sv(e.title), query.q) &&
                !contains_ci(sv(e.artist), query.q) && !contains_ci(sv(e.album), query.q))
                continue;
            if (matched >= query.offset && page.items.size() < limit)
                page.items.push_back(to_track(e));
            ++matched;
        }
        page.total = matched;
    }
    done(Result<TrackPage>::ok(std::move(page)));
}

void MediaLibrary::rescan() {
    Impl& m = *impl_;
    if (!storage_.has_sd()) return;
    const bool online = storage_.sd_cell().get().online;
    m.post(core::IoLane::Bulk, [&m, online] {
        const uint32_t g = m.current_gen();
        if (m.check_available()) {
            m.start_scan(g);
        } else if (online && m.storage.sd_cell().get().online) {
            m.mount(g);
        }
    });
}

std::optional<Track> MediaLibrary::track(const std::string& id) const {
    Impl& m = *impl_;
    std::lock_guard<os::Mutex> lock(m.mu);
    auto it = m.find_locked(id);
    if (it == m.index.end()) return std::nullopt;
    return to_track(*it);
}

void MediaLibrary::set_on_removed(std::function<void(const std::string& path)> cb) {
    std::lock_guard<os::Mutex> lock(impl_->mu);
    impl_->on_removed = std::move(cb);
}

// ---- Writer ---------------------------------------------------------------

bool MediaLibrary::submit(core::IoLane lane, std::function<void()> job) {
    if (!job) return false;
    return impl_->post(lane, std::move(job));
}

Result<std::unique_ptr<MediaLibrary::Writer>> MediaLibrary::begin_write(std::string path) {
    using R = Result<std::unique_ptr<Writer>>;
    WISP_ASSERT_SD_IO_THREAD("MediaLibrary::begin_write");
    Impl& m = *impl_;
    Result<std::string> rel = normalize_for_write(path);
    if (!rel) return forward<std::unique_ptr<Writer>>(rel);
    if (is_bookkeeping(rel.value()))
        return R::err(ErrorCode::InvalidArgument, "reserved name: " + rel.value());
    const uint32_t g = m.current_gen();
    WISP_TRY(m.check_available());
    if (m.stopping.load()) return R::err(ErrorCode::StorageUnavailable, "media library stopped");
    {
        std::lock_guard<os::Mutex> lock(m.mu);
        for (const os::LargeString& w : m.writing)
            if (sv(w) == rel.value()) return R::err(ErrorCode::Busy, "being written");
    }
    m.resolve_leftovers(rel.value());
    auto [parent, name] = split_parent(rel.value());
    WISP_TRY(storage_.make_dirs(m.abs(parent)));
    std::unique_ptr<Writer> w(new Writer(*this, g, rel.value()));
    w->staging_ = os::LargeBuffer::allocate(core::kIoChunk);
    if (!w->staging_.ok()) {
        w->finished_ = true;
        return R::err(ErrorCode::NoMemory, "no memory for a write buffer");
    }
    {
        std::lock_guard<os::Mutex> lock(m.mu);
        m.writing.push_back(large(rel.value()));
    }
    // A ".part" left by an earlier writer of this name.
    if (auto r = storage_.remove_tree(w->part_); !r) {
        w->abort();
        return forward<std::unique_ptr<Writer>>(r);
    }
    return R::ok(std::move(w));
}

MediaLibrary::Writer::Writer(MediaLibrary& lib, uint32_t gen, std::string rel)
    : lib_(lib), gen_(gen), rel_(std::move(rel)) {
    auto [parent, name] = split_parent(rel_);
    const std::string dir = lib_.impl_->abs(parent);
    abs_ = dir + "/" + name;
    part_ = dir + "/." + name + std::string(kPartSuffix);
    done_ = dir + "/." + name + std::string(kDoneSuffix);
}

MediaLibrary::Writer::~Writer() {
    if (finished_) return;
    if (os::sd_io_allowed()) {
        abort();
    } else {
        // Left for the next listing or scan of its folder to remove.
        WLOG_W(kTag, "writer of %s dropped off the storage worker", rel_.c_str());
        finish();
    }
}

Result<void> MediaLibrary::Writer::flush() {
    if (staged_ == 0) return Result<void>::ok();
    auto r = lib_.storage_.append(part_, std::span<const uint8_t>(staging_.data(), staged_));
    staged_ = 0;
    return r;
}

Result<void> MediaLibrary::Writer::append(std::span<const uint8_t> data) {
    WISP_ASSERT_SD_IO_THREAD("MediaLibrary::Writer::append");
    if (finished_) return Result<void>::err(ErrorCode::InvalidArgument, "writer finished");
    if (lib_.impl_->current_gen() != gen_ || !lib_.impl_->check_available()) {
        abort();
        return Result<void>::err(ErrorCode::StorageUnavailable, "card changed");
    }
    while (!data.empty()) {
        const size_t n = std::min(data.size(), staging_.size() - staged_);
        std::memcpy(staging_.data() + staged_, data.data(), n);
        staged_ += n;
        written_ += n;
        data = data.subspan(n);
        if (staged_ == staging_.size()) {
            if (auto r = flush(); !r) {
                abort();
                return r;
            }
        }
    }
    return Result<void>::ok();
}

Result<void> MediaLibrary::Writer::commit() {
    WISP_ASSERT_SD_IO_THREAD("MediaLibrary::Writer::commit");
    if (finished_) return Result<void>::err(ErrorCode::InvalidArgument, "writer finished");
    Impl& m = *lib_.impl_;
    core::StorageManager& st = lib_.storage_;
    auto failed = [this](Result<void> r) {
        abort();
        return r;
    };
    if (m.current_gen() != gen_ || !m.check_available())
        return failed(Result<void>::err(ErrorCode::StorageUnavailable, "card changed"));
    if (auto r = flush(); !r) return failed(r);
    // An empty upload still makes a file.
    if (written_ == 0) {
        if (auto r = st.append(part_, {}); !r) return failed(r);
    }
    auto [parent, name] = split_parent(rel_);
    auto old = m.entry_of(parent, name);
    if (old && old.value().kind != core::EntryKind::File)
        return failed(Result<void>::err(ErrorCode::InvalidArgument, "a folder has that name"));
    if (auto r = st.remove_tree(done_); !r) return failed(r);
    if (auto r = st.rename(part_, done_); !r) return failed(r);
    // From here the data is complete under ".done": a power cut before the
    // last rename is recovered by the next listing of this folder.
    if (old) {
        if (auto r = st.remove_tree(abs_); !r) {
            finish();
            return r;
        }
    }
    Result<void> r = st.rename(done_, abs_);
    finish();
    if (!r) return r;
    m.after_write(rel_, gen_);
    return Result<void>::ok();
}

void MediaLibrary::Writer::abort() {
    if (finished_) return;
    WISP_ASSERT_SD_IO_THREAD("MediaLibrary::Writer::abort");
    (void)lib_.storage_.remove_tree(part_);
    finish();
}

void MediaLibrary::Writer::finish() {
    finished_ = true;
    staging_ = os::LargeBuffer();
    Impl& m = *lib_.impl_;
    std::lock_guard<os::Mutex> lock(m.mu);
    auto it = std::find_if(m.writing.begin(), m.writing.end(),
                           [&](const os::LargeString& w) { return sv(w) == rel_; });
    if (it != m.writing.end()) m.writing.erase(it);
}

}  // namespace wisp::media
