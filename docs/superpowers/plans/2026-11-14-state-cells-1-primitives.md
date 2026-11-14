# State Cells 1: Primitives and the Topic Registry — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The building blocks of phase D exist, tested, with no change in what the device does: `wisp::FixedString`, `core::Dispatcher` (implemented by `EventBus` and `UiRuntime`), `core::StateCell<T>` with `CellSubscription`, `core::EdgeInbox<E, N>`, the topic registry `wisp/core/topics.hpp` with its CI check `tools/check_topics.py`, and `mem` reporting dropped bus events and live cell subscriptions.

**Architecture:** A `DispatchSlot` is an intrusive unit of work; a `Dispatcher` runs queued slots on its own thread, at most once per queueing (that is the coalescing). `EventBus` runs slots on `wisp.bus` in a loop that drains slots and events alternately until both are empty; `UiRuntime` runs them at the top of every UI loop iteration, paused or not. A `StateCell<T>` owns a value, a revision and a list of subscriptions, each a slot that, when it runs, copies the value under the cell's lock and calls the subscriber outside every lock. An `EdgeInbox` is a fixed ring drained by one slot. Nothing in `components/` subscribes to a cell yet; plans 2 to 4 do that.

**Tech Stack:** C++20 (`-fno-exceptions -fno-rtti`), GoogleTest, Python 3 (`unittest`) for the check script, the simulator build (`build/sim`).

