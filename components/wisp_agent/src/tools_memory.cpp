// memory.list, add, update and remove (mvp spec 3.2, 5.1): thin adapters
// over MemoryStore. Every one of them reads or writes notes.json, so all
// four run on the flash thread (ToolThread::Fs), list() included: it
// never touches flash itself, but keeping the whole set on one thread
// means a list() a caller issues right after an add() sees it, without a
// second hop to find out.
//
// MemoryStore's own error messages ("empty", "too_long", "memory_full",
// "not_found") already are the tool error codes the model should act on,
// so fail() passes them through unchanged rather than mapping by
// ErrorCode category.

#include <cstdint>
#include <string>
#include <utility>

#include "tool_io.hpp"
#include "wisp/agent/tools.hpp"
#include "wisp/ui/strings.hpp"

namespace wisp::agent {

namespace {

using detail::arg_str;
using detail::done;

constexpr uint32_t kMemoryMs = 5000;
constexpr const char* kAddSchema =
    R"({"type":"object","properties":{"text":{"type":"string"}},"required":["text"]})";
constexpr const char* kIdSchema =
    R"({"type":"object","properties":{"id":{"type":"integer"}},"required":["id"]})";
constexpr const char* kUpdateSchema =
    R"({"type":"object","properties":{"id":{"type":"integer"},"text":{"type":"string"}},)"
    R"("required":["id","text"]})";

ui::Lang lang_of(const ToolCtx& ctx) { return ui::parse_lang(ctx.lang).value_or(ui::Lang::Zh); }

ToolOutput fail(const Error& e) { return tool_error(e.message, e.message); }

uint32_t id_arg(const Value& a) {
    const Value* v = a.find("id");
    return v != nullptr && v->type() == Value::Type::Int ? static_cast<uint32_t>(v->as_int()) : 0;
}

}  // namespace

Result<void> add_memory_tools(ToolRegistry& reg, MemoryToolDeps deps) {
    // Lives as long as the tools, so in PSRAM with the registry.
    auto d =
        std::allocate_shared<MemoryToolDeps>(os::LargeAllocator<MemoryToolDeps>(), std::move(deps));

    WISP_TRY(reg.add({
        .name = "memory.list",
        .description = "Every memory note: id, text and when it was last added or changed.",
        .thread = ToolThread::Fs,
        .timeout_ms = kMemoryMs,
        .run =
            [d](const Value&, ToolCtx&) {
                List items;
                for (const core::MemoryNote& n : d->store.list()) {
                    items.push_back(
                        Value(Map{{"id", Value(static_cast<int64_t>(n.id))},
                                  {"text", Value(std::string(n.text.data(), n.text.size()))},
                                  {"updated", Value(n.updated)}}));
                }
                return done({{"notes", Value(std::move(items))}});
            },
    }));

    WISP_TRY(reg.add({
        .name = "memory.add",
        .description = "Remembers one long-term note (a preference, what to call the user, "
                       "where they live and the like); never a one-off fact. At most 200 "
                       "characters, at most 64 notes: merge related ones before adding another.",
        .schema_json = kAddSchema,
        .risk = ToolRisk::Note,
        .thread = ToolThread::Fs,
        .timeout_ms = kMemoryMs,
        .run =
            [d](const Value& a, ToolCtx& ctx) {
                auto r = d->store.add(arg_str(a, "text"));
                if (!r) return fail(r.error());
                ToolOutput out = done({{"id", Value(static_cast<int64_t>(r.value()))}});
                out.note = ui::tr(ui::StrId::AgentNoteMemoryAdded, lang_of(ctx));
                return out;
            },
    }));

    WISP_TRY(reg.add({
        .name = "memory.update",
        .description = "Replaces one memory note's text; same limits as memory_add.",
        .schema_json = kUpdateSchema,
        .risk = ToolRisk::Note,
        .thread = ToolThread::Fs,
        .timeout_ms = kMemoryMs,
        .run =
            [d](const Value& a, ToolCtx& ctx) {
                const uint32_t id = id_arg(a);
                auto r = d->store.update(id, arg_str(a, "text"));
                if (!r) return fail(r.error());
                ToolOutput out = done();
                out.note = ui::tr_fill(ui::StrId::AgentNoteMemoryUpdated, lang_of(ctx),
                                       std::to_string(id));
                return out;
            },
    }));

    return reg.add({
        .name = "memory.remove",
        .description = "Deletes one memory note.",
        .schema_json = kIdSchema,
        .risk = ToolRisk::Note,
        .thread = ToolThread::Fs,
        .timeout_ms = kMemoryMs,
        .run =
            [d](const Value& a, ToolCtx& ctx) {
                const uint32_t id = id_arg(a);
                auto r = d->store.remove(id);
                if (!r) return fail(r.error());
                ToolOutput out = done();
                out.note = ui::tr_fill(ui::StrId::AgentNoteMemoryRemoved, lang_of(ctx),
                                       std::to_string(id));
                return out;
            },
    });
}

}  // namespace wisp::agent
