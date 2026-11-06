// scene.open, close, exit, delete, data_read, data_write, files_list,
// files_read, files_remove, logs and eval (agent-core spec 4.2): thin
// adapters over SceneRuntime, SceneRepository, SceneStore and SceneFiles.
//
// Threads: the scene task does the work of open/close/exit/eval and of a
// running scene's store; the agent's thread only waits for it. Everything
// that touches flash goes to the storage worker: files_list and files_read
// run there (ToolThread::Fs); the tools that must also ask the user or
// wait for the scene task run on the agent's thread and send their flash
// work there with run_io().
//
// A stopped scene's store is opened for the one call on the worker, with
// the runtime's own limits, then flushed and closed. A scene opened at that
// very moment loads the store from disk, so it can miss a write that is
// still being flushed; the window is the length of that one flush.
//
// Everything a scene wrote -- its data, files, log lines, eval results
// and errors -- goes back wrapped by scene_data() (spec 6.3).

#include <memory>
#include <string>
#include <string_view>
#include <utility>

#include "tool_io.hpp"
#include "wisp/agent/limits.hpp"
#include "wisp/agent/tools.hpp"
#include "wisp/base/base64.hpp"
#include "wisp/base/json.hpp"
#include "wisp/core/log.hpp"
#include "wisp/core/scene_store.hpp"
#include "wisp/ui/strings.hpp"

namespace wisp::agent {

namespace {

using detail::arg_str;
using detail::done;
using detail::fail;
using detail::run_io;

constexpr uint32_t kSceneMs = 10000;
constexpr uint32_t kDeleteMs = 30000;
constexpr uint32_t kDataMs = 5000;
constexpr uint32_t kFilesMs = 10000;
constexpr size_t kDefaultLogs = 50;

constexpr const char* kIdOnly =
    R"({"type":"object","properties":{"id":{"type":"string","maxLength":32}},"required":["id"]})";

ui::Lang lang_of(const ToolCtx& ctx) { return ui::parse_lang(ctx.lang).value_or(ui::Lang::Zh); }

std::string note(ui::StrId id, const ToolCtx& ctx, std::initializer_list<std::string_view> v) {
    return ui::tr_fill_n(id, lang_of(ctx), v);
}

// The tool code of a SceneFiles error (wisp/core/scene_files.hpp).
ToolOutput files_fail(const Error& e) {
    const std::string_view code = core::scene_files_code(e);
    if (code == "bad_name") return tool_error("bad_path", e.message);
    for (const char* known : {"too_large", "quota_exceeded", "unavailable", "not_found"}) {
        if (code == known) return tool_error(code, e.message);
    }
    return fail(e);
}

int64_t int_arg(const Value& a, const char* key, int64_t fallback) {
    const Value* v = a.find(key);
    if (v == nullptr) return fallback;
    if (v->type() == Value::Type::Int) return v->as_int();
    if (v->type() == Value::Type::Double) return static_cast<int64_t>(v->as_double());
    return fallback;
}

bool ends_with(std::string_view s, std::string_view tail) {
    return s.size() >= tail.size() && s.substr(s.size() - tail.size()) == tail;
}

struct Ops {
    SceneOpsDeps d;

    // The scene's name for notes and questions: the running scene's, else
    // the manifest's, else the id.
    std::string name_of(const std::string& id) {
        std::string name = d.runtime.running_name(id);
        if (!name.empty()) return name;
        auto m = d.repo.manifest(id);
        return m && !m.value().name.empty() ? m.value().name : id;
    }

    bool busy(const std::string& id) const { return d.repo.installing(id) || d.repo.removing(id); }

