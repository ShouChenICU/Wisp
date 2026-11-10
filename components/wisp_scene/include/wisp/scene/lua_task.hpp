#pragma once

// Task coroutines (agent-tools spec 6): a script the host runs as a task
// may wait for wisp.http, wisp.files and wisp.sleep results instead of
// passing a callback. Each task is a Lua thread the host made with
// start_task(); a binding called from it without a callback submits its
// job, registers the thread in its callback table where a function would
// go, and yields. When the result's message arrives, run_http_callback(),
// run_files_callback() or run_timer() finds the thread there and resumes it
// with Vm::resume(), each resumption a fresh execution slice; the time spent
// waiting counts toward no limit.
//
// Only the task thread itself may wait: a coroutine the script made inside
// it, and code that is not in a task at all (an event handler, scene.eval),
// still needs the callback (an argument error, as before), and so does a
// task stopped inside a C call that cannot yield (a sort comparator, a
// metamethod). A plain coroutine.yield() at the task's own level ends the
// task with an error: nothing would ever resume it.
//
// Every task ends exactly once: SceneApi::task_done runs when it returns,
// raises, faults or is cancelled. It runs on the scene task inside the call
// that finished the task (start_task() itself, when the first slice
// already finishes it), as that call's last step; it must not destroy the
// Vm (post that instead). Once a task (or anything else) faults the VM,
// waiting tasks are never resumed again: the next result for one finishes
// it with the VM's fault, and cancel_tasks() finishes the rest.
//
// A finished, failed or cancelled task thread is dropped, never closed
// (lua_closethread): see vm.hpp's hooks-off rule. Its pending __close
// values do not run.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <optional>

#include "lua.hpp"
#include "wisp/base/result.hpp"
#include "wisp/base/value.hpp"
#include "wisp/os/large.hpp"
#include "wisp/scene/vm.hpp"

namespace wisp::scene {

// The most JSON a task's (or scene.eval's) result may take (agent-core
// spec 4.2).
inline constexpr size_t kEvalResultMaxBytes = 64 * 1024;

// The longest wisp.sleep (agent-tools spec 6).
inline constexpr uint32_t kMaxSleepMs = 300000;

struct TaskOutcome {
    bool ok = false;
    os::LargeString json;  // ok: return values as JSON (none -> null, one -> it, several -> array)
    std::optional<ScriptError> error;  // !ok
};

// Called once per task when it returns, errors, faults or is cancelled.
using TaskDone = std::function<void(uint32_t task, TaskOutcome outcome)>;

// Starts the function on top of the stack (below it nothing of ours) in a new
// task coroutine with `args` pushed as one table argument (nil when null).
// Runs its first slice at once. Returns the task id (> 0), or the error that
// stopped it from starting. The function is popped either way. `slice_ms`
// is each slice's time limit (0: SceneApi::task_slice_ms, and if that is 0
// too the VM's exec_ms); a result whose JSON exceeds `max_json` ends the task
// with "result too large". Needs install_api(); refuses a faulted VM.
Result<uint32_t> start_task(Vm& vm, const Value* args, uint32_t slice_ms, size_t max_json);

// Drops every unfinished task without resuming it (its waits are forgotten).
// Each gets task_done with ScriptFault::Runtime, message "cancelled". Safe on
// a faulted VM; a host calls it before it destroys a VM that ran tasks.
void cancel_tasks(Vm& vm);

// The id of the task coroutine running `L`, 0 when `L` is not one (the main
// thread, or a coroutine the script made itself).
uint32_t current_task(lua_State* L);

}  // namespace wisp::scene