**Spec:** `docs/superpowers/specs/2026-11-14-state-cells-design.md` (approved 2026-11-14; its "Approval" section records the owner's answers), sections 3.2, 3.3, 3.5, 3.7, 3.9, 4 step 1, 5 and 7. Where this plan departs from it, "Deviations from the spec" below says so.

## The four plans of phase D

The spec's migration (section 4) touches most components; one plan would have about fifty tasks. It is split into four plans, each of which leaves `bash tools/ci.sh` green and the device's behaviour unchanged unless its own text says otherwise. Three (as first suggested) would leave one plan near thirty tasks, too many for one reviewer.

| Plan | File | Delivers |
|---|---|---|
| 1 (this) | `2026-11-14-state-cells-1-primitives.md` | the primitives, the registry and its check, `mem` counters |
| 2 | `2026-11-14-state-cells-2-cells.md` | every cell of spec 3.4 set where its event is published today; the hand-written publishes become mirrors; the forwarded stream pinned by a golden test |
| 3 | `2026-11-14-state-cells-3-subscribers.md` | every state subscriber of spec 3.8 moved from the bus to a cell, one consumer per task; the folded mirrors of spec 1.4 deleted with their subscriptions |
| 4 | `2026-11-14-state-cells-4-edges-cleanup.md` | button listeners and inboxes, the scene lifecycle inbox, the compensations C1-C8 removed one per task, the documents |

All four run on the branch `feat/state-cells`. Plans 2-4 list what to re-verify when they start, because this plan fixes the interfaces they consume.

## Global Constraints

- Branch: `feat/state-cells` (already created from `main`; it holds the spec commit). No commits on `main`. Ask the owner before pushing or merging (AGENTS.md section 7).
- AGENTS.md applies: `wisp::Result<T>` for errors, no exceptions or RTTI, comments explain why (no review ids, no "Task N", no "C1"-style bare ids: `tools/ci.sh`'s guard rejects `\b[CIMN][0-9]+:` and `(C1)` outside `docs/superpowers/`), English in code and docs, `clang-format` clean, test output without warnings.
- Memory: every object this plan allocates per subscription or per inbox lives in `Mem::Large` (`os::make_large`, `os::LargeAllocator`, `os::LargeFunction`). The only new internal-RAM objects are lock objects: one `os::Mutex` per `EventBus` and per `UiRuntime` for the ready list (this plan), one per `StateCell` and one per `EdgeInbox` (when plans 2-4 create them). No allocation happens in `set()`, `schedule()` or `push()`.
- Locks: a cell's lock, a ready list's lock and an inbox's lock are leaf locks. Nothing is called while one is held, except (a) a cell's mirror, which only calls `EventBus::publish()` (itself a leaf: `publish_mutex_` and a non-blocking push), and (b) `Dispatcher::schedule()` called by `set()`/`subscribe()` under the cell's lock, which only takes the ready list's lock. The order is `cell → bus publish_mutex_` and `cell → ready list`; no path takes them the other way. No callback, handler or `read(F)` body runs while a ready-list lock is held.
- Threads: a slot runs only on its dispatcher's thread (`wisp.bus` or the UI task), never concurrently with itself, never inside `schedule()`. Callbacks must not touch flash (they run on `wisp.bus` or the UI task, both `flash_access = false`).
- Misuse that would corrupt memory later (destroying a cell or a dispatcher that still has subscriptions, re-entering a cell from inside its own `read()`/`update()`/mirror) goes through one helper, `core::detail::misuse(const char* what)`: in host builds it prints `what` to stderr and calls `std::abort()` (death tests pin the text); in ESP-IDF builds (`ESP_PLATFORM`) it logs `WLOG_E` once per call site and returns. A misuse that would be a use-after-free on the device too (destroying an inbox from its own handler) calls `std::abort()` on both.
- Every task ends with `cmake --build build/sim && ctest --test-dir build/sim --output-on-failure -R '<the task's filter>'` green. Task 11 runs `bash tools/ci.sh`.
- New test binaries are added to `tests/CMakeLists.txt` with `wisp_test(...)` and a `TIMEOUT` (20 s, 30 s for the stress tests), so a deadlock fails one case instead of hanging the run. Death tests set `GTEST_FLAG_SET(death_test_style, "threadsafe")` and restore it (as `tests/os/flash_guard_test.cpp` does).
- No sleeps as synchronisation in tests: gates (`os::Semaphore`), `ManualDispatcher::run_all()`, `EventBus::wait_idle()` and `wisp::test::wait_until()`. A negative check ("has not returned yet") may wait a bounded 20 ms, as `EventBus.UnsubscribeFromExternalThreadWaitsForInFlightDispatch` already does.
- Commit messages: Conventional Commits, ending with the `Co-Authored-By` line of the session's attribution rules.
- Device-only checks are listed at the end; they are never reported as done.

## Review Focus

1. A subscription released from inside its own callback, while a `set()` from that same callback has queued it again: no further call, the subscription's memory is freed exactly once after the run returns, and the dispatcher never touches it afterwards. Tests in Task 4 (`StateCell.UnsubscribeInsideItsOwnCallbackStopsFurtherCalls`) and Task 2 (`SlotQueue.CancelInsideOwnRunDefersToFinish`).
2. A slow subscriber while another thread sets 1000 values: exactly one more call, with the last value; calls never overlap; the values it sees strictly increase. Test in Task 4 (`StateCell.SlowSubscriberGetsExactlyOneMoreCallWithTheLastValue`).
3. The bus saturated by telemetry while a cell changes: every subscriber ends at the last value, `wait_idle()` returns only after the last slot ran, and with `ready_` saturated no event or slot is stranded. Tests in Task 2 (`EventBus.SaturatedTokensStrandNothing`) and Task 5 (`StateCell.SubscribersConvergeOnASaturatedBus`).
4. The UI runtime paused, with its post queue full: a slot scheduled from another thread still runs; after `stop()` no slot runs and `cancel()` never waits. Tests in Task 3.
5. Destroying a cell or a dispatcher with live subscriptions, and calling `get()` on a cell inside its own `read()`: the simulator aborts with the call site named, instead of a later use-after-free or a deadlock. Death tests in Tasks 2, 4 and 6.

## Deviations from the spec

1. **`Dispatcher::cancel()` returns `bool`, and `DispatchSlot` gains `finish`.** Spec 3.2 declares `void cancel(DispatchSlot&)` that "on [the dispatcher's thread] returns at once". When the slot being cancelled is the one running (a subscription released inside its own callback), its owner must not free it yet: the dispatcher still touches the slot when the run returns. `cancel()` returns `false` in exactly that case, and the dispatcher calls `slot.finish(slot)` once it no longer touches the slot; the owner frees it there. In every other case it returns `true` and the owner frees at once.
2. **`Dispatcher` counts attached slots.** Spec 7 names the shutdown order as the main risk ("now it aborts in the simulator") but gives the dispatcher no way to notice. `Dispatcher` gets non-virtual `attach()`/`detach()`/`attached()`; cells and inboxes attach for as long as they hold a slot on it; `~EventBus` and `~UiRuntime` report `attached() != 0` through `core::detail::misuse()`.
3. **Callbacks are `os::LargeFunction`, not `std::function`.** `std::function` keeps a callable with more than two pointers of captures on the default heap, which is internal RAM on the device (`wisp/os/large.hpp`). Spec 3.3's memory paragraph wants subscriptions in `Mem::Large`, so `StateCell<T>::Callback` is `os::LargeFunction<void(const T&)>` and `EdgeInbox`'s handler is `os::LargeFunction<void(const E&)>`. (Captured state must therefore be copyable, which every planned subscriber's is.)
4. **`Mirror` takes `const T& before`.** The spec's `const T* before` never has a null case: a cell always has a value, and `seed()` does not call the mirror.
5. **`EventBus` alternates one slot and one event** per step of its drain loop instead of "every queued slot, then every queued event". Both drain until both lists are empty, which is what the spec's argument needs; alternating also bounds how long a burst of one kind delays the other.
6. **`UiRuntime::schedule()` wakes the UI task whenever its ready list goes from empty to non-empty from another thread,** not only while paused. Checking `paused` first races with the loop entering its paused wait right after it last ran the slots; pushing the no-op on every empty-to-non-empty transition cannot miss, costs at most one post per batch, and a full post queue wakes the task anyway (the spec's argument).
7. **Topic constants are `inline constexpr char kName[]`,** not `std::string_view`: they must convert implicitly to `std::string` for `EventBus::subscribe(std::string, ...)` and `Event::topic`. `Topic::name` in `kAll` stays a `std::string_view`.
8. **Step 1's "no caller changes"** is read as "no behaviour change": the check this step adds (spec 3.7, "no topic string literal elsewhere") can only pass once every call site names a constant, so Task 9 replaces the literals, mechanically.
9. **`FixedString` is `wisp::FixedString`** in `wisp/base/fixed_string.hpp`: `wisp_base` uses namespace `wisp` (`wisp/base/value.hpp`), not `wisp::core`.
10. **The `Kind` column of `docs/architecture.md`'s topic table arrives in plan 4,** together with the cell per topic (spec 4 step 6). `kAll` records each topic's final kind from this plan on; the check compares names only.
11. **Live cell subscriptions are counted per `EventBus`** (every cell is built with one), and `mem` prints them through a new `App::dispatch_report()`: the firmware's `mem` command calls the port's `mem_report()` (`firmware/main/main.cpp`), which cannot see App. Inbox lines join that report in plan 4, when the inboxes exist.
12. **An inbox's overflow warning is throttled by count** (the first overflow, then every 64th), like `EventBus`'s drop warning, rather than by time: deterministic to test, and no clock needed.
13. **Slot passes and inbox runs are bounded** (rulings made while executing the plan; where a task's text below says otherwise, these hold). `UiRuntime` runs, per loop iteration, only the slots queued when the pass started, and its paused loop waits one loop period (10 ms) between passes while a slot stays queued; an `EdgeInbox` run delivers only the edges waiting when it started and schedules itself again if more remain, so an edge pushed by the handler is delivered in the next run. Otherwise a slot or handler that re-arms itself every time would hold its dispatcher forever.
14. **The button topic patterns are `button.<name>.<edge>`,** where `<name>` is the device id after `button.` (the topic is `<device id>.<edge>`), not `button.<id>.<edge>` as the spec, Task 7 and plan 4 write: read literally, the old form expands to `button.button.power.down`.
15. **No `detail` namespace inside `wisp::core`** (it would hide `wisp::detail` for every includer): the internals are `core::cell_detail` and `core::edge_detail`, and the misuse reporter is `core::dispatch_misuse()`. A host-only handler (`set_misuse_handler`) lets tests run the teardown paths that follow a reported misuse; after such a report a cell's teardown waits at most 3 s and then aborts.
16. **`FixedString::assign_utf8()`** cuts at a code-point boundary and is what display text uses; `assign()` stays byte-exact for identifiers. `tools/check_topics.py` also reports a topic-shaped row anywhere in the "Event topics" section outside the topic table.

---

### Task 1: `wisp::FixedString<N>`

**Files:**
- Create: `components/wisp_base/include/wisp/base/fixed_string.hpp` (header-only)
- Create: `tests/base/fixed_string_test.cpp`; Modify: `tests/CMakeLists.txt` (`wisp_test(NAME fixed_string_test SRCS base/fixed_string_test.cpp LIBS wisp_base)`)

**Interfaces:**
- Produces:

```cpp
namespace wisp {
// Up to N bytes stored inline, so a struct of these copies without
// allocating (state cells, spec 3.3 of the state-cells design). Bytes,
// not characters: an SSID is raw bytes and may hold any value, NUL
// included; truncation counts bytes.
template <size_t N>
class FixedString {
    static_assert(N > 0 && N <= 255, "the size is kept in one byte");
public:
    constexpr FixedString() = default;
    FixedString(std::string_view s);              // NOLINT(google-explicit-constructor); truncates
    bool assign(std::string_view s);              // false when bytes past N were dropped; zero-fills the tail
    std::string_view view() const;
    std::string str() const;
    size_t size() const;
    bool empty() const;
    static constexpr size_t capacity() { return N; }
    friend bool operator==(const FixedString& a, const FixedString& b);  // size, then the first size() bytes
    friend bool operator==(const FixedString& a, std::string_view b);
private:
    std::array<char, N> bytes_{};
    uint8_t size_ = 0;
};
}  // namespace wisp
```

- [ ] **Step 1: Write the failing tests** in `tests/base/fixed_string_test.cpp`:

```cpp
TEST(FixedString, IsTriviallyCopyable)
// static_assert(std::is_trivially_copyable_v<wisp::FixedString<32>>) and the same for <72>.
TEST(FixedString, AssignWithinCapacityKeepsEveryByte)
// a 32-byte string holding '\0' at index 3 and '\xff' at 31: assign() == true,
// size() == 32, view() == the input byte for byte.
TEST(FixedString, AssignPastCapacityTruncatesAndSaysSo)
// FixedString<4>: assign("abcdef") == false, view() == "abcd"; assign("ab") == true, size() == 2.
TEST(FixedString, EqualityIgnoresBytesPastTheSize)
// a.assign("abcdef") then a.assign("ab"); b.assign("ab"): a == b; a == std::string_view("ab");
// a != FixedString<8>("abc").
TEST(FixedString, EmptyByDefault)
// size() == 0, empty(), view().empty(), == "".
```

- [ ] **Step 2: Run** `cmake --build build/sim && ctest --test-dir build/sim -R FixedString --output-on-failure`. Expected: build fails (no header).
- [ ] **Step 3: Implement** the header. `assign()` copies `min(s.size(), N)` bytes and zero-fills the rest of `bytes_`.
- [ ] **Step 4: Run** the same filter. Expected: PASS.
- [ ] **Step 5: Commit** `feat(base): add FixedString for allocation-free snapshots`.

---

### Task 2: `core::Dispatcher`, and `EventBus` as one

**Files:**
- Create: `components/wisp_core/include/wisp/core/dispatcher.hpp`, `components/wisp_core/src/dispatcher.cpp` (add to `SRCS` in `components/wisp_core/CMakeLists.txt`)
- Modify: `components/wisp_core/include/wisp/core/event_bus.hpp`, `components/wisp_core/src/event_bus.cpp` (`run_dispatcher()`, `wait_idle()`, destructor, file header)
- Create: `tests/support/manual_dispatcher.hpp`, `tests/core/dispatcher_test.cpp`; Modify: `tests/core/event_bus_test.cpp`, `tests/CMakeLists.txt` (`wisp_test(NAME dispatcher_test SRCS core/dispatcher_test.cpp LIBS wisp_core wisp_port_sim_os TIMEOUT 20)`)

**Interfaces:**
- Produces in `dispatcher.hpp` (namespace `wisp::core`):

```cpp
// A unit of work owned by whoever scheduled it (a cell subscription, an
// inbox). Intrusive: scheduling never allocates. `flags` and `next` belong
// to the dispatcher holding it and are guarded by its ready-list lock.
struct DispatchSlot {
    static constexpr uint8_t kQueued = 1, kRunning = 2, kCancelled = 4;
    void (*run)(DispatchSlot&) = nullptr;
    // Called by the dispatcher after a run during which cancel() returned
    // false, once it no longer touches the slot; the owner frees it here.
    void (*finish)(DispatchSlot&) = nullptr;
    DispatchSlot* next = nullptr;
    uint8_t flags = 0;
};

class Dispatcher {
public:
    virtual ~Dispatcher() = default;
    // Any thread. Queues `slot` to run once on this dispatcher's thread;
    // a slot already queued stays queued once (coalescing). A slot that is
    // running is queued again and runs once more after the current run.
    // Never blocks on a run, never allocates, never fails.
    virtual void schedule(DispatchSlot& slot) = 0;
    // Unqueues `slot`. Returns true when the owner may free it now: off
    // this dispatcher's thread, after waiting for a run of `slot` in
    // progress (only that one) to return. Returns false only when called
    // on this dispatcher's thread from inside `slot`'s own run; the
    // dispatcher then calls slot.finish after the run. Never waits after
    // the dispatcher has stopped. Must not be called while holding a lock
    // the slot's own run takes.
    [[nodiscard]] virtual bool cancel(DispatchSlot& slot) = 0;
    virtual bool on_dispatch_thread() const = 0;

    // Owners of a slot on this dispatcher attach once when they create it
    // and detach once when they free it; a dispatcher destroyed with
    // attached() != 0 reports misuse.
    void attach();
    void detach();
    uint32_t attached() const;

protected:
    // For implementations' destructors: misuse("<who> destroyed with N attached slots").
    void check_detached(const char* who) const;

private:
    std::atomic<uint32_t> attached_{0};
};

// The ready list EventBus, UiRuntime and the tests' ManualDispatcher share:
// a FIFO of slots under one leaf lock, at most one slot running at a time.
class SlotQueue {
public:
    bool push(DispatchSlot& slot);               // true when the list was empty before
    DispatchSlot* take();                         // pops the head, marks it running; nullptr when empty
    void done(DispatchSlot& slot);                // ends take()'s run; calls finish when cancelled inside it
    bool cancel(DispatchSlot& slot, bool on_dispatch_thread);  // Dispatcher::cancel()'s rule; off the thread polls every 1 ms
    size_t pending() const;                       // queued slots plus the one running
private:
    mutable os::Mutex mu_;
    DispatchSlot* head_ = nullptr;
    DispatchSlot* tail_ = nullptr;
    DispatchSlot* running_ = nullptr;
    size_t queued_ = 0;
};

namespace detail {
// Host builds: prints "wisp: misuse: <what>" to stderr and aborts.
// ESP_PLATFORM: WLOG_E("core", "misuse: %s", what) and returns.
void misuse(const char* what);
}  // namespace detail
```

- Produces in `EventBus`: `class EventBus : public Dispatcher` with `schedule`, `cancel`, `on_dispatch_thread()` (`tls_dispatching == this`), plus:
  - `void note_cell_subscription(int delta);` and `uint32_t cell_subscriptions() const;` (an atomic; `StateCell` is the only caller of the first; `mem` reads the second).
  - Member `SlotQueue slots_;`.
  - Dispatch loop: on every wake (`ready_.take`), repeat { if stopping or another generation: give the token back and return; run at most one slot (`take()` → `run` → `done()`); dispatch at most one event (`queue_.pop(e, 0)` → `dispatch(e)` → `inflight_ -= 1`) } until a step does neither. `ready_` tokens become hints: `schedule()` gives one when `push()` returns true.
  - `wait_idle()` polls `inflight_ == 0 && slots_.pending() == 0`.
  - `~EventBus()` calls `stop()` then `check_detached("EventBus")`.
  - `schedule()` after `stop()` queues the slot; it runs after the next `start()` (as events do).
- Produces `tests/support/manual_dispatcher.hpp`: `class ManualDispatcher : public core::Dispatcher` over a `SlotQueue`; `size_t run_all()` runs queued slots on the calling thread until none is queued (with `on_dispatch_thread()` true only inside it) and returns how many ran; `size_t queued() const`; the destructor calls `check_detached("ManualDispatcher")`.

- [ ] **Step 1: Write the failing tests.**

```cpp
// dispatcher_test.cpp -- slots record their runs in a std::vector<int>.
TEST(SlotQueue, PushTwiceQueuesOnce)
// push(a) == true, push(a) == false, pending() == 1; take() == &a; take() == nullptr.
TEST(SlotQueue, RunsInFifoOrder)
// push a, b, c -> take() yields a, b, c (done() after each).
TEST(SlotQueue, PushWhileRunningQueuesItAgain)
// take() == &a (running); push(a) == true; pending() == 2; done(a); take() == &a.
TEST(SlotQueue, CancelUnlinksAQueuedSlot)
// push a, b; cancel(a, false) == true; take() == &b; pending() == 1.
TEST(SlotQueue, CancelInsideOwnRunDefersToFinish)
// take() == &a; push(a) (queued again); cancel(a, true) == false; a.finish not called yet;
// done(a) -> a.finish called exactly once; pending() == 0; take() == nullptr.
TEST(SlotQueue, CancelFromAnotherThreadWaitsForTheRun)
// thread R: take() == &a, then blocks on gate G inside "the run"; test thread starts thread C:
// cancel(a, false) then sets `returned`. After 20 ms `returned` is false; R: done(a), opens nothing else;
// wait_until(returned, 1000); the result was true.
TEST(ManualDispatcher, RunsOnTheCallingThreadAndSaysSo)
// a slot whose run records on_dispatch_thread(): true; called outside run_all(): false.
TEST(Dispatcher, DestroyedWithAnAttachedSlotAborts)
// EXPECT_DEATH({ ManualDispatcher d; d.attach(); }, "ManualDispatcher destroyed with 1 attached slot");

// event_bus_test.cpp
TEST(EventBus, ScheduledSlotRunsOnTheDispatcher)
// started bus; schedule(a); wait_idle(1000) == true; a ran once, on_dispatch_thread() inside it was
// true, and its thread id differs from the test's.
TEST(EventBus, SlotsAndEventsBothDrain)
// a handler blocks on gate G for the first event "first"; while it blocks: publish 3 events "e",
// schedule slots a and b. Open G. wait_idle(1000) == true; 4 events and both slots ran once each.
TEST(EventBus, WaitIdleWaitsForQueuedAndRunningSlots)
// slot a blocks on gate G; schedule(a); wait_until(a entered); wait_idle(50) == false;
// open G; wait_idle(1000) == true.
TEST(EventBus, SaturatedTokensStrandNothing)
// EventBus bus(4) (ready_ saturates at 5 tokens). A handler blocks on gate G for "first".
// While it blocks: publish 20 events "n" with payload i = 0..19 and schedule slots a, b, c.
// Open G. wait_idle(1000) == true; the delivered "n" events are i = 16..19 in order (the rest
// dropped; dropped() == 16); a, b, c each ran exactly once.
TEST(EventBus, CancelOffTheDispatcherWaitsOnlyForThatSlot)
// slot a blocks on gate G and is running; slot b is queued behind it. cancel(b) from the test
// thread returns true while G is still closed (checked by `returned` before opening G).
// Open G; wait_idle(1000); b never ran.
TEST(EventBus, NoSlotRunsWhileStopped)
// start, stop; schedule(a); after 20 ms a has not run; start(); wait_idle(1000); a ran once.
TEST(EventBus, CountsCellSubscriptions)
// note_cell_subscription(+1) twice, (-1) once -> cell_subscriptions() == 1.
TEST(EventBus, DestroyedWithAnAttachedSlotAborts)
// EXPECT_DEATH({ EventBus bus; bus.attach(); }, "EventBus destroyed with 1 attached slot");
```

Every existing `EventBus` test stays unchanged and green; `StopFromHandlerThenStartRepeatedlyNeverStrandsAnEvent` and `OverflowDropsOldestAndCounts` are the ones the new loop must keep.

- [ ] **Step 2: Run** `ctest --test-dir build/sim -R 'SlotQueue|ManualDispatcher|Dispatcher|EventBus' --output-on-failure`. Expected: build fails (no `dispatcher.hpp`).
- [ ] **Step 3: Implement.** Update `event_bus.hpp`'s file header (what `wisp.bus` runs now, the token-as-hint rule) and the comment at the token give-back in `run_dispatcher()` (it now protects the whole drain, not one event). `misuse()` messages use the exact texts the death tests match.
- [ ] **Step 4: Run** the same filter plus `-R 'WakeLock|ButtonService|PowerMonitor|Reminders|WsHub'` (heavy bus users). Expected: PASS.
- [ ] **Step 5: Commit** `feat(core): let EventBus run dispatch slots`.

---

### Task 3: `UiRuntime` as a dispatcher

**Files:**
- Modify: `components/wisp_ui/include/wisp/ui/ui_runtime.hpp` (base class, three overrides, class comment), `components/wisp_ui/src/ui_runtime.cpp` (`Impl::slots`, `ui_main()`, `stop()`, destructor)
- Test: `tests/ui/ui_runtime_test.cpp`

**Interfaces:**
- Consumes: `core::Dispatcher`, `core::SlotQueue`, `core::DispatchSlot` (Task 2).
- Produces: `class UiRuntime : public core::Dispatcher` with
  - `void schedule(core::DispatchSlot&) override;` — `push()` into `Impl::slots`; when it returns true and `!on_ui_thread()`, also `post_queue.push([] {}, 0)` (the wake; a failed push means the queue is non-empty, which wakes the task too).
  - `bool cancel(core::DispatchSlot&) override;` — `slots.cancel(slot, on_ui_thread())`; after `stop()` no run is in progress, so it never waits.
  - `bool on_dispatch_thread() const override { return on_ui_thread(); }`
  - Loop rule: `run_slots()` (take/run/done until none is queued) runs in every iteration of `ui_main()`: in the paused branch before the blocking `pop` and again after that `pop`'s job; in the active branch right after `drain_post_queue()` and before the paused re-check and the frame hook (so slots scheduled by posted jobs run in the same iteration, at most about 10 ms after `schedule()` from another thread). The final drain after the loop exits runs posted jobs only, not slots (after `stop()` no slot runs).
  - `~UiRuntime()`: `stop()`, then `check_detached("UiRuntime")`.

- [ ] **Step 1: Write the failing tests** (`FakeDisplay` 480×480 as the file's other tests):

```cpp
TEST(UiRuntime, ScheduledSlotRunsOnTheUiThread)
// started; schedule(a) from the test thread; wait_until(a ran, 1000); inside a, on_ui_thread() and
// on_dispatch_thread() were true.
TEST(UiRuntime, SlotsRunInFifoOrder)
// schedule a, b, c from a posted job (so they queue before the next iteration); they run a, b, c.
TEST(UiRuntime, SlotScheduledWhilePausedRunsWithoutAPost)
// set_paused(true); run_blocking(no-op) (the task has reached the paused branch);
// schedule(a) from the test thread, posting nothing else; wait_until(a ran, 1000).
TEST(UiRuntime, SlotRunsWhenThePostQueueIsFull)
// set_paused(true); post a job that blocks on gate G; wait until it runs; post no-ops until
// post() returns false (the queue is full); schedule(a); open G; wait_until(a ran, 2000).
TEST(UiRuntime, CancelInsideItsOwnRunDefersTheFinish)
// a's run calls cancel(a) and records the result: false; a.finish runs once, after a returned.
TEST(UiRuntime, CancelFromAnotherThreadWaitsForTheRun)
// a blocks on gate G on the UI task; cancel(a) from a second thread has not returned after
// 20 ms; open G; it returns true.
TEST(UiRuntime, NoSlotRunsAfterStopAndCancelDoesNotWait)
// stop(); schedule(a); cancel(a) returns true at once; a never ran; the UiRuntime is destroyed
// without misuse (a detached first).
```

- [ ] **Step 2: Run** `ctest --test-dir build/sim -R UiRuntime --output-on-failure`. Expected: build fails (no override).
- [ ] **Step 3: Implement.** Update the class comment and `set_paused()`'s comment (the paused branch also runs slots).
- [ ] **Step 4: Run** `ctest --test-dir build/sim -R 'UiRuntime|SystemLayer|HomeTest|ShellTalk|PowerManager' --output-on-failure`. Expected: PASS (the loop change must not move posted work or pausing).
- [ ] **Step 5: Commit** `feat(ui): let UiRuntime run dispatch slots`.

---

### Task 4: `core::StateCell<T>` and `CellSubscription`

**Files:**
- Create: `components/wisp_core/include/wisp/core/state_cell.hpp`, `components/wisp_core/src/state_cell.cpp` (the non-template core; add to `SRCS`)
- Create: `tests/core/state_cell_test.cpp`; Modify: `tests/CMakeLists.txt` (`wisp_test(NAME state_cell_test SRCS core/state_cell_test.cpp LIBS wisp_core wisp_port_sim_os TIMEOUT 30)`)
- Modify: `AGENTS.md` section 3 (second commit of this task)

**Interfaces:**
- Consumes: `Dispatcher`, `SlotQueue`, `detail::misuse`, `EventBus::note_cell_subscription` (Task 2).
- Produces in `state_cell.hpp` (namespace `wisp::core`):

```cpp
namespace detail {
class CellCore;  // non-template: os::Mutex, std::atomic<uint32_t> revision, the
                 // intrusive list of subscriptions, EventBus&, the host-only
                 // re-entry check
struct CellSub;  // DispatchSlot + list links + CellCore* + Dispatcher* +
                 // uint32_t delivered + bool handle_released
}  // namespace detail

// Move-only. reset() or destruction unsubscribes: the subscription leaves
// the cell's list (no set() schedules it again), then its slot is
// cancelled. Off the dispatcher's thread that waits for this
// subscription's callback in progress (only this one); inside its own
// callback it returns at once, no further call happens, and the memory
// goes when the run returns (DispatchSlot::finish).
class CellSubscription {
public:
    CellSubscription() = default;
    CellSubscription(CellSubscription&&) noexcept;
    CellSubscription& operator=(CellSubscription&&) noexcept;  // resets the old one first
    ~CellSubscription();
    void reset();
    explicit operator bool() const;
};

template <class T>
class StateCell {
    static_assert(std::is_copy_constructible_v<T>);
    static_assert(sizeof(T) <= 256, "a callback copies T onto the dispatcher's stack");
public:
    using Callback = os::LargeFunction<void(const T&)>;
    using Mirror = void (*)(EventBus& bus, const T& before, const T& after);

    StateCell(EventBus& bus, T initial, Mirror mirror = nullptr);
    ~StateCell();  // live subscriptions: misuse("StateCell destroyed with N live subscriptions")

    T get() const;
    template <class F> auto read(F&& f) const -> std::invoke_result_t<F&, const T&>;  // under the lock: short, no calls out
    uint32_t revision() const;  // atomic load; starts at 1

    [[nodiscard]] CellSubscription subscribe(Callback cb, Dispatcher* on = nullptr) const;

    bool set(const T& v);                       // false, and nothing happens, when v == current
    template <class F> bool update(F&& mutate); // mutate(T&) on a copy under the lock, then as set()
    bool seed(const T& v);                      // as set() without the mirror
};
```

Rules the implementation must keep (each pinned by a test below):
- `T` needs `operator==`. `set()`/`update()`/`seed()` that change the value store it, increment the revision, call the mirror (`set`/`update` only) with the old and new value, then `schedule()` every subscription on its dispatcher, all under the cell's lock, in that order. No allocation.
- `subscribe()` allocates the subscription (`os::make_large`; abort on exhaustion like `os::LargeAllocator`), and under the cell's lock links it, calls `on->attach()` (`on` defaults to the cell's `EventBus`), `bus.note_cell_subscription(+1)` and `schedule()`s it, so the first run delivers the current value (`delivered` starts at 0, the revision at 1).
- A subscription's run: under the cell's lock, if `revision == delivered` return; else copy the value and the revision; unlock; set `delivered`; call the callback with the copy. Nothing after the callback touches the cell or the subscription, except through `DispatchSlot::finish`.
- `reset()`: under the cell's lock unlink; then `cancel()`; `true` → `detach()`, `note_cell_subscription(-1)`, free; `false` (inside its own callback) → `detach()`, `note_cell_subscription(-1)`, set `handle_released`, and `finish` frees after the run.
- `~StateCell()` with live subscriptions: `misuse(...)`; on the device (where `misuse` returns) it then orphans each: unlinks it, cancels it (a `false` there, the cell destroyed from a callback of its own, calls `std::abort()`: there is no safe way on), sets its `CellCore*` and `Dispatcher*` to null, detaches it and counts it down, so a later `reset()` only frees.
- Host builds only: a `thread_local const detail::CellCore*` names the cell whose lock this thread holds (saved and restored around each hold); taking a cell's lock while it names that same cell calls `misuse("StateCell re-entered from inside its own read(), update() or mirror")`.

- [ ] **Step 1: Write the failing tests** (`T = int` unless stated; `ManualDispatcher md` from Task 2; an `EventBus bus` started in the fixture; `Seen` records `(value, std::this_thread::get_id())` per call):

```cpp
TEST(StateCell, StartsAtTheInitialValueAndRevisionOne)
// StateCell<int> c(bus, 7): get() == 7, revision() == 1.
TEST(StateCell, SetChangesTheValueAndTheRevision)
// set(8) == true; get() == 8; revision() == 2.
TEST(StateCell, EqualSetIsANoOp)
// subscribe on md; run_all() (initial call); set(7) == false; revision() == 1; run_all() == 0;
// mirror count 0.
TEST(StateCell, UpdateIsReadModifyWrite)
// update([](int& v) { v += 5; }) == true -> 12; update([](int&) {}) == false.
TEST(StateCell, SeedMovesTheValueWithoutTheMirror)
// mirror counts calls: seed(9) == true, mirror count 0, revision() == 2, a subscriber on md gets 9.
TEST(StateCell, LateSubscriberGetsTheCurrentValueOnce)
// set 1..5, then subscribe on md: run_all() == 1 with value 5; run_all() == 0.
TEST(StateCell, CallbackNeverRunsInsideSet)
// subscribe on md, run_all(); set(1) from the test thread: no call until run_all(), then one call.
TEST(StateCell, IntermediateValuesAreSkipped)
// subscribe on md, run_all(); set 1..10; run_all(): exactly one call, value 10.
TEST(StateCell, SlowSubscriberGetsExactlyOneMoreCallWithTheLastValue)
// subscribe on bus; the first call (initial 0) blocks on gate G. Another thread sets 1..1000 and
// joins. Open G. bus.wait_idle(2000) == true. Calls == 2; values seen == {0, 1000}; the two calls
// never overlapped (an atomic in-callback counter never exceeded 1).
TEST(StateCell, TwoDispatchersAreIndependent)
// sub A on bus, sub B on md (md never stepped yet); set(3). bus.wait_idle(1000) == true and A's
// last value is 3, while B has not been called at all (md stalled does not delay the bus);
// run_all(): B called exactly once, with 3 (its initial 0 was coalesced away).
TEST(StateCell, UnsubscribeInsideItsOwnCallbackStopsFurtherCalls)
// on md: the callback calls c.set(v + 1) (queues itself again) then sub.reset() on its own handle.
// run_all() == 1; run_all() == 0; calls == 1; md.attached() == 0; bus.cell_subscriptions() == 0.
TEST(StateCell, SetSubscribeAndUnsubscribeOthersFromACallback)
// A's callback: set(42) on cell c2, subscribe a new sub to c2 on md (kept), reset sub B of c.
// run_all() until 0: c2 == 42, the new sub got 42, B was not called after its reset.
TEST(StateCell, UnsubscribeFromAnotherThreadWaitsOnlyForItsOwnCallback)
// on bus: sub A's callback blocks on gate G (running); sub B is queued behind it. B.reset() on the
// test thread returns while G is closed. A.reset() on thread T has not returned after 20 ms;
// open G; T's reset returns; A was called once.
TEST(StateCell, SetWhileHoldingTheCallbacksLockDoesNotDeadlock)
// mutex M; sub on bus whose callback locks M. Test thread: lock M, set(1), unlock M.
// bus.wait_idle(1000) == true and the callback saw 1.
TEST(StateCell, CountsLiveSubscriptionsOnItsBusAndAttachesToItsDispatcher)
// two subs (bus, md): bus.cell_subscriptions() == 2, md.attached() == 1, bus.attached() == 1;
// reset both: 0, 0, 0.
TEST(StateCell, DestroyedWithALiveSubscriptionAborts)
// EXPECT_DEATH({ ManualDispatcher d; CellSubscription s; { StateCell<int> c(bus, 0);
//   s = c.subscribe([](const int&) {}, &d); } }, "StateCell destroyed with 1 live subscription");
TEST(StateCell, GetInsideItsOwnReadAborts)
// EXPECT_DEATH(c.read([&](const int&) { return c.get(); }),
//              "StateCell re-entered from inside its own read\\(\\)");
TEST(StateCell, WorksWithAStructOfFixedStrings)
// T = struct { wisp::FixedString<32> ssid; int rssi; bool operator==(const T&) const = default; }:
// set/get round-trip; equal set is a no-op.
```

- [ ] **Step 2: Run** `ctest --test-dir build/sim -R StateCell --output-on-failure`. Expected: build fails (no header).
- [ ] **Step 3: Implement.** The header comment states the delivery rule ("a callback means: changed since your last call"; A→B→A delivers A again), the lock rules of Global Constraints, the reentrancy list of spec 3.3 and "callbacks must not touch flash".
- [ ] **Step 4: Run** `ctest --test-dir build/sim -R 'StateCell|EventBus|SlotQueue' --output-on-failure`. Expected: PASS.
- [ ] **Step 5: Commit** `feat(core): add StateCell and CellSubscription`.
- [ ] **Step 6: AGENTS.md rule.** Add to section 3, after "Single UI thread":

> - **State and events**: state that other components follow lives in a `core::StateCell` owned by the component it describes; others read it (`get()`/`read()`) and follow it with `subscribe()`. The event bus carries notifications and telemetry, which may be lost. No component builds its own copy of another component's state from bus events. A consumer that needs every occurrence (an input edge, a lifecycle edge) gets a `core::EdgeInbox`, never a cell. Callbacks run on the dispatcher they were given (`wisp.bus` by default, the `UiRuntime` for UI code), keep to memory operations and never touch flash.

  Commit `docs(agents): state other components follow lives in a StateCell`.

---

### Task 5: Mirrors, concurrent setters and the saturated bus

**Files:**
- Create: `tests/support/bus_flood.hpp`
- Modify: `tests/core/state_cell_test.cpp`
- Modify (only if a test fails): `components/wisp_core/include/wisp/core/state_cell.hpp`, `components/wisp_core/src/state_cell.cpp`

**Interfaces:**
- Consumes: Tasks 2 and 4.
- Produces `tests/support/bus_flood.hpp`:

```cpp
namespace wisp::test {
// Holds a started bus's dispatcher inside a handler on "test.flood.hold"
// until release(), so whatever is published meanwhile overflows the queue
// (drop-oldest) exactly as a stalled wisp.bus would. Deterministic: hold()
// returns once the dispatcher is inside the handler.
class BusFlood {
public:
    explicit BusFlood(core::EventBus& bus);   // subscribes
    ~BusFlood();                              // release(), then unsubscribes
    void hold();                              // publishes the hold event, waits until it is being handled
    void flood(size_t events);                // publishes `events` "test.flood" events
    void release();                           // lets the handler return
};
}
```

- [ ] **Step 1: Write the tests** (they may pass at once if Task 4 is right; each must be seen to fail against a deliberately broken build once, see Step 2):

```cpp
TEST(StateCell, MirrorPublishesEachChangeWithBeforeAndAfter)
// mirror publishes {"before","after"} on "test.cell"; set 1, 2, 2, 3 -> wait_idle -> recorded
// events (0,1), (1,2), (2,3); seed(9) adds nothing.
TEST(StateCell, MirrorEventsFollowRevisionOrderUnderConcurrentSetters)
// EventBus big(2048); 4 threads x 250 update([](int& v) { ++v; }); join; big.wait_idle(5000);
// 1000 "test.cell" events whose "after" values are exactly 1..1000 in order; dropped() == 0.
TEST(StateCell, SubscribersConvergeOnASaturatedBus)
// EventBus bus(64); BusFlood f(bus); f.hold(); f.flood(10000) while another thread sets 1..100;
// f.release(); bus.wait_idle(5000) == true; the bus subscriber's last value == 100; an md
// subscriber after run_all() saw 100; bus.dropped() > 0.
TEST(StateCell, WaitIdleCoversTheLastSlot)
// sub on bus records its last value; set(5); bus.wait_idle(1000) == true; the recorded value is 5
// at the moment wait_idle returned (read without further waiting).
```

- [ ] **Step 2: Run** `ctest --test-dir build/sim -R StateCell --output-on-failure`. Expected: PASS. To prove the tests can fail, temporarily move the mirror call after the lock is released in `set()` and run `MirrorEventsFollowRevisionOrderUnderConcurrentSetters` (expect out-of-order failures within a few runs: `--repeat until-fail:20`), then restore. Record the observation in the commit body.
- [ ] **Step 3: Commit** `test(core): pin cell mirrors and convergence on a saturated bus`.

---

### Task 6: `core::EdgeInbox<E, N>`

**Files:**
- Create: `components/wisp_core/include/wisp/core/edge_inbox.hpp` (template), `components/wisp_core/src/edge_inbox.cpp` (non-template base: lock, slot, counters, throttled log; add to `SRCS`)
- Create: `tests/core/edge_inbox_test.cpp`; Modify: `tests/CMakeLists.txt` (`wisp_test(NAME edge_inbox_test SRCS core/edge_inbox_test.cpp LIBS wisp_core wisp_port_sim_os TIMEOUT 20)`)

**Interfaces:**
- Consumes: `Dispatcher` (Task 2).
- Produces:

```cpp
namespace wisp::core {
// Every edge, in order, on one consumer's dispatcher (spec 3.5). push() never blocks and
// never allocates; when N edges are already waiting it drops the oldest, counts it and
// warns (the first overflow, then every 64th).
template <class E, size_t N>
class EdgeInbox {
    static_assert(std::is_trivially_copyable_v<E>);
    static_assert(N > 0);
public:
    using Handler = os::LargeFunction<void(const E&)>;
    // `name` (a string literal) is the log's and mem's label. The ring of N edges is
    // allocated in Mem::Large here (aborts when PSRAM is exhausted). Attaches to `on`.
    EdgeInbox(const char* name, Dispatcher& on, Handler handler);
    // Cancels the slot (waits for a drain in progress off the dispatcher's thread),
    // detaches. Destroying it from inside its own handler aborts on every build.
    ~EdgeInbox();
    void push(const E& e);       // any thread
    uint32_t overflows() const;  // atomic
    const char* name() const;
};
}
```

Drain rule: the slot's run takes one edge under the inbox's lock, releases it, calls the handler, and repeats until the ring is empty, so edges pushed during the handler are handled in the same run.

- [ ] **Step 1: Write the failing tests** (`struct Edge { int v; };`, `ManualDispatcher md`):

```cpp
TEST(EdgeInbox, DeliversEveryEdgeInOrder)
// N = 8; push 1, 2, 3; md.run_all(); handler saw 1, 2, 3; overflows() == 0.
TEST(EdgeInbox, HandlerRunsOnlyOnItsDispatcher)
// push from the test thread: nothing until run_all(); inside, on_dispatch_thread() == true.
TEST(EdgeInbox, OverflowDropsTheOldestAndCountsIt)
// N = 4; push 1..6; overflows() == 2; run_all(): 3, 4, 5, 6.
TEST(EdgeInbox, OverflowWarningIsThrottled)
// CaptureSink as in event_bus_test.cpp; N = 4; push 134 edges without draining:
// overflows() == 130; exactly 3 warnings containing "edge inbox test overflowed" (the 1st, 64th, 128th).
TEST(EdgeInbox, ConcurrentProducersKeepTheirOwnOrder)
// N = 256 on a started EventBus; 4 threads push {thread * 1000 + i} for i < 50; bus.wait_idle(2000);
// 200 edges; for each thread the i values arrive increasing.
TEST(EdgeInbox, EdgesPushedByTheHandlerAreDrainedInTheSameRun)
// the handler pushes v + 10 when v < 10; push 1; run_all() == 1 run; handler saw 1, 11.
TEST(EdgeInbox, DestroyedWithQueuedEdgesRunsNothingAfterwards)
// push 1, 2; destroy the inbox; md.run_all() == 0; md.attached() == 0.
TEST(EdgeInbox, DestroyedFromItsOwnHandlerAborts)
// EXPECT_DEATH (the handler resets the unique pointer holding its inbox), "EdgeInbox destroyed from its own handler".
```

- [ ] **Step 2: Run** `ctest --test-dir build/sim -R EdgeInbox --output-on-failure`. Expected: build fails.
- [ ] **Step 3: Implement.** Warning text: `"edge inbox %s overflowed (%u edges dropped)"`.
- [ ] **Step 4: Run** the same filter plus `-R 'StateCell|SlotQueue'`. Expected: PASS.
- [ ] **Step 5: Commit** `feat(core): add EdgeInbox for edges that must not be lost`.

---

### Task 7: The topic registry

**Files:**
- Create: `components/wisp_core/include/wisp/core/topics.hpp`
- Modify: `components/wisp_admin/include/wisp/admin/ws_hub.hpp`, `components/wisp_admin/src/ws_hub.cpp` (a static `forwards()` over the existing `kForwardedPrefixes`; `on_event`'s frame built by a shared function)
- Create: `tests/core/topics_test.cpp`; Modify: `tests/CMakeLists.txt` (`wisp_test(NAME topics_test SRCS core/topics_test.cpp LIBS wisp_core)`), `tests/admin/ws_hub_test.cpp`

**Interfaces:**
- Produces `topics.hpp` (namespace `wisp::core::topics`):

```cpp
enum class Kind : uint8_t { CellMirror, Notification, Telemetry, InputEdge };
struct Topic { std::string_view name; Kind kind; bool forwarded; };

inline constexpr char kButtonPrefix[] = "button.";          // + <device id> + "." + edge
inline constexpr char kButtonDown[] = "down", kButtonUp[] = "up",
                      kButtonShort[] = "short", kButtonLong[] = "long";
inline constexpr char kPowerState[] = "power.state";
inline constexpr char kPowerWakelock[] = "power.wakelock";
inline constexpr char kPowerChargingChanged[] = "power.charging.changed";
inline constexpr char kPowerBatteryLow[] = "power.battery.low";
inline constexpr char kNotificationPosted[] = "notification.posted";
inline constexpr char kNotificationDismissed[] = "notification.dismissed";
inline constexpr char kStorageSdInserted[] = "storage.sd.inserted";
inline constexpr char kStorageSdRemoved[] = "storage.sd.removed";
inline constexpr char kSystemBootCompleted[] = "system.boot.completed";
inline constexpr char kSystemFactoryReset[] = "system.factory_reset";
inline constexpr char kSystemFirmware[] = "system.firmware";
inline constexpr char kSystemSettings[] = "system.settings";
inline constexpr char kSystemTime[] = "system.time";
inline constexpr char kNetWifiEnabled[] = "net.wifi.enabled";
inline constexpr char kNetWifiState[] = "net.wifi.state";
inline constexpr char kNetApState[] = "net.ap.state";
inline constexpr char kNetPairingCode[] = "net.pairing.code";
inline constexpr char kNetPairingFailed[] = "net.pairing.failed";
inline constexpr char kSceneState[] = "scene.state";
inline constexpr char kModelProfiles[] = "model.profiles";
inline constexpr char kAgentState[] = "agent.state";
inline constexpr char kVoiceState[] = "voice.state";
inline constexpr char kMemoryChanged[] = "memory.changed";
inline constexpr char kTodoChanged[] = "todo.changed";

inline constexpr Topic kAll[] = {
    {"button.<id>.down", Kind::InputEdge, false}, {"button.<id>.up", Kind::InputEdge, false},
    {"button.<id>.short", Kind::InputEdge, false}, {"button.<id>.long", Kind::InputEdge, false},
    {kPowerState, Kind::CellMirror, true},          {kPowerWakelock, Kind::Telemetry, true},
    {kPowerChargingChanged, Kind::CellMirror, true}, {kPowerBatteryLow, Kind::CellMirror, true},
    {kNotificationPosted, Kind::Notification, false}, {kNotificationDismissed, Kind::Notification, false},
    {kStorageSdInserted, Kind::CellMirror, true},   {kStorageSdRemoved, Kind::CellMirror, true},
    {kSystemBootCompleted, Kind::Notification, true}, {kSystemFactoryReset, Kind::CellMirror, true},
    {kSystemFirmware, Kind::CellMirror, true},      {kSystemSettings, Kind::CellMirror, true},
    {kSystemTime, Kind::CellMirror, true},          {kNetWifiEnabled, Kind::CellMirror, true},
    {kNetWifiState, Kind::CellMirror, true},        {kNetApState, Kind::CellMirror, true},
    {kNetPairingCode, Kind::CellMirror, true},      {kNetPairingFailed, Kind::Notification, true},
    {kSceneState, Kind::Notification, true},        {kModelProfiles, Kind::Notification, false},
    {kAgentState, Kind::CellMirror, false},         {kVoiceState, Kind::CellMirror, false},
    {kMemoryChanged, Kind::Notification, true},     {kTodoChanged, Kind::Notification, true},
};
constexpr const Topic* find(std::string_view name);  // nullptr when unregistered
```

  The header comment defines the four kinds as delivery promises (spec 3.1): a `CellMirror` topic is the bus copy of a state cell (follow the cell; the event may be lost); `Notification` and `Telemetry` may be lost and nobody's correctness depends on them; an `InputEdge` topic is the observable copy of edges whose consumers use an `EdgeInbox`. `scene.state` is a `Notification`: no cell mirrors it (spec 3.4: the overview cell plus the lifecycle inbox replace it for consumers). Kinds describe phase D's end state; plans 2-4 make the code match them.
- Produces in `WsHub`: `static bool forwards(std::string_view topic);` (true when a `kForwardedPrefixes` entry is a prefix of `topic`), and `std::string event_frame(const core::Event& e);` (free function in `wisp::admin`, declared in `ws_hub.hpp`: the exact JSON `on_event()` sends, `{"t":"event","topic":...,"payload":...}`), which `on_event()` now calls. Plan 2's golden test records frames through it.

- [ ] **Step 1: Write the failing tests:**

```cpp
// topics_test.cpp
TEST(Topics, NamesAreUnique)
TEST(Topics, EveryConstantIsRegistered)
// find(k) != nullptr for each of the 24 name constants; find("power.stat") == nullptr.
TEST(Topics, TheRegistryHas28Rows)
// std::size(kAll) == 28 (four button patterns + 24 topics).

// ws_hub_test.cpp
TEST(WsHub, ForwardedFlagsMatchTheRegistry)
// for every t in topics::kAll: EXPECT_EQ(t.forwarded, WsHub::forwards(t.name)) << t.name.
TEST(WsHub, ForwardsMatchesWholePrefixes)
// forwards("net.wifi.state") && forwards("todo.changed") && !forwards("voice.state")
// && !forwards("netx.y") && !forwards("button.power.down").
TEST(WsHub, EventFrameIsWhatClientsReceive)
// publish {"system.time", payload {"state":"synced"}} to a hub with an events client; the frame the
// FakeWsConnection received equals event_frame() of that event.
```

- [ ] **Step 2: Run** `ctest --test-dir build/sim -R 'Topics|WsHub' --output-on-failure`. Expected: build fails.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** the same filter. Expected: PASS.
- [ ] **Step 5: Commit** `feat(core): add the event topic registry`.

---

### Task 8: `tools/check_topics.py`

**Files:**
- Create: `tools/check_topics.py`, `tools/check_topics_test.py`
- Modify: `tools/ci_tool_tests.py` (`"check_topics_test": ()` in `TESTS`)

**Interfaces:**
- Consumes: `topics.hpp`'s format (Task 7): `inline constexpr char kName[] = "...";` (several per declaration allowed) and `kAll`'s `{kName or "literal", Kind::X, bool}` entries.
- Produces: `check(root: pathlib.Path) -> list[str]` returning `"path:line: message"` lines, and `main()` (exit 1 with the lines on stdout when any, else exit 0 silently), with these rules:
  1. The names in `kAll` (constants resolved) equal the first-column names of the table under `## Event topics` in `docs/architecture.md`. A cell is split on ` / `; each piece is one backticked name; a piece starting with `.` replaces the last dotted segment of the previous name (`` `button.<id>.down` / `.up` `` gives `button.<id>.up`). Messages: `docs/architecture.md: topic "x" is registered but has no row` and `docs/architecture.md:<line>: topic "x" has a row but is not registered`.
  2. In `components/**/*.{cpp,hpp}` and `simulator/**/*.{cpp,hpp}`, except `components/wisp_core/include/wisp/core/topics.hpp`, no ordinary string literal (comments and raw strings skipped) equals a registered topic name without `<`. Message: `<path>:<line>: topic literal "x"; use wisp::core::topics::kX`.
  
  `--root` defaults to the repository root.
- [ ] **Step 1: Write the failing self-test** `tools/check_topics_test.py` (scratch trees under `tempfile.TemporaryDirectory()`, as `check_layering_test.py` builds them):
  - `test_matching_registry_and_table_pass`
  - `test_registered_topic_without_a_row_fails` (message names the topic)
  - `test_row_without_a_registered_topic_fails`
  - `test_literal_topic_in_a_publish_call_fails` (`bus.publish({"power.state", ...})` in `components/x/src/a.cpp`; message has the path, line and `kPowerState`)
  - `test_literal_in_a_comment_or_raw_string_is_ignored`
  - `test_suffix_pieces_expand` (`` `button.<id>.down` / `.up` `` and `` `storage.sd.inserted` / `storage.sd.removed` ``)
  - `test_prefix_literals_are_allowed` (`subscribe("net.", ...)` passes)
- [ ] **Step 2: Run** `python3 tools/check_topics_test.py`. Expected: FAIL (no module).
- [ ] **Step 3: Implement** `tools/check_topics.py` (stdlib only; docstring states the two rules and the spec section).
- [ ] **Step 4: Run** `python3 tools/check_topics_test.py && python3 tools/ci_tool_tests_test.py`. Expected: PASS. `python3 tools/check_topics.py` on the real tree still reports the literals Task 9 replaces; that is expected here.
- [ ] **Step 5: Commit** `feat(tools): check the topic registry against the docs and the code`.

---

### Task 9: Every call site names its topic; CI runs the check

**Files:**
- Modify (literal → `topics::k...`, including topic comparisons such as `e.topic == "agent.state"`; behaviour unchanged): `components/wisp_app/src/{app.cpp, app_impl.hpp, system_duties.cpp, scene_wiring.cpp, factory_reset.cpp, voice_wiring.cpp, agent_wiring.cpp, power_manager.cpp, self_check_screen.cpp, status_shell.cpp, net_wiring.cpp}`, `components/wisp_core/src/{wakelock.cpp, power_monitor.cpp, reminders.cpp, model_profiles.cpp, notification_service.cpp, memory_store.cpp, storage_manager.cpp}`, `components/wisp_core/include/wisp/core/reminders.hpp` (`kTodoChangedTopic` becomes `= topics::kTodoChanged`, kept for its existing users), `components/wisp_net/src/{pairing_service.cpp, time_sync.cpp, wifi_service.cpp}`, `components/wisp_scene/src/scene_runtime.cpp`, `components/wisp_shell/src/{home.cpp, conversation.cpp, shell.cpp}`, `components/wisp_agent/src/agent_service.cpp`, `components/wisp_voice/src/voice_service.cpp`, `simulator/main.cpp`. `python3 tools/check_topics.py` is the list: whatever it reports is in scope, nothing else.
- Modify: `tools/ci.sh` (after the `check_layering.py` block: `run_tool_test check_topics_test`, then `echo "==> tools/check_topics.py"; python3 tools/check_topics.py`)
- Modify: `tools/check_topics_test.py` (one more test)

**Interfaces:**
- Consumes: Tasks 7 and 8. Produces nothing new.

- [ ] **Step 1: Write the failing test:** `test_repository_is_clean` in `tools/check_topics_test.py` (`check(repo_root) == []`).
- [ ] **Step 2: Run** `python3 tools/check_topics_test.py`. Expected: that test FAILS, listing the literals.
- [ ] **Step 3: Replace** each reported literal with its constant (add `#include "wisp/core/topics.hpp"`). Prefix literals (`"button."`, `"net."`, `"net.wifi."`, `"notification."`, the WsHub prefixes) and button topics built from a device id (`ButtonService::publish_event()`, the `"button.power.short"` comparisons in `system_duties.cpp`) stay as they are: the check does not flag them, and plan 4 replaces the button consumers.
- [ ] **Step 4: Run** `python3 tools/check_topics_test.py && python3 tools/check_topics.py && cmake --build build/sim && ctest --test-dir build/sim --output-on-failure`. Expected: PASS, no test changed.
- [ ] **Step 5: Commit** `refactor: name every event topic through the registry`.

---

### Task 10: `mem` reports dropped events and live cell subscriptions

**Files:**
- Modify: `components/wisp_app/include/wisp/app/app.hpp`, `components/wisp_app/src/app.cpp` (`App::dispatch_report()`)
- Modify: `firmware/main/main.cpp` (the `mem` command appends the report)
- Test: `tests/app/app_boot_test.cpp`

**Interfaces:**
- Consumes: `EventBus::dropped()`, `EventBus::cell_subscriptions()` (Task 2); `wisp::test::BusFlood` (Task 5).
- Produces: `std::string App::dispatch_report() const;` — one line `bus dropped=<n> cell_subs=<n>` (no trailing newline). Plan 4 appends one line per inbox, `inbox <name> overflows=<n>`. Any thread; reads atomics only.
- `mem` prints `wisp::port::esp::mem_report() + "\n" + app.dispatch_report()`; its help text becomes `"mem                          internal/DMA/PSRAM heap, task stacks, bus drops and cell subscriptions"`.

- [ ] **Step 1: Write the failing test:**

```cpp
TEST(AppBoot, DispatchReportShowsBusDropsAndCellSubscriptions)
// The file's Harness, started. dispatch_report() matches ^bus dropped=0 cell_subs=[0-9]+$.
// BusFlood f(app.bus()); f.hold(); f.flood(200); f.release(); app.bus().wait_idle(2000);
// dispatch_report() starts with "bus dropped=" + std::to_string(app.bus().dropped()) and dropped() > 0.
```

- [ ] **Step 2: Run** `ctest --test-dir build/sim -R AppBoot --output-on-failure`. Expected: build fails.
- [ ] **Step 3: Implement** both files. The firmware is built by the repository's GitHub CI (and by the owner with ESP-IDF); `bash tools/ci.sh` does not build it, so read the change twice: the lambda already receives `wisp::app::App&`.
- [ ] **Step 4: Run** the same filter. Expected: PASS.
- [ ] **Step 5: Commit** `feat(app): report bus drops and cell subscriptions in mem`.

---

### Task 11: Documents and full check

**Files:**
- Modify: `docs/architecture.md`:
  - the `wisp.bus` row of the task table: "Event dispatch, and the callbacks of state cells and edge inboxes that name no other dispatcher: ... callbacks do only memory operations ...";
  - the event-bus paragraph after the topic table: the drain loop (slots and events alternately until both are empty; tokens are hints), `wait_idle()` covering slots, `dropped()` now shown by `mem`;
  - a new subsection "State cells and edge inboxes" under "Event topics": the three paths of spec 3.1 in one short table, the delivery rule, the lock order, which dispatcher runs what (`wisp.bus` by default; the UI runtime's slots at the top of every loop iteration, paused too), the registry (`wisp/core/topics.hpp`, checked by `tools/check_topics.py` against the table above);
  - "Serial console": `mem`'s new line `bus dropped=<n> cell_subs=<n>`.
- Modify: `docs/testing.md`, first paragraph: "layering" becomes "layering, the event topic registry".

- [ ] **Step 1:** Edit the documents. Keep to what exists after this plan (no cell is used by a component yet; say nothing about migration progress).
- [ ] **Step 2:** Run `bash tools/ci.sh`. Expected: exit 0, no warnings in the test output; its simulator smoke tests (boot, network, scenes, an agent conversation) pass unchanged, which is the check that the UI loop and the bus loop changes did not move behaviour.
- [ ] **Step 3: Commit** `docs: describe dispatch slots, state cells and the topic registry`.

**Device checks for the owner (not run by the agent):**
- `mem` at idle on both boards: the new `bus dropped=0 cell_subs=0` line; internal free and largest block unchanged within noise against the last values in `docs/hardware/<board>.md` (this plan adds two `os::Mutex` objects, about 200 bytes of internal RAM); `wisp.bus` and `wisp_ui` stack high-water marks.
- `WISP_PERF` on Home and in a scene: frame rate not below the baseline (the UI loop now checks an empty ready list once per iteration).
