#include "wisp/agent/io_runner.hpp"

#include <mutex>
#include <utility>

namespace wisp::agent {

os::TaskConfig IoRunner::task_config() {
    os::TaskConfig cfg;
    cfg.name = "wisp.tool.io";
    cfg.stack_bytes = kStackBytes;
    cfg.priority = kPriority;
    // Never touches flash (AGENTS.md section 5): the port may put the stack in PSRAM.
    cfg.flash_access = false;
    cfg.hot_stack = false;
    return cfg;
}

IoRunner::~IoRunner() { join_all(); }

void IoRunner::join_finished_locked() {
    for (Slot& s : slots_) {
        if (s.task && s.done->load(std::memory_order_acquire)) {
            s.task->join();
            s.task.reset();
            s.done.reset();
        }
    }
}

bool IoRunner::post(std::function<void()> fn) {
    std::lock_guard<os::Mutex> lock(mu_);
    join_finished_locked();
    for (Slot& s : slots_) {
        if (s.task) continue;
        auto done = std::make_shared<std::atomic<bool>>(false);
        auto task = os::Task::spawn(task_config(), [job = std::move(fn), done]() mutable {
            job();
            // Drop what the job captured (the registry's call state) now rather than
            // when the task is joined, which may be much later.
            job = nullptr;
            done->store(true, std::memory_order_release);
        });
        if (!task) return false;
        s.task = std::move(task);
        s.done = std::move(done);
        return true;
    }
    return false;
}

void IoRunner::join_all() {
    // Held throughout, so no post() slips a task in behind the joins; the only
    // caller is shutdown, after the agent stopped calling tools.
    std::lock_guard<os::Mutex> lock(mu_);
    for (Slot& s : slots_) {
        if (!s.task) continue;
        s.task->join();
        s.task.reset();
        s.done.reset();
    }
}

size_t IoRunner::running() const {
    std::lock_guard<os::Mutex> lock(mu_);
    size_t n = 0;
    for (const Slot& s : slots_) {
        if (s.task) ++n;
    }
    return n;
}

}  // namespace wisp::agent
