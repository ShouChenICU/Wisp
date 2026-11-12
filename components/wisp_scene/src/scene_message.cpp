#include "scene_message.hpp"

namespace wisp::scene {

const char* message_kind(const msg::Body& body) {
    static constexpr const char* kNames[] = {
        "load",       "open",          "close",       "exit",      "reload",     "show",
        "hide",       "event",         "list_range",  "http_done", "store_done", "device",
        "clock_set",  "check_sources", "wake",        "restore",   "notify",     "sensor",
        "files_done", "quit",          "check_build", "eval",      "store_dump", "store_set",
        "lua_run",    "lua_stop",      "tool_call",   "tool_stop"};
    static_assert(std::size(kNames) == std::variant_size_v<msg::Body>);
    return kNames[body.index()];
}

void fail_message(SceneMessage& m, const Result<void>& r) {
    std::shared_ptr<SceneWaiter> done;
    if (auto* b = std::get_if<msg::Load>(&m.body)) done = b->done;
    if (auto* b = std::get_if<msg::Open>(&m.body)) done = b->done;
    if (auto* b = std::get_if<msg::Close>(&m.body)) done = b->done;
    if (auto* b = std::get_if<msg::Exit>(&m.body)) done = b->done;
    if (auto* b = std::get_if<msg::Reload>(&m.body)) done = b->done;
    if (done) done->finish(r);
    std::shared_ptr<SceneQuery> query;
    if (auto* b = std::get_if<msg::Eval>(&m.body)) query = b->job;
    if (auto* b = std::get_if<msg::StoreDump>(&m.body)) query = b->job;
    if (auto* b = std::get_if<msg::StoreSet>(&m.body)) query = b->job;
    if (query) {
        query->finish(r ? Result<os::LargeString>::err(ErrorCode::Busy, "not handled")
                        : Result<os::LargeString>::err(r.error().code, r.error().message));
    }
    if (auto* b = std::get_if<msg::LuaRun>(&m.body)) {
        b->job->finish(r ? Result<LuaRunResult>::err(ErrorCode::Busy, "not handled")
                         : Result<LuaRunResult>::err(r.error().code, r.error().message));
    }
    if (auto* b = std::get_if<msg::ToolCall>(&m.body)) {
        b->job->finish(r ? Result<ToolCallResult>::err(ErrorCode::Busy, "not handled")
                         : Result<ToolCallResult>::err(r.error().code, r.error().message));
    }
    if (auto* b = std::get_if<msg::CheckSources>(&m.body)) {
        std::lock_guard<os::Mutex> lock(b->job->mu);
        if (!b->job->done) b->job->finish_locked(r);
    }
    if (auto* b = std::get_if<msg::CheckBuild>(&m.body)) {
        std::lock_guard<os::Mutex> lock(b->job->mu);
        if (!b->job->done) b->job->finish_locked(r);
    }
}

}  // namespace wisp::scene
