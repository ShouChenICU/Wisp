// todo.list, add, update, done and remove (mvp spec 3.2, 5.2): thin
// adapters over ReminderScheduler. The mutating ones write todos.json, so
// the whole set runs on the flash thread (ToolThread::Fs), list included,
// so that a list right after an add sees it.
//
// Times cross the wire as local "YYYY-MM-DDTHH:MM" (the device's UTC
// offset); a due time or repeat rule of the wrong shape is bad_args naming
// the field. The scheduler's own error messages ("empty", "too_long",
// "todo_full", "clock_not_set", "not_found", ...) are the tool error codes.

#include <cstdint>
#include <optional>
#include <string>
#include <utility>

#include "tool_io.hpp"
#include "wisp/agent/tools.hpp"
#include "wisp/ui/strings.hpp"

namespace wisp::agent {

namespace {

using detail::arg_str;
using detail::done;

constexpr uint32_t kTodoMs = 5000;
constexpr const char* kListSchema = R"({"type":"object","properties":{"filter":{"type":"string",)"
                                    R"("enum":["open","done","all","due_today"]}}})";
constexpr const char* kAddSchema =
    R"({"type":"object","properties":{"title":{"type":"string"},"due":{"type":"string"},)"
    R"("repeat":{"type":["string","object"]}},"required":["title"]})";
constexpr const char* kUpdateSchema =
    R"({"type":"object","properties":{"id":{"type":"integer"},"fields":{"type":"object",)"
    R"("properties":{"title":{"type":"string"},"repeat":{"type":["string","object"]}}}},)"
    R"("required":["id","fields"]})";
constexpr const char* kIdSchema =
    R"({"type":"object","properties":{"id":{"type":"integer"}},"required":["id"]})";

ui::Lang lang_of(const ToolCtx& ctx) { return ui::parse_lang(ctx.lang).value_or(ui::Lang::Zh); }

ToolOutput fail(const Error& e) { return tool_error(e.message, e.message); }

ToolOutput bad_field(const char* field, const char* why) {
    return tool_error("bad_args", why, Map{{"field", Value(field)}});
}

uint32_t id_arg(const Value& a) {
    const Value* v = a.find("id");
    return v != nullptr && v->type() == Value::Type::Int ? static_cast<uint32_t>(v->as_int()) : 0;
}

std::string title_of(core::ReminderScheduler& s, uint32_t id) {
    for (const core::Todo& t : s.list(core::TodoFilter::All)) {
        if (t.id == id) return std::string(t.title.data(), t.title.size());
    }
    return {};
}

Value todo_value(const core::Todo& t, int offset) {
    Map m{{"id", Value(static_cast<int64_t>(t.id))},
          {"title", Value(std::string(t.title.data(), t.title.size()))},
          {"done", Value(t.done)},
          {"due", t.due ? Value(core::format_local_minute(*t.due, offset)) : Value()},
          {"repeat", core::to_value(t.repeat)}};
    if (t.snooze_until)
        m["snoozed_until"] = Value(core::format_local_minute(*t.snooze_until, offset));
    return Value(std::move(m));
}

constexpr const char* kDueWhy = "due must be local time \"YYYY-MM-DDTHH:MM\"";
constexpr const char* kRepeatWhy = R"(repeat must be "none", "daily" or {"weekly":[1-7,...]})";

}  // namespace

Result<void> add_todo_tools(ToolRegistry& reg, TodoToolDeps deps) {
    auto d = std::make_shared<TodoToolDeps>(std::move(deps));
    auto offset = [d] { return d->scheduler.utc_offset_min(); };

    WISP_TRY(reg.add({
        .name = "todo.list",
        .description = "The todos: id, title, done, due (local time YYYY-MM-DDTHH:MM or null), "
                       "repeat and snoozed_until. filter: open (default), done, all, or "
                       "due_today (open ones due today).",
        .schema_json = kListSchema,
        .thread = ToolThread::Fs,
        .timeout_ms = kTodoMs,
        .run =
            [d, offset](const Value& a, ToolCtx&) {
                const std::string& f = arg_str(a, "filter");
                const core::TodoFilter filter = f == "done"        ? core::TodoFilter::Done
                                                : f == "all"       ? core::TodoFilter::All
                                                : f == "due_today" ? core::TodoFilter::DueToday
                                                                   : core::TodoFilter::Open;
                const int off = offset();
                List items;
                for (const core::Todo& t : d->scheduler.list(filter)) {
                    items.push_back(todo_value(t, off));
                }
                return done({{"todos", Value(std::move(items))}});
            },
    }));

    WISP_TRY(reg.add({
        .name = "todo.add",
        .description =
            "Adds a todo of at most 100 characters (at most 200 todos). due: local time "
            "\"YYYY-MM-DDTHH:MM\"; the device then rings and shows a reminder at that time. "
            "repeat (needs due): \"none\" (default), \"daily\" or {\"weekly\":[1,3,5]} "
            "(1 = Monday); a repeating todo rings again at the same time instead of ending.",
        .schema_json = kAddSchema,
        .risk = ToolRisk::Note,
        .thread = ToolThread::Fs,
        .timeout_ms = kTodoMs,
        .run =
            [d, offset](const Value& a, ToolCtx& ctx) {
                std::optional<int64_t> due;
                if (const Value* v = a.find("due");
                    v != nullptr && v->type() != Value::Type::Null) {
                    due = core::parse_local_minute(v->as_string(), offset());
                    if (!due) return bad_field("due", kDueWhy);
                }
                core::RepeatRule rule;
                if (const Value* v = a.find("repeat");
                    v != nullptr && v->type() != Value::Type::Null) {
                    auto r = core::parse_repeat(*v);
                    if (!r) return bad_field("repeat", kRepeatWhy);
                    rule = *r;
                }
                const std::string& title = arg_str(a, "title");
                auto r = d->scheduler.add(title, due, rule);
                if (!r) return fail(r.error());
                ToolOutput out = done({{"id", Value(static_cast<int64_t>(r.value()))}});
                out.note = ui::tr_fill(ui::StrId::AgentNoteTodoAdded, lang_of(ctx), title);
                return out;
            },
    }));

    WISP_TRY(reg.add({
        .name = "todo.update",
        .description = "Changes a todo. fields: any of title, due (local \"YYYY-MM-DDTHH:MM\", "
                       "or null to remove it) and repeat, as for todo_add. A new due time or "
                       "repeat makes it ring afresh.",
        .schema_json = kUpdateSchema,
        .risk = ToolRisk::Note,
        .thread = ToolThread::Fs,
        .timeout_ms = kTodoMs,
        .run =
            [d, offset](const Value& a, ToolCtx& ctx) {
                const uint32_t id = id_arg(a);
                const Value& f = *a.find("fields");
                core::TodoFields fields;
                if (const Value* v = f.find("title")) fields.title = v->as_string();
                if (const Value* v = f.find("due")) {
                    if (v->type() == Value::Type::Null) {
                        fields.due = std::optional<int64_t>();
                    } else {
                        std::optional<int64_t> due =
                            v->type() == Value::Type::String
                                ? core::parse_local_minute(v->as_string(), offset())
                                : std::nullopt;
                        if (!due) return bad_field("fields.due", kDueWhy);
                        fields.due = due;
                    }
                }
                if (const Value* v = f.find("repeat")) {
                    auto r = core::parse_repeat(*v);
                    if (!r) return bad_field("fields.repeat", kRepeatWhy);
                    fields.repeat = *r;
                }
                auto r = d->scheduler.update(id, fields);
                if (!r) return fail(r.error());
                ToolOutput out = done();
                out.note = ui::tr_fill(ui::StrId::AgentNoteTodoUpdated, lang_of(ctx),
                                       title_of(d->scheduler, id));
                return out;
            },
    }));

    WISP_TRY(reg.add({
        .name = "todo.done",
        .description = "Marks a todo done. For a repeating one this ends only the current "
                       "occurrence; it rings again next time.",
        .schema_json = kIdSchema,
        .risk = ToolRisk::Note,
        .thread = ToolThread::Fs,
        .timeout_ms = kTodoMs,
        .run =
            [d](const Value& a, ToolCtx& ctx) {
                const uint32_t id = id_arg(a);
                auto r = d->scheduler.done(id);
                if (!r) return fail(r.error());
                ToolOutput out = done();
                out.note = ui::tr_fill(ui::StrId::AgentNoteTodoDone, lang_of(ctx),
                                       title_of(d->scheduler, id));
                return out;
            },
    }));

    return reg.add({
        .name = "todo.remove",
        .description = "Deletes a todo.",
        .schema_json = kIdSchema,
        .risk = ToolRisk::Note,
        .thread = ToolThread::Fs,
        .timeout_ms = kTodoMs,
        .run =
            [d](const Value& a, ToolCtx& ctx) {
                const uint32_t id = id_arg(a);
                const std::string title = title_of(d->scheduler, id);
                auto r = d->scheduler.remove(id);
                if (!r) return fail(r.error());
                ToolOutput out = done();
                out.note = ui::tr_fill(ui::StrId::AgentNoteTodoRemoved, lang_of(ctx), title);
                return out;
            },
    });
}

}  // namespace wisp::agent
