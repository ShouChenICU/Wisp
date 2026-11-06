#include "wisp/agent/scene_workspace.hpp"

#include <algorithm>
#include <cstring>
#include <memory>
#include <utility>
#include <vector>

#include "wisp/agent/limits.hpp"
#include "wisp/core/log.hpp"
#include "wisp/core/scene_install.hpp"
#include "wisp/core/scene_manifest.hpp"
#include "wisp/core/zip.hpp"
#include "wisp/os/large.hpp"
#include "wisp/os/os.hpp"
#include "wisp/scene/scene_data.hpp"
#include "wisp/ui/strings.hpp"

namespace wisp::agent {

namespace {

constexpr const char* kTag = "agent";
// A draft holds at most what a package may (SceneRepoConfig's default
// max_code_bytes), so every draft that fits can be installed.
constexpr uint64_t kMaxDraftBytes = 512 * 1024;
constexpr std::string_view kManifest = "scene.json";

template <class T>
Result<T> fail(ErrorCode code, std::string_view tag, const std::string& text) {
    return Result<T>::err(code, std::string(tag) + ": " + text);
}

template <class T>
Result<T> pass(const Error& e) {
    return Result<T>::err(e.code, e.message);
}

bool code_word(std::string_view s) {
    if (s.empty()) return false;
    for (char c : s)
        if (!((c >= 'a' && c <= 'z') || c == '_')) return false;
    return true;
}

bool ends_with(std::string_view s, std::string_view x) {
    return s.size() >= x.size() && s.substr(s.size() - x.size()) == x;
}

// StorageManager's write bookkeeping, never a draft file.
bool reserved_name(std::string_view name) {
    return ends_with(name, ".tmp") || ends_with(name, ".new");
}

Result<void> check_id(const std::string& id) {
    if (!core::valid_scene_id(id)) return fail<void>(ErrorCode::InvalidArgument, "bad_id", id);
    return Result<void>::ok();
}

Result<void> check_path(std::string_view path) {
    if (!core::valid_package_path(path))
        return fail<void>(ErrorCode::InvalidArgument, "bad_path",
                          "not a scene package path: " + std::string(path));
    return Result<void>::ok();
}

std::span<const uint8_t> bytes_of(std::string_view s) {
    return {reinterpret_cast<const uint8_t*>(s.data()), s.size()};
}

std::string_view text_of(const os::LargeBuffer& b) {
    return {reinterpret_cast<const char*>(b.data()), b.size()};
}

using Files = os::LargeVector<std::pair<std::string, os::LargeBuffer>>;

// fn(relative path, size) for every file under `dir`, write bookkeeping
// skipped. NotFound if `dir` does not exist. Flash thread.
template <class Fn>
Result<void> walk(core::StorageManager& storage, const std::string& dir, Fn&& fn) {
    std::vector<std::string> pending{""};
    bool top = true;
    while (!pending.empty()) {
        const std::string rel = std::move(pending.back());
        pending.pop_back();
        auto listed = storage.list_dir(rel.empty() ? dir : dir + "/" + rel);
        if (!listed) {
            if (top || listed.error().code != ErrorCode::NotFound)
                return pass<void>(listed.error());
            continue;
        }
        top = false;
        for (const core::DirEntry& e : listed.value()) {
            if (reserved_name(e.name)) continue;
            std::string child = rel.empty() ? e.name : rel + "/" + e.name;
            if (e.kind == core::EntryKind::Dir) {
                pending.push_back(std::move(child));
            } else if (e.kind == core::EntryKind::File) {
                WISP_TRY(fn(child, e.size));
            }
        }
    }
    return Result<void>::ok();
}

ui::Lang lang_of(const ToolCtx& ctx) { return ui::parse_lang(ctx.lang).value_or(ui::Lang::Zh); }

// "main.lua:3: message" as an InstallError; the suggestion is the name a
// "did you mean 'x'?" offers.
InstallError parse_error(const std::string& id, std::string_view text) {
    InstallError e;
    std::string_view msg = text;
    const size_t colon = text.find(':');
    if (colon != std::string_view::npos && colon > 0 &&
        text.substr(0, colon).find(' ') == std::string_view::npos) {
        e.file = std::string(text.substr(0, colon));
        msg = text.substr(colon + 1);
        size_t digits = 0;
        while (digits < msg.size() && msg[digits] >= '0' && msg[digits] <= '9') ++digits;
        if (digits > 0 && digits < msg.size() && msg[digits] == ':') {
            e.line = std::atoi(std::string(msg.substr(0, digits)).c_str());
            msg = msg.substr(digits + 1);
        }
        while (!msg.empty() && msg.front() == ' ') msg.remove_prefix(1);
    }
    static constexpr std::string_view kMean = "did you mean '";
    if (const size_t at = msg.find(kMean); at != std::string_view::npos) {
        const size_t from = at + kMean.size();
        const size_t to = msg.find('\'', from);
        if (to != std::string_view::npos) e.suggestion = std::string(msg.substr(from, to - from));
    }
    // Scene code chooses what error() says.
    e.message = scene::scene_data(id, msg);
    return e;
}

ToolOutput check_failed(std::vector<InstallError> errors) {
    List list;
    for (const InstallError& e : errors) list.push_back(to_value(e));
    return tool_error("check_failed", "the draft was not installed; see errors",
                      Map{{"errors", Value(std::move(list))}});
}

ToolOutput from_error(const Error& e) { return tool_error(error_code(e), e.message); }

// The last kCheckLogsPerLevel messages pushed, kept in Mem::Large: the
// ring may hold hundreds of matching lines, and each would otherwise be a
// small string in internal RAM (AGENTS.md section 8).
class LastLines {
public:
    LastLines() : lines_(kCheckLogsPerLevel) {}
    void push(std::string_view msg) {
        lines_[pushed_ % lines_.size()].assign(msg.data(), msg.size());
        ++pushed_;
    }
    // Oldest first, each wrapped with scene_data().
    List take(const std::string& id) const {
        const size_t n = std::min(pushed_, lines_.size());
        List out;
        out.reserve(n);
        for (size_t i = pushed_ - n; i < pushed_; ++i) {
            const os::LargeString& l = lines_[i % lines_.size()];
            out.push_back(Value(scene::scene_data(id, std::string_view(l.data(), l.size()))));
        }
        return out;
    }

private:
    os::LargeVector<os::LargeString> lines_;
    size_t pushed_ = 0;
};

}  // namespace

Value to_value(const InstallError& e) {
    return Value(Map{{"file", Value(e.file)},
                     {"line", Value(static_cast<int64_t>(e.line))},
                     {"message", Value(e.message)},
                     {"suggestion", Value(e.suggestion)}});
}

std::string error_code(const Error& e) {
    const size_t p = e.message.find(": ");
    const std::string_view head = std::string_view(e.message).substr(0, p);
    if (code_word(head)) return std::string(head);
    return to_string(e.code);
}

SceneWorkspace::SceneWorkspace(SceneWorkspaceDeps deps) : deps_(std::move(deps)) {
    while (deps_.drafts_root.size() > 1 && deps_.drafts_root.back() == '/')
        deps_.drafts_root.pop_back();
}

std::string SceneWorkspace::draft_dir(const std::string& id) const {
    return deps_.drafts_root + "/" + id;
}

bool SceneWorkspace::draft_exists_here(const std::string& id) {
    return static_cast<bool>(deps_.storage.list_dir(draft_dir(id)));
}

Result<void> SceneWorkspace::ensure_draft_here(const std::string& id) {
    if (draft_exists_here(id)) return Result<void>::ok();
    Result<core::ScenePackage> pkg = deps_.repo.load(id);
    if (!pkg) {
        // A new scene: the draft begins with its first file.
        if (pkg.error().code == ErrorCode::NotFound) return Result<void>::ok();
        return pass<void>(pkg.error());
    }
    for (const auto& [path, body] : pkg.value().files)
        WISP_TRY(deps_.storage.write_atomic(draft_dir(id) + "/" + path, body.span()));
    WLOG_I(kTag, "draft of %s started from the installed package", id.c_str());
    return Result<void>::ok();
}

Result<uint64_t> SceneWorkspace::draft_bytes_here(const std::string& id) {
    return deps_.storage.tree_size(draft_dir(id));
}

std::string SceneWorkspace::name_here(const std::string& id) {
    auto raw = deps_.storage.read_file_large(draft_dir(id) + "/" + std::string(kManifest));
    if (raw) {
        auto m = core::parse_scene_manifest(text_of(raw.value()));
        if (m && !m.value().name.empty()) return m.value().name;
    }
    auto m = deps_.repo.manifest(id);
    if (m && !m.value().name.empty()) return m.value().name;
    return id;
}

bool SceneWorkspace::has_draft(const std::string& id) {
    if (!core::valid_scene_id(id)) return false;
    auto r = core::call_on_worker<bool>(&deps_.worker,
                                        [&] { return Result<bool>::ok(draft_exists_here(id)); });
    return r && r.value();
}

Value SceneWorkspace::list() {
    auto r = core::call_on_worker<Value>(&deps_.worker, [&]() -> Result<Value> {
        List out;
        std::vector<std::string> seen;
        auto installed = deps_.repo.list();
        if (!installed) return pass<Value>(installed.error());
        for (const core::SceneInfo& info : installed.value()) {
            const std::string& id = info.manifest.id;
            seen.push_back(id);
            out.push_back(Value(Map{
                {"id", Value(id)},
                {"name", Value(info.manifest.name)},
                {"version", Value(info.manifest.version)},
                {"state", Value(core::to_string(deps_.runtime.status(id).state))},
                {"bytes", Value(static_cast<int64_t>(info.code_bytes))},
                {"draft", Value(draft_exists_here(id))},
            }));
        }
        auto drafts = deps_.storage.list_dir(deps_.drafts_root);
        if (drafts) {
            for (const core::DirEntry& e : drafts.value()) {
                if (e.kind != core::EntryKind::Dir || !core::valid_scene_id(e.name)) continue;
                if (std::find(seen.begin(), seen.end(), e.name) != seen.end()) continue;
                std::string version;
                auto raw =
                    deps_.storage.read_file_large(draft_dir(e.name) + "/" + std::string(kManifest));
                if (raw) {
                    auto m = core::parse_scene_manifest(text_of(raw.value()));
                    if (m) version = m.value().version;
                }
                auto bytes = draft_bytes_here(e.name);
                out.push_back(Value(Map{
                    {"id", Value(e.name)},
                    {"name", Value(name_here(e.name))},
                    {"version", Value(version)},
                    {"state", Value("draft")},
                    {"bytes", Value(static_cast<int64_t>(bytes ? bytes.value() : 0))},
                    {"draft", Value(true)},
                }));
            }
        }
        return Result<Value>::ok(Value(std::move(out)));
    });
    if (!r) {
        WLOG_W(kTag, "scene list failed: %s", r.error().message.c_str());
        return Value(List{});
    }
    return std::move(r.value());
}

Result<Value> SceneWorkspace::list_files(const std::string& id) {
    WISP_TRY(check_id(id));
    return core::call_on_worker<Value>(&deps_.worker, [&]() -> Result<Value> {
        List files;
        const bool draft = draft_exists_here(id);
        if (draft) {
            WISP_TRY(walk(deps_.storage, draft_dir(id),
                          [&](const std::string& rel, uint64_t size) -> Result<void> {
                              files.push_back(
                                  Value(Map{{"path", Value(rel)},
                                            {"bytes", Value(static_cast<int64_t>(size))}}));
                              return Result<void>::ok();
                          }));
        } else {
            auto pkg = deps_.repo.load(id);
            if (!pkg) {
                if (pkg.error().code == ErrorCode::NotFound)
                    return fail<Value>(ErrorCode::NotFound, "not_found",
                                       "no draft and no installed scene " + id);
                return pass<Value>(pkg.error());
            }
            for (const auto& [path, body] : pkg.value().files)
                files.push_back(Value(Map{{"path", Value(path)},
                                          {"bytes", Value(static_cast<int64_t>(body.size()))}}));
        }
        std::sort(files.begin(), files.end(), [](const Value& a, const Value& b) {
            return a.find("path")->as_string() < b.find("path")->as_string();
        });
        return Result<Value>::ok(
            Value(Map{{"draft", Value(draft)}, {"files", Value(std::move(files))}}));
    });
}

Result<os::LargeString> SceneWorkspace::read_file(const std::string& id, std::string_view path) {
    using R = Result<os::LargeString>;
    WISP_TRY(check_id(id));
    WISP_TRY(check_path(path));
    return core::call_on_worker<os::LargeString>(&deps_.worker, [&]() -> R {
        os::LargeBuffer body;
        if (draft_exists_here(id)) {
            auto r = deps_.storage.read_file_large(draft_dir(id) + "/" + std::string(path));
            if (!r) {
                if (r.error().code == ErrorCode::NotFound)
                    return fail<os::LargeString>(ErrorCode::NotFound, "not_found",
                                                 "no " + std::string(path) + " in the draft");
                return pass<os::LargeString>(r.error());
            }
            body = std::move(r.value());
        } else {
            auto pkg = deps_.repo.load(id);
            if (!pkg) {
                if (pkg.error().code == ErrorCode::NotFound)
                    return fail<os::LargeString>(ErrorCode::NotFound, "not_found",
                                                 "no draft and no installed scene " + id);
                return pass<os::LargeString>(pkg.error());
            }
            bool found = false;
            for (auto& [p, b] : pkg.value().files) {
                if (p == path) {
                    body = std::move(b);
                    found = true;
                    break;
                }
            }
            if (!found)
                return fail<os::LargeString>(ErrorCode::NotFound, "not_found",
                                             "no " + std::string(path) + " in scene " + id);
        }
        if (body.size() > kReadMaxBytes)
            return fail<os::LargeString>(ErrorCode::QuotaExceeded, "too_large",
                                         std::string(path) + " has " + std::to_string(body.size()) +
                                             " bytes, more than " + std::to_string(kReadMaxBytes));
        return R::ok(os::LargeString(text_of(body)));
    });
}

Result<void> SceneWorkspace::write_file(const std::string& id, std::string_view path,
                                        std::string_view content) {
    WISP_TRY(check_id(id));
    WISP_TRY(check_path(path));
    return core::call_on_worker<void>(&deps_.worker, [&]() -> Result<void> {
        WISP_TRY(ensure_draft_here(id));
        const std::string target = draft_dir(id) + "/" + std::string(path);
        auto total = draft_bytes_here(id);
        if (!total) return pass<void>(total.error());
        auto old = deps_.storage.tree_size(target);
        if (!old) return pass<void>(old.error());
        const uint64_t after = total.value() - old.value() + content.size();
        if (after > kMaxDraftBytes)
            return fail<void>(ErrorCode::QuotaExceeded, "quota_exceeded",
                              "the draft would hold " + std::to_string(after) +
                                  " bytes, more than " + std::to_string(kMaxDraftBytes));
        return deps_.storage.write_atomic(target, bytes_of(content));
    });
}

Result<void> SceneWorkspace::edit_file(const std::string& id, std::string_view path,
                                       std::string_view old_text, std::string_view new_text,
                                       size_t* count) {
    if (count != nullptr) *count = 0;
    WISP_TRY(check_id(id));
    WISP_TRY(check_path(path));
    if (old_text.empty())
        return fail<void>(ErrorCode::InvalidArgument, "bad_args", "old must not be empty");
    auto current = read_file(id, path);
    if (!current) return pass<void>(current.error());
    const os::LargeString& text = current.value();
    size_t n = 0;
    size_t at = std::string_view::npos;
    for (size_t pos = text.find(old_text.data(), 0, old_text.size()); pos != os::LargeString::npos;
         pos = text.find(old_text.data(), pos + old_text.size(), old_text.size())) {
        if (n++ == 0) at = pos;
    }
    if (count != nullptr) *count = n;
    if (n == 0)
        return fail<void>(ErrorCode::NotFound, "not_found",
                          "old text not found in " + std::string(path));
    if (n > 1)
        return fail<void>(
            ErrorCode::AlreadyExists, "not_unique",
            "old text occurs " + std::to_string(n) + " times in " + std::string(path));
    os::LargeString edited(text.substr(0, at));
    edited.append(new_text.data(), new_text.size());
    edited.append(text, at + old_text.size());
    return write_file(id, path, std::string_view(edited.data(), edited.size()));
}

Result<void> SceneWorkspace::delete_file(const std::string& id, std::string_view path) {
    WISP_TRY(check_id(id));
    WISP_TRY(check_path(path));
    return core::call_on_worker<void>(&deps_.worker, [&]() -> Result<void> {
        WISP_TRY(ensure_draft_here(id));
        const std::string target = draft_dir(id) + "/" + std::string(path);
        auto listed = deps_.storage.list_dir(target);
        // A file is "not a directory" to list_dir().
        if (listed || listed.error().code != ErrorCode::InvalidArgument)
            return fail<void>(ErrorCode::NotFound, "not_found",
                              "no " + std::string(path) + " in the draft of " + id);
        return deps_.storage.remove_tree(target);
    });
}

Result<void> SceneWorkspace::discard(const std::string& id, std::string* name) {
    WISP_TRY(check_id(id));
    return core::call_on_worker<void>(&deps_.worker, [&]() -> Result<void> {
        if (!draft_exists_here(id))
            return fail<void>(ErrorCode::NotFound, "not_found", "no draft of " + id);
        if (name != nullptr) *name = name_here(id);
        return deps_.storage.remove_tree(draft_dir(id));
    });
}

ToolOutput SceneWorkspace::install(const std::string& id, ToolCtx& ctx) {
    if (auto ok = check_id(id); !ok) return from_error(ok.error());
    if (deps_.repo.dev()) return tool_error("unsupported", "scenes are read-only in dev mode");

    // 1. The draft, read on the flash thread.
    Files files;
    Result<void> read = core::call_on_worker<void>(&deps_.worker, [&]() -> Result<void> {
        if (!draft_exists_here(id))
            return fail<void>(ErrorCode::NotFound, "not_found", "no draft of " + id);
        return walk(deps_.storage, draft_dir(id), [&](const std::string& rel, uint64_t) {
            auto body = deps_.storage.read_file_large(draft_dir(id) + "/" + rel);
            if (!body) return pass<void>(body.error());
            files.emplace_back(rel, std::move(body.value()));
            return Result<void>::ok();
        });
    });
    if (!read) return from_error(read.error());

    // 2. scene.json and the files it names.
    auto manifest_file = std::find_if(files.begin(), files.end(),
                                      [](const auto& f) { return f.first == kManifest; });
    if (manifest_file == files.end())
        return check_failed({{std::string(kManifest), 0, "the draft has no scene.json", ""}});
    auto parsed = core::parse_scene_manifest(text_of(manifest_file->second));
    if (!parsed) return check_failed({{std::string(kManifest), 0, parsed.error().message, ""}});
    const core::SceneManifest& m = parsed.value();
    if (m.id != id)
        return check_failed({{std::string(kManifest), 0,
                              "id '" + m.id + "' does not match the draft's id '" + id + "'",
                              "\"id\": \"" + id + "\""}});
    std::vector<InstallError> errors;
    bool has_entry = false;
    bool has_icon = m.icon.empty();
    for (const auto& [path, body] : files) {
        has_entry = has_entry || path == m.entry;
        has_icon = has_icon || path == m.icon;
        if (path.find('/') == std::string::npos && path != kManifest && path != m.entry)
            errors.push_back(
                {path, 0, "only the entry file may sit at the top level", "lib/" + path});
    }
    if (!has_entry) errors.push_back({m.entry, 0, "the entry file is missing", ""});
    if (!has_icon) errors.push_back({m.icon, 0, "the icon file is missing", ""});
    if (!errors.empty()) return check_failed(std::move(errors));

    // 3. New http hosts need the user's yes.
    std::vector<std::string> added;
    {
        auto old = deps_.repo.manifest(id);
        for (const std::string& h : m.http_hosts) {
            const bool known =
                old && std::find(old.value().http_hosts.begin(), old.value().http_hosts.end(), h) !=
                           old.value().http_hosts.end();
            if (!known) added.push_back(h);
        }
    }
    if (!added.empty()) {
        const ui::Lang lang = lang_of(ctx);
        std::string text = ui::tr_fill(ui::StrId::AgentConfirmHosts, lang, m.name);
        for (size_t i = 0; i < added.size(); ++i) {
            if (i > 0) text += ui::tr(ui::StrId::AgentListSep, lang);
            text += added[i];
        }
        if (!deps_.confirm || !deps_.confirm(text, ctx))
            return tool_error("denied", "the user did not allow the new http hosts");
    }

    // 4. Every .lua file compiles.
    os::LargeVector<core::SourceFile> sources;
    for (const auto& [path, body] : files)
        if (ends_with(path, ".lua")) sources.push_back({path, text_of(body)});
    Result<void> compiled = deps_.runtime.check_sources(std::span<const core::SourceFile>(sources));
    if (!compiled) {
        if (compiled.error().code != ErrorCode::InvalidArgument)
            return from_error(compiled.error());
        return check_failed({parse_error(id, compiled.error().message)});
    }
    sources.clear();

    // 5. The entry runs and its tree builds.
    auto package = std::make_shared<core::ScenePackage>();
    package->manifest = m;
    core::ZipWriter zip;
    for (auto& [path, body] : files) {
        zip.add(path, body.span());
        package->files.emplace_back(path, std::move(body));
    }
    files.clear();
    Result<void> built = deps_.runtime.check_build(package);
    package.reset();
    if (!built) {
        if (built.error().code != ErrorCode::InvalidArgument) return from_error(built.error());
        return check_failed({parse_error(id, built.error().message)});
    }

    // 6-8 and the check.
    os::LargeBuffer archive = zip.finish();
    if (!archive.ok()) return tool_error("no_memory", "no memory for the package");
    const ui::Lang lang = lang_of(ctx);
    const std::string note = ui::tr_fill(ui::StrId::AgentNoteInstalled, lang, m.name) +
                             ui::tr_fill(ui::StrId::AgentVersionSuffix, lang, m.version);
    return install_zip(id, archive.span(), true, note, ctx);
}

ToolOutput SceneWorkspace::rollback(const std::string& id, ToolCtx& ctx) {
    if (auto ok = check_id(id); !ok) return from_error(ok.error());
    if (deps_.repo.dev()) return tool_error("unsupported", "scenes are read-only in dev mode");
    if (!deps_.repo.has_prev(id))
        return tool_error("not_found", "scene " + id + " has no previous version");
    auto zip = deps_.repo.prev_zip(id);
    if (!zip) return from_error(zip.error());
    std::string name = id;
    if (auto m = deps_.repo.manifest(id); m && !m.value().name.empty()) name = m.value().name;
    const std::string note = ui::tr_fill(ui::StrId::AgentNoteRolledBack, lang_of(ctx), name);
    return install_zip(id, zip.value().span(), false, note, ctx);
}

ToolOutput SceneWorkspace::install_zip(const std::string& id, std::span<const uint8_t> zip,
                                       bool drop_draft, const std::string& note, ToolCtx&) {
    // 6. Lines the scene logs from here on belong to the new version.
    const uint64_t since_ms = os::now_ms();
    auto installed = core::install_scene(deps_.repo, deps_.runtime, zip,
                                         core::InstallOptions{.keep_prev = true});
    if (!installed) return from_error(installed.error());
    const core::SceneManifest& m = installed.value().manifest;

    // 7. In front: install_scene() already reopened one that was there.
    Map out{{"ok", Value(true)},
            {"id", Value(id)},
            {"name", Value(m.name)},
            {"version", Value(m.version)}};
    const bool in_front = deps_.runtime.foreground() == id &&
                          deps_.runtime.status(id).state == core::SceneState::Running;
    bool open_ok = true;
    if (!in_front) {
        Result<void> opened = deps_.runtime.open(id);
        open_ok = static_cast<bool>(opened);
        if (!opened) {
            WLOG_W(kTag, "%s installed but not opened: %s", id.c_str(),
                   opened.error().message.c_str());
            out["open_error"] = Value(opened.error().message);
        }
    }

    // 8. The draft is what was just installed.
    if (drop_draft) {
        Result<void> dropped = core::call_on_worker<void>(
            &deps_.worker, [&] { return deps_.storage.remove_tree(draft_dir(id)); });
        if (!dropped)
            WLOG_W(kTag, "%s: draft not removed: %s", id.c_str(), dropped.error().message.c_str());
    }

    // The check (spec 5.1).
    bool stable = false;
    if (deps_.inspector != nullptr) {
        auto settled = deps_.inspector->wait_stable();
        stable = settled && settled.value();
    }
    List lua_errors;
    const core::SceneStatus status = deps_.runtime.status(id);
    if (status.state == core::SceneState::Error)
        lua_errors.push_back(Value(scene::scene_data(id, status.error)));
    LastLines warns, errs;
    const std::string tag = "scene." + id;
    log::for_each_ring_entry([&](const LogEntryView& e) {
        if (e.ts_ms < since_ms || e.tag != tag) return;
        if (e.level == LogLevel::Warn) warns.push(e.msg);
        if (e.level == LogLevel::Error) errs.push(e.msg);
    });
    std::string tree;
    // A failed open may leave another scene in front, whose tree would
    // describe the wrong scene: then the check carries none, and
    // open_error says why.
    const bool described = open_ok || deps_.runtime.foreground() == id;
    if (deps_.inspector != nullptr && described) {
        auto t = deps_.inspector->tree();
        // On an error, its message: "scene_error: ..." for a scene that
        // faulted, with the scene's text already wrapped.
        tree = t ? std::move(t.value()) : t.error().message;
    }
    out["check"] = Value(Map{
        {"lua_errors", Value(std::move(lua_errors))},
        {"logs", Value(Map{{"W", Value(warns.take(id))}, {"E", Value(errs.take(id))}})},
        {"tree", Value(std::move(tree))},
        {"stable", Value(stable)},
    });
    ToolOutput result;
    result.result = Value(std::move(out));
    result.note = note;
    return result;
}

}  // namespace wisp::agent
