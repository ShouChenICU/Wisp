# LLM Provider Seam and TurnRunner (J1) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The agent talks to the model through `net::LlmProvider` (with `LlmClient` as its only implementation) and runs each turn in a `TurnRunner` separate from `AgentService::Impl`, with no change in behaviour. The one deliberate exception is that the estimate calibration is keyed by profile id and model.

**Architecture:** Step 1 of spec section 9. The message, call and result types move into a new `wisp/net/llm_provider.hpp`, and `LlmResult::error` becomes `net::LlmError`. `AgentDeps` holds `net::LlmProvider&` and no `ModelProfileService`. Today's `loop()`, `compress()`, `request()`, `run_tool()`, `skip_calls()` and `finish_abort()` move into `agent::TurnRunner`, which runs against a `TurnEnv` that `AgentService::Impl` implements. Task 1 captures goldens of the request bodies and of the turn's event sequence before anything changes, and they stay green and unedited through every later task.

**Tech Stack:** C++20 (`-fno-exceptions -fno-rtti`), GoogleTest, the simulator build (`build/sim`), ESP-IDF v6.0.3 (firmware build check only).

**Spec:** `docs/superpowers/specs/2026-11-15-llm-tool-providers-design.md` (approved 2026-11-16; all section 12 proposals adopted). This plan implements sections 3.1 and 3.10, the J1 row of section 9, and the J1 parts of section 7. Sections 3.2-3.9 and 4 belong to plans J2-J4.

## Global Constraints

- Work in the worktree `.superpowers/worktrees/llm-providers`, branch `feat/llm-providers`. Never push, merge, switch branches or `git stash`. Never add `.claude/`, `.playwright-mcp/` or `.superpowers/`. Delete files only by literal absolute paths inside the worktree.
- Only one process may build in `build/sim` at a time. Configure once with `cmake -S simulator -B build/sim -G Ninja` if it does not exist yet.
- **Refactor only.** The request bytes, the `AgentEvent` sequence, the stored messages, the log lines and the REST/WebSocket shapes do not change. The goldens of Task 1 run in every task: `export GOLDEN='LlmBodyGolden|AgentTurnGolden'` once per shell. A golden that needs editing is a bug in the task, not in the golden. The single intended behaviour change is Task 4: calibration keyed by profile id and model, which closes `docs/backlog.md:231`. No golden depends on it.
- AGENTS.md applies in full. In particular:
  - Layering (`tools/check_layering.py`): `wisp_net` sits before `wisp_agent`. `llm_provider.hpp` includes only standard headers and `wisp_os`/`wisp_net` headers. After Task 3, nothing in `components/wisp_agent/` names `ModelProfileService`.
  - No platform headers in portable layers (`esp_*`, `freertos/*`, …).
  - Large buffers use `os::LargeString`/`os::LargeVector`/`os::alloc(Mem::Large)`. Bodies, reply text and tool arguments stay `os::LargeString` as today. `TurnRunner` lives on `wisp.agent`'s stack and holds only references and small scalars.
  - Only `wisp.fs` touches flash. `TurnRunner` appends only through `TurnEnv::append()`, which `Impl` runs on the Interactive lane (`run_io_interactive`). Test doubles append through `StorageWorker::run_sync`.
  - TDD: write the failing test first, run it, see it fail for the stated reason (a missing header or symbol counts), then implement. Test output must be clean, with no warnings.
  - Comments explain why. They contain no review ids, no "Task N", no "round N" and no `.superpowers/` paths (`tools/ci.sh` greps for these). Point at spec sections instead ("spec 3.10").
  - Conventional Commits. Every commit message ends with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- `-Werror=missing-field-initializers` is on: every aggregate this plan adds (`LlmModelInfo`, `TurnInput`, `TurnControls`, `FakeStep`, …) gives every member a default member initializer.
- Every task ends with `cmake --build build/sim 2>&1 | tee build/sim-build.log | tail -3` and `grep -c 'warning:' build/sim-build.log` (expected `0`; `build/` is git-ignored), then focused `ctest --test-dir build/sim -R '<filter>' --output-on-failure`. The last task runs `bash tools/ci.sh` and the firmware build for both boards.
- Device checks are never reported as done.

## Review Focus

These are the five failures most likely to reach a user that no single feature test would catch. Each one is pinned by a test in the task named.

1. **A stored profile's requests change by a byte** (field order, `null` content, escaping, tools left out), which costs the prefix cache or gets refused by a strict gateway. Pinned by `LlmBodyGolden.*` and by `AgentTurnGolden.ToolTurnBodies` and `AgentTurnGolden.ContextSourceBody` (Task 1). Neither may be edited afterwards.
2. **The screen, speech or web chat sees events in a different order** (a `Message` before its `Delta`s, `State idle` after `TurnEnd`, a `ToolStart` for a skipped call). Pinned by the `AgentTurnGolden` traces (Task 1).
3. **A stop, Wi-Fi loss or stall no longer aborts the request in flight** because the watch, ticket or last-byte state moved into `TurnControls` and one side reads a stale copy. Pinned by `AgentTurnGolden.StopMidStreamKeepsTheText` (Task 1), the existing `AgentLoop.StopDuringStreamAborts`, `AgentLoop.LostWifiAbortsTheRequest` and `AgentLoop.StalledStreamTimesOut` (run in Tasks 3-5), and `TurnRunner.ArmedAfterAStopAbortsAtOnce` (Task 5).
4. **An error maps to a different reason or note once the string becomes an enum** (for example `transport` with body vs without, or `context_length` retried twice). Pinned by `LlmClient.ErrorWordsAreTodays` (Task 2), `AgentTurnGolden.UnauthorizedEndsWithANote` and `AgentTurnGolden.ContextRefusalCompressesAndRetries` (Task 1), and `TurnRunner.ErrorsMapToReasons` (Task 5).
5. **The calibration leaks across profiles, or resets when nothing changed** (for example a turn with no active model, key `("","")`). Pinned by the `Calibration.*` tests and `AgentLoop.SameModelOnAnotherProfileStartsUncalibrated` (Task 4), with the existing `AgentLoop.UsageCalibratesTheEstimate` and `AgentLoop.ModelSwitchResetsTheCalibration`.

## Plan decisions (where the spec is silent or imprecise)

