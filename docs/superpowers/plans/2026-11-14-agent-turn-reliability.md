# Agent Turn Reliability Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The model sees its tool results whole, a turn survives context overflow and flaky networks, and the user sees why a turn ended and can stop one on purpose but never by accident.

**Architecture:** Context assembly distinguishes the running turn (results sent whole, arguments whole) from earlier turns (results cut to 4 KB, large arguments elided). `LlmClient` retries transport failures that delivered no body and reports a provider's error code; the agent loop reacts to a context-length error by compressing harder and retrying once. `AgentEvent::TurnEnd` carries the turn's reason and note into `TurnView`, which Home shows. A hold records without stopping the turn; the transcript's submission stops it.

**Tech Stack:** C++20 (`-fno-exceptions -fno-rtti`), GoogleTest, LVGL 9.5 (shell), the simulator build (`build/sim`).

**Spec:** `docs/superpowers/plans/2026-11-14-architecture-optimization-roadmap.md` (phase A), with `docs/superpowers/specs/2026-11-05-agent-core-design.md` section 3 ("每条工具结果在**历史中**最多保留 4 KB") and `docs/superpowers/specs/2026-11-06-mvp-design.md` sections 4 and 6 as the behaviour being restored or amended. Task 9 records the amendments in those specs.

## Global Constraints

- Branch: `feat/agent-turn-reliability`, created from `main`. No commits on `main`.
- AGENTS.md applies: `wisp::Result<T>`, no exceptions/RTTI, `os::Large*` containers for anything that can exceed a few KB, comments explain why (no review ids, no change history), user-facing strings in zh/ja/en through `ui::StrId`.
- Every task ends with `cmake --build build/sim && ctest --test-dir build/sim --output-on-failure -R '<the task's tests>'` green; the last task runs `bash tools/ci.sh`.
- Commit messages: Conventional Commits, ending with the `Co-Authored-By` line of the session's attribution rules.
- The agent's outbound access and confirmation rules do not change in this plan.
- No API key may reach a log, an event, a `Message` or `LlmResult`.

## Review Focus

1. A turn whose single tool result is larger than the model's whole window (64 KB `scene.read_file` on a 32 K model): the loop must cut that result instead of failing every request. Test in Task 4.
2. A provider's 4xx body that quotes the Authorization header or the key: `LlmResult::detail` must not contain it. Test in Task 5.
3. A transport failure after the first delta arrived: no retry (the reply would be duplicated on screen and in speech). Test in Task 5.
4. A hold cancelled by an upward swipe while a web-started turn runs: the turn keeps running and its steps stay on Home. Test in Task 7.
5. A turn that ends in error while the Settings page is in front: the overlay shows the note; returning to Home within the fade time still shows it. Tests in Tasks 6 and 8.

---

### Task 1: Tool results of the running turn are sent whole

**Files:**
- Modify: `components/wisp_agent/include/wisp/agent/limits.hpp`
- Modify: `components/wisp_agent/src/context.cpp` (`Layout::bytes`, `Layout::emit`, `tool_text_bytes`)
- Modify: `components/wisp_agent/include/wisp/agent/context.hpp` (file header, item 2)
- Test: `tests/agent/context_test.cpp`

**Interfaces:**
- Produces: `inline constexpr size_t kLiveToolResultBytes = 96 * 1024;` in `limits.hpp` (the largest tool payload, `kReadMaxBytes`/`kHttpMaxMaxBytes` = 64 KB, plus JSON escaping and metadata).
- Produces: the rule "a `Role::Tool` message whose `turn` equals the last message's `turn` is cut to `kLiveToolResultBytes`; any other to `kToolResultKeepBytes`". The live turn is `history.back().turn` (0 for an empty history). `build_summary_request` keeps `kToolResultKeepBytes` for everything.

- [ ] **Step 1: Write the failing tests** in `tests/agent/context_test.cpp`. Rename `ToolResultsCutTo4KiBInHistory` to `EarlierTurnsToolResultsCutTo4KiB` and give it a second turn (`c.next_turn(); c.begin(); c.add(Role::User, "next");`) so its assertions hold for the first turn. Add:

