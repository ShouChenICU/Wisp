#include "wisp/core/scene_repository.hpp"

#include <mutex>
#include <string>
#include <utility>

#include "wisp/core/log.hpp"
#include "wisp/core/zip.hpp"

namespace wisp::core {

namespace {

constexpr const char* kTag = "scene";
constexpr std::string_view kManifest = "scene.json";
constexpr std::string_view kTmpSuffix = ".tmpdir";
constexpr std::string_view kNewSuffix = ".newdir";
// Same bound as the zip reader's entry limit, so a dev working directory
// cannot hold a package that could never be installed.
constexpr size_t kMaxFiles = 256;

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

Result<SceneManifest> SceneRepository::read_manifest(std::string_view id) {
    if (!valid_scene_id(id)) {
        return Result<SceneManifest>::err(ErrorCode::InvalidArgument,
                                          "invalid scene id: " + std::string(id));
    }
    Result<os::LargeBuffer> raw =
        storage_.read_file_large(code_dir(id) + "/" + std::string(kManifest));
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

Result<SceneInfo> SceneRepository::info(std::string_view id) {
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

Result<ScenePackage> SceneRepository::load(std::string_view id) {
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

Result<SceneInfo> SceneRepository::install(std::span<const uint8_t> zip,
                                           const SourceChecker& check) {
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
    const std::string& id = manifest.id;

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

    std::lock_guard<os::Mutex> lock(mutex_);
    // An earlier replace of this id that was cut short completes first, so
    // its staging names are free again.
    {
        Result<void> pending = finish_replace(id);
        if (!pending) {
            return fail<SceneInfo>(pending.error());
        }
    }

    Result<SceneManifest> previous = read_manifest(id);

    const std::string tmp = config_.code_root + "/." + id + std::string(kTmpSuffix);
    const std::string neu = config_.code_root + "/." + id + std::string(kNewSuffix);
    Result<void> staged = storage_.remove_tree(tmp);
    for (const auto& [path, body] : files) {
        if (!staged) {
            break;
        }
        staged = storage_.write_atomic(tmp + "/" + path, body.span());
    }
    if (staged) {
        staged = storage_.rename(tmp, neu);
    }
    if (!staged) {
        storage_.remove_tree(tmp);
        return fail<SceneInfo>(staged.error());
    }
    files.clear();

    Result<void> replaced = finish_replace(id);
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

Result<void> SceneRepository::finish_replace(const std::string& id) {
    const std::string neu = config_.code_root + "/." + id + std::string(kNewSuffix);
    Result<Kind> staged = probe(storage_, neu);
    if (!staged) {
        return Result<void>::err(staged.error().code, staged.error().message);
    }
    if (staged.value() == Kind::Missing) {
        return Result<void>::ok();
    }
    if (staged.value() != Kind::Dir) {
        return storage_.remove_tree(neu);
    }

    // Packages never carry data/, so anything at <id>/data is the scene's
    // data, whatever the configured roots look like, and .newdir/data
    // exists only if this move already happened before an interruption.
    const std::string dir = code_dir(id);
    Result<Kind> old_data = probe(storage_, dir + "/data");
    if (!old_data) {
        return Result<void>::err(old_data.error().code, old_data.error().message);
    }
    Result<Kind> new_data = probe(storage_, neu + "/data");
    if (!new_data) {
        return Result<void>::err(new_data.error().code, new_data.error().message);
    }
    if (old_data.value() != Kind::Missing) {
        if (new_data.value() != Kind::Missing) {
            // Only possible if an earlier replace failed after the move and
            // the scene then wrote data again. Deleting either copy could
            // lose what the user wrote last; leave both for a person.
            return Result<void>::err(
                ErrorCode::AlreadyExists,
                "both " + dir + "/data and " + neu + "/data exist; not replacing scene " + id);
        }
        WISP_TRY(storage_.rename(dir + "/data", neu + "/data"));
    }
    WISP_TRY(storage_.remove_tree(dir));
    return storage_.rename(neu, dir);
}

Result<void> SceneRepository::recover() {
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
    using R = Result<os::LargeBuffer>;
    std::lock_guard<os::Mutex> lock(mutex_);
    Result<SceneManifest> m = read_manifest(id);
    if (!m) {
        return fail<os::LargeBuffer>(m.error());
    }
    ZipWriter writer;
    auto add_from = [&](const std::string& base) {
        return [&, base](const std::string& rel, uint64_t) -> Result<void> {
            Result<os::LargeBuffer> buf = storage_.read_file_large(base + "/" + rel);
            if (!buf) {
                return Result<void>::err(buf.error().code, buf.error().message);
            }
            writer.add(rel, buf.value().span());
            return Result<void>::ok();
        };
    };
    const std::string dir = code_dir(id);
    Result<void> walked = walk_package(storage_, dir, m.value().entry, add_from(dir));
    if (walked && with_data) {
        const std::string data_base = config_.data_root + "/" + std::string(id);
        walked = walk_files(storage_, data_base, "data", add_from(data_base));
    }
    if (!walked) {
        return fail<os::LargeBuffer>(walked.error());
    }
    os::LargeBuffer out = writer.finish();
    if (!out.ok()) {
        return R::err(ErrorCode::NoMemory, "no memory for the exported zip");
    }
    return R::ok(std::move(out));
}

Result<void> SceneRepository::remove(std::string_view id_view, bool with_data) {
    using R = Result<void>;
    if (config_.dev) {
        return R::err(ErrorCode::Unsupported, "scenes are read-only in dev mode");
    }
    if (!valid_scene_id(id_view)) {
        return R::err(ErrorCode::InvalidArgument, "invalid scene id: " + std::string(id_view));
    }
    const std::string id(id_view);
    std::lock_guard<os::Mutex> lock(mutex_);
    WISP_TRY(finish_replace(id));

    const std::string dir = code_dir(id);
    Result<SceneManifest> m = read_manifest(id);
    if (!m && m.error().code != ErrorCode::NotFound &&
        m.error().code != ErrorCode::InvalidArgument) {
        return R::err(m.error().code, m.error().message);
    }
    const bool installed = m || m.error().code == ErrorCode::InvalidArgument;
    // As in finish_replace(), <id>/data is the scene's data however the
    // roots are spelled; data_dir(id) covers a data root of its own.
    Result<Kind> kept = probe(storage_, dir + "/data");
    if (!kept) {
        return R::err(kept.error().code, kept.error().message);
    }
    Result<Kind> data = probe(storage_, data_dir(id));
    if (!data) {
        return R::err(data.error().code, data.error().message);
    }
    const bool has_data = kept.value() != Kind::Missing || data.value() != Kind::Missing;
    if (!installed && !(with_data && has_data)) {
        return R::err(ErrorCode::NotFound, "scene not installed: " + id);
    }

    // scene.json goes first, so a remove cut short already reads as "not
    // installed" instead of as a package with missing files.
    WISP_TRY(storage_.remove_tree(dir + "/" + std::string(kManifest)));
    if (with_data) {
        WISP_TRY(storage_.remove_tree(dir));
        // A no-op when the data was inside `dir`.
        WISP_TRY(storage_.remove_tree(config_.data_root + "/" + id));
        WLOG_I(kTag, "removed %s with its data", id.c_str());
        return R::ok();
    }
    if (kept.value() == Kind::Missing) {
        WISP_TRY(storage_.remove_tree(dir));
        WLOG_I(kTag, "removed %s", id.c_str());
        return R::ok();
    }

    // The data directory stays in place.
    Result<std::vector<DirEntry>> listed = storage_.list_dir(dir);
    if (!listed) {
        return R::err(listed.error().code, listed.error().message);
    }
    for (const DirEntry& e : listed.value()) {
        if (e.name == "data") {
            continue;
        }
        // remove_tree() refuses a bookkeeping name itself but removes it
        // along with the file it belongs to.
        std::string name = e.name;
        if (reserved_name(name)) {
            name.resize(name.size() - 4);
            if (name == "data") {
                continue;
            }
        }
        WISP_TRY(storage_.remove_tree(dir + "/" + name));
    }
    WLOG_I(kTag, "removed %s, keeping its data", id.c_str());
    return R::ok();
}

}  // namespace wisp::core