1. **`TurnEnv` gains `uint64_t now_ms() const`.** Spec 3.10's `TurnEnv` has no clock, but `request()` and the stall bookkeeping use `deps.now_ms`, which the tests move ahead (`AgentHarness::advance`). `run_tool()`'s `ctx.deadline_ms` stays on `os::now_ms()`, as today. Spec contradiction, small: recorded here.
2. **`LlmResult` gains `std::string profile_id`**, set by `LlmClient` next to `model`. Today the calibration takes its key from the reply (`scale_model = res.model`, `agent_service.cpp:762, 783`), so that a profile switch in the middle of a turn calibrates the profile that answered. Keying by profile and model needs the answering profile's id too. This is additive; spec 3.1 lists only `reasoning` as new (J2).
3. **`llm_provider.hpp` includes `wisp/net/http.hpp` (for `TransportError`, used by `LlmResult::transport`) and `wisp/net/request_watch.hpp` (`LlmCall::watch`), not `model_profiles.hpp`.** Spec 3.1 says it includes only `model_profiles.hpp` and `wisp_os`. J1's `LlmModelInfo` has no dialect, so `wisp_core` is not needed yet. J2 adds the include if `LlmDialect` lives in `wisp_core`.
4. **`to_string(LlmError)` lives in a new `components/wisp_net/src/llm_provider.cpp`.** Its words are exactly today's strings, so the `llm` and `agent` log lines keep their words.
5. **`TurnRunner` takes `const ContextSource&`**, not `ContextSource` by value as spec 3.10 shows. A per-turn `std::function` copy would be a small default-heap (internal RAM) allocation for nothing, and `AgentDeps::context` outlives every turn.
6. **`Calibration` is an internal class in `components/wisp_agent/src/calibration.hpp`** (header-only) and is owned by `Impl` (spec 3.10). `TurnRunner::run()` calls `begin_turn()` with the `active_model()` it reads once per turn. With no active model the key is `("", "")`, as today's `""` model.
7. **Internal types move to `components/wisp_agent/src/turn_runner.hpp`:** `Abort`, `Outcome`, `TurnControls`, `TurnEnv`, `TurnInput`, and the helpers `sv()` and `summary()`, which `Impl` still uses for `TurnEnd::note`. `AgentService::kRequestMs` and `kDeadlineMarginMs` stay public on `AgentService`; the runner reads them from there.
8. **Notes.** `TurnEnv::note(StrId)` and `note_text()` write a note of the running turn. `Impl` keeps a `uint32_t turn_no` (wisp.agent only), set before `run()`. Where today's code passes `&out`, the runner forms the text with `ui::tr(id, env.lang())`, calls `note_text()` and sets `out.last_text` itself. `Impl`'s own notes (interrupted, rolled over) keep its existing helpers with an explicit turn.
9. **`AgentHarness` keeps the real `LlmClient` over `FakeLlmServer` by default** (so the golden and `AgentLoop` tests keep exercising the client) and gains `net::LlmProvider* provider = nullptr`, which `deps()` uses instead when it is set.
10. **Goldens are captured at `72fd6d7`.** The spec says `06279ef`; the only commits since then are docs (`git diff --stat 06279ef HEAD` lists only the spec), so the code is the same.
11. **No version bump or `CHANGELOG.md` entry in this plan.** Phase J is released as one merge (project rule: few, bundled releases), and the merge sets the version.

---

### Task 1: Pin the request bodies and the turn's event sequence

**Files:**
- Modify: `tests/net/llm_client_test.cpp` (append suite `LlmBodyGolden`)
- Create: `tests/agent/agent_turn_golden_test.cpp`
- Modify: `tests/CMakeLists.txt`: after the `agent_loop_test` entry (`:523-524`), add `wisp_test(NAME agent_turn_golden_test SRCS agent/agent_turn_golden_test.cpp LIBS wisp_agent wisp_port_sim_os TIMEOUT 60)`