```cpp
TEST(Context, RunningTurnToolResultIsSentWhole) {
    Conv c;
    c.next_turn();
    c.begin();
    c.add(Role::User, "read it");
    const std::string big(10000, 'x');
    c.call("c1", "scene.read_file", R"({"id":"a"})", big);  // no c.end(): the turn runs

    const BuiltRequest r = build_history(c.h());
    ASSERT_EQ(r.messages.size(), 3u);
    EXPECT_EQ(text(r.messages[2]), big);
}

TEST(Context, RunningTurnToolResultCutAtTheLiveCap) {
    Conv c;
    c.next_turn();
    c.begin();
    c.add(Role::User, "read it");
    const std::string huge(wisp::agent::kLiveToolResultBytes + 5000, 'y');
    c.call("c1", "http.fetch", "{}", huge);

    const BuiltRequest r = build_history(c.h());
    const std::string cut = text(r.messages[2]);
    EXPECT_TRUE(cut.ends_with("bytes total]"));
    EXPECT_LT(cut.size(), wisp::agent::kLiveToolResultBytes + 64);
}

TEST(Context, EstimateCountsTheLiveResultWhole) {
    Conv c;
    c.next_turn();
    c.begin();
    c.add(Role::User, "u");
    c.call("c1", "scene.read_file", "{}", std::string(30000, 'x'));
    EXPECT_GE(build_history(c.h()).est_tokens, 10000u);
}
```

- [ ] **Step 2: Run** `ctest --test-dir build/sim -R Context --output-on-failure`. Expected: the three new tests FAIL (result cut to 4096).
- [ ] **Step 3: Implement.** `Layout` stores `live_turn_` from `history.back().turn` in its constructor; `tool_text_bytes(const Message&)` becomes a non-static member that picks the cap by `m.turn == live_turn_`; `emit`'s `Role::Tool` case uses the same cap. Update the comment on `kToolResultKeepBytes` ("every tool result of an earlier turn") and item 2 of `context.hpp`'s header.
- [ ] **Step 4: Run** the same ctest filter. Expected: PASS, including the untouched `ToolResultCutKeepsWholeCharacters` (move it to an earlier turn if it relied on the old rule).
- [ ] **Step 5: Commit** `fix(agent): send the running turn's tool results whole`.

---

### Task 2: `docs.read` pages long topics

**Files:**
- Modify: `components/wisp_agent/src/docs.cpp`
- Modify: `components/wisp_agent/include/wisp/agent/limits.hpp`
- Modify: `components/wisp_agent/src/prompt.cpp:163` (one clause: "long topics come in pages; pass next_offset")
- Test: `tests/agent/docs_test.cpp`, `tests/agent/context_test.cpp` (`FixedPrefixIsStableAcrossRequests` stays green)

**Interfaces:**
- Produces: `inline constexpr size_t kDocsPageBytes = 16384;`
- Produces: `docs.read` arguments `{"topic": string, "offset"?: integer >= 0}`; result `{"ok":true,"topic","text","total_bytes":N}` plus `"next_offset":M` when text remains. A page ends on the last `'\n'` at or before `offset + kDocsPageBytes` (on a UTF-8 boundary when the page has no newline). `offset` past the end: `tool_error("bad_offset", ...)` with `total_bytes`.

- [ ] **Step 1: Write the failing tests**:

```cpp
TEST(Docs, LongTopicComesInPagesThatJoinToTheWhole) {
    const std::string whole = *wisp::agent::read_doc("lua");
    ASSERT_GT(whole.size(), wisp::agent::kDocsPageBytes);
    std::string joined;
    int64_t offset = 0;
    for (int page = 0; page < 8; ++page) {
        const Value r = run_docs("lua", offset);  // helper: calls the registered tool
        const std::string text = r.find("text")->as_string();
        EXPECT_LE(text.size(), wisp::agent::kDocsPageBytes);
        EXPECT_EQ(r.find("total_bytes")->as_int(), static_cast<int64_t>(whole.size()));
        joined += text;
        const Value* next = r.find("next_offset");
        if (next == nullptr) break;
        EXPECT_EQ(text.back(), '\n');
        offset = next->as_int();
    }
    EXPECT_EQ(joined, whole);
}

TEST(Docs, ShortTopicHasNoNextOffset) {
    const Value r = run_docs("widget/label", 0);
    EXPECT_EQ(r.find("next_offset"), nullptr);
}

TEST(Docs, OffsetPastTheEndIsAnError) {
    const Value r = run_docs("widget/label", 1 << 20);
    EXPECT_EQ(r.find("ok")->as_bool(), false);
    EXPECT_EQ(r.find("error")->as_string(), "bad_offset");
}
```

