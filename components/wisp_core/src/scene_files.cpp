#include "wisp/core/scene_files.hpp"

#include <algorithm>
#include <cstring>
#include <mutex>
#include <utility>

#include "wisp/base/json.hpp"
#include "wisp/base/value.hpp"
#include "wisp/core/log.hpp"
#include "wisp/core/scene_manifest.hpp"

namespace wisp::core {

namespace {

constexpr const char* kTag = "scene_files";
constexpr std::string_view kDlSuffix = ".dl";
constexpr std::string_view kDoneSuffix = ".done";
constexpr const char* kCodes[] = {"bad_name", "too_large", "quota_exceeded", "unavailable",
                                  "not_found"};
// How long push() keeps trying when the worker's queue is full.
constexpr uint32_t kSubmitRetryMs = 10;
constexpr uint32_t kSubmitGiveUpMs = 5000;
// How often a push() waiting for room looks at cancel() in any case.
constexpr uint32_t kWaitSliceMs = 50;

bool ends_with(std::string_view s, std::string_view x) {
    return s.size() >= x.size() && s.substr(s.size() - x.size()) == x;
}

template <class T>
Result<T> fail(ErrorCode code, const char* msg) {
    return Result<T>::err(code, msg);
}

// A storage error in the form the file header promises.
template <class T>
Result<T> storage_fail(const Error& e) {
    switch (e.code) {
        case ErrorCode::StorageUnavailable:
            return Result<T>::err(e.code, "unavailable");
        case ErrorCode::NotFound:
            return Result<T>::err(e.code, "not_found");
        case ErrorCode::QuotaExceeded:
            return Result<T>::err(e.code, "quota_exceeded");
        default:
            WLOG_W(kTag, "%s", e.message.c_str());
            return Result<T>::err(e.code, e.message);
    }
}

bool contains(const std::vector<std::string>& v, std::string_view s) {
    return std::find(v.begin(), v.end(), s) != v.end();
}

char lower(char c) { return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c; }

bool same_folded(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (size_t i = 0; i < a.size(); ++i)
        if (lower(a[i]) != lower(b[i])) return false;
    return true;
}

// The entry named `name` in `entries`; with `fold`, one differing only in
// case when there is no exact match.
const DirEntry* find_entry(const std::vector<DirEntry>& entries, std::string_view name, bool fold) {
    for (const DirEntry& e : entries)
        if (e.name == name) return &e;
    if (fold)
        for (const DirEntry& e : entries)
            if (same_folded(e.name, name)) return &e;
    return nullptr;
}

}  // namespace

const char* to_string(FilesLocation l) {
    switch (l) {
        case FilesLocation::Sd:
            return "sd";
        case FilesLocation::Internal:
            return "int";
        case FilesLocation::None:
            break;
    }
    return "none";
}

bool valid_file_name(std::string_view name) {
    if (name.empty() || name.size() > 64 || name.front() == '.') return false;
    for (char c : name) {
        const bool ok = (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') ||
                        (c >= '0' && c <= '9') || c == '.' || c == '_' || c == '-';
        if (!ok) return false;
    }
    return !ends_with(name, ".tmp") && !ends_with(name, ".new");
}

const char* scene_files_code(const Error& e) {
    for (const char* code : kCodes)
        if (e.message == code) return code;
    return to_string(e.code);
}

std::string sd_scene_dir(const StorageManager& storage, std::string_view id) {
    // roots(Library) lists the card's library first, and only while the
    // card is online (storage_manager.hpp).
    std::vector<std::string> roots = storage.roots(Area::Library);
    if (roots.size() < 2) return {};
    return roots.front() + "/scenes/" + std::string(id);
}

SceneFiles::SceneFiles(StorageManager& storage, SceneFilesConfig config)
    : storage_(storage), config_(std::move(config)) {
    while (config_.data_root.size() > 1 && config_.data_root.back() == '/')
        config_.data_root.pop_back();
}

bool SceneFiles::sd_online() const { return storage_.roots(Area::Library).size() > 1; }

FilesWant SceneFiles::want_of(std::string_view scene) const {
    if (!config_.declared) return FilesWant::Auto;
    Result<FilesWant> w = config_.declared(scene);
    return w ? w.value() : FilesWant::Auto;
}

void SceneFiles::log_orphans() {
    if (!config_.declared) return;
    std::vector<std::string> roots = storage_.roots(Area::Library);
    if (roots.size() < 2) return;
    const std::string scenes = roots.front() + "/scenes";
    auto listed = storage_.list_dir(scenes);
    if (!listed) return;
    for (const DirEntry& e : listed.value()) {
        if (e.kind != EntryKind::Dir || !valid_scene_id(e.name)) continue;
        Result<FilesWant> w = config_.declared(e.name);
        if (w || w.error().code != ErrorCode::NotFound) continue;
        auto files = storage_.list_dir(scenes + "/" + e.name + "/files");
        if (!files) continue;
        // Kept: the scene may be installed again, or the user may want them.
        WLOG_W(kTag, "%s/%s/files: %zu entries of no installed scene, kept", scenes.c_str(),
               e.name.c_str(), files.value().size());
    }
}

uint64_t SceneFiles::quota_of(FilesLocation loc) const {
    return loc == FilesLocation::Sd ? config_.sd_quota : config_.int_quota;
}

std::string SceneFiles::record_path(std::string_view scene) const {
    return config_.data_root + "/" + std::string(scene) + "/data/files_loc.json";
}

std::string SceneFiles::dir_of(std::string_view scene, FilesLocation loc) const {
    if (loc == FilesLocation::Internal)
        return config_.data_root + "/" + std::string(scene) + "/files";
    if (loc == FilesLocation::Sd) {
        std::string sd = sd_scene_dir(storage_, scene);
        return sd.empty() ? sd : sd + "/files";
    }
    return {};
}

Result<uint64_t> SceneFiles::count_dir(const std::string& dir,
                                       const std::vector<std::string>& skip) {
    auto listed = storage_.list_dir(dir);
    if (!listed) {
        if (listed.error().code == ErrorCode::NotFound) return Result<uint64_t>::ok(0);
        return storage_fail<uint64_t>(listed.error());
    }
    bool changed = false;
    const bool fold = !dir.starts_with(config_.data_root + "/");
    for (const DirEntry& e : listed.value()) {
        std::string_view name = e.name;
        if (e.kind != EntryKind::File || name.empty() || name.front() != '.') continue;
        if (ends_with(name, kDlSuffix)) {
            std::string_view target = name.substr(1, name.size() - 1 - kDlSuffix.size());
            if (contains(skip, target)) continue;
            if (auto r = storage_.remove_tree(dir + "/" + e.name); !r)
                WLOG_W(kTag, "%s", r.error().message.c_str());
            changed = true;
        } else if (ends_with(name, kDoneSuffix)) {
            const std::string target(name.substr(1, name.size() - 1 - kDoneSuffix.size()));
            if (!valid_file_name(target) || contains(skip, target)) continue;
            // Complete, and newer than any file <name> still beside it. A
            // directory of that name is not ours to remove.
            const DirEntry* old = find_entry(listed.value(), target, fold);
            if (old != nullptr && old->kind != EntryKind::File) {
                WLOG_W(kTag, "%s/%s is not a file: %s kept, not finished", dir.c_str(),
                       old->name.c_str(), e.name.c_str());
                continue;
            }
            Result<void> r = Result<void>::ok();
            if (old != nullptr) r = storage_.remove_tree(dir + "/" + old->name);
            if (r) r = storage_.rename(dir + "/" + e.name, dir + "/" + target);
            if (!r) WLOG_W(kTag, "%s", r.error().message.c_str());
            changed = true;
        }
    }
    if (changed) {
        listed = storage_.list_dir(dir);
        if (!listed) return storage_fail<uint64_t>(listed.error());
    }
    uint64_t used = 0;
    for (const DirEntry& e : listed.value())
        if (e.kind == EntryKind::File && valid_file_name(e.name)) used += e.size;
    return Result<uint64_t>::ok(used);
}

Result<DirEntry> SceneFiles::entry_of(const std::string& dir, std::string_view name) {
    auto listed = storage_.list_dir(dir);
    if (!listed) return storage_fail<DirEntry>(listed.error());
    // Only the card is FAT; internal storage keeps case.
    const bool fold = !dir.starts_with(config_.data_root + "/");
    if (const DirEntry* e = find_entry(listed.value(), name, fold)) return Result<DirEntry>::ok(*e);
    return fail<DirEntry>(ErrorCode::NotFound, "not_found");
}

Result<uint64_t> SceneFiles::size_of(const std::string& dir, std::string_view name) {
    auto e = entry_of(dir, name);
    if (!e) return Result<uint64_t>::err(e.error().code, e.error().message);
    if (e.value().kind != EntryKind::File) return fail<uint64_t>(ErrorCode::NotFound, "not_found");
    return Result<uint64_t>::ok(e.value().size);
}

Result<SceneFiles::Record*> SceneFiles::ensure(std::string_view scene, bool choose) {
    using R = Result<Record*>;
    if (!valid_scene_id(scene)) return R::err(ErrorCode::InvalidArgument, "invalid scene id");
    Record* r = nullptr;
    {
        std::lock_guard<os::Mutex> lock(mu_);
        auto it = records_.find(scene);
        if (it == records_.end()) it = records_.emplace(std::string(scene), Record{}).first;
        r = &it->second;
    }
    if (!r->loaded) {
        FilesLocation loc = FilesLocation::None;
        auto raw = storage_.read_file_large(record_path(scene));
        if (raw) {
            auto parsed = from_json(std::string_view(
                reinterpret_cast<const char*>(raw.value().data()), raw.value().size()));
            const Value* v = parsed ? parsed.value().find("location") : nullptr;
            if (v != nullptr && v->type() == Value::Type::String) {
                if (v->as_string() == "sd") loc = FilesLocation::Sd;
                if (v->as_string() == "int") loc = FilesLocation::Internal;
            }
            if (loc == FilesLocation::None)
                WLOG_W(kTag, "%.*s: unreadable location record", static_cast<int>(scene.size()),
                       scene.data());
            const FilesWant want = loc == FilesLocation::None ? FilesWant::Auto : want_of(scene);
            if ((want == FilesWant::Sd && loc == FilesLocation::Internal) ||
                (want == FilesWant::Int && loc == FilesLocation::Sd)) {
                // spec 7.2: the first choice stands; files are never moved.
                WLOG_W(kTag, "%.*s: declares files \"%s\" but they are kept on %s",
                       static_cast<int>(scene.size()), scene.data(), to_string(want),
                       to_string(loc));
            }
        } else if (raw.error().code != ErrorCode::NotFound) {
            return storage_fail<Record*>(raw.error());
        }
        std::lock_guard<os::Mutex> lock(mu_);
        r->loc = loc;
        r->loaded = true;
        r->counted = false;
    }
    if (r->loc == FilesLocation::None) {
        const FilesWant want = want_of(scene);
        const bool card = sd_online();
        // "sd" never falls back to internal storage.
        if (want == FilesWant::Sd && !card)
            return fail<Record*>(ErrorCode::StorageUnavailable, "unavailable");
        if (!choose) return R::ok(r);
        const FilesLocation loc = want == FilesWant::Int
                                      ? FilesLocation::Internal
                                      : (card ? FilesLocation::Sd : FilesLocation::Internal);
        const std::string json = std::string(R"({"location":")") + to_string(loc) + "\"}";
        Result<void> saved = storage_.write_atomic(
            record_path(scene),
            std::span<const uint8_t>(reinterpret_cast<const uint8_t*>(json.data()), json.size()));
        if (!saved) return storage_fail<Record*>(saved.error());
        std::lock_guard<os::Mutex> lock(mu_);
        r->loc = loc;
        r->counted = false;
    }
    if (!r->counted) {
        const std::string dir = dir_of(scene, r->loc);
        if (dir.empty()) return fail<Record*>(ErrorCode::StorageUnavailable, "unavailable");
        std::vector<std::string> skip;
        {
            std::lock_guard<os::Mutex> lock(mu_);
            skip = r->downloading;
        }
        auto used = count_dir(dir, skip);
        if (!used) return R::err(used.error().code, used.error().message);
        std::lock_guard<os::Mutex> lock(mu_);
        r->used = used.value();
        r->counted = true;
    }
    return R::ok(r);
}

Result<void> SceneFiles::scan() {
    std::lock_guard<os::Mutex> io(io_mu_);
    auto listed = storage_.list_dir(config_.data_root);
    if (!listed) {
        if (listed.error().code == ErrorCode::NotFound) return Result<void>::ok();
        return Result<void>::err(listed.error().code, listed.error().message);
    }
    for (const DirEntry& e : listed.value()) {
        if (e.kind != EntryKind::Dir || !valid_scene_id(e.name)) continue;
        {
            std::lock_guard<os::Mutex> lock(mu_);
            auto it = records_.find(e.name);
            if (it != records_.end()) {
                it->second.loaded = false;
                it->second.counted = false;
            }
        }
        auto r = ensure(e.name, false);
        if (!r && r.error().code != ErrorCode::StorageUnavailable)
            WLOG_W(kTag, "%s: not counted: %s", e.name.c_str(), r.error().message.c_str());
    }
    log_orphans();
    return Result<void>::ok();
}

void SceneFiles::on_sd_changed() {
    std::lock_guard<os::Mutex> io(io_mu_);
    std::vector<std::string> on_card;
    {
        std::lock_guard<os::Mutex> lock(mu_);
        for (auto& [id, r] : records_) {
            if (r.loc != FilesLocation::Sd) continue;
            r.counted = false;
            on_card.push_back(id);
        }
    }
    if (!sd_online()) return;
    for (const std::string& id : on_card) {
        auto r = ensure(id, false);
        if (!r) WLOG_W(kTag, "%s: not counted: %s", id.c_str(), r.error().message.c_str());
    }
    log_orphans();
}

Result<void> SceneFiles::write(std::string_view scene, std::string_view name,
                               std::span<const uint8_t> data) {
    if (!valid_file_name(name)) return fail<void>(ErrorCode::InvalidArgument, "bad_name");
    if (data.size() > config_.max_write) return fail<void>(ErrorCode::InvalidArgument, "too_large");
    std::lock_guard<os::Mutex> io(io_mu_);
    auto rec = ensure(scene, true);
    if (!rec) return Result<void>::err(rec.error().code, rec.error().message);
    Record& r = *rec.value();
    const std::string dir = dir_of(scene, r.loc);
    if (dir.empty()) return fail<void>(ErrorCode::StorageUnavailable, "unavailable");
    auto old = size_of(dir, name);
    if (!old && old.error().code != ErrorCode::NotFound) {
        return Result<void>::err(old.error().code, old.error().message);
    }
    const uint64_t replaced = old ? old.value() : 0;
    {
        std::lock_guard<os::Mutex> lock(mu_);
        const uint64_t base = r.used + r.pending - std::min(replaced, r.used);
        if (base + data.size() > quota_of(r.loc))
            return fail<void>(ErrorCode::QuotaExceeded, "quota_exceeded");
        r.pending += data.size();
    }
    Result<void> written = storage_.write_atomic(dir + "/" + std::string(name), data);
    std::lock_guard<os::Mutex> lock(mu_);
    r.pending -= data.size();
    if (!written) return storage_fail<void>(written.error());
    r.used = r.used - std::min(replaced, r.used) + data.size();
    return Result<void>::ok();
}

Result<void> SceneFiles::append(std::string_view scene, std::string_view name,
                                std::span<const uint8_t> data) {
    if (!valid_file_name(name)) return fail<void>(ErrorCode::InvalidArgument, "bad_name");
    if (data.size() > config_.max_write) return fail<void>(ErrorCode::InvalidArgument, "too_large");
    std::lock_guard<os::Mutex> io(io_mu_);
    auto rec = ensure(scene, true);
    if (!rec) return Result<void>::err(rec.error().code, rec.error().message);
    Record& r = *rec.value();
    const std::string dir = dir_of(scene, r.loc);
    if (dir.empty()) return fail<void>(ErrorCode::StorageUnavailable, "unavailable");
    {
        std::lock_guard<os::Mutex> lock(mu_);
        if (r.used + r.pending + data.size() > quota_of(r.loc))
            return fail<void>(ErrorCode::QuotaExceeded, "quota_exceeded");
        r.pending += data.size();
    }
    Result<void> appended = storage_.append(dir + "/" + std::string(name), data);
    std::lock_guard<os::Mutex> lock(mu_);
    r.pending -= data.size();
    if (!appended) return storage_fail<void>(appended.error());
    r.used += data.size();
    return Result<void>::ok();
}

Result<os::LargeBuffer> SceneFiles::read(std::string_view scene, std::string_view name,
                                         uint64_t offset, size_t len) {
    using R = Result<os::LargeBuffer>;
    if (!valid_file_name(name))
        return fail<os::LargeBuffer>(ErrorCode::InvalidArgument, "bad_name");
    if (len > config_.max_read)
        return fail<os::LargeBuffer>(ErrorCode::InvalidArgument, "too_large");
    std::lock_guard<os::Mutex> io(io_mu_);
    auto rec = ensure(scene, false);
    if (!rec) return R::err(rec.error().code, rec.error().message);
    if (rec.value()->loc == FilesLocation::None)
        return fail<os::LargeBuffer>(ErrorCode::NotFound, "not_found");
    const std::string dir = dir_of(scene, rec.value()->loc);
    if (dir.empty()) return fail<os::LargeBuffer>(ErrorCode::StorageUnavailable, "unavailable");
    auto data = storage_.read_range(dir + "/" + std::string(name), offset, len);
    if (!data) return storage_fail<os::LargeBuffer>(data.error());
    return data;
}

Result<std::vector<SceneFileEntry>> SceneFiles::list(std::string_view scene) {
    using R = Result<std::vector<SceneFileEntry>>;
    std::vector<SceneFileEntry> out;
    std::lock_guard<os::Mutex> io(io_mu_);
    auto rec = ensure(scene, false);
    if (!rec) return R::err(rec.error().code, rec.error().message);
    if (rec.value()->loc == FilesLocation::None) return R::ok(std::move(out));
    const std::string dir = dir_of(scene, rec.value()->loc);
    if (dir.empty())
        return fail<std::vector<SceneFileEntry>>(ErrorCode::StorageUnavailable, "unavailable");
    auto listed = storage_.list_dir(dir);
    if (!listed) {
        if (listed.error().code == ErrorCode::NotFound) return R::ok(std::move(out));
        return storage_fail<std::vector<SceneFileEntry>>(listed.error());
    }
    for (DirEntry& e : listed.value()) {
        if (e.kind != EntryKind::File || !valid_file_name(e.name)) continue;
        if (out.size() == config_.max_list) break;
        out.push_back({std::move(e.name), e.size, e.mtime});
    }
    return R::ok(std::move(out));
}

Result<void> SceneFiles::remove(std::string_view scene, std::string_view name) {
    if (!valid_file_name(name)) return fail<void>(ErrorCode::InvalidArgument, "bad_name");
    std::lock_guard<os::Mutex> io(io_mu_);
    auto rec = ensure(scene, false);
    if (!rec) return Result<void>::err(rec.error().code, rec.error().message);
    Record& r = *rec.value();
    if (r.loc == FilesLocation::None) return fail<void>(ErrorCode::NotFound, "not_found");
    const std::string dir = dir_of(scene, r.loc);
    if (dir.empty()) return fail<void>(ErrorCode::StorageUnavailable, "unavailable");
    auto size = size_of(dir, name);
    if (!size) return Result<void>::err(size.error().code, size.error().message);
    Result<void> removed = storage_.remove_tree(dir + "/" + std::string(name));
    if (!removed) return storage_fail<void>(removed.error());
    std::lock_guard<os::Mutex> lock(mu_);
    r.used -= std::min(size.value(), r.used);
    return Result<void>::ok();
}

Result<SceneFilesInfo> SceneFiles::info(std::string_view scene) {
    using R = Result<SceneFilesInfo>;
    std::lock_guard<os::Mutex> io(io_mu_);
    auto rec = ensure(scene, false);
    if (!rec) return R::err(rec.error().code, rec.error().message);
    SceneFilesInfo out;
    // Asked before mu_: push() takes mu_ on the HTTP task, which must not
    // wait behind the storage lock.
    const bool card = sd_online();
    out.want = want_of(scene);
    std::lock_guard<os::Mutex> lock(mu_);
    const Record& r = *rec.value();
    out.location = r.loc;
    if (r.loc == FilesLocation::None) {
        const bool sd = out.want == FilesWant::Sd || (out.want == FilesWant::Auto && card);
        out.quota = quota_of(sd ? FilesLocation::Sd : FilesLocation::Internal);
        return R::ok(out);
    }
    out.quota = quota_of(r.loc);
    out.used = r.used + r.pending;
    return R::ok(out);
}

Result<void> SceneFiles::remove_all(std::string_view scene) {
    if (!valid_scene_id(scene)) return fail<void>(ErrorCode::InvalidArgument, "invalid scene id");
    std::lock_guard<os::Mutex> io(io_mu_);
    Result<void> first = storage_.remove_tree(dir_of(scene, FilesLocation::Internal));
    if (std::string sd = sd_scene_dir(storage_, scene); !sd.empty()) {
        Result<void> r = storage_.remove_tree(sd);
        if (first && !r) first = r;
    }
    Result<void> r = storage_.remove_tree(record_path(scene));
    if (first && !r) first = r;
    {
        std::lock_guard<os::Mutex> lock(mu_);
        auto it = records_.find(scene);
        // A download in flight keeps its entry; the next use reads the
        // (now missing) record again.
        if (it != records_.end()) {
            if (it->second.downloading.empty()) {
                records_.erase(it);
            } else {
                it->second.loaded = false;
                it->second.counted = false;
                it->second.used = 0;
            }
        }
    }
    if (!first) return storage_fail<void>(first.error());
    return first;
}

Result<std::shared_ptr<FileDownload>> SceneFiles::begin_download(std::string_view scene,
                                                                 std::string_view name) {
    using R = Result<std::shared_ptr<FileDownload>>;
    if (!valid_file_name(name)) return R::err(ErrorCode::InvalidArgument, "bad_name");
    std::lock_guard<os::Mutex> io(io_mu_);
    auto rec = ensure(scene, true);
    if (!rec) return R::err(rec.error().code, rec.error().message);
    Record& r = *rec.value();
    const std::string dir = dir_of(scene, r.loc);
    if (dir.empty()) return R::err(ErrorCode::StorageUnavailable, "unavailable");
    {
        std::lock_guard<os::Mutex> lock(mu_);
        if (contains(r.downloading, name))
            return R::err(ErrorCode::Busy, "a download of this file is in flight");
    }
    auto dl = std::allocate_shared<FileDownload>(os::LargeAllocator<FileDownload>(), *this,
                                                 std::string(scene), std::string(name), dir);
    Result<void> made = storage_.remove_tree(dl->dl_path());
    if (made) made = storage_.append(dl->dl_path(), {});
    if (!made) return storage_fail<std::shared_ptr<FileDownload>>(made.error());
    std::lock_guard<os::Mutex> lock(mu_);
    r.downloading.emplace_back(name);
    return R::ok(std::move(dl));
}

bool SceneFiles::reserve(const std::string& scene, uint64_t n) {
    std::lock_guard<os::Mutex> lock(mu_);
    auto it = records_.find(scene);
    if (it == records_.end()) return false;
    Record& r = it->second;
    if (r.used + r.pending + n > quota_of(r.loc)) return false;
    r.pending += n;
    return true;
}

void SceneFiles::release(const std::string& scene, const std::string& name, uint64_t reserved) {
    std::lock_guard<os::Mutex> lock(mu_);
    auto it = records_.find(scene);
    if (it == records_.end()) return;
    Record& r = it->second;
    r.pending -= std::min(reserved, r.pending);
    std::erase(r.downloading, name);
}

Result<uint64_t> SceneFiles::finish_download(FileDownload& dl, bool ok) {
    using R = Result<uint64_t>;
    os::LargeBuffer rest;
    bool good = false;
    uint64_t reserved = 0;
    uint64_t size = 0;
    {
        // From here on push() takes nothing more, so the reservation and
        // what was written are final (the appends queued before this job
        // have run: the worker is FIFO).
        std::lock_guard<os::Mutex> lock(dl.mu_);
        if (dl.finished_) return R::err(ErrorCode::Busy, "finished");
        dl.finished_ = true;
        good = ok && dl.error_.empty() && !dl.cancelled_;
        rest = std::move(dl.staging_);
        rest.truncate(dl.staged_);
        dl.staged_ = 0;
        reserved = dl.reserved_;
        size = dl.written_;
    }
    dl.wake_.give();
    std::lock_guard<os::Mutex> io(io_mu_);
    const std::string dl_path = dl.dl_path();
    const std::string target = dl.dir_ + "/" + dl.name_;
    const std::string done = dl.dir_ + "/." + dl.name_ + std::string(kDoneSuffix);
    Result<void> r = Result<void>::ok();
    uint64_t replaced = 0;
    if (good && rest.ok() && rest.size() != 0) {
        r = storage_.append(dl_path, rest.span());
        size += rest.size();
    }
    std::string old_name = dl.name_;
    bool blocked = false;
    if (good && r) {
        auto old = entry_of(dl.dir_, dl.name_);
        if (old) {
            old_name = old.value().name;
            replaced = old.value().kind == EntryKind::File ? old.value().size : 0;
            blocked = old.value().kind != EntryKind::File;
        }
        r = storage_.remove_tree(done);
        if (r) r = storage_.rename(dl_path, done);
        // A directory of that name is not ours to remove: both stay.
        if (r && blocked) {
            WLOG_W(kTag, "%s/%s is not a file: download kept as %s", dl.dir_.c_str(),
                   old_name.c_str(), done.c_str());
            r = Result<void>::err(ErrorCode::AlreadyExists, "not a file: " + old_name);
        }
        if (r && old) r = storage_.remove_tree(dl.dir_ + "/" + old_name);
        if (r) r = storage_.rename(done, target);
    }
    // A ".done" that a failed step left behind is complete: the next count
    // finishes it, so it stays (and the scene is counted again).
    bool recount = false;
    if (!good) {
        storage_.remove_tree(dl_path);
    } else if (!r) {
        storage_.remove_tree(dl_path);
        recount = true;
    }
    std::string error;
    {
        std::lock_guard<os::Mutex> lock(dl.mu_);
        if (good && !r)
            dl.fail_locked(r.error().code == ErrorCode::StorageUnavailable
                               ? "unavailable"
                               : to_string(r.error().code));
        error = dl.error_;
    }
    {
        std::lock_guard<os::Mutex> lock(mu_);
        auto it = records_.find(dl.scene_);
        if (it != records_.end()) {
            Record& rec = it->second;
            rec.pending -= std::min(reserved, rec.pending);
            std::erase(rec.downloading, dl.name_);
            if (good && r) rec.used = rec.used - std::min(replaced, rec.used) + size;
            if (recount) rec.counted = false;
        }
    }
    if (good && r) return R::ok(size);
    if (!r) WLOG_W(kTag, "%s: download not kept: %s", dl.scene_.c_str(), r.error().message.c_str());
    if (error == "quota_exceeded") return R::err(ErrorCode::QuotaExceeded, "quota_exceeded");
    if (error == "unavailable") return R::err(ErrorCode::StorageUnavailable, "unavailable");
    if (!error.empty()) return R::err(ErrorCode::IoError, error);
    return R::err(ErrorCode::IoError, "download discarded");
}

FileDownload::FileDownload(SceneFiles& files, std::string scene, std::string name, std::string dir)
    : files_(files), scene_(std::move(scene)), name_(std::move(name)), dir_(std::move(dir)) {}

FileDownload::~FileDownload() = default;

std::string FileDownload::dl_path() const { return dir_ + "/." + name_ + std::string(kDlSuffix); }

Result<uint64_t> FileDownload::finish(bool ok) { return files_.finish_download(*this, ok); }

void FileDownload::fail_locked(const char* code) {
    if (error_.empty()) error_ = code;
}

std::string FileDownload::error() const {
    std::lock_guard<os::Mutex> lock(mu_);
    return error_;
}

void FileDownload::cancel() {
    {
        std::lock_guard<os::Mutex> lock(mu_);
        cancelled_ = true;
    }
    wake_.give();
}

void FileDownload::abandon() {
    uint64_t reserved = 0;
    {
        std::lock_guard<os::Mutex> lock(mu_);
        if (finished_) return;
        finished_ = true;
        reserved = reserved_;
        staging_ = os::LargeBuffer();
        staged_ = 0;
    }
    wake_.give();
    files_.release(scene_, name_, reserved);
}

bool FileDownload::push(std::span<const uint8_t> chunk) {
    {
        // Reserved under mu_ (taken before SceneFiles' table lock, never
        // after it), so finish() reads a reserved_ that matches the table.
        std::lock_guard<os::Mutex> lock(mu_);
        if (!error_.empty() || cancelled_ || finished_) return false;
        if (chunk.empty()) return true;
        if (!files_.reserve(scene_, chunk.size())) {
            fail_locked("quota_exceeded");
            return false;
        }
        reserved_ += chunk.size();
    }
    while (!chunk.empty()) {
        os::LargeBuffer full;
        {
            std::lock_guard<os::Mutex> lock(mu_);
            if (!error_.empty() || cancelled_ || finished_) return false;
            if (!staging_.ok()) {
                staging_ = os::LargeBuffer::allocate(kChunk);
                staged_ = 0;
                if (!staging_.ok()) {
                    fail_locked("no_memory");
                    return false;
                }
            }
            const size_t n = std::min(chunk.size(), kChunk - staged_);
            std::memcpy(staging_.data() + staged_, chunk.data(), n);
            staged_ += n;
            chunk = chunk.subspan(n);
            if (staged_ == kChunk) {
                full = std::move(staging_);
                staged_ = 0;
                queued_ += kChunk;
            }
        }
        if (full.ok() && !dispatch(std::move(full))) return false;
        // Back-pressure: the HTTP task waits for the worker, not the other
        // way round, and cancel() or a failure ends the wait.
        for (;;) {
            {
                std::lock_guard<os::Mutex> lock(mu_);
                if (!error_.empty() || cancelled_ || finished_) return false;
                if (queued_ <= kMaxQueued) break;
            }
            wake_.take(kWaitSliceMs);
        }
    }
    return true;
}

bool FileDownload::dispatch(os::LargeBuffer buf) {
    const auto& submit = files_.config_.submit_io;
    if (!submit) {
        append(buf);
        std::lock_guard<os::Mutex> lock(mu_);
        return error_.empty();
    }
    // The job keeps this object alive until it has run, and owns the bytes.
    auto job = [self = shared_from_this(), holder = std::make_shared<os::LargeBuffer>(
                                               std::move(buf))] { self->append(*holder); };
    for (uint32_t waited = 0;; waited += kSubmitRetryMs) {
        if (submit(job)) return true;
        std::lock_guard<os::Mutex> lock(mu_);
        if (cancelled_ || finished_ || waited >= kSubmitGiveUpMs) {
            queued_ -= std::min(queued_, kChunk);
            fail_locked("busy");
            return false;
        }
        os::sleep_ms(kSubmitRetryMs);
    }
}

void FileDownload::append(const os::LargeBuffer& buf) {
    {
        std::lock_guard<os::Mutex> lock(mu_);
        if (!error_.empty() || cancelled_ || finished_) {
            queued_ -= std::min(queued_, buf.size());
            wake_.give();
            return;
        }
    }
    Result<void> r = files_.storage_.append(dl_path(), buf.span());
    {
        std::lock_guard<os::Mutex> lock(mu_);
        queued_ -= std::min(queued_, buf.size());
        if (r) {
            written_ += buf.size();
        } else {
            fail_locked(r.error().code == ErrorCode::StorageUnavailable
                            ? "unavailable"
                            : to_string(r.error().code));
        }
    }
    wake_.give();
}

}  // namespace wisp::core