    // Opens the stopped scene's store on the worker, runs `fn` on it, then
    // flushes (when `write`) and closes it. NotFound when the scene is not
    // installed.
    template <typename T>
    Result<T> with_stopped_store(const std::string& id, bool write,
                                 std::function<Result<T>(core::SceneStore&)> fn,
                                 const ToolCtx& ctx) {
        auto self = std::make_shared<Ops>(*this);
        return run_io<T>(
            d.submit_io,
            [self, id, write, fn = std::move(fn)]() -> Result<T> {
                auto m = self->d.repo.manifest(id);
                if (!m) return Result<T>::err(m.error().code, m.error().message);
                // The worker runs the store's jobs inline: this is the worker.
                core::SceneStoreDeps sd{self->d.storage,
                                        [](std::function<void()> job) {
                                            job();
                                            return true;
                                        },
                                        [](uint32_t, std::function<void()>) {}};
                auto store = core::SceneStore::create(std::move(sd), self->d.repo.data_dir(id));
                if (Result<void> loaded = store->load(); !loaded) {
                    store->close();
                    return Result<T>::err(loaded.error().code, loaded.error().message);
                }
                if (store->data_version() == 0) {
                    store->set_initial_version(m.value().schema_version);
                }
                Result<T> out = fn(*store);
                if (write && out) {
                    Result<void> flushed = Result<void>::ok();
                    store->flush([&flushed](Result<void> r) { flushed = std::move(r); });
                    if (!flushed)
                        out = Result<T>::err(flushed.error().code, flushed.error().message);
                }
                store->close();
                return out;
            },
            kDataMs, ctx);
    }
};

}  // namespace

Result<void> add_scene_ops_tools(ToolRegistry& reg, SceneOpsDeps deps) {
    auto ops = std::make_shared<Ops>(Ops{std::move(deps)});

    WISP_TRY(reg.add({
        .name = "scene.open",
        .description = "Opens a scene in the foreground (one in the background comes forward).",
        .schema_json = kIdOnly,
        .risk = ToolRisk::Note,
        .thread = ToolThread::Agent,
        .timeout_ms = kSceneMs,
        .run =
            [ops](const Value& a, ToolCtx& ctx) {
                const std::string id = arg_str(a, "id");
                if (ops->busy(id)) return tool_error("busy", "the scene is being replaced");
                if (Result<void> r = ops->d.runtime.open(id); !r) return fail(r.error());
                const core::SceneStatus st = ops->d.runtime.status(id);
                return done({{"state", Value(core::to_string(st.state))}},
                            note(ui::StrId::AgentNoteOpened, ctx, {ops->name_of(id)}));
            },
    }));

    WISP_TRY(reg.add({
        .name = "scene.close",
        .description = "Back: a scene that declares \"background\" goes to the background, any "
                       "other closes.",
        .schema_json = kIdOnly,
        .risk = ToolRisk::Note,
        .thread = ToolThread::Agent,
        .timeout_ms = kSceneMs,
        .run =
            [ops](const Value& a, ToolCtx& ctx) {
                const std::string id = arg_str(a, "id");
                if (ops->d.runtime.status(id).state == core::SceneState::Stopped) {
                    return tool_error("not_found", "the scene is not running");
                }
                const std::string name = ops->name_of(id);
                if (Result<void> r = ops->d.runtime.close(id); !r) return fail(r.error());
                const core::SceneState st = ops->d.runtime.status(id).state;
                return done(
                    {{"state", Value(core::to_string(st))}},
                    note(st == core::SceneState::Background ? ui::StrId::AgentNoteToBackground
                                                            : ui::StrId::AgentNoteClosed,
                         ctx, {name}));
            },
    }));

    WISP_TRY(reg.add({
        .name = "scene.exit",
        .description = "Closes a scene completely, in the foreground or the background.",
        .schema_json = kIdOnly,
        .risk = ToolRisk::Note,
        .thread = ToolThread::Agent,
        .timeout_ms = kSceneMs,
        .run =
            [ops](const Value& a, ToolCtx& ctx) {
                const std::string id = arg_str(a, "id");
                if (ops->d.runtime.status(id).state == core::SceneState::Stopped) {
                    return tool_error("not_found", "the scene is not running");
                }
                const std::string name = ops->name_of(id);
                if (Result<void> r = ops->d.runtime.exit(id); !r) return fail(r.error());
                const core::SceneState st = ops->d.runtime.status(id).state;
                return done({{"state", Value(core::to_string(st))}},
                            note(ui::StrId::AgentNoteExited, ctx, {name}));
            },
    }));

    WISP_TRY(reg.add({
        .name = "scene.delete",
        .description = "Deletes a scene after the user agrees on the device; with_data also "
                       "deletes its data and files.",
        .schema_json = R"({"type":"object","properties":{"id":{"type":"string","maxLength":32},)"
                       R"("with_data":{"type":"boolean"}},"required":["id"]})",
        .risk = ToolRisk::Confirm,
        .thread = ToolThread::Agent,
        .timeout_ms = kDeleteMs,
        .run =
            [ops](const Value& a, ToolCtx& ctx) {
                const std::string id = arg_str(a, "id");
                const Value* wd = a.find("with_data");
                const bool with_data = wd != nullptr && wd->as_bool();
                if (ops->busy(id)) return tool_error("busy", "the scene is being replaced");
                auto removal = ops->d.repo.removal(id, with_data);
                if (!removal) return fail(removal.error());
                const std::string& name = removal.value().name;
                std::string ask = note(ui::StrId::AgentConfirmDelete, ctx, {name});
                if (with_data) ask += ui::tr(ui::StrId::AgentConfirmWithData, lang_of(ctx));
                if (!ops->d.confirm || !ops->d.confirm(ask, ctx)) {
                    return tool_error("denied", "the user said no");
                }
                if (!ops->d.delete_scene) return tool_error("unavailable", "cannot delete");
                struct Wait {
                    os::Semaphore done{1, 0};
                    Result<void> result = Result<void>::ok();
                };
                auto w = std::make_shared<Wait>();
                ops->d.delete_scene(id, with_data, [w](Result<void> r) {
                    w->result = std::move(r);
                    w->done.give();
                });
                if (!w->done.take(kDeleteMs)) return tool_error("timeout", "delete timed out");
                if (!w->result) return fail(w->result.error());
                return done({}, note(ui::StrId::AgentNoteDeleted, ctx, {name}));
            },
    }));

    WISP_TRY(reg.add({
        .name = "scene.data_read",
        .description = "The scene's stored data (wisp.store) as JSON, at most 16 KB; "
                       "\"truncated\" when cut.",
        .schema_json = kIdOnly,
        .thread = ToolThread::Agent,
        .timeout_ms = kDataMs,
        .run =
            [ops](const Value& a, ToolCtx& ctx) {
                const std::string id = arg_str(a, "id");
                if (ops->busy(id)) return tool_error("busy", "the scene is being replaced");
                auto r = ops->d.runtime.store_dump(id, kDataReadMaxBytes);
                if (!r && r.error().code == ErrorCode::NotFound) {
                    r = ops->with_stopped_store<os::LargeString>(
                        id, false,
                        [](core::SceneStore& s) {
                            return Result<os::LargeString>::ok(s.dump_json(kDataReadMaxBytes));
                        },
                        ctx);
                }
                if (!r) return fail(r.error());
                const std::string_view text(r.value().data(), r.value().size());
                return done({{"data", Value(scene_data(id, text))},
                             {"truncated", Value(ends_with(text, R"("truncated":true})"))}});
            },
    }));

    WISP_TRY(reg.add({
        .name = "scene.data_write",
        .description = "Sets one key of the scene's wisp.store to a JSON value; a running scene "
                       "sees it at once.",
        .schema_json = R"({"type":"object","properties":{"id":{"type":"string","maxLength":32},)"
                       R"("key":{"type":"string","maxLength":64},"value":{"type":)"
                       R"(["object","array","string","number","boolean"]}},)"
                       R"("required":["id","key","value"]})",
        .risk = ToolRisk::Note,
        .thread = ToolThread::Agent,
        .timeout_ms = kDataMs,
        .run =
            [ops](const Value& a, ToolCtx& ctx) {
                const std::string id = arg_str(a, "id");
                const std::string key = arg_str(a, "key");
                if (ops->busy(id)) return tool_error("busy", "the scene is being replaced");
                const std::string json = to_json(*a.find("value"));
                Result<void> r = ops->d.runtime.store_set(id, key, json);
                if (!r && r.error().code == ErrorCode::NotFound) {
                    auto w = ops->with_stopped_store<bool>(
                        id, true,
                        [key, json](core::SceneStore& s) -> Result<bool> {
                            WISP_TRY(s.kv_set(key, json));
                            return Result<bool>::ok(true);
                        },
                        ctx);
                    r = w ? Result<void>::ok()
                          : Result<void>::err(w.error().code, w.error().message);
                }
                if (!r) return fail(r.error());
                return done({},
                            note(ui::StrId::AgentNoteDataWritten, ctx, {ops->name_of(id), key}));
            },
    }));

    WISP_TRY(reg.add({
        .name = "scene.files_list",
        .description = "The scene's private files (wisp.files): name, size and mtime.",
        .schema_json = kIdOnly,
        .thread = ToolThread::Fs,
        .timeout_ms = kFilesMs,
        .run =
            [ops](const Value& a, ToolCtx&) {
                if (ops->d.files == nullptr) return tool_error("unavailable", "no scene files");
                auto r = ops->d.files->list(arg_str(a, "id"));
                if (!r) return files_fail(r.error());
                List files;
                for (const core::SceneFileEntry& e : r.value().entries) {
                    files.emplace_back(
                        Map{{"name", Value(std::string(e.name.data(), e.name.size()))},
                            {"size", Value(static_cast<int64_t>(e.size))},
                            {"mtime", Value(e.mtime)}});
                }
                return done({{"files", Value(std::move(files))},
                             {"truncated", Value(r.value().truncated)}});
            },
    }));

    WISP_TRY(reg.add({
        .name = "scene.files_read",
        .description = "Reads a scene file from `offset`, at most `len` bytes (default and "
                       "limit 64 KB). Binary content comes as base64 with \"encoding\".",
        .schema_json = R"({"type":"object","properties":{"id":{"type":"string","maxLength":32},)"
                       R"("name":{"type":"string","maxLength":128},)"
                       R"("offset":{"type":"integer","minimum":0},)"
                       R"("len":{"type":"integer","minimum":0}},"required":["id","name"]})",
        .thread = ToolThread::Fs,
        .timeout_ms = kFilesMs,
        .run =
            [ops](const Value& a, ToolCtx&) {
                if (ops->d.files == nullptr) return tool_error("unavailable", "no scene files");
                const std::string id = arg_str(a, "id");
                const int64_t len = int_arg(a, "len", static_cast<int64_t>(kReadMaxBytes));
                if (len > static_cast<int64_t>(kReadMaxBytes)) {
                    return tool_error("too_large", "len is at most 65536");
                }
                auto r = ops->d.files->read(id, arg_str(a, "name"),
                                            static_cast<uint64_t>(int_arg(a, "offset", 0)),
                                            static_cast<size_t>(len));
                if (!r) return files_fail(r.error());
                const std::string_view bytes(reinterpret_cast<const char*>(r.value().data()),
                                             r.value().size());
                Map out{{"bytes", Value(static_cast<int64_t>(bytes.size()))}};
                if (detail::is_text(bytes)) {
                    out["content"] = Value(scene_data(id, bytes));
                } else {
                    out["content"] =
                        Value(scene_data(id, base64_encode(std::span<const uint8_t>(
                                                 r.value().data(), r.value().size()))));
                    out["encoding"] = Value("base64");
                }
                return done(std::move(out));
            },
    }));

    WISP_TRY(reg.add({
        .name = "scene.files_remove",
        .description = "Deletes one of the scene's files after the user agrees on the device.",
        .schema_json = R"({"type":"object","properties":{"id":{"type":"string","maxLength":32},)"
                       R"("name":{"type":"string","maxLength":128}},"required":["id","name"]})",
        .risk = ToolRisk::Confirm,
        .thread = ToolThread::Agent,
        .timeout_ms = kFilesMs,
        .run =
            [ops](const Value& a, ToolCtx& ctx) {
                if (ops->d.files == nullptr) return tool_error("unavailable", "no scene files");
                const std::string id = arg_str(a, "id");
                const std::string file = arg_str(a, "name");
                core::SceneFiles* files = ops->d.files;
                auto st = run_io<core::SceneFileEntry>(
                    ops->d.submit_io, [files, id, file] { return files->stat(id, file); }, kFilesMs,
                    ctx);
                if (!st) return files_fail(st.error());
                const std::string name = ops->name_of(id);
                if (!ops->d.confirm ||
                    !ops->d.confirm(note(ui::StrId::AgentConfirmFileRemove, ctx, {name, file}),
                                    ctx)) {
                    return tool_error("denied", "the user said no");
                }
                auto r = run_io<bool>(
                    ops->d.submit_io,
                    [files, id, file]() -> Result<bool> {
                        WISP_TRY(files->remove(id, file));
                        return Result<bool>::ok(true);
                    },
                    kFilesMs, ctx);
                if (!r) return files_fail(r.error());
                return done({}, note(ui::StrId::AgentNoteFileRemoved, ctx, {name, file}));
            },
    }));

    WISP_TRY(reg.add({
        .name = "scene.logs",
        .description = "The scene's newest `n` log lines (print, wisp.log; default 50, at most "
                       "100), oldest first.",
        .schema_json = R"({"type":"object","properties":{"id":{"type":"string","maxLength":32},)"
                       R"("n":{"type":"integer","minimum":1,"maximum":100}},"required":["id"]})",
        .thread = ToolThread::Agent,
        .run =
            [](const Value& a, ToolCtx&) {
                const std::string id = arg_str(a, "id");
                const size_t n = static_cast<size_t>(int_arg(a, "n", kDefaultLogs));
                const std::string tag = "scene." + id;
                // The last n matching lines, kept in a ring in PSRAM.
                os::LargeVector<os::LargeString> ring(n);
                size_t seen = 0;
                log::for_each_ring_entry([&](const LogEntryView& e) {
                    if (e.tag != tag) return;
                    static constexpr char kLevel[] = {'D', 'I', 'W', 'E'};
                    os::LargeString& line = ring[seen % n];
                    line.clear();
                    line += std::to_string(e.ts_ms);
                    line += ' ';
                    line += kLevel[static_cast<int>(e.level) & 3];
                    line += ' ';
                    line.append(e.msg.data(), e.msg.size());
                    line += '\n';
                    ++seen;
                });
                const size_t count = seen < n ? seen : n;
                os::LargeString text;
                for (size_t i = seen - count; i < seen; ++i) text += ring[i % n];
                return done(
                    {{"logs", Value(scene_data(id, std::string_view(text.data(), text.size())))},
                     {"count", Value(static_cast<int64_t>(count))}});
            },
    }));

    WISP_TRY(reg.add({
        .name = "scene.eval",
        .description = "Runs Lua in a running scene's VM (at most 200 ms) and returns its "
                       "return values as JSON. Errors leave the scene running.",
        .schema_json = R"({"type":"object","properties":{"id":{"type":"string","maxLength":32},)"
                       R"("lua":{"type":"string","maxLength":16384}},"required":["id","lua"]})",
        .risk = ToolRisk::Note,
        .thread = ToolThread::Agent,
        .run =
            [ops](const Value& a, ToolCtx& ctx) {
                const std::string id = arg_str(a, "id");
                auto r = ops->d.runtime.eval(id, arg_str(a, "lua"), kEvalMs);
                if (!r) {
                    if (r.error().code == ErrorCode::InvalidArgument) {
                        return tool_error("scene_error", scene_data(id, r.error().message));
                    }
                    if (r.error().code == ErrorCode::NotFound) {
                        return tool_error("not_found", "the scene is not running");
                    }
                    return fail(r.error());
                }
                const std::string_view text(r.value().data(), r.value().size());
                return done({{"result", Value(scene_data(id, text))}},
                            note(ui::StrId::AgentNoteEval, ctx, {ops->name_of(id)}));
            },
    }));

    return Result<void>::ok();
}

}  // namespace wisp::agent