Write `run_docs(topic, offset)` in the test file following how `UnknownTopicListsTopics` already calls the tool.

- [ ] **Step 2: Run** `ctest --test-dir build/sim -R Docs --output-on-failure`. Expected: FAIL (no `total_bytes`).
- [ ] **Step 3: Implement** the paging in the tool's `run` lambda; add `"offset":{"type":"integer","minimum":0}` to `schema_json`; append "Long topics come in pages: pass the result's next_offset to read on." to the description.
- [ ] **Step 4: Run** `ctest --test-dir build/sim -R 'Docs|Context|ToolRegistry' --output-on-failure`. Expected: PASS.
- [ ] **Step 5: Commit** `feat(agent): page long docs.read topics`.

---

### Task 3: Large call arguments of earlier turns are elided

**Files:**
- Modify: `components/wisp_agent/include/wisp/agent/limits.hpp`
- Modify: `components/wisp_agent/src/context.cpp` (`Layout::bytes`, `Layout::emit`, Assistant case)
- Test: `tests/agent/context_test.cpp`

**Interfaces:**
- Produces: `inline constexpr size_t kCallArgsKeepBytes = 2048;`
- Produces: the rule "an assistant call of a turn other than the live turn whose `args` exceed `kCallArgsKeepBytes` is sent with arguments `{"elided_bytes":N}`" (valid JSON; N is the original size). Live-turn calls are sent as written. `build_summary_request` applies the same elision to every call.

- [ ] **Step 1: Write the failing tests**:

```cpp
TEST(Context, EarlierTurnsLargeCallArgsAreElided) {
    Conv c;
    c.next_turn();
    c.begin();
    c.add(Role::User, "write it");
    const std::string args = R"({"id":"a","content":")" + std::string(9000, 'z') + R"("})";
    c.call("c1", "scene.write_file", args, R"({"ok":true})");
    c.end();
    c.next_turn();
    c.begin();
    c.add(Role::User, "next");

    const BuiltRequest r = build_history(c.h());
    ASSERT_EQ(r.messages[1].tool_calls.size(), 1u);
    EXPECT_EQ(std::string(r.messages[1].tool_calls[0].arguments),
              R"({"elided_bytes":)" + std::to_string(args.size()) + "}");
}

TEST(Context, RunningTurnCallArgsStayWhole) {
    Conv c;
    c.next_turn();
    c.begin();
    c.add(Role::User, "write it");
    const std::string args = R"({"content":")" + std::string(9000, 'z') + R"("})";
    c.call("c1", "scene.write_file", args, R"({"ok":true})");
    const BuiltRequest r = build_history(c.h());
    EXPECT_EQ(std::string(r.messages[1].tool_calls[0].arguments), args);
}
```

- [ ] **Step 2: Run** `ctest --test-dir build/sim -R Context`. Expected: the first new test FAILS.
- [ ] **Step 3: Implement** in the Assistant cases of `bytes()` and `emit()` and in `build_summary_request`.
- [ ] **Step 4: Run** `ctest --test-dir build/sim -R 'Context|AgentLoop'`. Expected: PASS.
- [ ] **Step 5: Commit** `fix(agent): elide large call arguments of earlier turns`.

---

### Task 4: Context-overflow recovery and estimate calibration

**Files:**
- Modify: `components/wisp_agent/include/wisp/agent/context.hpp`, `components/wisp_agent/src/context.cpp`
- Modify: `components/wisp_agent/src/agent_service.cpp` (`Impl::loop`, around lines 599-662)
- Modify: `components/wisp_ui/include/wisp/ui/strings.hpp`, `components/wisp_ui/src/strings_{zh,ja,en}.cpp` (new `StrId::AgentErrContext`)
- Test: `tests/agent/context_test.cpp`, `tests/agent/agent_loop_test.cpp`

