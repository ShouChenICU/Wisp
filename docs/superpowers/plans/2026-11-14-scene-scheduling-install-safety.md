# Scene Scheduling and Install Safety Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A scene never loses an HTTP or files result, a tap on the scene in front is answered before background work, a long download no longer blocks every other request, `scene.rollback` returns to the version the user had before the agent's turn, and replacing a list's items never shows an empty list for a frame.

**Architecture:** Each VM (scene slot or the agent's `lua.run` VM) gets a `CompletionBox`, a fixed ring in `Mem::Large` sized by a new per-VM in-flight cap; producers put results there and the scene queue carries only a wake-up. The scene task's loop picks work from three lanes: the message queue (UI events and control, in arrival order), the scene in front (its results and timers), then everything else (background and headless scenes, the agent's Lua) round-robin, with bursts that keep any lane from starving. `net::SceneHttp` runs a second worker for downloads. `SceneWorkspace` remembers, per scene, the agent turn whose first install wrote `.prev`, and later installs of that turn leave `.prev` alone. `SceneView` keeps a list's old rows after a reset until the first rows of the new items arrive.

**Tech Stack:** C++20 (`-fno-exceptions -fno-rtti`), Lua 5.4 (vendored), GoogleTest, LVGL 9.5, the simulator build (`build/sim`).

**Spec:** `docs/superpowers/plans/2026-11-14-architecture-optimization-roadmap.md` (phase C), with `docs/superpowers/specs/2026-11-04-scene-runtime-core-design.md` sections 3.1, 4.3, 5.3, 5.6; `2026-11-05-scene-device-design.md` section 7.1; `2026-11-05-agent-core-design.md` section 4.1 (`.prev`); `2026-11-09-agent-tools-design.md` section 4.2 (the 1000 ms slice) as the behaviour being amended. Task 7 records the amendments.

## Global Constraints

- Branch: `feat/scene-scheduling` (already created from `main`). No commits on `main`.
- AGENTS.md applies: `wisp::Result<T>`, no exceptions/RTTI, `Mem::Large` (`os::make_large`, `os::LargeAllocator`, `os::Large*` containers) for every new buffer that can exceed a few KB or lives as long as a scene, comments explain why (no review ids, no change history), the longjmp rule of `vm.hpp` in every binding that raises.
- Threads: the scene task (`wisp.scene`) owns slots, VMs and the lane state; the `wisp.shttp`/`wisp.shttp.dl` workers, `wisp.fs` and the scene task itself put results into boxes; the UI thread owns `SceneView`. None of the new code touches flash on a task with `flash_access = false`.
- Every task ends with `cmake --build build/sim && ctest --test-dir build/sim --output-on-failure -R '<the task's filter>'` green; Task 7 runs `bash tools/ci.sh`.
- `docs/scene-api.md` is generated: edit the text in `components/wisp_scene/src/schema.cpp` (`schema_markdown()`), run `ctest -R DocIsUpToDate` (it writes `build/sim/scene-api.md`), copy that over `docs/scene-api.md`, then `python3 tools/gen_agent_docs.py` (regenerates `components/wisp_agent/src/docs_data.cpp`).
- Commit messages: Conventional Commits, ending with the `Co-Authored-By` line of the session's attribution rules.
- Device-only checks are listed at the end; they are never reported as done.

## Review Focus

1. A burst of results while the scene queue is full (immediate refusals posted from inside `submit()` on the scene task itself): every callback runs exactly once, in submission order; none is logged as dropped. Tests in Task 2.
2. A scene that exits or faults while results wait in its box: none of its callbacks runs, the payloads are freed and counted in `stale_dropped()`, and nothing reaches the next instance of the same scene. Tests in Task 2.
3. A tap on the scene in front while a background timer, a background result and the agent's Lua resume are all due: the tap runs first, the front's own results next, background work after; and with the front saturated, the background still gets one item after every `kFrontBurst` front items. Tests in Task 3.
4. A download streaming for minutes: an agent `http.fetch` (`HttpPrincipal::Agent`) and a scene's `wisp.http.get` still complete meanwhile; `cancel_scene()` stops a scene's requests in both lanes with no `done()`. Tests in Task 4.
5. Two installs of one scene in one agent turn, then `scene.rollback`: the version from before the turn comes back. A later turn, a discard, a rollback or an install by anyone else starts a new baseline. Tests in Task 5.

## Deviations from the roadmap

1. **"Bounded by the instance's in-flight limits"**: there are none today. `wisp.files` has no per-scene limit at all, and `wisp.http`'s refusals (`rate_limited`, `permission_denied`, ...) are answered at once without bound: a loop of 10 000 refused calls in one 200 ms message makes 10 000 results. Task 1 adds the limit: `kMaxCallsInFlight = 16` calls per VM (HTTP, download and files together) from the moment the callback is registered until it runs. A call beyond it raises a Lua error (`pcall` catches it). It cannot be answered through the callback like the other refusals: that answer would itself be a result past the bound. The ring in each box then never needs more than 16 entries.
2. **"Foreground UI events" as the first lane**: the queue also carries control messages (open/close, queries such as `eval`, `lua.run` start and stop, tool-call start and stop, device events). Reordering them would break request order (a `LuaStop` handled before its `LuaRun`, a back gesture handled before an earlier tap). So the first lane is the whole message queue in arrival order. Once results leave the queue it holds no bulk work, and each message runs at most one Lua step (at most 200 ms, after Task 3 also for the agent's Lua).
3. **Strict lane order**: a front scene whose timers or results never stop would stop the background scenes and the agent's Lua forever. Lanes keep their order with two bursts: after `kQueueBurst = 8` queue messages in a row one lane item runs if any waits, and after `kFrontBurst = 4` front items in a row one background item runs if any waits.
4. **"One draft session ... ends when the draft is discarded or the turn that created it ends"**: install step 8 (`SceneWorkspace::install_zip`) removes the draft, so the next edit in the same turn makes a new draft by copying the just-installed package (`ensure_draft_here`). The draft's lifetime cannot carry the session. The session is defined as (scene id, agent turn): it begins with the first successful `scene.install` of that id in that turn, and it ends when the turn ends (the next install carries another `ToolCtx::turn`), at `scene.discard_draft`, at `scene.rollback`, at a restart (sessions live in RAM), or when anything else replaces or removes the scene (`SceneRepository::changed_since`). Tool calls outside a turn (`turn == 0`: tests, future callers) keep today's behaviour and always write `.prev`.
5. **List "generation"**: no generation number is needed on the wire. `W.list_reset` is followed at once by `drop_rows(list)` in the prelude (`prelude.cpp`, `W.widget`), and `build_list_rows()` only ever builds from the current items, so in patch-queue order every `ListRows` after a `ListReset` is the new items'. Two cases the roadmap leaves open drop the old rows at once instead of keeping them: a reset whose window is empty (no rows will ever come), and a reset whose range request could not be queued (`on_list_range` now reports that). Otherwise stale rows could stay on screen indefinitely.

---

### Task 1: A per-VM cap on calls in flight

**Files:**
- Modify: `components/wisp_scene/src/scene_api.hpp` (constant, `SceneApi::calls_in_flight`, the wisp.http/wisp.files paragraphs of the file header)
- Modify: `components/wisp_scene/src/bind_http.cpp` (`submit()`, `run_http_callback()`, file header "err:" paragraph)
- Modify: `components/wisp_scene/src/bind_files.cpp` (`detail::register_files_callback()`, `run_files_callback()`)
- Modify: `components/wisp_scene/src/schema.cpp` (the wisp.http and wisp.files sections of `schema_markdown()`), then regenerate `docs/scene-api.md` and `components/wisp_agent/src/docs_data.cpp` (Global Constraints)
- Test: `tests/scene/http_binding_test.cpp`, `tests/scene/files_binding_test.cpp`, `tests/scene/scene_run_lua_test.cpp`

**Interfaces:**
- Produces in `scene_api.hpp`: `inline constexpr uint32_t kMaxCallsInFlight = 16;` and in `SceneApi`: `uint32_t calls_in_flight = 0;` with the comment "wisp.http, wisp.http.download and wisp.files calls whose callback (or waiting task) is registered and has not run; at most kMaxCallsInFlight".
- Rule: `submit()` in `bind_http.cpp` (before it takes an id) and `detail::register_files_callback()` (which `download()` and every files call go through) raise `luaL_error(L, "too many calls waiting for results (at most %d)", kMaxCallsInFlight)` when `api.calls_in_flight >= kMaxCallsInFlight`, else increment it. Both frames hold only trivially destructible locals at that point (`vm.hpp` longjmp rule). `run_http_callback()` and `run_files_callback()` decrement it first thing, before `lua_checkstack` (a result whose callback was dropped by `cancel_task()` still counts down).
- The `check_build()` VM (`check_build.cpp`) never answers, so an entry file that starts more than 16 calls at load fails the install check with this error, exactly as the real open would fail.

- [ ] **Step 1: Write the failing tests.**

```cpp
// http_binding_test.cpp
TEST_F(HttpBinding, CallsPastTheCapRaise)
// script at load: 16 x wisp.http.get("https://other.example.com/", cb) (permission_denied,
// answered at once), then print(pcall(wisp.http.get, "https://other.example.com/", cb)).
// expect: the pcall line starts "false\t" and contains "too many calls waiting";
// exactly 16 "permission_denied" lines follow; status Running.
TEST_F(HttpBinding, ARunCallbackFreesItsPlace)
// 16 calls at load; the 16th callback makes one more call whose callback prints "again".
// expect: "again" is printed; no "too many" line anywhere.

// files_binding_test.cpp
TEST_F(FilesBinding, HttpAndFilesShareTheCap)
// 10 refused wisp.http.get + 6 wisp.files.read("../bad", 0, 1, cb), then
// print(pcall(wisp.files.list, cb)) -> "false\t...too many calls waiting"; 16 answers arrive.

// scene_run_lua_test.cpp
TEST_F(RunLua, CallsPastTheCapRaise)
// run("for i = 1, 16 do wisp.http.request({url = 'bad'}, function() end) end "
//     "return select(2, pcall(wisp.http.request, {url = 'bad'}, function() end))")
// expect: r.ok, the JSON string contains "too many calls waiting".
```

- [ ] **Step 2: Run** `ctest --test-dir build/sim -R 'HttpBinding|FilesBinding|RunLua' --output-on-failure`. Expected: the four new tests FAIL (no error is raised).
- [ ] **Step 3: Implement** the counter, the checks and the decrements. Add to the wisp.http and wisp.files sections of `schema_markdown()`: "At most 16 calls of `wisp.http` and `wisp.files` together may wait for their results; one more raises an error." Regenerate both generated files.
- [ ] **Step 4: Run** the same filter plus `-R 'DocIsUpToDate|SceneRuntimeTest|SceneBackground|CallTool'`. Expected: PASS.
- [ ] **Step 5: Commit** `feat(scene): cap the calls a scene may have waiting for results`.

---

### Task 2: Results go to a completion box, never dropped

**Files:**
- Create: `components/wisp_scene/src/completion_box.hpp`, `components/wisp_scene/src/completion_box.cpp` (add to `SRCS` in `components/wisp_scene/CMakeLists.txt`)
- Modify: `components/wisp_scene/src/scene_message.hpp` (`Slot::box`, `AgentRun::box`; `msg::Completed` replaces `msg::HttpDone` and `msg::FilesDone` in `msg::Body`), `scene_message.cpp` (`message_kind()`: "completed")
- Modify: `components/wisp_scene/include/wisp/scene/scene_runtime.hpp` (file header "HTTP" and "Files" paragraphs; `wire_http_api`/`wire_files_api` signatures; `run_completions()`)
- Modify: `components/wisp_scene/src/scene_runtime.cpp` (`wire_http_api` ~1416-1440, `wire_files_api` ~1465-1574, `add_slot` ~1355, `handle` ~818-823, `fault` ~1680, `close_slot` ~1872, `run` ~662)
- Modify: `components/wisp_scene/src/scene_runtime_lua.cpp` (`lua_start` wiring, `lua_message` removed, `lua_end`)
- Modify: `tests/CMakeLists.txt` (new `completion_box_test`, with the `components/wisp_scene/src` include like `scene_runtime_test`)
- Test: `tests/scene/completion_box_test.cpp` (new), `tests/scene/http_binding_test.cpp`, `tests/scene/files_binding_test.cpp`, `tests/scene/scene_run_lua_test.cpp`, `tests/scene/scene_runtime_test.cpp` (`StaleMessagesAreDropped`), `tests/scene/scene_background_test.cpp` (`CallbacksSurviveBackgroundAndDieWithExit`)

**Interfaces:**
- Consumes: `kMaxCallsInFlight` (Task 1).
- Produces `completion_box.hpp`:

```cpp
// One VM's finished wisp.http / wisp.files calls waiting for the scene task
// (scene_runtime.hpp, "Results"). Producers: the wisp.shttp workers, the
// wisp.fs worker and the scene task itself (refusals made inside a call).
// Allocated once per VM with std::allocate_shared(os::LargeAllocator) and
// shared with every deliver lambda, so a late producer never touches a
// freed box. `mu_` is a leaf lock: nothing else is locked or called
// while it is held.
class CompletionBox {
public:
    static constexpr size_t kCapacity = kMaxCallsInFlight;
    using Payload = std::variant<net::SceneHttpResult, FilesResult>;
    struct Item {
        uint32_t id = 0;     // the binding's callback id
        bool files = false;  // which callback table (and which no-memory answer)
        os::LargeUnique<Payload> payload;  // null: no memory to keep the result
    };
    // Any thread. Moves `p` into a make_large<Payload>; on allocation
    // failure keeps the item with a null payload. False (and `p` dropped)
    // when closed or full. *wake is set when the caller must send the
    // scene task a wake-up: the box was not already waiting for one.
    bool put(uint32_t id, bool files, Payload p, bool* wake);
    // Any thread: the wake-up could not be queued; the next put() asks again.
    void wake_failed();
    // Scene task. Oldest first; taking the last item clears the wake flag.
    bool take(Item& out);
    bool empty() const;
    // Scene task. No put() succeeds afterwards; returns how many items
    // were waiting (they are freed here).
    size_t close();
private:
    mutable os::Mutex mu_;
    bool open_ = true;
    bool wake_sent_ = false;
    std::array<Item, kCapacity> ring_;
    size_t head_ = 0, size_ = 0;
};
```

- Produces in `scene_message.hpp`: `std::shared_ptr<CompletionBox> box;` in `Slot` and `AgentRun`; `struct Completed {};` in `msg` ("results are waiting in the box of SceneMessage::instance"). `msg::HttpDone` and `msg::FilesDone` are removed.
- Produces in `SceneRuntime`: `void wire_http_api(SceneApi&, const std::string& id, uint32_t instance, const std::shared_ptr<CompletionBox>& box);` and the same extra parameter on `wire_files_api`. `bool run_completions();` (scene task: runs one item of every non-empty box, front slot first, then the other slots, then the agent run; true if any ran) -- an interim step that Task 3 replaces with its lanes. Interim loop: while any box is non-empty, `run()` pops with timeout 0, and calls `run_completions()` only when no message was waiting, so a close or exit already queued is handled before the results of the scene it closes.
- Deliver rule (both `deliver` lambdas): `box->put(...)`; a `false` return is a closed box (the instance is gone) and the result is dropped silently; when `*wake`, lock `link->mu` and, if `link->rt`, `post_scene_message(make_message(id, instance, msg::Completed{}))`; if that fails (or `link->rt` is null), `box->wake_failed()`. No "dropped: scene queue full" log remains. A lost wake-up cannot strand items: `run()` checks every box before it blocks, and the queue only refuses a push while it holds messages, so the task is awake anyway.
- Scene task rule: an item runs as `run_step(slot, slot.scene->http(id, *payload))` / `->files(...)`, or for the agent run `lua_settle(run_http_callback(...))` / `run_files_callback`, each with its own 200 ms budget and the existing slow-message log (`"slow message http|files <ms> ms"`). A null payload runs the callback with `SceneHttpResult{.error = "other"}` (the proxy's word for no memory) or `FilesResult{.error = "no_memory"}`. A full box (only a broken count could cause it) refuses the put; the producer logs `WLOG_E("scene.<id>", "a result was refused: box full")`.
- Lifetime rule: `add_slot()` allocates the box before `wire_http`/`wire_files` (failure: `nullptr`, the existing no-memory path); `lua_start()` likewise, before wiring (failure: `no_memory()`). `close_slot()` and `fault()` call `stale_dropped_ += s.box->close()` right after `s.scene.reset()` (no VM is left to run a callback); `lua_end()` does the same after `cancel_tasks()`. `handle(msg::Completed)` counts the message in `stale_dropped_` when no slot or agent run has its instance, else does nothing (the box is read by `run_completions()`).
- Memory: box and ring in `Mem::Large` (16 items of 16 bytes plus the mutex); each payload a `make_large` (PSRAM); the response bodies are already `os::LargeBuffer`. The worst case is bounded by 16 results per VM, at most 5 slots and the agent run.

- [ ] **Step 1: Write the failing tests.**

```cpp
// completion_box_test.cpp
TEST(CompletionBox, KeepsOrderAndAsksForOneWake)
// put ids 1,2,3: *wake true, false, false; take() yields 1,2,3 then false;
// the next put asks for a wake again.
TEST(CompletionBox, FailedWakeIsAskedForAgain)
// put -> wake true; wake_failed(); put -> wake true.
TEST(CompletionBox, CloseFreesAndRefuses)
// two puts; close() == 2; put() == false; take() == false.
TEST(CompletionBox, NoMemoryStillKeepsTheCall)
// wisp::port::sim::set_alloc_failure(wisp::os::Mem::Large, true) around one put:
// put() == true; take() gives id and files as put, payload == nullptr.
TEST(CompletionBox, FullRingRefuses)
// kCapacity puts succeed, the next returns false.

// http_binding_test.cpp (configure_config: c.queue_capacity = 2)
TEST_F(HttpBinding, RefusalsPastAFullQueueAllArrive)
// 6 x wisp.http.get("https://other.example.com/" .. i, function(err) print(i, err) end) at load.
// expect: 6 lines "1\tpermission_denied" .. "6\tpermission_denied" in order;
// log_has("scene.h", "dropped") is false.
TEST_F(HttpBinding, ExitDropsWaitingResults)
// configure_deps also sets d.device_status to a one-shot gate (StallGate, below);
// the scene's button "stall" makes 3 refused gets, then calls wisp.device.info().
// click_ref("stall"); wait until the gate is entered; rt->close_async("h", ...);
// release. expect: no callback line; stale_dropped() rises by 3; status Stopped.
TEST_F(HttpBinding, FaultDropsWaitingResults)
// 3 refused gets at load whose first callback does error("boom").
// expect: status Error with "boom"; the other two callbacks print nothing.

// files_binding_test.cpp (queue_capacity = 2)
TEST_F(FilesBinding, BadNamesPastAFullQueueAllArrive)
// 6 x wisp.files.read("../x", 0, 1, cb) -> 6 "bad_name" lines in order.

// scene_run_lua_test.cpp (queue_capacity = 2)
TEST_F(RunLua, RefusalsPastAFullQueueAllArrive)
// "local n = 0 for i = 1, 6 do wisp.http.request({url = 'bad'}, function() n = n + 1 end) end "
// "while n < 6 do wisp.sleep(10) end return n" -> r.ok, json "6".
```

`StallGate` (declared in `tests/support/scene_runtime_fixture.hpp`, used again in Task 3): `std::atomic<bool> armed`; `device_status()` returns `Value()` unless `armed.exchange(false)`, in which case it sets an `entered` promise and waits on a `release` future. Helpers `arm()`, `wait_entered()`, `release()`; the destructor releases.

Update `SceneRuntimeTest.StaleMessagesAreDropped` and `SceneBackground*.CallbacksSurviveBackgroundAndDieWithExit` to post `msg::Completed{}` for the old instance instead of `msg::HttpDone` (same `stale_dropped()` assertions).

- [ ] **Step 2: Run** `ctest --test-dir build/sim -R 'CompletionBox|HttpBinding|FilesBinding|RunLua' --output-on-failure`. Expected: build failure (no `completion_box.hpp`), then, once it compiles, the "PastAFullQueue" tests FAIL with 2 of 6 lines.
- [ ] **Step 3: Implement** (the interim loop above). Rewrite the file header's HTTP and Files paragraphs: results wait in the VM's box (at most `kMaxCallsInFlight`); a close or fault frees them uncalled and counts them stale.
- [ ] **Step 4: Run** the same filter plus `-R 'SceneRuntimeTest|SceneBackground|CallTool|SceneEval|LuaRun|SceneWorkspace'`. Expected: PASS.
- [ ] **Step 5: Commit** `fix(scene): keep every http and files result in a per-scene completion box`.

---

### Task 3: Scheduling lanes and a 200 ms agent slice

**Files:**
- Modify: `components/wisp_scene/include/wisp/scene/scene_runtime.hpp` (constants; a "Scheduling" paragraph in the file header replacing "Each message still has its own 200 ms ..." and the run_lua paragraph's slice sentence; private lane state)
- Modify: `components/wisp_scene/src/scene_runtime.cpp` (`run` ~662, `run_scene_clock` ~2068, `run_timers` ~2092, `show_scene` ~1973, `scene_wait_ms` ~2054)
- Modify: `components/wisp_scene/src/scene_message.hpp` (`Slot::due_timers`, `AgentRun::due_timers`)
- Modify: `components/wisp_scene/src/scene_runtime_lua.cpp` (`lua_clock` keeps only the deadline; its timers become lane items)
- Modify: `components/wisp_scene/src/schema.cpp` ("200 ms for scene tools, 1000 ms for Agent Lua" becomes "200 ms for scene tools and for Agent Lua"), regenerate the two docs files
- Modify: `tests/CMakeLists.txt` (new `scene_lanes_test`, `LIBS wisp_scene wisp_port_sim_os TIMEOUT 60`, src include)
- Test: `tests/scene/scene_lanes_test.cpp` (new), `tests/scene/scene_run_lua_test.cpp` (`SliceLimit`), `tests/agent/lua_tools_test.cpp` (unchanged; must stay green)

**Interfaces:**
- Consumes: the boxes and `run_completions()` (Task 2), which this task removes.
- Produces in `scene_runtime.hpp`:
  - `inline constexpr uint32_t kLuaRunSliceMs = 200;` (was 1000)
  - `inline constexpr uint32_t kQueueBurst = 8;` and `inline constexpr uint32_t kFrontBurst = 4;`
  - private: `uint32_t queue_run_ = 0, front_run_ = 0, back_cursor_ = 0;` `void housekeeping();` `bool front_has_work(uint64_t now);` `bool back_has_work(uint64_t now);` `void run_front_item();` `void run_back_item();` `bool run_item(Slot&, uint64_t now);` (one result, else one due timer) `void run_timers_of(Slot&);` (all due timers now, for `show_scene()`'s spec 3.4 order).
- Lanes (the scene task, `run()`):
  1. **Queue**: one message from `queue_`, handled as today (`enforce_limits(false)` first while a scene is in front; the slow-message log).
  2. **Front**: one item of `front_` (running, `scene` not null): its oldest box item, else one id from `Slot::due_timers` (refilled from `TimerSet::due(now)` when empty). Its `wisp.tools` call in front counts here too, as its resumptions are results and timers of that VM.
  3. **Back**: one item of the next source after `back_cursor_` (by instance, wrapping) that has work: a background or headless slot (as in 2), or the agent run (its box, else its `due_timers` through `run_timer()` and `lua_settle()`).
- Choice, before each item, with `F`/`B` = lane 2/3 has work: pop from the queue (timeout 0) unless `queue_run_ >= kQueueBurst` and (`F` or `B`); if nothing was popped or the burst is spent, run Front if `F` and (`front_run_ < kFrontBurst` or `!B`); else Back if `B`; else Front if `F`; else block on `queue_.pop()` for `min(scene_wait_ms(), delayed deadlines)` (unchanged computation).
- Counters: `queue_run_` counts queue messages handled in a row while `F` or `B`; it resets when a lane-2/3 item runs and when neither has work. `front_run_` counts Front items run in a row while `B`; it resets when a Back item runs and when `B` is false. So a burst only ever counts work that kept another lane waiting, and the tests below can predict the order exactly.
- `housekeeping()` runs before every item and holds today's per-pass steps in today's order: `follow_visibility()`, `run_delayed()`, `enforce_limits(false)` with a scene in front, the agent run's deadline (`lua_clock()`), the minute polls of `run_scene_clock()` (minute, battery and lang handlers stay there: at most once a minute per scene), `tool_clock()`, `tool_settle()`, `persist_background()`, `close_if_asked()`. `run_timers()` leaves the per-pass steps; `show_scene()` calls `run_timers_of(front)`.
- A due-timer id that was cancelled after `due()` returned it runs nothing (`run_timer()`'s `run_entry` finds no function, `bind_timer.cpp` ~108), as within today's loop.

- [ ] **Step 1: Write the failing tests** in `scene_lanes_test.cpp`: a `SceneLanes` fixture on `SceneRuntimeFixture` with a `SceneHttp` over `FakeHttpClient` (as `HttpBinding`), the `StallGate` of Task 2 as `device_status`, a background scene `bg` (scene.json `"background": true`, a manifest like `bg_manifest` in `scene_background_test.cpp`; opened, then sent back, only by the tests that use it) and a front scene `fg` with a button `ref = "tap"` (prints "tap") and a button `ref = "stall"` whose handler calls `wisp.device.info()`, then `print("stalled")`, plus what each test adds before or after that call. The order is read from `log::ring_entries()` after the `stalled` line, filtered to tags `scene.fg`, `scene.bg` and `agent.lua` (without the proxy's `http: ...` refusal lines, as `HttpBinding::printed()` does). Each test arms the gate, clicks `stall`, waits for `wait_entered()`, sets up what it needs (waiting until `os::now_ms()` has passed a timer's due time where one must be due; the order asserted does not depend on how long) and releases.

```cpp
TEST_F(SceneLanes, TapRunsBeforeABackgroundTimer)
// bg: wisp.timer.every(50, print "bg"). While stalled: click_ref("tap") (fg prints "tap");
// wait 60 ms. expect order: "stalled", "tap", then the first "bg".
TEST_F(SceneLanes, FrontResultRunsBeforeABackgroundTimer)
// fg's stall handler first makes one refused get whose callback prints "fg http".
// expect: "fg http" before the first "bg" after "stalled".
TEST_F(SceneLanes, BackgroundGetsATurnAfterFourFrontItems)
// fg's stall handler makes 6 refused gets (prints "f1".."f6"); bg's 50 ms timer is due.
// expect after "stalled": f1 f2 f3 f4 bg f5 f6.
TEST_F(SceneLanes, AgentLuaWaitsBehindTheFront)
// before the stall: a thread runs rt->run_lua("wisp.sleep(50) print('agent') return 1", {});
// wait until rt->vm_pool_used() grew. While stalled: click "tap"; wait 60 ms.
// expect: "tap" before "agent"; the run returns ok.
TEST_F(SceneLanes, QueueBurstLetsAResultThrough)
// no bg. fg's stall handler makes one refused get ("fg http") after wisp.device.info()
// returns; while stalled, 10 clicks on "tap".
// expect: "fg http" after the 8th "tap" and before the 9th (the stall message itself
// ran while no other lane had work, so it does not count).
```

`scene_run_lua_test.cpp`: `SliceLimit` expects `"200 ms"` in the message and `ms >= 150`.

- [ ] **Step 2: Run** `ctest --test-dir build/sim -R 'SceneLanes|RunLua' --output-on-failure`. Expected: the lane tests FAIL (today the pass runs every due timer before the next message), `SliceLimit` FAILS on "1000 ms".
- [ ] **Step 3: Implement** the lanes, the constants and the header paragraph. Remove `run_completions()`. Update the schema text and regenerate.
- [ ] **Step 4: Run** `ctest --test-dir build/sim -R 'Scene|RunLua|LuaRun|HttpBinding|FilesBinding|Notify|Sensor|Store|Migrate|DocIsUpToDate' --output-on-failure`. Expected: PASS, including `SceneBackground.SeveralBackgroundScenesRunTheirOwnTimers`, `SceneBackground.TimersRunWhileScreenOffForDeclaredScene` and the eviction-order tests (`BackgroundGrowthEvicts*`).
- [ ] **Step 5: Commit** `feat(scene): run the front scene's work first and cut agent Lua slices to 200 ms`.

---

### Task 4: Two HTTP lanes

**Files:**
- Modify: `components/wisp_net/include/wisp/net/scene_http.hpp` (file header "Accepted requests run one at a time on the wisp.shttp task ... the scenes' other requests wait behind it", `task_config`, limits, private state)
- Modify: `components/wisp_net/src/scene_http.cpp` (`Job` ~301, `task_config` ~343, `start` ~353, `stop` ~376, `submit` queueing ~504-556, `cancel_scene` ~559, `set_deferring` ~595, `idle` ~629, `promote_locked` ~649, `run` ~659, `execute` ~708)
- Test: `tests/net/scene_http_test.cpp`, `tests/scene/files_binding_test.cpp`

**Interfaces:**
- Produces in `scene_http.hpp`:
  - `enum class HttpLane : uint8_t { Interactive, Download };` A request goes to `Download` when `SceneHttpRequest::download` is set (`wisp.http.download`, for either principal), else `Interactive` (scene requests, the agent's `http.fetch`, `lua.run`'s `wisp.http`).
  - `static os::TaskConfig task_config(HttpLane lane);` `"wisp.shttp"` / `"wisp.shttp.dl"`, both 8192 bytes, priority 3, core -1, `flash_access = false`. The no-argument overload goes.
  - `SceneHttpLimits::download_queue = 2;` (downloads waiting behind the one in flight; past it `"rate_limited"`, like `queue` for the other lane). The Agent principal bypasses both caps, as it bypasses `queue` today.
  - private: `struct Lane { Deque<os::LargeUnique<Job>> queue; Job* current = nullptr; os::LargeUnique<RequestWatch> watch; os::Semaphore wake{1024, 0}; std::unique_ptr<os::Task> task; };` `std::array<Lane, 2> lanes_;` (indexed by `HttpLane`), replacing `queue_`, `current_`, `watch_`, `wake_`, `task_`; `void run(HttpLane);` `void execute(Job&, Lane&);` `Job::seq` (`uint64_t`, taken under `mu_` at acceptance).
- Shared between the lanes, all under `mu_` as today: `scenes_` (one per-minute window per scene for both lanes), `deferred_`, `deferring_`; `deliver_mu_` (both workers call `done()` under it, so `cancel_scene()`'s barrier still covers both).
- Rules:
  - Only the Interactive worker times out deferred requests (`expire_locked`, `expired_`). Both workers call `promote_locked()`, which walks `deferred_` oldest first and moves each job into its own lane's queue while that lane has room, skipping (and keeping in place) jobs whose lane is full; `set_deferring(false)` wakes both.
  - `set_deferring(true)` moves both lanes' waiting jobs to the front of `deferred_`, ordered by `Job::seq`.
  - `cancel_scene()` drops the scene's jobs from both queues and `deferred_`, marks each lane's `current` of that scene and aborts its ticket on that lane's watch, then takes the `deliver_mu_` barrier.
  - `start()` makes both watches and spawns both tasks; if either fails, the other is stopped and joined and the proxy stays off (`NoMemory`, `submit()` answers `"other"`). `stop()` stops and joins both; `idle()` covers both.
  - `TlsGate` is untouched: every client still takes `TlsGate::process()` only around connect and handshake (`esp_http_client.cpp` ~247), so at most one handshake runs while the two lanes each hold an established session.
- Memory: `Lane` lives inside the `SceneHttp` object (`os::make_large`, PSRAM). The new worker's stack is 8 KB in PSRAM (`flash_access = false`). Internal RAM: one more TCB, one more `RequestWatch` timer handle, one more semaphore (owner's device check).

- [ ] **Step 1: Write the failing tests** (`scene_http_test.cpp`; a download is `get(...)` with `download = true` and an `on_body` that returns true; a `responder` gives the download URL a 64 KB body and the other URLs `{}`; `client.before_piece` blocks on a one-shot gate consumed by the first piece, so only the download waits):

```cpp
TEST(SceneHttpTask, ConfigOfBothLanes)
// task_config(Interactive): "wisp.shttp"; task_config(Download): "wisp.shttp.dl";
// both 8192, priority 3, core -1, !flash_access. Replaces SceneHttpTask.Config.
TEST_F(SceneHttpTest, RequestCompletesWhileADownloadStreams)
// download held in before_piece; then get("https://api.example.com/x") -> its result
// arrives (error "", status 200) while the download has not; release -> the download's arrives.
TEST_F(SceneHttpTest, AgentFetchCompletesWhileADownloadStreams)
// same with agent_get("https://x.example.com/").
TEST_F(SceneHttpTest, DownloadsRunOneAtATime)
// download A held; download B submitted -> client.calls() stays 1; release -> B runs.
TEST_F(SceneHttpTest, FullDownloadQueueIsRateLimited)
// limits.download_queue = 1: A held, B queued, C -> "rate_limited" at once.
TEST_F(SceneHttpTest, CancelSceneStopsBothLanes)
// download of "s" held in before_piece; then client.hold = true and a get of "s" is held;
// cancel_scene("s"); release the piece gate; client.hold = false.
// expect: no done() for either; client.saw_cancel.
TEST_F(SceneHttpTest, DeferredRequestsGoToTheirLanes)
// set_deferring(true); submit a download and a get; set_deferring(false):
// both complete, both deferred = true.
TEST_F(SceneHttpTest, IdleCoversBothLanes)
// idle() false while only the download is held; true after.

// files_binding_test.cpp
TEST_F(FilesBinding, GetCompletesDuringADownload)
// the scene downloads (held in before_piece) then gets; "get done" is printed before
// "download done"; release.
```

- [ ] **Step 2: Run** `ctest --test-dir build/sim -R 'SceneHttp|FilesBinding' --output-on-failure`. Expected: build failure on `task_config(HttpLane)`, then the lane tests FAIL (the get waits behind the download).
- [ ] **Step 3: Implement.** Keep the existing tests' expectations (`FullQueueIsRateLimited`, `AgentPrincipalBypassesQueueCap`, the deferral tests) unchanged. Rewrite the file header paragraph on workers.
- [ ] **Step 4: Run** `ctest --test-dir build/sim -R 'SceneHttp|HttpBinding|FilesBinding|SceneBackground|RunLua|HttpFetch|NetworkWindow' --output-on-failure`. Expected: PASS.
- [ ] **Step 5: Commit** `feat(net): give scene downloads their own http worker`.

---

### Task 5: `.prev` is written once per agent turn

**Files:**
- Modify: `components/wisp_agent/include/wisp/agent/tool_registry.hpp` (`ToolCtx`)
- Modify: `components/wisp_agent/src/agent_service.cpp` (`run_tool`, where `ToolCtx ctx;` is filled, ~831)
- Modify: `components/wisp_agent/include/wisp/agent/scene_workspace.hpp` (file header steps 6 and rollback; private members; `install_zip` signature)
- Modify: `components/wisp_agent/src/scene_workspace.cpp` (`discard` ~431, `install` ~441, `rollback` ~548, `install_zip` ~561)
- Modify: `components/wisp_agent/src/tools_scene_authoring.cpp` (the `scene.install` and `scene.rollback` descriptions: "the version from before this turn")
- Test: `tests/agent/scene_workspace_test.cpp`, `tests/agent/agent_loop_test.cpp`

**Interfaces:**
- Produces in `ToolCtx`: `uint32_t turn = 0;` "the agent turn the call belongs to; 0 outside a turn". `AgentService::Impl::run_tool()` sets `ctx.turn = turn;`. `ToolRegistry::call()` already copies the whole context to the tool's thread (`CallState::ctx`).
- Produces in `SceneWorkspace` (private):

```cpp
// The scenes this turn already installed (deviations 4): their .prev is
// the version from before the turn. RAM only; one entry per scene.
struct Session {
    std::string id;
    uint32_t turn = 0;
    uint64_t generation = 0;  // SceneRepository::changed_since()'s `now` after our install
};
os::Mutex sessions_mu_;  // install/rollback run on wisp.agent, discard on wisp.fs
std::vector<Session> sessions_;
bool in_session(const std::string& id, uint32_t turn);  // keeps a valid entry
void note_install(const std::string& id, uint32_t turn); // records or refreshes
void end_session(const std::string& id);
ToolOutput install_zip(const std::string& id, std::span<const uint8_t> zip, bool drop_draft,
                       bool keep_prev, const std::string& note, ToolCtx& ctx);
```

- Rules:
  - `install()`: `keep_prev = !in_session(id, ctx.turn)`; `in_session` is true only for `turn != 0`, an entry of that id with the same turn, and `changed_since(entry.generation, now)` not naming the id (anything else that replaced or removed it ends the session; the entry is erased then). After a successful `install_scene()`: `note_install(id, ctx.turn)` (skipped for turn 0), taking the generation from `changed_since(UINT64_MAX, now)`, and erasing entries of other turns.
  - `rollback()`: `keep_prev = true` as today; on success `end_session(id)`. A failed rollback leaves the session.
  - `discard()`: `end_session(id)` on success.
  - `SceneRepository` is unchanged: `keep_prev = false` already removes the replaced package and leaves `.prev/<id>` alone (`finish_replace`, `scene_repository.cpp` ~713-721).
  - A failed install records nothing, so a first install that fails starts no session.

- [ ] **Step 1: Write the failing tests** (`scene_workspace_test.cpp`; `begin()` installs counter 1.0.0; `prev_version()` reads `.prev/counter/scene.json`'s version; `ctx.turn` set per test):

```cpp
TEST_F(SceneWorkspaceTest, SecondInstallInATurnKeepsTheBaseline)
// turn 5: install 2.0.0, install 3.0.0 -> prev_version() == "1.0.0", installed 3.0.0;
// rollback -> installed 1.0.0.
TEST_F(SceneWorkspaceTest, ANewTurnStartsANewBaseline)
// turn 5 install 2.0.0; turn 6 install 3.0.0 -> prev 2.0.0.
TEST_F(SceneWorkspaceTest, DiscardEndsTheSession)
// turn 5 install 2.0.0; write 3.0.0; discard; write 3.0.0; install -> prev 2.0.0.
TEST_F(SceneWorkspaceTest, RollbackEndsTheSession)
// turn 5: install 2.0.0, install 3.0.0, rollback (installed 1.0.0, prev 3.0.0),
// install 4.0.0 -> prev 1.0.0.
TEST_F(SceneWorkspaceTest, AnotherInstallerEndsTheSession)
// turn 5 install 2.0.0; core::install_scene(*repo, *rt, <zip of 2.5.0 built with
// core::ZipWriter>, {}); turn 5 install 3.0.0 -> prev 2.5.0.
TEST_F(SceneWorkspaceTest, CallsOutsideATurnAlwaysKeepPrev)
// turn 0: install 2.0.0, install 3.0.0 -> prev 2.0.0 (today's behaviour).

// agent_loop_test.cpp
TEST_F(AgentLoop, ToolCallsCarryTheTurn)
// add_tool("scene.list") recording ctx.turn; two scripted turns each calling it once
// -> recorded {1, 2}.
```

`InstallKeepsPrevious` and `RollbackSwapsWithPrev` keep turn 0 and stay as they are.

- [ ] **Step 2: Run** `ctest --test-dir build/sim -R 'SceneWorkspaceTest|ToolsTest|AutoCheck|AgentLoop' --output-on-failure`. Expected: `ToolCtx::turn` does not compile, then `SecondInstallInATurnKeepsTheBaseline`, `RollbackEndsTheSession` and `AnotherInstallerEndsTheSession` FAIL.
- [ ] **Step 3: Implement.** Update `scene_workspace.hpp`'s header: step 6 keeps the installed version as the previous one "unless this turn already installed the scene"; rollback "returns to the version from before the turn's first install".
- [ ] **Step 4: Run** the same filter plus `-R 'SceneOps|SceneInstall|SceneRepository'`. Expected: PASS.
- [ ] **Step 5: Commit** `fix(agent): keep the pre-turn version as the rollback target`.

---

### Task 6: A list reset keeps its old rows until the new ones arrive

**Files:**
- Modify: `components/wisp_scene/include/wisp/scene/scene_view.hpp` (`ViewCallbacks::on_list_range`; the list paragraph of the file header ~83; `update_window` declaration)
- Modify: `components/wisp_scene/src/scene_view_internal.hpp` (`Node::rows_stale`)
- Modify: `components/wisp_scene/src/scene_view_list.cpp` (`apply_list_reset` ~35, `update_window` ~59, `apply_list_rows` ~110)
- Modify: `components/wisp_scene/src/scene_runtime.cpp` (`ui_show_view`'s `on_list_range` ~2243 returns the push result)
- Modify: `tests/support/scene_view_fixture.hpp` (`on_list_range` returns `!refuse_ranges`; new `std::atomic<bool> refuse_ranges{false};`)
- Test: `tests/scene/scene_view_list_test.cpp`

**Interfaces:**
- Produces: `std::function<bool(NodeId list, uint32_t first, uint32_t count)> on_list_range;` "false: the request was not passed on, so no rows will come for it"; `bool SceneView::update_window(Node& list, bool force);` returns the callback's answer (true when nothing was asked).
- Produces in `Node`: `bool rows_stale = false;` "a ListReset came and no ListRows since: `built` holds the old items' rows".
- Rules (UI thread):
  - `apply_list_reset`: no `drop_rows()`; drop the built rows whose index is `>= r.count`; set `rows`, height and scroll clamp as today; `rows_stale = !built.empty()`; `update_window(*n, true)`, which also drops rows outside the new window. If it returned false or the new window's `count` is 0: `drop_rows(*n)`, `rows_stale = false`.
  - `apply_list_rows`: when `rows_stale`, `drop_rows(*list)` and clear it before building.
  - A `Build` replaces the tree and its lists as today. Clicks on a stale row reach the scene with node ids the prelude already dropped and find no handler (the existing behaviour `ItemsReplacedWhileScrolledClampsScroll` pins).
  - Stale rows count towards `kMaxViewNodes` only until they are dropped, which happens before the first new row is checked (`row_ok`).

- [ ] **Step 1: Write the failing tests.** Add to `SceneViewList` a helper `run_unpumped(src)` that loads and calls a chunk and runs `flush_reactive()` without `pump()`, and `apply_batch()` that applies `batch.take(kInstance)` on the UI thread without answering ranges.

```cpp
TEST_F(SceneViewList, ResetKeepsOldRowsUntilNewRowsArrive)
// open kTwoHundredRows; old = row_node(0); take_ranges();
// run_unpumped("_G.items:set(<200 new items 'new i'>)"); apply_batch():
// rows_built() unchanged and > 0; view->object(old) != nullptr; a range for kList was asked.
// pump(): view->object(old) == nullptr; label_of(row_node(0)) == "new 1".
TEST_F(SceneViewList, ResetToFewerItemsDropsRowsPastTheEnd)
// items set to 3 new ones, apply_batch(): rows_built() == 3 (the old rows 0-2);
// pump(): labels of rows 0-2 are the new items.
TEST_F(SceneViewList, ResetToNoItemsDropsEveryRow)
// items set to {}, apply_batch(): rows_built() == 0.
TEST_F(SceneViewList, UnansweredResetDropsTheOldRows)
// refuse_ranges = true; items replaced, apply_batch(): rows_built() == 0.
```

- [ ] **Step 2: Run** `ctest --test-dir build/sim -R 'SceneViewList|SceneView|SceneRuntimeTest' --output-on-failure`. Expected: `ResetKeepsOldRowsUntilNewRowsArrive` and `ResetToFewerItemsDropsRowsPastTheEnd` FAIL (rows dropped at the reset).
- [ ] **Step 3: Implement.** Check the list on round, rounded and rect shapes in `examples_test`'s existing list scenes (the ledger example): the first and last rows stay fully visible at both scroll ends after a reset (AGENTS.md section 3, "Screen shape").
- [ ] **Step 4: Run** the same filter plus `-R 'Examples|PatchQueue|SwipeRule'`. Expected: PASS, including `ItemsReplacedWhileScrolledClampsScroll`, `RefreshRebuildsRows` and `StaleRowsOutsideTheWindowAreDropped`.
- [ ] **Step 5: Commit** `fix(scene): keep a list's rows on screen until its new rows arrive`.

---

### Task 7: Documents and full check

**Files:**
- Modify: `docs/superpowers/specs/2026-11-04-scene-runtime-core-design.md` (3.1: the queue carries control and UI events, results wait in each VM's box, the three lanes with `kQueueBurst`/`kFrontBurst`; 4.3: a reset keeps old rows until new rows arrive; 5.3: two workers, `download_queue`, the in-flight cap; 5.6 limits table: calls in flight 16; 2.3: the second worker's TCB in internal RAM)
- Modify: `docs/superpowers/specs/2026-11-05-scene-device-design.md` (7.1: a download has its own worker and no longer holds up other requests)
- Modify: `docs/superpowers/specs/2026-11-09-agent-tools-design.md` (4.2 line ~176 and 4.3 line ~182: the agent's slice is 200 ms)
- Modify: `docs/superpowers/specs/2026-11-05-agent-core-design.md` (4.1 table rows `scene.install`/`scene.rollback` and the `.prev` rule at ~232: written by the first install of a turn; the session's ends as in deviation 4)
- Modify: `docs/architecture.md` (task table: `wisp.shttp` now interactive requests only; new row `wisp.shttp.dl`, `net::SceneHttp`, 8192, `false`, PSRAM on the device, downloads one at a time with the 5-minute deadline; and the stop-order sentence "scene proxy `SceneHttp` (may wait for a request in progress)" becomes "joins both workers")
- Modify: `docs/hardware/waveshare_esp32_s3_touch_amoled_2_16.md` (~491: add the `wisp.shttp.dl` row as declared; its measured high-water mark is left for the owner)
- Modify: `CHANGELOG.md` (an `Unreleased` entry; the version bump happens at merge, AGENTS.md section 7)

- [ ] **Step 1:** Edit the documents above. Each amended spec paragraph states the new rule with its constant name.
- [ ] **Step 2:** Run `bash tools/ci.sh`. Expected: exit 0, no warnings in test output.
- [ ] **Step 3:** In the simulator (`build/sim`), open the weather example and replace a list's items in the ledger example; run a `lua.run` against `tools/fake_openai.py` that fetches while a scene downloads. Record what was observed in the commit body.
- [ ] **Step 4: Commit** `docs: record the scene scheduling and install safety rules`.

**Device checks for the owner (not run by the agent):**
- `mem` at the worst case: a large `wisp.http.download` in flight and an agent `http.fetch` of an HTTPS page at the same time, with STA+AP and both httpd instances up. Internal free and largest block against `kWorstInternalFreeMin`/`kWorstInternalLargestMin` (`ram_budget.hpp`), and the stack high-water marks of `wisp.shttp` and `wisp.shttp.dl`. Record them in `docs/hardware/<board>.md`.
- `mem` idle after boot: the cost of the second worker (TCB, watch timer, semaphore) against `kIdleInternalFreeMin`.
- The agent's usual `lua.run` scripts (JSON over a fetched page, a loop over a 64 KB result) on the S3 at the 200 ms slice: none ends in `timeout`.
- Tap latency on the front scene while a background scene runs a 50 ms timer and the agent waits on fetches: no visible lag.
- `WISP_PERF` while scrolling and while replacing the items of a long list (ledger example) against the baseline, and no blank frame on the reset, on the 2.16 and the 1.75C round panel.
- Two `scene.install` calls of one scene in one spoken turn, then "undo" (`scene.rollback`): the version from before the turn comes back after a restart too (the `.prev` on flash).
