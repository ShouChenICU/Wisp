// GET/DELETE /memory and /todos (mvp spec 5.3): thin views of the agent's
// MemoryStore and ReminderScheduler. list() on either service is memory
// only and runs on the calling thread (an HTTP server thread); remove()
// writes the file, so -- unlike the services that hop to the flash
// thread themselves (PairingService, WifiService) -- it is this route's
// job to run it on AdminDeps::worker (see their headers).
//
// The stored field is "snooze_until" (ReminderScheduler's own name); the
// agent's todo.* tools happen to call it "snoozed_until" in their output,
// but the REST shape here uses the one name the file and the service use.

#include <charconv>
#include <cstdint>
#include <functional>
#include <memory>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "admin_internal.hpp"
#include "wisp/core/memory_store.hpp"
#include "wisp/core/reminders.hpp"
#include "wisp/core/storage_worker.hpp"

namespace wisp::admin::detail {

namespace {

// How long a request waits for the flash thread to run one delete.
constexpr uint32_t kFlashWaitMs = 5000;

// Runs `fn` (a mutating MemoryStore/ReminderScheduler call) on the flash
// thread, or inline without one (tests). `fn`'s Result travels through a
// shared slot since the job may outlive a timed-out wait.
Result<void> on_flash(core::StorageWorker* worker, std::function<Result<void>()> fn) {
    if (worker == nullptr) return fn();
    auto out = std::make_shared<std::optional<Result<void>>>();
    Result<void> r =
        worker->run_sync([out, fn] { out->emplace(fn()); }, kFlashWaitMs, core::IoLane::Normal);
    if (!r) return r;
    return std::move(**out);
}

net::HttpResponse unavailable(const char* what) {
    return net::HttpResponse::error(503, "unavailable", what);
}

// The path segment "*" matched, as a uint32_t id; nullopt for anything
// that is not exactly one (no note or todo id is ever 0).
std::optional<uint32_t> id_param(const RequestContext& ctx) {
    std::optional<std::string> raw = percent_decode(ctx.param);
    if (!raw || raw->empty()) return std::nullopt;
    uint32_t v = 0;
    auto [end, ec] = std::from_chars(raw->data(), raw->data() + raw->size(), v);
    if (ec != std::errc() || end != raw->data() + raw->size() || v == 0) return std::nullopt;
    return v;
}

// A MemoryStore/ReminderScheduler error: not_found is 404, everything
// else (including "memory_unavailable"/"todo_unavailable") the generic
// service error mapping.
net::HttpResponse store_error(const Error& e) {
    if (e.code == ErrorCode::NotFound) return net::HttpResponse::error(404, "not_found", e.message);
    return service_error(e);
}

net::HttpResponse get_memory(AdminDeps& deps, const RequestContext&) {
    if (deps.memory == nullptr) return unavailable("memory notes are not available");
    List items;
    for (const core::MemoryNote& n : deps.memory->list()) {
        items.push_back(Value(Map{
            {"id", Value(static_cast<int64_t>(n.id))},
            {"text", Value(std::string(n.text.data(), n.text.size()))},
            {"updated", Value(n.updated)},
        }));
    }
    return net::HttpResponse::json(200, Value(Map{{"notes", Value(std::move(items))}}));
}

net::HttpResponse delete_memory(AdminDeps& deps, const RequestContext& ctx) {
    if (deps.memory == nullptr) return unavailable("memory notes are not available");
    std::optional<uint32_t> id = id_param(ctx);
    if (!id) return net::HttpResponse::error(404, "not_found", "no such note");
    core::MemoryStore* memory = deps.memory;
    Result<void> r = on_flash(deps.worker, [memory, id = *id] { return memory->remove(id); });
    if (!r) return store_error(r.error());
    return no_content();
}

Value todo_value(const core::Todo& t, int utc_offset_min) {
    Map m{{"id", Value(static_cast<int64_t>(t.id))},
          {"title", Value(std::string(t.title.data(), t.title.size()))},
          {"done", Value(t.done)},
          {"repeat", core::to_value(t.repeat)}};
    if (t.due) m["due_local"] = Value(core::format_local_minute(*t.due, utc_offset_min));
    if (t.snooze_until) {
        m["snooze_until"] = Value(core::format_local_minute(*t.snooze_until, utc_offset_min));
    }
    return Value(std::move(m));
}

net::HttpResponse get_todos(AdminDeps& deps, const RequestContext&) {
    if (deps.todos == nullptr) return unavailable("todos are not available");
    const int offset = deps.todos->utc_offset_min();
    List items;
    for (const core::Todo& t : deps.todos->list(core::TodoFilter::All)) {
        items.push_back(todo_value(t, offset));
    }
    return net::HttpResponse::json(200, Value(Map{{"todos", Value(std::move(items))}}));
}

net::HttpResponse delete_todo(AdminDeps& deps, const RequestContext& ctx) {
    if (deps.todos == nullptr) return unavailable("todos are not available");
    std::optional<uint32_t> id = id_param(ctx);
    if (!id) return net::HttpResponse::error(404, "not_found", "no such todo");
    core::ReminderScheduler* todos = deps.todos;
    Result<void> r = on_flash(deps.worker, [todos, id = *id] { return todos->remove(id); });
    if (!r) return store_error(r.error());
    return no_content();
}

}  // namespace

void add_memory_routes(std::vector<Route>& routes) {
    using net::Method;
    routes.push_back({Method::Get, "/memory", false, get_memory});
    routes.push_back({Method::Delete, "/memory/*", false, delete_memory});
    routes.push_back({Method::Get, "/todos", false, get_todos});
    routes.push_back({Method::Delete, "/todos/*", false, delete_todo});
}

}  // namespace wisp::admin::detail