**Interfaces:**
- Consumes: `net::LlmResult::error == "context_length"` and `LlmResult::usage` (Task 5 produces the former; until Task 5 lands the loop test fakes it).
- Produces:
  - `std::optional<std::pair<uint64_t, uint64_t>> pick_compression(std::span<const Message> history, uint32_t window, size_t fixed_tokens = 0, uint32_t keep_recent = kKeepRecentTurns);`
  - `struct BuildOptions { size_t live_tool_bytes = kLiveToolResultBytes; };` and `BuiltRequest build_request(std::span<const Message>, const DeviceSnapshot&, BuildOptions = {});` (same extra parameter on `build_history`).
  - In `Impl`: `uint32_t est_scale_pct = 100;` (estimate multiplier, clamped to 50..300), updated after each successful request as `usage.prompt * 100 / max(1, req.est_tokens + tools_tokens)`.
  - New string `AgentErrContext`: zh "对话太长，模型装不下了。请开始新对话，或换用上下文更大的模型。", en "The conversation no longer fits the model. Start a new one or use a model with a larger context.", ja the equivalent.

Behaviour the loop must have, in order, before each request:
1. Estimate = `(req.est_tokens + tools_tokens) * est_scale_pct / 100`.
2. If the estimate needs compression and `pick_compression(..., kKeepRecentTurns)` finds a range: compress and rebuild (as today).
3. If it still needs compression: try `pick_compression(..., keep_recent = 1)`.
4. If it still exceeds 100% of the window: rebuild with `BuildOptions{.live_tool_bytes = kToolResultKeepBytes}` (the live turn's results are cut like old ones; their cut note tells the model to read in smaller pieces).
5. A reply with `error == "context_length"`: raise `est_scale_pct` to 150 if it is lower, run steps 2-4 treating the request as over the window, and retry once per turn; a second such error ends the turn with `AgentErrContext`.

- [ ] **Step 1: Write the failing tests.**

`context_test.cpp`:

```cpp
TEST(Context, CompressionCanReachThePreviousTurn) {
    // Three ended turns, each a user message and a 30 KB assistant reply.
    Conv c;
    uint64_t end_of_turn_2 = 0;
    for (int t = 1; t <= 3; ++t) {
        c.next_turn();
        c.begin();
        c.add(Role::User, "u");
        c.add(Role::Assistant, std::string(30000, 'a'));
        c.end();
        if (t == 2) end_of_turn_2 = c.last_seq();
    }
    EXPECT_FALSE(pick_compression(c.h(), 20000, 0, 3).has_value());
    const auto r = pick_compression(c.h(), 20000, 0, 1);
    ASSERT_TRUE(r.has_value());
    EXPECT_EQ(r->second, end_of_turn_2);
}

TEST(Context, LiveCapOptionCutsTheRunningTurn) {
    Conv c;
    c.next_turn();
    c.begin();
    c.add(Role::User, "u");
    c.call("c1", "scene.read_file", "{}", std::string(30000, 'x'));
    const BuiltRequest r =
        build_history(c.h(), {.live_tool_bytes = wisp::agent::kToolResultKeepBytes});
    EXPECT_LT(text(r.messages[2]).size(), wisp::agent::kToolResultKeepBytes + 64);
}
```

`agent_loop_test.cpp` (use the file's scripted-LLM fixture):

```cpp
TEST_F(AgentLoop, ContextLengthErrorCompressesAndRetriesOnce)
// script: turn 1 and 2 done; turn 3: reply 1 = error "context_length",
// then a summary reply, then a normal text reply.
// expect: TurnEnd status "done"; the history holds a Role::Summary;
// exactly one retried chat request after the summary request.

TEST_F(AgentLoop, SecondContextLengthErrorEndsTheTurnWithANote)
// script: two "context_length" errors in a row.
// expect: TurnEnd status "error"; last note == tr(StrId::AgentErrContext, "zh").

TEST_F(AgentLoop, OversizedLiveResultIsCutInsteadOfOverflowing)
// window 8192 tokens; a tool returns 60 KB; next request's tool message
// is shorter than kToolResultKeepBytes + 64 and the turn continues.

TEST_F(AgentLoop, UsageCalibratesTheEstimate)
// reply usage.prompt = 2x the bytes/3 estimate; with a window the plain
// estimate fits under 75% but the scaled one does not, the next request
// is preceded by a summary request.
```

Write each with the fixture's existing scripting calls and the assertions stated in its comment.

- [ ] **Step 2: Run** `ctest --test-dir build/sim -R 'Context|AgentLoop' --output-on-failure`. Expected: the new tests FAIL to compile or fail.
- [ ] **Step 3: Implement** `keep_recent`, `BuildOptions`, the string, and the loop behaviour above. Keep the existing `compressed` flag's meaning (one compression attempt per request) and add `bool overflow_retried` per turn.
- [ ] **Step 4: Run** the same filter plus `-R Strings`. Expected: PASS.
- [ ] **Step 5: Commit** `fix(agent): recover from context overflow and calibrate the token estimate`.

---

### Task 5: `LlmClient` retries bodiless transport failures and reports provider errors

**Files:**
- Modify: `components/wisp_net/include/wisp/net/llm_client.hpp`
- Modify: `components/wisp_net/src/llm_client.cpp` (the retry loop, lines ~285-350)
- Test: `tests/net/llm_client_test.cpp`

**Interfaces:**
- Produces in `LlmResult`: `std::string code;` (the provider's `error.code`, else `error.type`, at most 64 bytes, `[A-Za-z0-9_.-]` only) and the new `error` value `"context_length"`.
- Produces in `LlmClient`: `static constexpr size_t kErrorBodyBytes = 2048;` `static constexpr uint32_t kMaxRetryAfterMs = 30000;`

Rules:
- A non-2xx body is buffered up to `kErrorBodyBytes` (never logged, never stored), parsed as JSON for `error.code`, `error.type` and `error.message`, then discarded. `error = "context_length"` when the code is `context_length_exceeded`, or the status is 400/413 and the message contains "context length", "maximum context" or "too many tokens" (case-insensitive). Only `code`, filtered to the character set above, leaves the function.
- A transport error other than `Cancelled` with `!result.any_body` and time left is retried with the same `kRetryDelaysMs` schedule and the same attempt budget as 429/5xx (shared counter).
- On 429/5xx with a `Retry-After` header holding whole seconds, the wait is `min(header, kMaxRetryAfterMs, left_ms())` instead of the schedule's delay.

- [ ] **Step 1: Write the failing tests** (the file's `Fixture` scripts responses; replace `TransportErrorNotRetried`):

```cpp
TEST(LlmClient, BodilessTransportErrorIsRetried)
// script: transport error (no body), then a normal stream "hi".
// expect: error == "", text == "hi", attempts == 2, one sleep of 1000 ms.

TEST(LlmClient, TransportErrorAfterADeltaIsNotRetried)
// script: one delta "he", then a transport error.
// expect: error == "transport", any_body, attempts == 1, text == "he".

TEST(LlmClient, TransportRetriesShareTheBudgetWith5xx)
// script: transport error, 503, transport error.
// expect: attempts == 3, error == "transport".

TEST(LlmClient, RetryAfterSecondsReplaceTheBackoff)
// script: 429 with "Retry-After: 7", then a stream.
// expect: one sleep totalling 7000 ms (in kWaitSliceMs slices).

TEST(LlmClient, RetryAfterIsCapped)
// "Retry-After: 600" -> the wait totals kMaxRetryAfterMs.

TEST(LlmClient, ContextLengthCodeIsReported)
// 400 {"error":{"code":"context_length_exceeded","message":"..."}}
// expect: error == "context_length", code == "context_length_exceeded", attempts == 1.

TEST(LlmClient, ContextLengthIsRecognisedFromTheMessage)
// 400 {"error":{"type":"invalid_request_error","message":"This model's maximum context length is 32768 tokens"}}
// expect: error == "context_length".

TEST(LlmClient, ErrorBodyNeverLeaksTheKey)
// 400 {"error":{"code":"sk-test-key","message":"bad key sk-test-key"}} with the profile key "sk-test-key"
// expect: error == "http"; result.code does not contain "sk-test"; nothing else in LlmResult does.
```

For the last test, a `code` equal to or containing the profile's key is dropped (empty `code`).

- [ ] **Step 2: Run** `ctest --test-dir build/sim -R LlmClient --output-on-failure`. Expected: new tests FAIL.
- [ ] **Step 3: Implement.** Keep the comment's intent at `req.on_body` ("may quote the key") by scrubbing rather than by not reading. Update the header comment of `LlmResult::error` and the class comment that says transport errors are not retried.
- [ ] **Step 4: Run** `ctest --test-dir build/sim -R 'LlmClient|AgentLoop'`. Expected: PASS.
- [ ] **Step 5: Commit** `fix(net): retry bodiless transport failures and report provider error codes`.

---

### Task 6: Turn outcomes reach `TurnView` and Home

**Files:**
- Modify: `components/wisp_agent/include/wisp/agent/agent_service.hpp` (`AgentEvent`)
- Modify: `components/wisp_agent/src/agent_service.cpp` (`Outcome`, the places that call `note(..., &out)`, the `TurnEnd` emit near line 448)
- Modify: `components/wisp_shell/include/wisp/shell/conversation.hpp`, `components/wisp_shell/src/conversation.cpp` (`on_agent`, `Kind::TurnEnd`; `begin_view`)
- Modify: `components/wisp_shell/src/home.cpp` (`hint_text`, the hint bookkeeping next to `hint_error_`)
- Test: `tests/agent/agent_loop_test.cpp`, `tests/shell/conversation_test.cpp`, `tests/shell/home_test.cpp`

**Interfaces:**
- Produces in `AgentEvent` (TurnEnd): `std::string reason;` one of `""` (done), `"stopped"`, `"step_limit"`, `"time_limit"`, `"no_model"`, `"unauthorized"`, `"network"`, `"network_lost"`, `"server"`, `"context"`; and `std::string note;` the localised note text of the turn (`Outcome::last_text` when the status is not `"done"`, cut to 200 code points).
- Produces in `TurnView`: `std::string end_reason;` and `os::LargeString end_note;` (both cleared by `begin_view`).
- The web page is unchanged: it already receives the note as a `note` message and the status in the `turn_end` marker. `ConversationController` is the only consumer of `Kind::TurnEnd`.
- Home rule: after a turn ends with a non-empty `end_reason`, the hint line shows `end_note` for `kFadeAfterMs` from `ended_ms`; for `"no_model"` and `"unauthorized"` it goes through `with_url`. A voice error or not-heard hint that arrives later replaces it. Readiness hints keep priority.

- [ ] **Step 1: Write the failing tests.**

```cpp
// agent_loop_test.cpp
TEST_F(AgentLoop, TurnEndCarriesReasonAndNote)
// script an "unauthorized" reply -> the TurnEnd event has status "error",
// reason "unauthorized", note == tr(StrId::AgentErrUnauthorized, "zh").
TEST_F(AgentLoop, DoneTurnHasNoReason)
// reason.empty() && note.empty().
TEST_F(AgentLoop, StoppedTurnReasonIsStopped)

// conversation_test.cpp
TEST_F(ConversationTest, TurnEndReasonReachesTheView)
// feed TurnBegin, then TurnEnd{status "error", reason "server", note "服务器出错了"}
// -> current().end_reason == "server", std::string(current().end_note) == "服务器出错了",
//    !current().running.
TEST_F(ConversationTest, ANewTurnClearsTheLastOutcome)

// home_test.cpp
TEST_F(HomeTest, FailedTurnShowsItsNoteThenFades)
// view with end_reason "server", end_note "N", ended_ms = t:
// hint label text == "N" at t + 1000; empty at t + kFadeAfterMs.
TEST_F(HomeTest, UnauthorizedNoteCarriesTheAdminUrl)
// admin_url returns "http://192.168.1.5:8900" -> the hint ends with "\n" + that URL when idle.
TEST_F(HomeTest, DoneTurnShowsNoNote)
```

Write each with its fixture's existing event-feeding helpers and the assertions in the comment.

- [ ] **Step 2: Run** `ctest --test-dir build/sim -R 'AgentLoop|ConversationTest|HomeTest' --output-on-failure`. Expected: FAIL.
- [ ] **Step 3: Implement.** `Outcome` gains `const char* reason = "";` set next to each `note(why, turn, &out)` and in `finish_abort`. Check the hint on all three screen shapes in `home_test`'s existing shape loop if the hint's geometry tests live there.
- [ ] **Step 4: Run** the same filter plus `-R 'AgentWs|Admin'`. Expected: PASS.
- [ ] **Step 5: Commit** `feat(shell): show why a turn ended on the home screen`.

---

### Task 7: A hold no longer stops the running turn; an explicit stop control

**Files:**
- Modify: `components/wisp_shell/include/wisp/shell/conversation.hpp`, `components/wisp_shell/src/conversation.cpp` (`hold_begin`, `on_transcript`, new `stop_turn`)
- Modify: `components/wisp_shell/include/wisp/shell/home.hpp`, `components/wisp_shell/src/home.cpp` (the step list)
- Modify: `components/wisp_ui/include/wisp/ui/strings.hpp`, `strings_{zh,ja,en}.cpp` (`StrId::HomeStop`: "停止" / "停止" / "Stop")
- Test: `tests/shell/conversation_test.cpp`, `tests/shell/home_test.cpp`

**Interfaces:**
- Produces: `void ConversationController::stop_turn();` stops speech, stops the agent's running turn and drops a waiting voice text (the body of today's `default:` branch without the capture).
- Behaviour:
  - `hold_begin()` in `Phase::Speaking`/`Busy`/`Idle`: `voice.stop_speaking()`, then `start_capture(Hold)`. It no longer calls `agent.stop_turn()`.
  - `on_transcript(text)`: when a turn is running or queued, call `deps_.agent.stop_turn()` first, then submit (a stop drops waiting messages, so the order matters). An empty/not-heard result and `hold_cancel()` stop nothing.
  - Home: while `view.running`, the step list's last row is a "Stop" control (label `HomeStop`, touch target at least 48 px high, at least `theme::kGapSm` from the row above); a tap calls `conv_->stop_turn()`. It is absent when no turn runs.

- [ ] **Step 1: Write the failing tests.** Rewrite `HoldInterruptsSpeechAndRunningTurns` as:

```cpp
TEST_F(ConversationTest, HoldStopsSpeechButNotTheTurn)
// a web turn runs and speech plays; hold_begin():
// voice.stop_speaking called once; agent.stop_turn call count == 0; a capture began.
TEST_F(ConversationTest, TranscriptOfAHoldStopsTheOldTurnThenSubmits)
// continue: hold_end(), transcript "新的问题" ->
// call order: agent.stop_turn, then agent.submit("新的问题", Source::Voice).
TEST_F(ConversationTest, CancelledHoldLeavesTheTurnRunning)
// hold_begin(), hold_cancel() -> stop_turn count == 0; current().running stays true; steps kept.
TEST_F(ConversationTest, NotHeardHoldLeavesTheTurnRunning)
TEST_F(ConversationTest, StopTurnStopsSpeechAndTheAgent)
// stop_turn() -> voice.stop_speaking and agent.stop_turn each once.

// home_test.cpp
TEST_F(HomeTest, RunningTurnShowsAStopControlThatStopsIt)
// view.running with one step: a clickable object labelled tr(HomeStop) exists,
// height >= 48, y gap to the last step row >= theme::kGapSm; LV_EVENT_CLICKED -> stop_turn called.
TEST_F(HomeTest, NoStopControlWhenIdle)
```

- [ ] **Step 2: Run** `ctest --test-dir build/sim -R 'ConversationTest|HomeTest|ShellTalk' --output-on-failure`. Expected: FAIL.
- [ ] **Step 3: Implement.** Keep the `dropped`/`end_speech` bookkeeping of the old branch inside `stop_turn()`. Update the file-header contract in `conversation.hpp` that describes what a hold does. Verify the stop row on round, rounded and rect shapes (`row_span`) in the Home test's shape loop.
- [ ] **Step 4: Run** the same filter plus `-R 'RapidTriggers|AtMostTwoModelConnections'`. Expected: PASS (those two tests pin the no-overlap and two-connection rules this change must keep).
- [ ] **Step 5: Commit** `fix(shell): a hold keeps the running turn; add an explicit stop`.

---

### Task 8: The talk overlay on every screen but Home

**Files:**
- Modify: `components/wisp_shell/src/shell.cpp` (`update_talk`, lines ~190-214), `components/wisp_shell/include/wisp/shell/shell.hpp`
- Modify: `components/wisp_shell/src/talk_overlay.cpp`, `components/wisp_shell/include/wisp/shell/talk_overlay.hpp`
- Test: `tests/shell/shell_talk_test.cpp`, `tests/shell/talk_overlay_test.cpp`

**Interfaces:**
- Consumes: `TurnView::end_reason`, `end_note` (Task 6).
- Produces: `bool Shell::home_in_front() const;` (`lv_screen_active() == home_screen_`). `update_talk()` runs whenever `!home_in_front()`; `scene_in_front()` stays for its other callers.
- Produces in `TalkOverlay::decide()`: a phase `Note` shown for `kSayingMs` (the existing five seconds) after a turn ends with a non-empty `end_reason`, text `end_note`; and for a voice error, the same error string Home shows. The card's height follows its text up to three lines (today: fixed 56 px, one and a half lines with `LONG_DOT`).

- [ ] **Step 1: Write the failing tests.** Replace `HoldWithNoSceneShowsNothing` with `HoldOnHomeShowsNoOverlay` (same assertions, Home in front) and add:

```cpp
TEST_F(ShellTalkTest, HoldOnASubPageShowsTheOverlay)
// open the Settings page, hold: overlay visible, phase Listening.
TEST_F(ShellTalkTest, HoldOnDiagnosticsShowsTheOverlay)
TEST_F(ShellTalkTest, FailedTurnOnASubPageShowsItsNote)
// TurnEnd reason "network", note "N" while Settings is in front -> overlay text "N",
// hidden after kSayingMs.

// talk_overlay_test.cpp
TEST(TalkOverlay, LongSayGrowsToThreeLines)
// a say of ~60 CJK characters on the 466 round shape: card height > 56 and
// <= 3 * line height + padding; text not ending in "...".
TEST(TalkOverlay, VoiceErrorIsShown)
```

- [ ] **Step 2: Run** `ctest --test-dir build/sim -R 'ShellTalk|TalkOverlay' --output-on-failure`. Expected: FAIL.
- [ ] **Step 3: Implement.** The overlay stays inside `hal::safe_rect()` at its new height on all three shapes; reuse the existing shape cases of `talk_overlay_test`.
- [ ] **Step 4: Run** `ctest --test-dir build/sim -R 'Shell|TalkOverlay|PageDrag'`. Expected: PASS.
- [ ] **Step 5: Commit** `fix(shell): show the talk overlay on every screen but home`.

---

### Task 9: Documents, backlog and full check

**Files:**
- Modify: `docs/superpowers/specs/2026-11-05-agent-core-design.md` (section 3: the live-turn rule, the argument elision, overflow recovery; section on retries)
- Modify: `docs/superpowers/specs/2026-11-06-mvp-design.md` (sections 4 and 6: what a hold does, the stop control, the overlay on sub-pages, turn-end notes on Home)
- Modify: `docs/architecture.md` (if it describes `agent.state`/TurnEnd payloads or the retry rule)
- Modify: `docs/backlog.md`: remove the 4b entries already implemented (`wisp.tools.register`, headless runs), the "or build the firmware in CI" clause (CI builds both boards), and the "Talk overlay" entry about `decide()` having no error phase (closed by Task 8); add the device checks below.
- Modify: `CHANGELOG.md` (an `Unreleased` entry; the version bump happens at merge, AGENTS.md section 7)

- [ ] **Step 1:** Edit the documents above. Each amended spec paragraph states the new rule with its constant name.
- [ ] **Step 2:** Run `bash tools/ci.sh`. Expected: exit 0, no warnings in test output.
- [ ] **Step 3:** Run the simulator end to end against `tools/fake_openai.py`: a turn that reads `docs.read {"topic":"lua"}` twice with `next_offset`; a turn against a stopped fake server (Home shows the network note); a hold during a running turn followed by an upward swipe (the turn continues). Record what was observed in the commit body.
- [ ] **Step 4: Commit** `docs: record the turn reliability rules and prune the backlog`.

**Device checks for the owner (not run by the agent):**
- `mem` after a turn with a 64 KB `scene.read_file` result: `Mem::Large` peak and `wisp.agent` stack high-water mark.
- A real turn over weak Wi-Fi: the retry is invisible apart from the delay; no doubled text or speech.
- The Stop row and the taller overlay on the 1.75C round panel: touch target, spacing, no clipping.
- A provider 400 for an over-long context on the owner's gateway: the turn recovers (note which `error.code` the gateway sends).
