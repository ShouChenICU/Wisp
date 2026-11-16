#pragma once

// IoRunner: ToolThread::Io's executor (spec 3.9).

#include <array>
#include <atomic>
#include <cstddef>
#include <functional>
#include <memory>

#include "wisp/os/os.hpp"

namespace wisp::agent {

// ToolThread::Io's executor (spec 3.9): every call gets its own task, wisp.tool.io,
// flash_access = false (PSRAM stack on the device), so a call the registry abandoned
// keeps running without holding up wisp.agent or the next call. At most kMaxTasks
// run at once (one abandoned, one new). Thread-safe.
class IoRunner {
public:
    static constexpr size_t kMaxTasks = 2;
    static constexpr size_t kStackBytes = 16 * 1024;
    static constexpr int kPriority = 3;
    static os::TaskConfig task_config();  // name "wisp.tool.io", the three values above

    IoRunner() = default;
    ~IoRunner();  // join_all()
    IoRunner(const IoRunner&) = delete;
    IoRunner& operator=(const IoRunner&) = delete;

    // Joins the finished tasks, then starts `fn` on a new task. false: kMaxTasks are
    // running, or the task could not be created.
    bool post(std::function<void()> fn);
    // Waits for every task (AgentWiring::stop()). A post() from another thread
    // meanwhile waits for it to return.
    void join_all();
    // Tasks not yet joined, finished or not.
    size_t running() const;

private:
    struct Slot {
        std::unique_ptr<os::Task> task;
        // Set by the task as its last action: joining it then never waits on the job.
        std::shared_ptr<std::atomic<bool>> done;
    };

    void join_finished_locked();

    mutable os::Mutex mu_;
    std::array<Slot, kMaxTasks> slots_{};
};

}  // namespace wisp::agent
