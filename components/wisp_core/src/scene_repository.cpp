#include "wisp/core/scene_repository.hpp"

#include <algorithm>
#include <iterator>
#include <mutex>
#include <string>
#include <utility>

#include "wisp/core/log.hpp"
#include "wisp/core/scene_files.hpp"
#include "wisp/core/zip.hpp"

namespace wisp::core {

namespace {

constexpr const char* kTag = "scene";

constexpr std::string_view kManifest = "scene.json";
constexpr std::string_view kTmpSuffix = ".tmpdir";
constexpr std::string_view kNewSuffix = ".newdir";
// The previous versions, one directory per scene (file header).
constexpr std::string_view kPrevDir = ".prev";
// Same bound as the zip reader's entry limit, so a dev working directory
// cannot hold a package that could never be installed.
constexpr size_t kMaxFiles = 256;

// A card slot exists but holds no online card.
bool sd_card_out(const StorageManager& storage) {
    for (const MediaStatus& m : storage.status().media)
        if (!m.online) return true;
    return false;
}
// The directories of a scene's own that sit inside its package directory
// on the device and never travel in a package: SceneStore's data/ and
// SceneFiles' files/.
constexpr std::string_view kSceneOwned[] = {"data", "files"};

bool starts_with(std::string_view s, std::string_view p) { return s.substr(0, p.size()) == p; }

bool ends_with(std::string_view s, std::string_view x) {
    return s.size() >= x.size() && s.substr(s.size() - x.size()) == x;
}

std::string_view last_segment(std::string_view path) {
    size_t slash = path.find_last_of('/');
    return slash == std::string_view::npos ? path : path.substr(slash + 1);
}

// StorageManager's write_atomic() bookkeeping names; never package files.
bool reserved_name(std::string_view name) {
    return ends_with(name, ".tmp") || ends_with(name, ".new");
}

// True if any '/'-separated segment of `path` is a bookkeeping name. A
// directory with such a name would be written but then skipped by every
// walk below, silently dropping the files inside it.
bool has_reserved_segment(std::string_view path) {
    while (!path.empty()) {
        size_t slash = path.find('/');
        if (reserved_name(path.substr(0, slash))) {
            return true;
        }
        if (slash == std::string_view::npos) {
            break;
        }
        path = path.substr(slash + 1);
    }
    return false;
}

// The files a package may contain: scene.json, the entry, lib/**/*.lua and
// assets/**. data/ belongs to SceneStore and never travels in a package.
bool allowed_file(std::string_view path, std::string_view entry) {
    if (has_reserved_segment(path)) {
        return false;
    }
    if (path == kManifest || path == entry) {
        return true;
    }
    if (starts_with(path, "lib/")) {
        return ends_with(path, ".lua") && last_segment(path).size() > 4;
    }
    return starts_with(path, "assets/") && path.size() > 7;
}

bool allowed_dir(std::string_view path) {
    return (starts_with(path, "lib/") || starts_with(path, "assets/")) &&
           !has_reserved_segment(path);
}

bool scene_owned(std::string_view name) {
    return std::find(std::begin(kSceneOwned), std::end(kSceneOwned), name) != std::end(kSceneOwned);
}

bool is_lua(std::string_view path) { return ends_with(path, ".lua"); }

std::string trim_slash(std::string s) {
    while (s.size() > 1 && s.back() == '/') {
        s.pop_back();
    }
    return s;
}

enum class Kind { Missing, Dir, Other };

Result<Kind> probe(StorageManager& storage, const std::string& path) {
    Result<std::vector<DirEntry>> r = storage.list_dir(path);
    if (r) {
        return Result<Kind>::ok(Kind::Dir);
    }
    switch (r.error().code) {
        case ErrorCode::NotFound:
            return Result<Kind>::ok(Kind::Missing);
        case ErrorCode::InvalidArgument:  // exists, but is not a directory
            return Result<Kind>::ok(Kind::Other);
        default:
            return Result<Kind>::err(r.error().code, r.error().message);
    }
}

// Calls fn(relative path, size) for every regular file under
// <base>/<prefix>, skipping write_atomic() bookkeeping. A missing directory
// has no files. The storage manager's 16-level path limit bounds the walk.
template <class Fn>
Result<void> walk_files(StorageManager& storage, const std::string& base, const std::string& prefix,
                        Fn&& fn) {
    std::vector<std::string> pending{prefix};
    while (!pending.empty()) {
        std::string rel = std::move(pending.back());
        pending.pop_back();
        Result<std::vector<DirEntry>> listed = storage.list_dir(base + "/" + rel);
        if (!listed) {
            if (listed.error().code == ErrorCode::NotFound) {
                continue;
            }
            return Result<void>::err(listed.error().code, listed.error().message);
        }
        for (const DirEntry& e : listed.value()) {
            if (reserved_name(e.name)) {
                continue;
            }
            std::string child = rel + "/" + e.name;
            if (e.kind == EntryKind::Dir) {
                pending.push_back(std::move(child));
            } else if (e.kind == EntryKind::File) {
                WISP_TRY(fn(child, e.size));
            }
        }
    }
    return Result<void>::ok();
}

// Calls fn(relative path, size) for every file of the package in `dir`
// (see allowed_file()); anything else in the directory is ignored.
template <class Fn>
Result<void> walk_package(StorageManager& storage, const std::string& dir, const std::string& entry,
                          Fn&& fn) {
    size_t count = 0;
    auto visit = [&](const std::string& rel, uint64_t size) -> Result<void> {
        if (!allowed_file(rel, entry)) {
            return Result<void>::ok();
        }
        if (++count > kMaxFiles) {
            return Result<void>::err(ErrorCode::InvalidArgument,
                                     "scene package has more than 256 files: " + dir);
        }
        return fn(rel, size);
    };

    Result<std::vector<DirEntry>> top = storage.list_dir(dir);
    if (!top) {
        return Result<void>::err(top.error().code, top.error().message);
    }
    for (const DirEntry& e : top.value()) {
        if (e.kind == EntryKind::File && (e.name == kManifest || e.name == entry)) {
            WISP_TRY(visit(e.name, e.size));
        }
    }
    WISP_TRY(walk_files(storage, dir, "lib", visit));
    WISP_TRY(walk_files(storage, dir, "assets", visit));
    return Result<void>::ok();
}

std::string join_hosts(const std::vector<std::string>& hosts) {
    std::string out = "[";
    for (size_t i = 0; i < hosts.size(); ++i) {
        if (i > 0) {
            out += ", ";
        }
        out += hosts[i];
    }
    out += "]";
    return out;
}

template <class T>
Result<T> fail(const Error& e) {
    return Result<T>::err(e.code, e.message);
}

}  // namespace

bool valid_package_path(std::string_view path) {
    if (path.empty() || path.front() == '/' || path.find('\\') != std::string_view::npos) {
        return false;
    }
    for (std::string_view rest = path; !rest.empty();) {
        const size_t slash = rest.find('/');
        const std::string_view seg = rest.substr(0, slash);
        if (seg.empty() || seg == "." || seg == ".." || reserved_name(seg)) {
            return false;
        }
        if (slash == std::string_view::npos) {
            break;
        }
        rest = rest.substr(slash + 1);
        if (rest.empty()) {
            return false;  // a trailing '/'
        }
    }
    if (path == kManifest) {
        return true;
    }
    if (path.find('/') == std::string_view::npos) {
        return is_lua(path) && path.size() > 4;
    }
    return allowed_file(path, kManifest);
}

SceneRepository::SceneRepository(StorageManager& storage, SceneRepoConfig config)
    : storage_(storage), config_(std::move(config)) {
    config_.code_root = trim_slash(std::move(config_.code_root));
    config_.data_root = trim_slash(std::move(config_.data_root));
}

std::string SceneRepository::code_dir(std::string_view id) const {
    return config_.code_root + "/" + std::string(id);
}

std::string SceneRepository::data_dir(std::string_view id) const {
    return config_.data_root + "/" + std::string(id) + "/data";
}

std::string SceneRepository::files_dir(std::string_view id) const {
    return config_.data_root + "/" + std::string(id) + "/files";
}

Result<SceneManifest> SceneRepository::read_manifest(std::string_view id) {
    return read_manifest_in(code_dir(id), id);
}

Result<SceneManifest> SceneRepository::read_prev_manifest(std::string_view id) {
    return read_manifest_in(prev_dir(id), id);
}

Result<SceneManifest> SceneRepository::read_manifest_in(const std::string& dir,
                                                        std::string_view id) {
    if (!valid_scene_id(id)) {
        return Result<SceneManifest>::err(ErrorCode::InvalidArgument,
                                          "invalid scene id: " + std::string(id));
    }
    Result<os::LargeBuffer> raw = storage_.read_file_large(dir + "/" + std::string(kManifest));
    if (!raw) {
        if (raw.error().code == ErrorCode::NotFound) {
            return Result<SceneManifest>::err(ErrorCode::NotFound,
                                              "scene not installed: " + std::string(id));
        }
        return fail<SceneManifest>(raw.error());
    }
    const os::LargeBuffer& buf = raw.value();
    Result<SceneManifest> m = parse_scene_manifest(
        std::string_view(reinterpret_cast<const char*>(buf.data()), buf.size()));
    if (!m) {
        return m;
    }
    if (m.value().id != id) {
        return Result<SceneManifest>::err(ErrorCode::InvalidArgument,
                                          "scene.json: id '" + m.value().id +
                                              "' does not match its directory '" + std::string(id) +
                                              "'");
    }
    return m;
}

Result<SceneManifest> SceneRepository::manifest(std::string_view id) {
    return call_on_worker<SceneManifest>(config_.worker, [&] { return manifest_here(id); });
}

Result<SceneManifest> SceneRepository::manifest_here(std::string_view id) {
    std::lock_guard<os::Mutex> lock(mutex_);
    return read_manifest(id);
}

Result<os::LargeBuffer> SceneRepository::read_asset(std::string_view id, std::string_view path,
                                                    size_t max_bytes) {
    return call_on_worker<os::LargeBuffer>(config_.worker,
                                           [&] { return read_asset_here(id, path, max_bytes); });
}

Result<os::LargeBuffer> SceneRepository::read_asset_here(std::string_view id, std::string_view path,
                                                         size_t max_bytes) {
    using R = Result<os::LargeBuffer>;
    if (!valid_scene_id(id)) return R::err(ErrorCode::InvalidArgument, "bad scene id");
    if (!valid_package_path(path) || path.rfind("assets/", 0) != 0)
        return R::err(ErrorCode::InvalidArgument, "not an asset path");
    std::lock_guard<os::Mutex> lock(mutex_);
    // One byte past the limit tells a file that is too large.
    R read = storage_.read_range(code_dir(id) + "/" + std::string(path), 0, max_bytes + 1);
    if (!read) return read;
    if (read.value().size() > max_bytes) return R::err(ErrorCode::QuotaExceeded, "too_large");
    return read;
}

Result<SceneInfo> SceneRepository::info(std::string_view id) {
    return call_on_worker<SceneInfo>(config_.worker, [&] { return info_here(id); });
}

Result<SceneInfo> SceneRepository::info_here(std::string_view id) {
    std::lock_guard<os::Mutex> lock(mutex_);
    return info_locked(id);
}

Result<SceneInfo> SceneRepository::info_locked(std::string_view id) {
    Result<SceneManifest> m = read_manifest(id);
    if (!m) {
        return fail<SceneInfo>(m.error());
    }
    SceneInfo info;
    info.manifest = std::move(m.value());
    Result<void> walked = walk_package(storage_, code_dir(id), info.manifest.entry,
                                       [&](const std::string&, uint64_t size) -> Result<void> {
                                           info.code_bytes += size;
                                           return Result<void>::ok();
                                       });
    if (!walked) {
        return fail<SceneInfo>(walked.error());
    }
    Result<uint64_t> data = storage_.tree_size(data_dir(id));
    if (!data) {
        return fail<SceneInfo>(data.error());
    }
    info.data_bytes = data.value();
    return Result<SceneInfo>::ok(std::move(info));
}

Result<std::vector<SceneInfo>> SceneRepository::list() {
    return call_on_worker<std::vector<SceneInfo>>(config_.worker, [&] { return list_here(); });
}

Result<std::vector<SceneInfo>> SceneRepository::list_here() {
    using R = Result<std::vector<SceneInfo>>;
    std::lock_guard<os::Mutex> lock(mutex_);
    std::vector<SceneInfo> out;
    Result<std::vector<DirEntry>> listed = storage_.list_dir(config_.code_root);
    if (!listed) {
        if (listed.error().code == ErrorCode::NotFound) {
            return R::ok(std::move(out));
        }
        return fail<std::vector<SceneInfo>>(listed.error());
    }
    for (const DirEntry& e : listed.value()) {
        if (e.kind != EntryKind::Dir || !valid_scene_id(e.name)) {
            continue;
        }
        Result<SceneInfo> one = info_locked(e.name);
        if (one) {
            out.push_back(std::move(one.value()));
        } else if (one.error().code != ErrorCode::NotFound) {
            // A directory left holding only kept data is not worth a warning.
            WLOG_W(kTag, "skipping scene '%s': %s", e.name.c_str(), one.error().message.c_str());
        }
    }
    return R::ok(std::move(out));
}

Result<std::vector<SceneManifest>> SceneRepository::manifests() {
    return call_on_worker<std::vector<SceneManifest>>(config_.worker,
                                                      [&] { return manifests_here(); });
}

Result<std::vector<SceneManifest>> SceneRepository::manifests_here() {
    using R = Result<std::vector<SceneManifest>>;
    std::lock_guard<os::Mutex> lock(mutex_);
    std::vector<SceneManifest> out;
    Result<std::vector<DirEntry>> listed = storage_.list_dir(config_.code_root);
    if (!listed) {
        if (listed.error().code == ErrorCode::NotFound) return R::ok(std::move(out));
        return fail<std::vector<SceneManifest>>(listed.error());
    }
    for (const DirEntry& e : listed.value()) {
        if (e.kind != EntryKind::Dir || !valid_scene_id(e.name)) continue;
        Result<SceneManifest> m = read_manifest(e.name);
        if (m) {
            out.push_back(std::move(m.value()));
        } else if (m.error().code != ErrorCode::NotFound) {
            WLOG_W(kTag, "skipping scene '%s': %s", e.name.c_str(), m.error().message.c_str());
        }
    }
    return R::ok(std::move(out));
}

std::vector<std::string> SceneRepository::changed_since(uint64_t since, uint64_t& now) const {
    std::lock_guard<os::Mutex> lock(marks_mutex_);
    now = changes_;
    std::vector<std::string> out;
    for (const auto& [id, at] : changed_)
        if (at > since) out.push_back(id);
    return out;
}

void SceneRepository::note_change(std::string_view id) {
    std::lock_guard<os::Mutex> lock(marks_mutex_);
    ++changes_;
    for (auto& [known, at] : changed_) {
        if (known == id) {
            at = changes_;
            return;
        }
    }
    changed_.emplace_back(std::string(id), changes_);
}

Result<ScenePackage> SceneRepository::load(std::string_view id) {
    return call_on_worker<ScenePackage>(config_.worker, [&] { return load_here(id); });
}

Result<ScenePackage> SceneRepository::load_here(std::string_view id) {
    using R = Result<ScenePackage>;
    std::lock_guard<os::Mutex> lock(mutex_);
    Result<SceneManifest> m = read_manifest(id);
    if (!m) {
        return fail<ScenePackage>(m.error());
    }
    ScenePackage pkg;
    pkg.manifest = std::move(m.value());
    const std::string dir = code_dir(id);
    size_t total = 0;
    bool has_entry = false;
    bool has_icon = pkg.manifest.icon.empty();
    Result<void> walked = walk_package(
        storage_, dir, pkg.manifest.entry, [&](const std::string& rel, uint64_t) -> Result<void> {
            Result<os::LargeBuffer> buf = storage_.read_file_large(dir + "/" + rel);
            if (!buf) {
                return Result<void>::err(buf.error().code, buf.error().message);
            }
            total += buf.value().size();
            if (total > config_.max_code_bytes) {
                return Result<void>::err(ErrorCode::QuotaExceeded,
                                         "scene package is larger than " +
                                             std::to_string(config_.max_code_bytes) + " bytes");
            }
            has_entry = has_entry || rel == pkg.manifest.entry;
            has_icon = has_icon || rel == pkg.manifest.icon;
            pkg.files.emplace_back(rel, std::move(buf.value()));
            return Result<void>::ok();
        });
    if (!walked) {
        return fail<ScenePackage>(walked.error());
    }
    if (!has_entry) {
        return R::err(ErrorCode::InvalidArgument, "missing entry file: " + pkg.manifest.entry);
    }
    if (!has_icon) {
        return R::err(ErrorCode::InvalidArgument, "missing icon file: " + pkg.manifest.icon);
    }
    return R::ok(std::move(pkg));
}

Result<SceneInfo> SceneRepository::install(
    std::span<const uint8_t> zip, const SourceChecker& check,
    const std::function<Result<void>(const SceneManifest&)>& before_replace, InstallOptions opt) {
    using R = Result<SceneInfo>;
    if (config_.dev) {
        return R::err(ErrorCode::Unsupported, "scenes are read-only in dev mode");
    }

    ZipLimits limits;
    limits.max_entries = kMaxFiles;
    limits.max_total = config_.max_code_bytes;
    Result<ZipReader> opened = ZipReader::open(zip, limits);
    if (!opened) {
        return fail<SceneInfo>(opened.error());
    }
    const ZipReader& reader = opened.value();

    const ZipEntry* manifest_entry = nullptr;
    for (const ZipEntry& e : reader.entries()) {
        if (!e.is_dir && e.path == kManifest) {
            manifest_entry = &e;
        }
    }
    if (manifest_entry == nullptr) {
        return R::err(ErrorCode::InvalidArgument, "scene package has no scene.json");
    }
    Result<os::LargeBuffer> manifest_raw = reader.read(*manifest_entry);
    if (!manifest_raw) {
        return fail<SceneInfo>(manifest_raw.error());
    }
    Result<SceneManifest> parsed = parse_scene_manifest(std::string_view(
        reinterpret_cast<const char*>(manifest_raw.value().data()), manifest_raw.value().size()));
    if (!parsed) {
        return fail<SceneInfo>(parsed.error());
    }
    const SceneManifest& manifest = parsed.value();
    Result<void> schemas = validate_scene_tool_schemas(manifest);
    if (!schemas) {
        return R::err(schemas.error().code, schemas.error().message);
    }
    const std::string& id = manifest.id;

    // From here until this install returns, opens of `id` are refused
    // (installing()), and so is a second install of it.
    {
        std::lock_guard<os::Mutex> lock(marks_mutex_);
        if (std::find(installing_.begin(), installing_.end(), id) != installing_.end()) {
            return R::err(ErrorCode::Busy, "scene " + id + " is already being installed");
        }
        installing_.push_back(id);
    }
    struct Unmark {
        SceneRepository& repo;
        const std::string& id;
        ~Unmark() {
            std::lock_guard<os::Mutex> lock(repo.marks_mutex_);
            auto it = std::find(repo.installing_.begin(), repo.installing_.end(), id);
            if (it != repo.installing_.end()) repo.installing_.erase(it);
        }
    } unmark{*this, id};

    // Everything is validated, read and compiled in memory first, so a
    // rejected package never costs a flash write.
    bool has_entry = false;
    bool has_icon = manifest.icon.empty();
    size_t lua_files = 0;
    for (const ZipEntry& e : reader.entries()) {
        std::string_view path(e.path.data(), e.path.size());
        if (e.is_dir) {
            if (!allowed_dir(path)) {
                return R::err(ErrorCode::InvalidArgument,
                              "unexpected directory in scene package: " + std::string(path));
            }
            continue;
        }
        if (!allowed_file(path, manifest.entry)) {
            return R::err(ErrorCode::InvalidArgument,
                          "unexpected file in scene package: " + std::string(path));
        }
        has_entry = has_entry || path == manifest.entry;
        has_icon = has_icon || path == manifest.icon;
        lua_files += is_lua(path) ? 1 : 0;
    }
    if (!has_entry) {
        return R::err(ErrorCode::InvalidArgument, "missing entry file: " + manifest.entry);
    }
    if (!has_icon) {
        return R::err(ErrorCode::InvalidArgument, "missing icon file: " + manifest.icon);
    }

    os::LargeVector<std::pair<std::string, os::LargeBuffer>> files;
    files.reserve(reader.entries().size());
    for (const ZipEntry& e : reader.entries()) {
        if (e.is_dir) {
            continue;
        }
        Result<os::LargeBuffer> body = reader.read(e);
        if (!body) {
            return fail<SceneInfo>(body.error());
        }
        files.emplace_back(std::string(e.path.data(), e.path.size()), std::move(body.value()));
    }

    if (check) {
        // In Mem::Large like everything else here: the uploader chooses how
        // many files there are.
        os::LargeVector<SourceFile> sources;
        sources.reserve(lua_files);
        for (const auto& [path, body] : files) {
            if (is_lua(path)) {
                sources.push_back(
                    {path,
                     std::string_view(reinterpret_cast<const char*>(body.data()), body.size())});
            }
        }
        Result<void> checked = check(std::span<const SourceFile>(sources));
        if (!checked) {
            return fail<SceneInfo>(checked.error());
        }
    }

    if (before_replace) {
        Result<void> ready = before_replace(manifest);
        if (!ready) {
            return fail<SceneInfo>(ready.error());
        }
    }

    // Staged in one flash-thread job per piece of at most kIoChunk, without
    // the lock: .<id>.tmpdir is this install's alone (installing() keeps a
    // second install of the id out, list() skips dot directories, and only
    // recover() at boot removes it). Other flash work queued meanwhile runs
    // between the pieces. Only the swap takes the lock.
    StorageWorker* worker = config_.worker;
    const std::string tmp = config_.code_root + "/." + id + std::string(kTmpSuffix);
    Result<void> staged = call_on_worker<void>(worker, [&]() -> Result<void> {
        std::lock_guard<os::Mutex> lock(mutex_);
        // An earlier replace of this id that was cut short completes first,
        // so its staging names are free again.
        WISP_TRY(finish_replace(id));
        return storage_.remove_tree(tmp);
    });
    for (const auto& [path, body] : files) {
        if (!staged) break;
        const std::string target = tmp + "/" + path;
        size_t offset = 0;
        do {
            const size_t n = std::min(kIoChunk, body.size() - offset);
            const std::span<const uint8_t> piece = body.span().subspan(offset, n);
            staged = call_on_worker<void>(worker, [&] {
                return offset == 0 ? storage_.write_atomic(target, piece)
                                   : storage_.append(target, piece);
            });
            offset += n;
        } while (staged && offset < body.size());
    }
    if (!staged) {
        (void)call_on_worker<void>(worker, [&] { return storage_.remove_tree(tmp); });
        return fail<SceneInfo>(staged.error());
    }
    files.clear();
    return call_on_worker<SceneInfo>(worker, [&] { return swap_here(manifest, opt.keep_prev); });
}

Result<SceneInfo> SceneRepository::swap_here(const SceneManifest& manifest, bool keep_prev) {
    const std::string& id = manifest.id;
    const std::string tmp = config_.code_root + "/." + id + std::string(kTmpSuffix);
    const std::string neu = config_.code_root + "/." + id + std::string(kNewSuffix);
    std::lock_guard<os::Mutex> lock(mutex_);
    Result<SceneManifest> previous = read_manifest(id);
    if (Result<void> renamed = storage_.rename(tmp, neu); !renamed) {
        storage_.remove_tree(tmp);
        return fail<SceneInfo>(renamed.error());
    }
    ++generation_;
    note_change(id);
    Result<void> replaced = finish_replace(id, keep_prev);
    if (!replaced) {
        return fail<SceneInfo>(replaced.error());
    }

    if (previous && previous.value().http_hosts != manifest.http_hosts) {
        const std::string tag = "scene." + id;
        WLOG_I(tag.c_str(), "http hosts changed: %s -> %s",
               join_hosts(previous.value().http_hosts).c_str(),
               join_hosts(manifest.http_hosts).c_str());
    }
    WLOG_I(kTag, "installed %s %s", id.c_str(), manifest.version.c_str());
    return info_locked(id);
}

std::string SceneRepository::prev_dir(std::string_view id) const {
    return config_.code_root + "/" + std::string(kPrevDir) + "/" + std::string(id);
}

Result<void> SceneRepository::finish_replace(const std::string& id, bool keep_prev) {
    const std::string neu = config_.code_root + "/." + id + std::string(kNewSuffix);
    Result<Kind> staged = probe(storage_, neu);
    if (!staged) {
        return Result<void>::err(staged.error().code, staged.error().message);
    }
    if (staged.value() == Kind::Missing) {
        return Result<void>::ok();
    }
    note_change(id);
    if (staged.value() != Kind::Dir) {
        return storage_.remove_tree(neu);
    }

    // Packages never carry data/ or files/, so anything at <id>/data or
    // <id>/files is the scene's, whatever the configured roots look like,
    // and the same name under .newdir exists only if this move already
    // happened before an interruption.
    const std::string dir = code_dir(id);
    for (std::string_view sub : kSceneOwned) {
        const std::string from = dir + "/" + std::string(sub);
        const std::string to = neu + "/" + std::string(sub);
        Result<Kind> old_kind = probe(storage_, from);
        if (!old_kind) {
            return Result<void>::err(old_kind.error().code, old_kind.error().message);
        }
        Result<Kind> new_kind = probe(storage_, to);
        if (!new_kind) {
            return Result<void>::err(new_kind.error().code, new_kind.error().message);
        }
        if (old_kind.value() == Kind::Missing) {
            continue;
        }
        if (new_kind.value() != Kind::Missing) {
            // Only possible if an earlier replace failed after the move and
            // the scene then wrote again. Deleting either copy could lose
            // what the user wrote last; leave both for a person.
            return Result<void>::err(
                ErrorCode::AlreadyExists,
                "both " + from + " and " + to + " exist; not replacing scene " + id);
        }
        WISP_TRY(storage_.rename(from, to));
    }
    if (config_.on_data_moved) config_.on_data_moved();
    Result<Kind> old_manifest = probe(storage_, dir + "/" + std::string(kManifest));
    if (!old_manifest) {
        return Result<void>::err(old_manifest.error().code, old_manifest.error().message);
    }
    if (keep_prev && old_manifest.value() == Kind::Other) {
        // What is left of the old package (data/ and files/ moved out
        // above) becomes the previous version.
        const std::string prev = prev_dir(id);
        WISP_TRY(storage_.remove_tree(prev));
        WISP_TRY(storage_.rename(dir, prev));
    } else {
        WISP_TRY(storage_.remove_tree(dir));
    }
    return storage_.rename(neu, dir);
}

Result<void> SceneRepository::recover() {
    return call_on_worker<void>(config_.worker, [&] { return recover_here(); });
}

Result<void> SceneRepository::recover_here() {
    if (config_.dev) {
        return Result<void>::ok();
    }
    std::lock_guard<os::Mutex> lock(mutex_);
    Result<std::vector<DirEntry>> listed = storage_.list_dir(config_.code_root);
    if (!listed) {
        if (listed.error().code == ErrorCode::NotFound) {
            return Result<void>::ok();
        }
        return Result<void>::err(listed.error().code, listed.error().message);
    }
    Result<void> first = Result<void>::ok();
    for (const DirEntry& e : listed.value()) {
        const std::string_view name = e.name;
        if (!starts_with(name, ".")) {
            continue;
        }
        Result<void> r = Result<void>::ok();
        if (ends_with(name, kTmpSuffix) && name.size() > 1 + kTmpSuffix.size()) {
            r = storage_.remove_tree(config_.code_root + "/" + e.name);
        } else if (ends_with(name, kNewSuffix) && name.size() > 1 + kNewSuffix.size()) {
            std::string_view id = name.substr(1, name.size() - 1 - kNewSuffix.size());
            if (!valid_scene_id(id)) {
                continue;
            }
            r = finish_replace(std::string(id));
            if (r) {
                WLOG_I(kTag, "finished the interrupted install of %.*s",
                       static_cast<int>(id.size()), id.data());
            }
        }
        if (!r) {
            WLOG_W(kTag, "recover %s: %s", e.name.c_str(), r.error().message.c_str());
            if (first) {
                first = r;
            }
        }
    }
    return first;
}

Result<os::LargeBuffer> SceneRepository::export_zip(std::string_view id, bool with_data) {
    return export_dir(id, false, with_data);
}

bool SceneRepository::has_prev(std::string_view id) {
    if (!valid_scene_id(id)) {
        return false;
    }
    Result<bool> found = call_on_worker<bool>(config_.worker, [&]() -> Result<bool> {
        std::lock_guard<os::Mutex> lock(mutex_);
        Result<Kind> kind = probe(storage_, prev_dir(id) + "/" + std::string(kManifest));
        if (!kind) {
            return Result<bool>::err(kind.error().code, kind.error().message);
        }
        return Result<bool>::ok(kind.value() == Kind::Other);
    });
    return found && found.value();
}

Result<os::LargeBuffer> SceneRepository::prev_zip(std::string_view id) {
    if (!valid_scene_id(id)) {
        return Result<os::LargeBuffer>::err(ErrorCode::InvalidArgument,
                                            "invalid scene id: " + std::string(id));
    }
    return export_dir(id, true, false);
}

Result<os::LargeBuffer> SceneRepository::export_dir(std::string_view id_view, bool prev,
                                                    bool with_data) {
    using R = Result<os::LargeBuffer>;
    // The file list first, then every file in flash-thread jobs of at most
    // kIoChunk, so other flash work queued meanwhile runs between them.
    // Each read checks under the lock that no replace or remove has landed
    // since the list was taken; one that has fails the export with Busy
    // rather than mixing two versions of the package.
    struct Item {
        std::string path;  // full path on storage
        std::string name;  // in the zip
    };
    StorageWorker* worker = config_.worker;
    const std::string id(id_view);
    std::vector<Item> items;
    uint64_t generation = 0;
    Result<void> listed = call_on_worker<void>(worker, [&]() -> Result<void> {
        std::lock_guard<os::Mutex> lock(mutex_);
        generation = generation_;
        const std::string dir = prev ? prev_dir(id) : code_dir(id);
        Result<SceneManifest> m = prev ? read_prev_manifest(id) : read_manifest(id);
        if (!m) {
            return Result<void>::err(m.error().code, m.error().message);
        }
        auto add_from = [&](const std::string& base) {
            return [&, base](const std::string& rel, uint64_t) -> Result<void> {
                items.push_back({base + "/" + rel, rel});
                return Result<void>::ok();
            };
        };
        WISP_TRY(walk_package(storage_, dir, m.value().entry, add_from(dir)));
        if (with_data) {
            const std::string data_base = config_.data_root + "/" + id;
            WISP_TRY(walk_files(storage_, data_base, "data", add_from(data_base)));
        }
        return Result<void>::ok();
    });
    if (!listed) {
        return fail<os::LargeBuffer>(listed.error());
    }

    ZipWriter writer;
    os::LargeVector<uint8_t> body;
    for (const Item& item : items) {
        body.clear();
        // Until a short read: a data file may grow or shrink meanwhile.
        bool more = true;
        while (more) {
            Result<void> read = call_on_worker<void>(worker, [&]() -> Result<void> {
                std::lock_guard<os::Mutex> lock(mutex_);
                if (generation_ != generation) {
                    return Result<void>::err(ErrorCode::Busy,
                                             "scene " + id + " changed during the export");
                }
                Result<os::LargeBuffer> chunk =
                    storage_.read_range(item.path, body.size(), kIoChunk);
                if (!chunk) {
                    return Result<void>::err(chunk.error().code, chunk.error().message);
                }
                body.insert(body.end(), chunk.value().data(),
                            chunk.value().data() + chunk.value().size());
                more = chunk.value().size() == kIoChunk;
                return Result<void>::ok();
            });
            if (!read) {
                return fail<os::LargeBuffer>(read.error());
            }
        }
        writer.add(item.name, std::span<const uint8_t>(body.data(), body.size()));
    }
    os::LargeBuffer out = writer.finish();
    if (!out.ok()) {
        return R::err(ErrorCode::NoMemory, "no memory for the exported zip");
    }
    return R::ok(std::move(out));
}

bool SceneRepository::installing(std::string_view id) const {
    std::lock_guard<os::Mutex> lock(marks_mutex_);
    return std::find(installing_.begin(), installing_.end(), id) != installing_.end();
}

void SceneRepository::begin_removing(std::string_view id) {
    std::lock_guard<os::Mutex> lock(marks_mutex_);
    removing_.emplace_back(id);
}

void SceneRepository::end_removing(std::string_view id) {
    std::lock_guard<os::Mutex> lock(marks_mutex_);
    auto it = std::find(removing_.begin(), removing_.end(), id);
    if (it != removing_.end()) removing_.erase(it);
}

bool SceneRepository::removing(std::string_view id) const {
    std::lock_guard<os::Mutex> lock(marks_mutex_);
    return std::find(removing_.begin(), removing_.end(), id) != removing_.end();
}

Result<SceneRemoval> SceneRepository::removal(std::string_view id, bool with_data) {
    return call_on_worker<SceneRemoval>(config_.worker,
                                        [&] { return removal_here(id, with_data); });
}

Result<SceneRemoval> SceneRepository::removal_here(std::string_view id_view, bool with_data) {
    using R = Result<SceneRemoval>;
    if (!valid_scene_id(id_view)) {
        return R::err(ErrorCode::InvalidArgument, "invalid scene id: " + std::string(id_view));
    }
    const std::string id(id_view);
    std::lock_guard<os::Mutex> lock(mutex_);
    SceneRemoval out;
    // Read the way remove() reads it: a package whose scene.json no longer
    // validates is still installed (and named by its id).
    Result<SceneManifest> m = read_manifest(id);
    if (m) {
        out.package = true;
        out.name = m.value().name;
    } else if (m.error().code == ErrorCode::InvalidArgument) {
        out.package = true;
        out.name = id;
    } else if (m.error().code != ErrorCode::NotFound) {
        return R::err(m.error().code, m.error().message);
    } else {
        out.name = id;
    }
    std::vector<std::string> owned = {code_dir(id) + "/data", data_dir(id), code_dir(id) + "/files",
                                      files_dir(id)};
    if (std::string sd = sd_scene_dir(storage_, id); !sd.empty()) owned.push_back(sd);
    for (const std::string& dir : owned) {
        Result<Kind> kind = probe(storage_, dir);
        if (!kind) {
            return R::err(kind.error().code, kind.error().message);
        }
        out.data = out.data || kind.value() != Kind::Missing;
    }
    if (!out.package && !(with_data && out.data)) {
        return R::err(ErrorCode::NotFound, "scene not installed: " + id);
    }
    return R::ok(std::move(out));
}

Result<void> SceneRepository::remove(std::string_view id, bool with_data) {
    return call_on_worker<void>(config_.worker, [&] { return remove_here(id, with_data); });
}

Result<void> SceneRepository::remove_here(std::string_view id_view, bool with_data) {
    using R = Result<void>;
    if (config_.dev) {
        return R::err(ErrorCode::Unsupported, "scenes are read-only in dev mode");
    }
    if (!valid_scene_id(id_view)) {
        return R::err(ErrorCode::InvalidArgument, "invalid scene id: " + std::string(id_view));
    }
    const std::string id(id_view);
    std::lock_guard<os::Mutex> lock(mutex_);
    ++generation_;
    note_change(id);
    WISP_TRY(finish_replace(id));

    const std::string dir = code_dir(id);
    Result<SceneManifest> m = read_manifest(id);
    if (!m && m.error().code != ErrorCode::NotFound &&
        m.error().code != ErrorCode::InvalidArgument) {
        return R::err(m.error().code, m.error().message);
    }
    const bool installed = m || m.error().code == ErrorCode::InvalidArgument;
    // As in finish_replace(), <id>/data and <id>/files are the scene's
    // however the roots are spelled; data_dir(id) and files_dir(id) cover a
    // data root of its own, and the SD card may hold files too.
    bool kept = false;
    bool has_data = false;
    std::vector<std::string> owned = {data_dir(id), files_dir(id)};
    if (std::string sd = sd_scene_dir(storage_, id); !sd.empty()) owned.push_back(sd);
    for (std::string_view sub : kSceneOwned) owned.push_back(dir + "/" + std::string(sub));
    for (size_t i = 0; i < owned.size(); ++i) {
        Result<Kind> kind = probe(storage_, owned[i]);
        if (!kind) {
            return R::err(kind.error().code, kind.error().message);
        }
        const bool present = kind.value() != Kind::Missing;
        has_data = has_data || present;
        // The last ones are those inside the package directory.
        if (i + std::size(kSceneOwned) >= owned.size()) kept = kept || present;
    }
    if (!installed && !(with_data && has_data)) {
        return R::err(ErrorCode::NotFound, "scene not installed: " + id);
    }

    // scene.json goes first, so a remove cut short already reads as "not
    // installed" instead of as a package with missing files.
    WISP_TRY(storage_.remove_tree(dir + "/" + std::string(kManifest)));
    WISP_TRY(storage_.remove_tree(prev_dir(id)));
    if (with_data) {
        WISP_TRY(storage_.remove_tree(dir));
        // A no-op when the data was inside `dir`.
        WISP_TRY(storage_.remove_tree(config_.data_root + "/" + id));
        if (std::string sd = sd_scene_dir(storage_, id); !sd.empty()) {
            WISP_TRY(storage_.remove_tree(sd));
        } else if (sd_card_out(storage_)) {
            WLOG_W(kTag,
                   "%s: the SD card is out, so its files there (if any) stay in "
                   "wisp/library/scenes/%s/files",
                   id.c_str(), id.c_str());
        }
        WLOG_I(kTag, "removed %s with its data", id.c_str());
        return R::ok();
    }
    if (!kept) {
        WISP_TRY(storage_.remove_tree(dir));
        WLOG_I(kTag, "removed %s", id.c_str());
        return R::ok();
    }

    // The data and files directories stay in place.
    Result<std::vector<DirEntry>> listed = storage_.list_dir(dir);
    if (!listed) {
        return R::err(listed.error().code, listed.error().message);
    }
    for (const DirEntry& e : listed.value()) {
        // remove_tree() refuses a bookkeeping name itself but removes it
        // along with the file it belongs to.
        std::string name = e.name;
        if (reserved_name(name)) {
            name.resize(name.size() - 4);
        }
        if (scene_owned(name)) {
            continue;
        }
        WISP_TRY(storage_.remove_tree(dir + "/" + name));
    }
    WLOG_I(kTag, "removed %s, keeping its data", id.c_str());
    return R::ok();
}

}  // namespace wisp::core