**Interfaces:**
- Consumes (today's code, unchanged): `net::build_chat_body(model, msgs, tools_json)` (`llm_client.hpp:126`), `net::LlmClient`, `test::FakeLlmServer`/`FakeReply` (`tests/support/fake_llm.hpp`), `test::AgentHarness` (`tests/support/agent_fixture.hpp`), `agent::AgentService` public API only, `agent::build_request`/`build_history`/`build_summary_request` (`context.hpp:107-130`).
- Produces: tests only. `agent_turn_golden_test.cpp` uses only `AgentHarness`, `FakeLlmServer` and the public `AgentService`/`context.hpp` API, so no later task has to touch it.

- [ ] **Step 1: Write the body goldens** (characterisation tests: they pass now). Compare raw strings, never parsed JSON.

```cpp
// tests/net/llm_client_test.cpp
TEST(LlmBodyGolden, EveryMessageShape)
// msgs: System "S"; User "q\"\nü"; Assistant text "A"; Assistant with text "" and two calls
// {id "c1", name "a_b", args "{\"x\":1}"}, {id "c2", name "c_d", args ""}; Assistant text "T" with
// one call {id "c3", name "a_b", args "{}"}; Tool {tool_call_id "c1", text "{\"ok\":true}"}.
// tools = R"([{"type":"function","function":{"name":"a_b","parameters":{}}}])"
// EXPECT_EQ(str(build_chat_body("m", msgs, tools)),
//   R"({"model":"m","stream":true,"stream_options":{"include_usage":true},"messages":[)"
//   R"({"role":"system","content":"S"},{"role":"user","content":"q\"\nü"},)"
//   R"({"role":"assistant","content":"A"},)"
//   R"({"role":"assistant","content":null,"tool_calls":[{"id":"c1","type":"function","function":{"name":"a_b","arguments":"{\"x\":1}"}},)"
//   R"({"id":"c2","type":"function","function":{"name":"c_d","arguments":""}}]},)"
//   R"({"role":"assistant","content":"T","tool_calls":[{"id":"c3","type":"function","function":{"name":"a_b","arguments":"{}"}}]},)"
//   R"({"role":"tool","tool_call_id":"c1","content":"{\"ok\":true}"}],"tools":)" + tools + "}");
TEST(LlmBodyGolden, NoToolsLeavesTheKeyOut)
// build_chat_body("m", {User "hi"}, "") ==
//   R"({"model":"m","stream":true,"stream_options":{"include_usage":true},"messages":[{"role":"user","content":"hi"}]})"
TEST(LlmBodyGolden, ClientSendsExactlyTheBuiltBody)
// Fixture with the stored profile (model "m", key "sk-test"); one call with the msgs and tools of
// EveryMessageShape. requests().size() == 1; body == str(build_chat_body("m", msgs, tools));
// url == std::string(kBase) + "/chat/completions" (the fixture's kBase); headers in order: Content-Type application/json, Accept text/event-stream,
// Authorization "Bearer <fixture key>".
```

If a literal above differs from what the code produces (the escaping of `ü` or of the newline), paste the code's output and say so in the commit body. The point is to pin today's bytes.

- [ ] **Step 2: Write the agent goldens** in `tests/agent/agent_turn_golden_test.cpp`. The fixture `AgentTurnGolden` has an `AgentHarness`, a listener that records events, `start(AgentConfig = {}, ContextSource = {})`, and these renderers:

```cpp
// One line per event; consecutive identical ToolPrep lines collapse into one (their count
// depends on timing, kToolProgressMs).
//   TurnBegin turn=<n> source=<to_string(source)> text=<text>
//   TurnEnd turn=<n> status=<status> reason=<reason> note=<note>
//   Delta <text> | ToolPrep <tool> | ToolStart <tool> <args> | ToolEnd <tool> ok=<0|1> <result>
//   State <to_string(state)>[ <tool> when Tool] | Confirm <text> | Message <seq> | Cleared ok=<0|1>
std::vector<std::string> trace();
// One line per stored message: "<seq> <role> t<turn> <text>" plus, by role, " marker=<m> status=<s>",
// " calls=<id>:<name>,..." (assistant), " call=<id> tool=<name> ok=<0|1>" (tool),
// " covers=<first>-<last>" (summary).
std::vector<std::string> transcript();
```

Scenarios. Each asserts `trace()` and `transcript()` against literal vectors. Notes come in as `ui::tr(StrId::..., ui::Lang::Zh)` concatenations, not pasted Chinese text. Write the literal from the key points below, run it, check the observed sequence against those points, then paste the full observed sequence.

```cpp
TEST_F(AgentTurnGolden, PlainReply)
// text_reply("hi"); submit("hello", Web). trace ==
// {"TurnBegin turn=1 source=web text=hello", "Message 1", "Message 2", "State thinking",
//  "State output", "Delta hi", "Message 3", "Message 4", "State idle",
//  "TurnEnd turn=1 status=done reason= note="}
TEST_F(AgentTurnGolden, ToolCallThenReply)
// tool "test.echo" (ToolThread::Agent, returns {"ok":true}); replies: tool call c1 test_echo
// {"a":1}, then text "done". Key points: ToolPrep before the reply's Message; ToolStart after
// it; "State tool test.echo"; ToolEnd after the tool's Message; "State thinking" again before
// the second request; final "TurnEnd ... status=done".
TEST_F(AgentTurnGolden, ToolTurnBodies)
// The same turn as ToolCallThenReply, without a context source: the raw bodies of
// requests 0 and 1 equal pasted literals (request 1 holds the assistant tool_calls with
// "content":null and the tool message; both carry the registry's tools array).
TEST_F(AgentTurnGolden, StepLimitSkipsTheRest)
// max_steps 1; one reply with calls c1 and c2 (two sse_tool indexes). Key points: one
// ToolStart (c1) only; c2 stored as a tool message with ok=0 and no ToolStart; note
// AgentStepLimit; "TurnEnd ... status=step_limit reason=step_limit note=<AgentStepLimit>".
TEST_F(AgentTurnGolden, UnauthorizedEndsWithANote)
// FakeReply{.status = 401}. Key points: "State error" before the note's Message;
// "TurnEnd turn=1 status=error reason=unauthorized note=<AgentErrUnauthorized>".
TEST_F(AgentTurnGolden, StopMidStreamKeepsTheText)
// FakeReply{chunks {sse_text("par")}, hold_after 1}; wait for server.held() == 1; stop_turn().
// Key points: "Delta par"; the partial assistant "par" stored; note AgentStopped;
// "TurnEnd ... status=stopped reason=stopped".
TEST_F(AgentTurnGolden, ContextRefusalCompressesAndRetries)
// window 4096 (a copy of agent_loop_test.cpp:195's set_window), tool "scene.list" registered,
// two turns of 3000-byte replies, then turn 3: a copy of agent_loop_test.cpp:1623's context_length_reply(),
// a summary reply "S", then text_reply("ok"). Key points: requests 3/4/5 are: with "tools", without,
// with; a stored summary; status done. The summary request's raw body ==
// str(build_chat_body("m", build_summary_request(h, s.covers_first, s.covers_last).messages, ""))
// where h is the stored history before the summary's seq.
TEST_F(AgentTurnGolden, ContextSourceBody)
// A context source returning a fixed DeviceSnapshot (lang "en", wall_s 1793923200,
// utc_offset_min 480, weekday 3, memory {"[1] likes tea"}, battery 80). After a plain turn:
// request 0's raw body == str(build_chat_body("m", build_request(h, snap_with_source).messages,
// tools_json)) where h is the stored history up to and including the user message and
// snap_with_source.source = Source::Web.
TEST_F(AgentTurnGolden, NoModelEndsWithANote)
// AgentHarness(false). No request; "TurnEnd ... status=error reason=no_model note=<AgentErrNoModel>".
```

- [ ] **Step 3: Run** `cmake --build build/sim && ctest --test-dir build/sim -R "$GOLDEN" --output-on-failure --repeat until-fail:3`. Expected: all PASS three times.
- [ ] **Step 4: Run** `ctest --test-dir build/sim -R 'LlmClient|AgentLoop' --output-on-failure`. Expected: PASS (nothing else changed).
- [ ] **Step 5: Commit** `test(agent): pin the request bodies and the turn's event sequence`.

---

### Task 2: `net::LlmProvider`, `LlmError` and `LlmClient : LlmProvider`

**Files:**
- Create: `components/wisp_net/include/wisp/net/llm_provider.hpp`, `components/wisp_net/src/llm_provider.cpp`
- Modify: `components/wisp_net/CMakeLists.txt`: add `src/llm_provider.cpp` to `SRCS` next to `src/llm_client.cpp`
- Modify: `components/wisp_net/include/wisp/net/llm_client.hpp`: the types at `:52-120` move out; include `llm_provider.hpp`; update the header comment's error words to `LlmError`
- Modify: `components/wisp_net/src/llm_client.cpp`: error assignments at `:340, 369, 375, 414, 428, 431, 438, 445, 449, 453, 456`, the clear at `:463`, `profile_id` near `:344`, log lines at `:479-489`, and the new `active_model()`
- Modify: `components/wisp_agent/include/wisp/agent/context.hpp:44`: include `wisp/net/llm_provider.hpp` instead of `llm_client.hpp`
- Modify: `components/wisp_agent/src/agent_service.cpp:624, 645-646, 776, 792-798`: enum comparisons; the log at `:646` uses `net::to_string(res.error)`
- Modify: `tests/net/llm_client_test.cpp`: `EXPECT_EQ(r.error, "...")` becomes `EXPECT_EQ(r.error, LlmError::...)`, and `mentions_key()` (`:112-116`) checks `to_string(r.error)`

**Interfaces:**
- Consumes: `net::TransportError` (`http.hpp:182`), `net::RequestWatch`, `core::ModelProfileService::get()`, `core::profile_context_window()`/`profile_vision()` (`model_profiles.hpp:131-132`).
- Produces (`namespace wisp::net`, `llm_provider.hpp`):

```cpp
enum class LlmError : uint8_t { None, NoModel, Unauthorized, RateLimited, Server, Http,
                                Transport, Cancelled, BadStream, ContextLength };
// Today's LlmResult::error strings ("" for None), for logs.
const char* to_string(LlmError e);

enum class LlmRole : uint8_t { System, User, Assistant, Tool };       // moved, unchanged
struct LlmToolCall { /* moved, unchanged */ };
struct LlmMessage  { /* moved, unchanged: role, text, tool_calls, tool_call_id */ };
enum class LlmFinish : uint8_t { Stop, ToolCalls, Length, Other };    // moved
struct LlmUsage { /* moved */ };
struct LlmResult {                       // moved; every field kept, error retyped
    LlmError error = LlmError::None;
    TransportError transport = TransportError::None;
    int status = 0;
    os::LargeString text;
    std::vector<LlmToolCall> tool_calls;
    LlmFinish finish = LlmFinish::Other;
    std::optional<LlmUsage> usage;
    uint32_t attempts = 0;
    bool any_body = false;
    std::string model;       // the profile's model name
    std::string profile_id;  // new: the profile that served the call ("" when none)
    std::string code;
};
struct LlmToolProgress { /* moved */ };
struct LlmCall { /* moved, every field of llm_client.hpp:102-120 */ };

struct LlmModelInfo {
    std::string profile_id{};
    std::string model{};
    uint32_t context_window = 0;  // core::profile_context_window()
    bool vision = false;          // core::profile_vision()
};

class LlmProvider {
public:
    virtual ~LlmProvider() = default;
    // The active profile as the next complete() will use it; nullopt when there is
    // none. Memory only (no flash, no network); any thread.
    virtual std::optional<LlmModelInfo> active_model() const = 0;
    // One request, retries included. Blocks; one call at a time per provider object;
    // callbacks run on the calling thread from inside the stream. wisp.agent only.
    virtual LlmResult complete(const LlmCall& call) = 0;
};
```

  `llm_client.hpp`: `class LlmClient final : public LlmProvider` with `std::optional<LlmModelInfo> active_model() const override;` and `LlmResult complete(const LlmCall& call) override;`. Its constructor, constants, `build_chat_body()` and `SseParser` are unchanged and stay in `llm_client.hpp`.
- What J2/J3 add here (not in this plan): J2 adds `LlmDialect dialect` to `LlmModelInfo`, `std::string_view reasoning` and `bool movable` to `LlmMessage`, `os::LargeString reasoning` to `LlmResult`, and a dialect parameter to `build_chat_body()`. J3 adds `LlmPartKind`, `LlmPart` and `std::span<const LlmPart> parts` to `LlmMessage`. Keep `LlmMessage` and `LlmModelInfo` as plain aggregates with defaulted members, so that these additions do not touch existing initializers.

- [ ] **Step 1: Write the failing tests** (`tests/net/llm_client_test.cpp`):

```cpp
TEST(LlmClient, ErrorWordsAreTodays)
// to_string: None "", NoModel "no_model", Unauthorized "unauthorized", RateLimited "rate_limited",
// Server "server", Http "http", Transport "transport", Cancelled "cancelled",
// BadStream "bad_stream", ContextLength "context_length".
TEST(LlmClient, ActiveModelDescribesTheActiveProfile)
// Fixture profile (no options): active_model() == {id of the profile, "m", 65536, false}.
// After update(options {context_window "32768", vision "true"}): {id, "m", 32768, true}.
// No request was made (server.requests().empty()).
TEST(LlmClient, NoActiveProfileHasNoModel)
// No llm profile: active_model() == nullopt; complete() -> error LlmError::NoModel, profile_id "".
TEST(LlmClient, ResultNamesTheProfileThatAnswered)
// One text reply: r.error == LlmError::None, r.model == "m", r.profile_id == the profile's id.
TEST(LlmClient, IsAnLlmProvider)
// net::LlmProvider& p = *f.client; p.complete(call) streams "hi" as StreamsTextDeltasInOrder does.
```

- [ ] **Step 2: Run** `cmake --build build/sim`. Expected: compile errors (`llm_provider.hpp` not found, `LlmError` undeclared).
- [ ] **Step 3: Implement.** Move the types verbatim, retype `error`, and add `profile_id`. `active_model()` reads `models_.get()`, finds the profile whose id is `active_for(ModelKind::Llm)`, and returns `{p.id, p.model, core::profile_context_window(p.options), core::profile_vision(p.options)}`. That is the same read `AgentService::Impl::active_llm()` does today (`agent_service.cpp:561-567`). `complete()` sets `result.profile_id = profile->id` next to `result.model` (`:344`). The log lines print `to_string(result.error)` with the same format strings. In `agent_service.cpp`, replace only the comparisons. `whole()` reads `res.error == net::LlmError::None || (res.error == net::LlmError::Transport && finished)`.
- [ ] **Step 4: Run** `python3 tools/check_layering.py && ctest --test-dir build/sim -R "LlmClient|SseParser|$GOLDEN|AgentLoop" --output-on-failure`. Expected: PASS. The goldens are unchanged.
- [ ] **Step 5: Commit** `refactor(net): put the agent's model calls behind net::LlmProvider`.

---

### Task 3: `AgentDeps` on the interface, and `FakeProvider`

**Files:**
- Modify: `components/wisp_agent/include/wisp/agent/agent_service.hpp:103, 107, 183-184`: include `wisp/net/llm_provider.hpp`, drop the `model_profiles.hpp` include if nothing else in the header uses it, `net::LlmProvider& llm;`, remove `models`
- Modify: `components/wisp_agent/src/agent_service.cpp:554-567`: `ActiveLlm` gains `std::string profile_id`, and `active_llm()` reads `deps.llm.active_model()` (nullopt: `{kDefaultContextWindow, "", ""}`, `limits.hpp:37`)
- Modify: `components/wisp_app/src/agent_wiring.cpp:449`: drop `.models`. `AgentApiAdapter` keeps its own `models` (`:130`, spec 3.1).
- Modify: `tests/support/agent_fixture.hpp:139-167`: drop `.models`; add `net::LlmProvider* provider = nullptr;`, and `deps()` passes `provider != nullptr ? *provider : *llm`
- Create: `tests/support/fake_provider.hpp`
- Modify: `tests/agent/agent_loop_test.cpp` (append three tests)

**Interfaces:**
- Consumes: `net::LlmProvider`, `LlmModelInfo`, `LlmCall`, `LlmResult`, `build_chat_body` (Task 2).
- Produces (`namespace wisp::test`, `fake_provider.hpp`):

```cpp
struct FakeStep {
    std::vector<std::string> deltas{};          // each: on_bytes(), then on_text()
    std::vector<net::LlmToolCall> calls{};      // each reported once through on_tool (name, arg size)
    net::LlmResult result{};                    // error, finish, usage, any_body; text/tool_calls
                                                // filled from deltas/calls when left empty
    bool hold = false;                          // after the deltas, poll cancelled() every 1 ms;
                                                // then error Cancelled, transport Cancelled
    std::function<void()> during{};             // called inside complete(), after the deltas
};
class FakeProvider final : public net::LlmProvider {
public:
    struct Recorded {
        std::string body;        // build_chat_body(model, call.messages, call.tools_json) at call time
        std::string tools_json;
        uint32_t timeout_ms = 0;
        size_t messages = 0;
    };
    void set_model(std::optional<net::LlmModelInfo> m);  // default {"p1", "m", 65536, false}
    void push(FakeStep s);
    std::optional<net::LlmModelInfo> active_model() const override;  // counts its calls
    // No model: NoModel. Script empty: error Server, status 500. Sets model/profile_id from
    // the current model, attempts = 1, any_body |= deltas or calls non-empty.
    net::LlmResult complete(const net::LlmCall& call) override;
    std::vector<Recorded> calls() const;
    int active_model_calls() const;
};
```

  All of its state is under one `std::mutex`. It allocates freely, since it is a test double.
- `AgentDeps` after this task: `bus, store, tools, net::LlmProvider& llm, run_io, run_io_interactive, locks, notes, screen_off, network_up, lang, now_ms, wall_s, context`.

- [ ] **Step 1: Write the failing tests** (`tests/agent/agent_loop_test.cpp`; `start()` passes `h->provider` through `deps()`):

```cpp
TEST_F(AgentLoop, RunsOverAnyProvider)
// FakeProvider fake; harness().provider = &fake; tool "scene.list"; fake steps: a call
// {id "c1", name "scene_list", args "{}"} with finish ToolCalls, then deltas {"hi"} finish Stop.
// Turn ends done; tool_calls() == {"scene.list"}; history roles marker,user,assistant,tool,
// assistant,marker; fake.calls()[1].body contains "\"tool_call_id\":\"c1\"";
// h->server.requests().empty().
TEST_F(AgentLoop, NoModelFromTheProvider)
// fake.set_model(nullopt), fake push {result.error NoModel}; last_reason() == "no_model";
// notes().back() == tr(AgentErrNoModel, Zh).
TEST_F(AgentLoop, WindowComesFromTheProvider)
// fake model context_window 1024; history seeded with two turns of 3000-byte replies
// (run through the fake); turn 3: the first recorded call of the turn has tools_json ""
// (a summary request) and a Summary is stored. The window is read once per turn:
// fake.active_model_calls() grows by exactly 1 per turn.
```

- [ ] **Step 2: Run** `cmake --build build/sim`. Expected: compile error (`fake_provider.hpp` missing; `AgentHarness::provider` unknown).
- [ ] **Step 3: Implement** the header changes, `active_llm()` over `deps.llm.active_model()`, the wiring and fixture edits, and `FakeProvider`. Calibration keeps its model-only key in this task.
- [ ] **Step 4: Run** `python3 tools/check_layering.py && test -z "$(grep -rn ModelProfileService components/wisp_agent)" && ctest --test-dir build/sim -R "$GOLDEN|AgentLoop|Confirm\.|AgentRoutes|ConversationTest|HistoryTest|HomeTest|AgentWiring" --output-on-failure`. Expected: PASS.
- [ ] **Step 5: Commit** `refactor(agent): take the model through net::LlmProvider`.

---

### Task 4: `Calibration` keyed by profile id and model

**Files:**
- Create: `components/wisp_agent/src/calibration.hpp` (header-only)
- Modify: `components/wisp_agent/src/agent_service.cpp`: replace `est_scale_pct`/`scale_model` and the constants (`:998-1007`) with `Calibration cal;` used at `:464-469` (`cal.begin_turn(llm.profile_id, llm.model)`), `:578` (`scaled`), `:701-702` (`cal.unscaled_window(window)`), `:757-763` (`observe(res.profile_id, res.model, prompt, sent)`) and `:782-783` (`overflow(res.profile_id, res.model)`)
- Create: `tests/agent/turn_runner_test.cpp` (suite `Calibration` for now)
- Modify: `tests/CMakeLists.txt`: `wisp_test(NAME turn_runner_test SRCS agent/turn_runner_test.cpp LIBS wisp_agent wisp_port_sim_os TIMEOUT 60)` and `target_include_directories(turn_runner_test PRIVATE ${WISP_ROOT}/components/wisp_agent/src)`, after the new golden entry
- Modify: `tests/agent/agent_loop_test.cpp` (append one test)

**Interfaces:**
- Produces (`namespace wisp::agent`, `calibration.hpp`):

```cpp
// The token estimate's multiplier (agent-core spec 3.3): the model's reported prompt
// tokens over the bytes / 3 estimate of the last request it answered, kept across turns
// for one (profile id, model) pair. wisp.agent only.
class Calibration {
public:
    static constexpr uint32_t kMinPct = 50;
    static constexpr uint32_t kMaxPct = 300;
    static constexpr uint32_t kOverflowPct = 150;
    // A turn on another profile or model starts again from 100.
    void begin_turn(std::string_view profile_id, std::string_view model);
    // pct = clamp(prompt_tokens * 100 / max(1, est_tokens), kMinPct, kMaxPct); the pair
    // becomes this one.
    void observe(std::string_view profile_id, std::string_view model, uint32_t prompt_tokens,
                 size_t est_tokens);
    // A context_length refusal: pct = max(pct, kOverflowPct); the pair becomes this one.
    void overflow(std::string_view profile_id, std::string_view model);
    size_t scaled(size_t est_tokens) const;           // est_tokens * pct / 100
    uint32_t unscaled_window(uint32_t window) const;  // window * 100 / pct (64-bit intermediate)
    uint32_t pct() const;
private:
    uint32_t pct_ = 100;
    std::string profile_id_, model_;
};
```

- [ ] **Step 1: Write the failing tests:**

```cpp
// tests/agent/turn_runner_test.cpp
TEST(Calibration, StartsAt100)                 // pct() == 100; scaled(300) == 300
TEST(Calibration, ObserveSetsTheClampedRatio)  // observe("p","m",2002,1001) -> 200;
                                               // (…,10,1000) -> 50; (…,10000,1000) -> 300; (…,5,0) -> 300
TEST(Calibration, OverflowRaisesToAtLeast150)  // from 100 -> 150; after observe 200 -> stays 200
TEST(Calibration, SamePairKeepsItAcrossTurns)  // observe p/m 200; begin_turn("p","m") -> 200
TEST(Calibration, AnotherModelStartsAgain)     // observe p/m 200; begin_turn("p","m2") -> 100
TEST(Calibration, AnotherProfileWithTheSameModelStartsAgain)  // begin_turn("q","m") -> 100
TEST(Calibration, NoModelIsAPairToo)           // begin_turn("","") twice after observe("","",…) keeps pct
TEST(Calibration, UnscaledWindow)              // after observe 200: unscaled_window(4096) == 2048
```

```cpp
// tests/agent/agent_loop_test.cpp
// Two profiles on different providers with the same model name do not share it
// (docs/backlog.md:231).
TEST_F(AgentLoop, SameModelOnAnotherProfileStartsUncalibrated)
// As ModelSwitchResetsTheCalibration (agent_loop_test.cpp:1751), but instead of renaming the
// model: create a second llm profile "other" (preset custom, base_url
// "https://other.example.com/v1", model "m", key "sk-2", options {context_window "4096"}) and
// activate() it, on the worker. Turn 3 sends its request whole: requests().size() == 3, no Summary.
```

- [ ] **Step 2: Run** `cmake --build build/sim`. Expected: compile error (`calibration.hpp` missing).
- [ ] **Step 3: Implement** `Calibration` and swap it into `Impl`. Today's constants `kMinScalePct`, `kMaxScalePct` and `kOverflowScalePct` go and their values move into the class. Before the swap, `AgentLoop.SameModelOnAnotherProfileStartsUncalibrated` must fail: with today's model-only key the second profile inherits the 200 % scale, so turn 3 is summarized. Confirm that failure once by building with the test but without the `Impl` swap, then make the swap.
- [ ] **Step 4: Run** `ctest --test-dir build/sim -R "Calibration|$GOLDEN|AgentLoop" --output-on-failure`. Expected: PASS.
- [ ] **Step 5: Commit** `fix(agent): key the estimate calibration by profile and model`. The body says that it closes `docs/backlog.md:231` (Task 6 removes the line).

---

### Task 5: `TurnRunner` and `TurnEnv`

**Files:**
- Create: `components/wisp_agent/src/turn_runner.hpp`, `components/wisp_agent/src/turn_runner.cpp`
- Modify: `components/wisp_agent/CMakeLists.txt`: add `src/turn_runner.cpp` to `SRCS` after `src/agent_service.cpp`
- Modify: `components/wisp_agent/src/agent_service.cpp`. Remove `Abort`, `Outcome`, `sv`, `prefix_code_points`/`summary` (`:29-73`, keeping `code_points` and `prefix_bytes`, which `submit()` and `post_notice()` use) and the moved members (`finish_abort` `:515-536`, `skip_calls` `:540-552`, `ActiveLlm`/`active_llm` `:554-567`, `build` `:569-574`, `scaled`, `request` `:582-616`, `whole` `:620-625`, `compress` `:633-661`, `loop` `:663-848`, `run_tool` `:850-943`). `struct AgentService::Impl final : TurnEnv`. The atomics `ticket`, `in_request` and `last_byte` (`:985-994`) become `TurnControls ctl;`, with `ctl.watch = watch.get()` set in `start()` and reset in `reset_start()`. `run_turn()` builds a `TurnRunner` and calls `run()`.
- Modify: `tests/agent/turn_runner_test.cpp` (append suite `TurnRunner`)

**Interfaces:**
- Consumes: `net::LlmProvider` (Task 2), `Calibration` (Task 4), `ToolRegistry`, `ConversationStore`, `ContextSource`, `context.hpp`'s builders, `AgentService::kRequestMs`/`kDeadlineMarginMs`, `ui::tr`/`tr_fill`.
- Produces (`namespace wisp::agent`, `turn_runner.hpp`, internal to `wisp_agent`):

```cpp
// Why a turn is being cut short; the first reason wins.
enum class Abort : uint8_t { None, Stop, Network, Stall, Time };

struct Outcome {                    // moved unchanged
    const char* status = "done";
    const char* reason = "";
    uint32_t steps = 0;
    uint32_t tokens = 0;
    os::LargeString last_text;
};

// The request in flight as the check timer and the Wi-Fi callback see it; owned by
// AgentService::Impl, written by TurnRunner on wisp.agent.
struct TurnControls {
    net::RequestWatch* watch = nullptr;  // null: no watch (RequestWatch::create() failed)
    std::atomic<uint32_t> ticket{0};
    std::atomic<uint64_t> last_byte{0};
    std::atomic<bool> in_request{false};
};

// What the service lends a turn (spec 3.10). Implemented by AgentService::Impl.
struct TurnEnv {
    virtual uint64_t append(Message m) = 0;   // on the storage worker, waits; 0 on failure
    virtual void note(ui::StrId id) = 0;      // a note of the running turn
    virtual void note_text(std::string_view t) = 0;
    virtual void set_state(AgentState s, std::string_view tool = {}) = 0;
    virtual void emit(const AgentEvent& e) = 0;
    virtual bool aborted() = 0;               // stop, network, stall, time (checks the limit)
    virtual Abort abort_reason() const = 0;
    virtual bool network_up() const = 0;
    virtual ui::Lang lang() const = 0;
    virtual TurnControls& controls() = 0;
    virtual uint64_t now_ms() const = 0;      // plan decision 1
protected:
    ~TurnEnv() = default;
};

struct TurnInput {
    uint32_t turn = 0;      // conversation turn number
    uint32_t serial = 0;    // ToolCtx::turn
    uint32_t max_steps = 0;
    uint64_t start_ms = 0;  // TurnEnv::now_ms() at the turn's start
    uint64_t turn_ms = 0;
    Source source = Source::Web;
};

class TurnRunner {
public:
    TurnRunner(net::LlmProvider& llm, ToolRegistry& tools, ConversationStore& store,
               const ContextSource& context, Calibration& cal, TurnEnv& env);
    // Reads llm.active_model() once (window, calibration pair), then today's loop.
    Outcome run(const TurnInput& in);
private:
    // Today's Impl members, moved with their bodies and comments: build(), request(),
    // whole(), compress() (+ Compressed), skip_calls(), run_tool(), finish_abort().
};

std::string_view sv(const os::LargeString& s);
std::string summary(std::string_view s);  // at most kSummaryChars code points
```

- What J2/J3 add here (not in this plan): J2 adds `std::optional<os::LargeString> make_carry(std::span<const Message> history)`, the length continuation inside `run()` and the per-turn reasoning keep. `Impl::run_turn()` will then build the runner before archiving, to call `make_carry()`. J3 adds the image slot and picks `tools.tools_json(vision)` from the `LlmModelInfo` that `run()` already reads. Neither changes `TurnEnv`, so keep every turn-local buffer a `TurnRunner` member or a local of `run()`, never a member of `Impl`.
- `Impl` mapping: `append`, `set_state`, `emit`, `aborted`, `network_up` and `lang` are today's members marked `override`. `note(id)` and `note_text(t)` use `turn_no`. `abort_reason()` is `static_cast<Abort>(abort.load())`. `controls()` returns `ctl`. `now_ms()` is `now()`. `trigger()` aborts `ctl.watch` with `ctl.ticket`. `on_tick()` and `on_wifi()` read `ctl.in_request` and `ctl.last_byte`. In `run_turn()`, after `set_state(Thinking)` (`:461`), it does `turn_no = turn; Outcome out = TurnRunner(deps.llm, deps.tools, deps.store, deps.context, cal, *this).run({turn, turn_serial, max_steps, start, turn_ms.load(), c.source});`. The rest of `run_turn()` is unchanged.
- `request()` in the runner keeps every line of today's `:582-616`, with `ctl` in place of the `Impl` atomics. `on_armed` checks `env.abort_reason() != Abort::None`, not `aborted()`, which is today's `abort.load()` test.

- [ ] **Step 1: Write the failing tests** (`tests/agent/turn_runner_test.cpp`). The fixture has an `AgentHarness h` (store and worker), a `FakeProvider fake`, a `Calibration cal`, and a `FakeTurnEnv env` that implements `TurnEnv`. `append` goes through `h.worker.run_sync(..., os::kForever, core::IoLane::Interactive)` into `*h.store`. It records each call as a trace line (`"append <role>"`, `"note <text>"`, `"state <s>"`, the events by kind). It has settable `Abort reason`, `bool aborted_flag`, `bool net = true` and a clock `uint64_t clock`. A helper `Outcome run(TurnInput in = {.turn = 1, .serial = 1, .max_steps = 8, .start_ms = env.clock, .turn_ms = 600000})` constructs a `TurnRunner` and runs it after appending a user message.

```cpp
TEST_F(TurnRunner, PlainReplyEndsDone)
// fake: deltas {"hi"}, finish Stop. out.status "done", out.reason "", out.last_text "hi";
// env trace has "state thinking", "state output", Delta "hi", "append assistant" in that order;
// fake.calls()[0].tools_json == str(h.tools->tools_json()).
TEST_F(TurnRunner, ToolCallFeedsTheResultBack)
// tool "test.echo"; fake: call c1 test_echo "{}" finish ToolCalls; then deltas {"ok"}.
// out.steps == 1; fake.calls()[1].body contains "\"role\":\"tool\",\"tool_call_id\":\"c1\"".
TEST_F(TurnRunner, ErrorsMapToReasons)
// One step each (fresh fixture per row, a loop over a table):
// NoModel -> ("error","no_model",AgentErrNoModel); Unauthorized -> ("error","unauthorized",
// AgentErrUnauthorized); Transport any_body=false -> ("error","network",AgentErrNetwork);
// Transport with deltas {"x"} -> ("error","network_lost",AgentErrNetworkLost) and "x" stored;
// Server, Http, BadStream -> ("error","server",AgentErrServer).
// out.last_text == tr(id, Zh); env trace has "state error" before the note.
TEST_F(TurnRunner, AbortReasonsMapToStatuses)
// env aborted before the first request: Time -> ("time_limit","time_limit", note
// tr_fill(AgentTimeLimit, "10") with turn_ms 600000); Network and Stall -> ("error",
// "network_lost"), "state error"; Stop -> ("stopped","stopped"). No provider call.
TEST_F(TurnRunner, NetworkDownBeforeARequest)
// env.net = false: ("error","network"), note AgentErrNetwork, fake.calls().empty().
TEST_F(TurnRunner, ReadsTheModelOncePerTurn)
// two-request tool turn: fake.active_model_calls() == 1.
TEST_F(TurnRunner, UsageCalibratesUnderTheAnsweringProfile)
// fake model {"p9","m9",65536,false}; step usage prompt = 2 x the request's estimate.
// cal.pct() == 200; then cal.begin_turn("p9","m9") keeps 200 and begin_turn("p1","m9") resets.
TEST_F(TurnRunner, RequestTimeoutLeavesTheMarginPastTheTurn)
// in.start_ms = env.clock - 1000, turn_ms 60000: fake.calls()[0].timeout_ms ==
// 59000 + AgentService::kDeadlineMarginMs; with turn_ms 600000: == AgentService::kRequestMs.
TEST_F(TurnRunner, ControlsFollowTheRequest)
// step.during checks env.ctl.in_request == true and env.ctl.last_byte == env.clock (advanced
// before the call); after run(): in_request false.
TEST_F(TurnRunner, ArmedAfterAStopAbortsAtOnce)
// env.reason = Stop with a real net::RequestWatch in env.ctl.watch; FakeProvider's step arms it
// (call.watch->arm(1000), call.on_armed(ticket)) in `during`: the watch reports aborted
// (RequestWatch::reason() != None) right after on_armed.
```

- [ ] **Step 2: Run** `cmake --build build/sim`. Expected: compile error (`turn_runner.hpp` missing).
- [ ] **Step 3: Implement** `turn_runner.{hpp,cpp}` by moving the code. Bodies and comments move unchanged apart from these renames: `deps.llm`/`deps.tools`/`deps.store`/`deps.context` become the runner's references, `est_scale_pct` becomes `cal`, `turn_start`/`turn_ms` become `in.start_ms`/`in.turn_ms`, `max_steps` becomes `in.max_steps`, `turn_serial` becomes `in.serial`, `now()` becomes `env.now_ms()`, and `note(id, turn, &out)` becomes the runner's note helper (plan decision 8). `run()` starts with today's `:464-469` (now `cal.begin_turn`) and then today's `loop()` body. Then strip `Impl`, add `TurnEnv` to it, and replace the atomics with `ctl`.
- [ ] **Step 4: Run** `python3 tools/check_layering.py && ctest --test-dir build/sim -R "TurnRunner|Calibration|$GOLDEN|AgentLoop|LlmClient" --output-on-failure --repeat until-fail:2`. Expected: PASS twice. The goldens are untouched: `git log --oneline -- tests/agent/agent_turn_golden_test.cpp` lists only the Task 1 commit, and `git diff -- tests/agent/agent_turn_golden_test.cpp` is empty.
- [ ] **Step 5: Check the size of the split.** `wc -l components/wisp_agent/src/agent_service.cpp components/wisp_agent/src/turn_runner.cpp`: `agent_service.cpp` shrinks by roughly 450 lines. `grep -n "est_scale_pct\|scale_model\|in_request = \|LlmResult" components/wisp_agent/src/agent_service.cpp` finds nothing.
- [ ] **Step 6: Commit** `refactor(agent): move the turn into TurnRunner`.

---

### Task 6: Documents, CI and the firmware build

**Files:**
- Modify: `docs/architecture.md`
  - `:82` (the `wisp.agent` row): "The loop of an agent turn (`agent::TurnRunner`, built per turn on this stack): …"
  - `:392`: add `turn_runner` (one turn: requests, compression, tools) and `calibration` (the estimate scale per profile and model) to the file list, and say that `agent_service` keeps the queue, framing, confirmation and events
  - `:397`: "scaled by a calibration per profile and model (`Calibration`, 50-300, from the reply's `usage.prompt`, reset when the active profile or its model changes)"
  - `:398`: "Model requests go through `net::LlmProvider` (`wisp/net/llm_provider.hpp`: `active_model()`, `complete()`, errors as `net::LlmError`); `net::LlmClient` is its chat-completions implementation (4a spec 3.6): …" (the rest of the bullet unchanged)
- Modify: `docs/superpowers/specs/2026-11-05-agent-core-design.md`
  - 3.6 (`:156`): add a bullet "Provider 接口（2026-11-16 修订，见 `2026-11-15-llm-tool-providers-design.md` 3.1）：Agent 只依赖 `net::LlmProvider`（`active_model()`、`complete()`），`LlmClient` 是它的 chat completions 实现；`LlmResult::error` 改为 `net::LlmError` 枚举，日志里的取值不变；`AgentDeps` 不再持有 `ModelProfileService`。"
  - 3.3 (`:125`): change "一轮开始时激活的模型名与上次校准的不同" to "一轮开始时激活的配置（id）或模型名与上次校准的不同（2026-11-16 修订）"
  - 2 (`:71`) architecture list: one line saying that the turn's loop lives in `TurnRunner` (`turn_runner.{hpp,cpp}`), lent the service's state through `TurnEnv` (spec 3.10 of the provider design)
- Modify: `docs/backlog.md`: delete `:231` (closed by Task 4). In `:230`, drop "nor for the 50/300 clamp of `est_scale_pct`" (now `Calibration.ObserveSetsTheClampedRatio`); keep the keep-1 half.
- Modify: `components/wisp_agent/include/wisp/agent/agent_service.hpp` header comment: one sentence that the turn itself runs in `TurnRunner` (`src/turn_runner.hpp`), and in `:27` "the active llm profile's context window" becomes "the provider's active model's context window (`net::LlmProvider::active_model()`)"

- [ ] **Step 1: Edit the documents** as listed. Run `grep -rn "LlmClient&\|deps.models\|scale_model" docs components` and fix any statement it shows to be stale (only `docs/` prose and comments; no code is expected to match).
- [ ] **Step 2: Run** `bash tools/ci.sh`. Expected: exit 0, with no `warning:` in the build part of the output.
- [ ] **Step 3: Build the firmware for both boards** (ESP-IDF v6.0.3):

```bash
cd firmware
for B in waveshare_esp32_s3_touch_amoled_2_16 waveshare_esp32_s3_touch_amoled_1_75c; do
  mkdir -p build-$B
  bash -c ". ~/esp/esp-idf-v6.0.3/export.sh >/dev/null 2>&1 && idf.py bmgr -c ../boards -b $B && idf.py -B build-$B -D SDKCONFIG=build-$B/sdkconfig -D SDKCONFIG_DEFAULTS='sdkconfig.defaults;components/gen_bmgr_codes/board_manager.defaults' -DWISP_BOARD=$B build" > build-$B/fw.log 2>&1
  echo "$B exit=$?"; tail -2 build-$B/fw.log; grep -c 'warning:' build-$B/fw.log
done
```

  Expected for each board: `exit=0`, "Project build complete", and a warning count of `0`. If the count is not 0, `grep warning: build-$B/fw.log` must show none from `components/`; record any from ESP-IDF itself in the commit body (the log stays inside the git-ignored build directory). If ESP-IDF is not installed, write "firmware build not run" in the commit body and add it to the device checks below.
- [ ] **Step 4: Commit** `docs: record the provider seam and TurnRunner`.

**Device checks (for the owner; not run by this plan):**
1. One spoken turn and one web turn with a tool call on each board: replies, tool calls, speech and the screen look as before.
2. `mem` during a spoken turn: the `wisp.agent` high-water mark against `docs/hardware/waveshare_esp32_s3_touch_amoled_2_16.md:467` (10112 B left before). `TurnRunner::run()` adds one frame level (spec 5), so a drop of a few hundred bytes is expected and more is not.
3. Two profiles with the same model name on different providers: switching between them starts the second one uncalibrated (log: no early summary on the first long turn).

---

## Self-review

- **Spec coverage (J1 scope).** 3.1: the header, `LlmError` and `to_string`, the moved types, `active_model()`, `LlmClient final : LlmProvider` and `AgentDeps` (Tasks 2-3). The comparisons at `agent_service.cpp:645, 776, 792-798` are made in Task 2. 3.10: `TurnEnv` as listed plus `now_ms()`, `TurnInput`, `TurnRunner` built per turn on `wisp.agent`'s stack, the ownership split, and `Calibration` keyed by profile and model with `Impl` (Tasks 4-5). Section 7: request-body goldens captured first (Task 1); `TurnRunner` tests over a `FakeProvider` for the J1 behaviour (Task 5); the `AgentLoop` tests keep passing over `LlmClient` and `FakeLlmServer` (every task). Section 9 step 1 and step 7's J1 documents (Task 6). Out of scope by design: `make_carry`, continuation, reasoning, `movable`, parts and images, `ToolProvider` (J2-J4). The Interfaces blocks of Tasks 2 and 5 say where those land.
- **Type consistency.** `LlmError`, `LlmModelInfo{profile_id, model, context_window, vision}`, `LlmResult::profile_id`, `Calibration::{begin_turn, observe, overflow, scaled, unscaled_window, pct}`, `TurnControls{watch, ticket, last_byte, in_request}`, `TurnInput{turn, serial, max_steps, start_ms, turn_ms, source}` and `FakeStep{deltas, calls, result, hold, during}` are used under the same names in every task.
- **Proportion.** Bodies are not written out. The moved code is named by its current lines, and new code by its signatures and tests.
