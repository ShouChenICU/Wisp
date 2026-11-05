#include "scene_message.hpp"

namespace wisp::scene {

const char* message_kind(const msg::Body& body) {
    static constexpr const char* kNames[] = {"load",       "open",          "close",      "exit",
                                             "reload",     "show",          "hide",       "event",
                                             "list_range", "http_done",     "store_done", "device",
                                             "clock_set",  "check_sources", "wake",       "quit"};
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
    if (auto* b = std::get_if<msg::CheckSources>(&m.body)) {
        std::lock_guard<os::Mutex> lock(b->job->mu);
        if (!b->job->done) b->job->finish_locked(r);
    }
}

}  // namespace wisp::scene
