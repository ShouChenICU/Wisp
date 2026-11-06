// scene.list, list_files, read_file, write_file, edit_file, delete_file,
// install, discard_draft and rollback (agent-core spec 4.1): thin adapters
// over SceneWorkspace. The draft tools run on the flash thread (Fs), since
// every one of them reads or writes files; install and rollback run on the
// agent's thread, as they wait for the scene task, the UI and the flash
// thread in turn (SceneWorkspace does the hops).

#include <string>
#include <utility>

#include "wisp/agent/scene_workspace.hpp"
#include "wisp/agent/tools.hpp"
#include "wisp/ui/strings.hpp"

namespace wisp::agent {

namespace {

constexpr const char* kIdOnly =
    R"({"type":"object","properties":{"id":{"type":"string","maxLength":32}},"required":["id"]})";
constexpr const char* kIdPath =
    R"({"type":"object","properties":{"id":{"type":"string","maxLength":32},)"
    R"("path":{"type":"string","maxLength":128}},"required":["id","path"]})";
constexpr const char* kWrite =
    R"({"type":"object","properties":{"id":{"type":"string","maxLength":32},)"
    R"("path":{"type":"string","maxLength":128},"content":{"type":"string"}},)"
    R"("required":["id","path","content"]})";
constexpr const char* kEdit =
    R"({"type":"object","properties":{"id":{"type":"string","maxLength":32},)"
    R"("path":{"type":"string","maxLength":128},"old":{"type":"string"},)"
    R"("new":{"type":"string"}},"required":["id","path","old","new"]})";

const std::string& arg(const Value& args, const char* key) {
    static const std::string kEmpty;
    const Value* v = args.find(key);
    return v != nullptr && v->type() == Value::Type::String ? v->as_string() : kEmpty;
}

ToolOutput fail(const Error& e, Map extra = {}) {
    return tool_error(error_code(e), e.message, std::move(extra));
}

ToolOutput done(Map fields = {}) {
    fields["ok"] = Value(true);
    ToolOutput out;
    out.result = Value(std::move(fields));
    return out;
}

// Whether `s` is UTF-8 a JSON string can carry as it is.
bool is_text(std::string_view s) {
    size_t i = 0;
    while (i < s.size()) {
        const unsigned char c = static_cast<unsigned char>(s[i]);
        size_t n = c < 0x80           ? 1
                   : (c >> 5) == 0x6  ? 2
                   : (c >> 4) == 0xE  ? 3
                   : (c >> 3) == 0x1E ? 4
                                      : 0;
        if (n == 0 || (c < 0x20 && c != '\n' && c != '\r' && c != '\t') || i + n > s.size())
            return false;
        for (size_t k = 1; k < n; ++k)
            if ((static_cast<unsigned char>(s[i + k]) & 0xC0) != 0x80) return false;
        i += n;
    }
    return true;
}

}  // namespace

Result<void> add_scene_authoring_tools(ToolRegistry& reg, SceneWorkspace& ws) {
    WISP_TRY(reg.add({
        .name = "scene.list",
        .description = "Installed scenes and drafts: id, name, version, state, bytes and "
                       "whether a draft exists.",
        .thread = ToolThread::Fs,
        .run = [&ws](const Value&, ToolCtx&) { return done({{"scenes", ws.list()}}); },
    }));
    WISP_TRY(reg.add({
        .name = "scene.list_files",
        .description = "The files of a scene's draft, or of the installed scene without one.",
        .schema_json = kIdOnly,
        .thread = ToolThread::Fs,
        .run =
            [&ws](const Value& a, ToolCtx&) {
                auto r = ws.list_files(arg(a, "id"));
                if (!r) return fail(r.error());
                Map out = r.value().as_map();
                return done(std::move(out));
            },
    }));
    WISP_TRY(reg.add({
        .name = "scene.read_file",
        .description = "One file of the draft (or of the installed scene without a draft), at "
                       "most 64 KB.",
        .schema_json = kIdPath,
        .thread = ToolThread::Fs,
        .run =
            [&ws](const Value& a, ToolCtx&) {
                const std::string& id = arg(a, "id");
                const std::string& path = arg(a, "path");
                const bool draft = ws.has_draft(id);
                auto r = ws.read_file(id, path);
                if (!r) return fail(r.error());
                const std::string_view text(r.value().data(), r.value().size());
                Map out{{"path", Value(path)},
                        {"bytes", Value(static_cast<int64_t>(text.size()))},
                        {"draft", Value(draft)}};
                if (is_text(text)) {
                    out["content"] = Value(scene_data(id, text));
                } else {
                    out["binary"] = Value(true);
                }
                return done(std::move(out));
            },
    }));
    WISP_TRY(reg.add({
        .name = "scene.write_file",
        .description = "Writes a whole file into the scene's draft (made from the installed "
                       "scene on the first change).",
        .schema_json = kWrite,
        .thread = ToolThread::Fs,
        .run =
            [&ws](const Value& a, ToolCtx&) {
                auto r = ws.write_file(arg(a, "id"), arg(a, "path"), arg(a, "content"));
                return r ? done() : fail(r.error());
            },
    }));
    WISP_TRY(reg.add({
        .name = "scene.edit_file",
        .description = "Replaces `old`, which must occur exactly once in the file, with `new` in "
                       "the draft.",
        .schema_json = kEdit,
        .thread = ToolThread::Fs,
        .run =
            [&ws](const Value& a, ToolCtx&) {
                size_t count = 0;
                auto r = ws.edit_file(arg(a, "id"), arg(a, "path"), arg(a, "old"), arg(a, "new"),
                                      &count);
                Map extra{{"count", Value(static_cast<int64_t>(count))}};
                return r ? done(std::move(extra)) : fail(r.error(), std::move(extra));
            },
    }));
    WISP_TRY(reg.add({
        .name = "scene.delete_file",
        .description = "Deletes a file from the draft only.",
        .schema_json = kIdPath,
        .thread = ToolThread::Fs,
        .run =
            [&ws](const Value& a, ToolCtx&) {
                auto r = ws.delete_file(arg(a, "id"), arg(a, "path"));
                return r ? done() : fail(r.error());
            },
    }));
    WISP_TRY(reg.add({
        .name = "scene.install",
        .description = "Checks the draft (scene.json, Lua syntax, the widget tree), installs it "
                       "keeping the old version for scene.rollback, opens it and reports a "
                       "check: Lua errors, new W/E logs and the UI tree.",
        .schema_json = kIdOnly,
        .risk = ToolRisk::Note,
        .thread = ToolThread::Agent,
        .timeout_ms = 30000,
        .run = [&ws](const Value& a, ToolCtx& ctx) { return ws.install(arg(a, "id"), ctx); },
    }));
    WISP_TRY(reg.add({
        .name = "scene.discard_draft",
        .description = "Throws the scene's draft away.",
        .schema_json = kIdOnly,
        .risk = ToolRisk::Note,
        .thread = ToolThread::Fs,
        .run =
            [&ws](const Value& a, ToolCtx& ctx) {
                std::string name;
                auto r = ws.discard(arg(a, "id"), &name);
                if (!r) return fail(r.error());
                ToolOutput out = done();
                out.note = ui::tr_fill(ui::StrId::AgentNoteDiscarded,
                                       ui::parse_lang(ctx.lang).value_or(ui::Lang::Zh), name);
                return out;
            },
    }));
    WISP_TRY(reg.add({
        .name = "scene.rollback",
        .description = "Swaps the installed scene with its previous version, opens it and "
                       "reports the same check as scene.install.",
        .schema_json = kIdOnly,
        .risk = ToolRisk::Note,
        .thread = ToolThread::Agent,
        .timeout_ms = 30000,
        .run = [&ws](const Value& a, ToolCtx& ctx) { return ws.rollback(arg(a, "id"), ctx); },
    }));
    return Result<void>::ok();
}

}  // namespace wisp::agent
