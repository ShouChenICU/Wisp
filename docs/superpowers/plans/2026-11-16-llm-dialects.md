# Dialects, Length Continuation and Rollover Carry (J2) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Five validated per-profile dialect options (with preset defaults) shape the chat-completions body. A reply cut by `finish_reason: "length"` is continued once. The 1 MiB rollover carries a summary of the old conversation into the new one as a `carry` message. A stored profile with no new option sends byte-identical requests.

**Architecture:** Steps 2 and 3 of spec section 9. `core::LlmDialect` (plain data in `wisp_core`) is resolved from the profile's options, then the preset's defaults, then the built-in defaults. `LlmClient` resolves it per call and passes it to `build_chat_body()`, which emits the output limit, `parallel_tool_calls` and `reasoning_content`, and merges movable system content into user messages under `system_msgs=first`. `TurnRunner` reads the dialect once per turn for the prompt wording and the reasoning keep. It also owns the length continuation and `make_carry()`. `AgentService::Impl::run_turn()` archives only after the carry request, and appends the `carry` line to the new conversation. `context.cpp` treats a carry as a live summary unit.

**Tech Stack:** C++20 (`-fno-exceptions -fno-rtti`), GoogleTest, the simulator build (`build/sim`), Preact/htm (web admin), Python 3 (repo checkers), ESP-IDF v6.0.3 (firmware build check only).

**Spec:** `docs/superpowers/specs/2026-11-15-llm-tool-providers-design.md` (approved 2026-11-16; all section 12 proposals adopted). This plan implements 3.2, 3.3, 3.4, 3.7 and 3.8, the J2 parts of sections 5-8, section 9 steps 2-3, and step 7's documents for them. It builds on plan J1, `docs/superpowers/plans/2026-11-16-llm-provider-seam.md`, which is assumed fully implemented. The names below come from J1's Interfaces blocks: `net::LlmProvider`, `LlmModelInfo`, `LlmCall`, `LlmResult`, `LlmError`, `TurnRunner`, `TurnEnv`, `TurnInput`, `TurnControls`, `Outcome`, `Calibration`, `FakeProvider`, `FakeStep`, and the `TurnRunner` fixture with its `FakeTurnEnv`.

## Global Constraints

- Work in the worktree `.superpowers/worktrees/llm-providers`, branch `feat/llm-providers`. Never push, merge, switch branches or `git stash`. Never add `.claude/`, `.playwright-mcp/` or `.superpowers/`. Delete files only by literal absolute paths inside the worktree. Other plan files in `docs/superpowers/plans/` are not this plan's to edit.
- Only one process may build in `build/sim` at a time. Configure once with `cmake -S simulator -B build/sim -G Ninja` if it does not exist yet.
- **Default dialect = today's bytes.** The J1 goldens run in every task: `export GOLDEN='LlmBodyGolden|AgentTurnGolden'` once per shell. **No J1 golden may change.** All of them use the `custom` preset with no options and call `build_chat_body()` without a dialect, so each resolves to `LlmDialect{}`, the dialect that keeps today's body. If a J1 golden needs editing, the task has a bug.
- Existing non-golden tests that change on purpose. These are the only ones:
  - `AdminAi.CrudActivateAndTestRoutes` (`tests/admin/routes_ai_test.cpp:81-85`): the `openai` preset row gains `"defaults":{"out_field":"max_completion_tokens"}` (Task 1).
  - Nothing else. `AgentLoop.RolloverArchivesBeforeTheNextTurn` (`tests/agent/agent_loop_test.cpp:1367-1399`) must stay **unedited and green**. It pins the no-carry path (plan decision 10).
- AGENTS.md applies in full. In particular:
  - Layering (`tools/check_layering.py`): `LlmDialect` lives in `wisp_core`, and `wisp_net` and `wisp_agent` use it. `wisp_shell` names `agent::Role::Carry` only, and `wisp_app` does the same.
  - Large buffers use `os::LargeString`/`os::LargeVector`/`os::alloc(Mem::Large)`: the request body, the reasoning text (`LlmResult::reasoning`, the per-turn keep), the partial reply held for a continuation, and the carry transcript and text. None of them is a `std::string` that can grow past a few KB.
  - Only `wisp.fs` touches flash. The carry append goes through `TurnEnv::append()`/`Impl::append()` (the Interactive lane), and the archive through `run_io`, as today. Reasoning and the partial reply are never stored.
  - API keys stay write-only. No new log line carries a key, an option value, reasoning text or reply text. Log lines carry metadata only (spec 4.4 "Logs" applies to the new agent lines too).
  - TDD: write the failing test first, run it and see it fail for the stated reason (a missing symbol counts), then implement. Test output must be clean, with no warnings.
  - Comments explain why. They contain no review ids, no "Task N", no "round N" and no `.superpowers/` paths (`tools/ci.sh` greps for these). Point at spec sections instead ("spec 3.7").
  - User-facing text in zh/ja/en: device notes go in `components/wisp_ui/src/strings_{zh,ja,en}.cpp`, web labels in `web/app/i18n/{zh,ja,en}.js` (`tools/check_i18n.py`). New zh/ja note text must be in the CJK font's character set: `gen_cjk_font_test` in `tools/ci.sh` fails otherwise. In that case rerun `tools/gen_cjk_font.py` as its header says and list "flash the fonts partition" under the device checks.
  - Conventional Commits. Every commit message ends with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- `-Werror=missing-field-initializers` is on. Every aggregate this plan adds or extends (`LlmDialect`, `PresetOption`, `ModelPreset`, `ResolvedProfile`, `LlmMessage`, `LlmModelInfo`, `LlmResult`, `ReasoningRef`, `BuildOptions`, `Message`) gives every new member a default member initializer. `kPresets` rows spell the new `defaults` member explicitly in every row (`{}` for none), so the constexpr table does not depend on the compiler's handling of omitted members.
- Every task ends with `cmake --build build/sim 2>&1 | tee build/sim-build.log | tail -3` and `grep -c 'warning:' build/sim-build.log` (expected `0`; `build/` is git-ignored), then the focused `ctest --test-dir build/sim -R '<filter>' --output-on-failure` that the task names. The last task runs `bash tools/ci.sh` and the firmware build for both boards.
- No version bump or `CHANGELOG.md` entry (J1 plan decision 11: phase J is released as one merge).
- Device checks are listed for the owner and never reported as done.

## Review Focus

The five failures most likely to reach a user that no single feature test would catch. Each one is pinned by the test named here, in the task that owns the code.

1. **A stored profile's request changes by a byte after the upgrade.** This happens if a preset default leaks onto `custom`, or if an unset option emits a field, or if `movable` changes the `anywhere` shape. Pinned by the unchanged J1 goldens (every task), `LlmDialectBody.DefaultDialectIsTodaysBody` and `LlmDialectBody.AnywhereIgnoresMovable` (Task 2), and `LlmClient.CustomProfileWithoutOptionsSendsTodaysBody` (Task 2).
2. **A profile and its API key disappear at boot because one option is bad.** This is the old `load_models()` rule. Hand edits and, later, a downgrade-then-upgrade with options from a newer firmware can both trigger it. Pinned by `ModelProfiles.LoadDropsOnlyTheBadOption`, `ModelProfiles.LoadKeepsAProfileWhoseOptionsAreNotAMap` and `ModelProfiles.SevenOptionsSurviveAReload` (Task 1).
3. **A continued reply shows or stores the seam wrong.** Possible forms: the text is doubled on screen, a second `Output` state appears, the continuation loops, a stop during it loses the partial text, or the cut-reply note replaces the done notice. Pinned by `TurnRunner.LengthReplyIsContinuedOnce`, `TurnRunner.SecondCutStoresTheTextWithANote`, `TurnRunner.StopDuringTheContinuationKeepsTheText` (Task 5) and `AgentLoop.ContinuedReplyIsOneMessageOnTheWire` (Task 5).
4. **A rollover that fails leaves the user stuck or loses data.** Possible forms: the carry request blocks archiving forever, a stop during it archives anyway, the web chat clears the new turn's bubble because `Cleared` arrives after `TurnBegin`, or an older firmware cannot read the file. Pinned by `AgentLoop.FailedCarryStillRollsOver`, `AgentLoop.StopDuringTheCarryKeepsTheConversation`, `AgentLoop.RolloverCarriesASummary` (event order) (Task 7), the unchanged `AgentLoop.RolloverArchivesBeforeTheNextTurn`, and `Conversation.UnknownRoleLineIsDropped` (Task 6).
5. **Reasoning text leaks or grows without bound.** Possible forms: it lands in `current.jsonl`, it reaches a later turn's request, it is duplicated by a retry, or one huge reasoning block makes PSRAM use unbounded. Pinned by `TurnRunner.ReasoningEchoedOnlyWithinTheTurn` (the store holds no reasoning), `ReasoningKeep.DropsTheOldestPastTheCap` (Task 4), and `LlmClient.ReasoningIsNotDuplicatedByARetry` and `LlmClient.ReasoningOverTheCapIsDropped` (Task 2).

## Plan decisions (where the spec is silent, imprecise or contradicts itself)

1. **`LlmDialect` is `core::LlmDialect` in `model_profiles.hpp`; `llm_provider.hpp` aliases it** (`using LlmDialect = core::LlmDialect;`) and includes `wisp/core/model_profiles.hpp`, as J1 plan decision 3 anticipated. Resolution is `core::resolve_llm_dialect(options, preset_id)`, a pure function. `ResolvedProfile` gains `std::string preset{}` (last member) so that `LlmClient::complete()` can resolve the dialect without a second lock.
2. **The dialect is resolved in two places.** `LlmClient` resolves it per call from the profile it is about to use, and builds the body from it. `TurnRunner` resolves it once per turn from `active_model()`, for the prompt wording and for whether to keep reasoning. A profile switch in the middle of a turn can leave the prompt's wording out of step with the body for the rest of that turn. The window has the same property today, and the next turn is consistent again. Accepted.
3. **`system_msgs=first` emits a content array of text parts**, `[{"type":"text","text":"<context>\n…\n</context>"},…,{"type":"text","text":<user text>}]`. Spec 3.4 says "text part", and the wire form of parts is spec 3.5's. J3 adds image parts to the same array. A user message that receives no merged content stays a plain string.
4. **Merge rule.** Each movable system message goes into the first user message after it, or else into the last user message before it. With no user message at all it stays `system` (this cannot happen in a turn). A target's parts are emitted in this order: the movable messages that came after it (the dynamic info: "its first part", spec 3.4), then those before it in message order, then its own text. Messages are marked movable by origin, not by position: summaries, the carry and the dynamic info are movable, and the fixed prompt never is. In every request `context.cpp` builds, this is exactly spec 3.4's "every system message after the first".
5. **Field placement.** New body fields follow `"tools"`: `,"parallel_tool_calls":<bool>` (only when `tools_json` is non-empty), then `,"max_tokens":N` or `,"max_completion_tokens":N`. `"reasoning_content"` is the last key of an assistant message. These are the spec's fields (spec 3.2), placed at the end so that a body without them is today's body.
6. **The `prompt.cpp` wording under `first`** adds one sentence telling the model that `<context>` blocks are written by the device, not by the user (exact text in Task 3). This is needed because the security rules say that only the user's messages and the system prompt instruct the model. The `anywhere` prompt is byte-identical to today's.
7. **Reasoning at the client.** `reasoning_content` deltas never set `any_body`, because they reach no listener and a retry loses nothing. `LlmResult::reasoning` is cleared at the start of each attempt, so a retry cannot duplicate it. One reply's reasoning past `LlmClient::kMaxReasoningBytes = 64 KiB` (the turn cap) is dropped whole: a cut reasoning block is not echoed. The keep in `TurnRunner` (`ReasoningKeep`) applies the turn cap `kReasoningKeepBytes = 64 KiB`, oldest dropped first (spec decision 3).
8. **Continuation details (spec 3.7).**
   - The second request reuses the same built request ("the usual messages"; it is not rebuilt or recompressed) plus the two extra messages. Under `echo` the partial assistant message carries the first reply's reasoning.
   - Only the first request's usage feeds `Calibration`. Both count in `out.tokens`.
   - A cut reply with empty text and no tool calls is not continued; the note follows.
   - An abort (stop, Wi-Fi, stall, time) during the continuation stores the text so far and ends through `finish_abort()`, without `AgentReplyCut`.
   - The `AgentReplyCut` note does not replace `out.last_text`, which keeps the reply for the done notice.
   - A continuation refused as `context_length` is a failed continuation: no overflow retry.
9. **Carry signature and timing.** `make_carry(const TurnInput& in, std::span<const Message> history)`. Spec 3.10 shows no `TurnInput`, but the request's timeout comes from the turn's limit (`in.start_ms`, `in.turn_ms`). `in.turn` is unused (0). `make_carry` reads `active_model()` itself and calls `cal.begin_turn()` for that pair. Its usage does not count in the turn's tokens and does not calibrate.
10. **The carry transcript.** It holds every live summary unit (summaries and a carry) first, then the newest whole turns while the transcript's estimate stays within `cal.unscaled_window(window) * kCarryInputPercent / 100` tokens (`kCarryInputPercent = 50`). It stops at the first turn that does not fit. An empty transcript makes no request: the rollover archives with today's `AgentRolledOver` note, and no `Thinking` state is shown. This is exactly what `AgentLoop.RolloverArchivesBeforeTheNextTurn` sets up, since its only turn is a 1 MiB note, so that test stays unedited.
11. **Rollover order in `run_turn()`.** Spec 3.8 says "after the wake locks and Thinking, before turn_begin". It also says that a stop "ends the turn as usual before archiving". And `archive()` emits `Cleared`, which must precede `TurnBegin`, or the web chat wipes the new turn's bubble. The order is therefore:
   - the turn's limits, start time, check timer and wake locks;
   - then, if `needs_rollover()`: `make_carry()`, which sets `Thinking` only when it sends a request;
   - if `aborted()` now: no archive. The turn is framed in the old conversation, and `run()` ends it at once through `finish_abort()`. The next turn tries the rollover again;
   - else `archive()` (`Cleared`), then the turn number, `TurnBegin`, the carry line (seq 1, turn 0), the note `AgentRolledOverCarry` (or `AgentRolledOver` without a carry), the `turn_begin` marker, and the user message.
   A non-abort carry failure (no model, `network_up()` false before the request, an error, empty text) archives without a carry (spec 3.8 "Failures"). Wi-Fi loss or a stall *during* the carry request is an abort, so it ends the turn before archiving, like a stop.
12. **`ConversationStore::archive()` returns `Result<uint32_t>`**: the archive's number, or 0 when the conversation was empty and nothing was archived. The carry's `from` needs it. `Impl::archive(uint32_t* n = nullptr)` passes it on.
13. **The load fix (spec 3.2).** Each stored option is checked alone by a new `check_option(key, value, kind)`, the per-key half of `check_options()`. A failing key is dropped, and so is a non-string value. A non-map `options` is dropped whole and the profile is kept. If more than `kMaxOptions` valid keys remain, the first 8 in key order are kept. Each profile that lost options logs one warning, `profile <id>: dropped <n> invalid options`, which names no key or value. The file is rewritten by the next change, as before.
14. **`GET /ai/presets`** always carries `"defaults"` (`{}` for none). The addition is additive, as spec 3.2 says. The web page reads the defaults from there and has no table of its own.
15. **The Shell's `group_turns()`** skips `Role::Carry` in its early `continue` (`history.cpp:57-58`). Its `switch` already has `default:`, so no case is needed.
16. **The web page's built-in defaults.** The page states the built-in values (`max_tokens`, `drop`, `anywhere`, "not sent") as display text for "(default: …)". They mirror `LlmDialect{}`, like the page's other mirrored limits (`models.js:103-115`). No setting schema is involved.

---

### Task 1: Dialect options, validation, preset defaults and the load fix (`wisp_core`)

**Files:**
- Modify: `components/wisp_core/include/wisp/core/model_profiles.hpp`
  - `ModelPreset` (`:81-91`): add `std::span<const PresetOption> defaults{}`
  - the options comment (`:97-102`): llm dialect options
  - after `:111`: the new constants, enums, `LlmDialect` and `resolve_llm_dialect()`
  - `ResolvedProfile` (`:238-245`): add `std::string preset{}`
  - `to_value(const ModelPreset&)` comment (`:369`): `defaults`
- Modify: `components/wisp_core/src/model_presets.cpp:19-35`: a `defaults` value in every row; `openai` → `{{"out_field","max_completion_tokens"}}`, `anthropic` → `{{"system_msgs","first"}}`, the rest `{}`
- Modify: `components/wisp_core/src/model_profiles.cpp`
  - `kKindOptions` (`:253-259`): five llm rows
  - split `check_options()` (`:261-294`) into `check_option()` plus the count check
  - `resolve_llm_dialect()`
  - `to_value(const ModelPreset&)` (`:435-447`)
  - `load_models()`'s options block (`:555-567`)
  - `active_for_request()` (`:889-895`) and the prober target (`:882`) fill `preset`
- Test: `tests/core/model_profiles_test.cpp` (append), `tests/admin/routes_ai_test.cpp:81-85` (the planned edit)

**Interfaces:**
- Produces (`namespace wisp::core`):

```cpp
struct PresetOption { std::string_view key{}; std::string_view value{}; };

// llm dialect options (spec 3.2). Each is llm-only; absent = today's request.
inline constexpr const char* kOptOutTokens = "out_tokens";       // decimal kMinOutTokens..kMaxOutTokens
inline constexpr const char* kOptOutField = "out_field";         // "max_tokens" | "max_completion_tokens"
inline constexpr const char* kOptReasoning = "reasoning";         // "drop" | "echo"
inline constexpr const char* kOptSystemMsgs = "system_msgs";     // "anywhere" | "first"
inline constexpr const char* kOptParallelTools = "parallel_tools";  // "true" | "false"
inline constexpr uint32_t kMinOutTokens = 256;
inline constexpr uint32_t kMaxOutTokens = 131072;

enum class LlmOutField : uint8_t { MaxTokens, MaxCompletionTokens };
enum class LlmReasoning : uint8_t { Drop, Echo };
enum class LlmSystemMsgs : uint8_t { Anywhere, First };

// The resolved dialect; LlmDialect{} is today's request.
struct LlmDialect {
    std::optional<uint32_t> out_tokens{};
    LlmOutField out_field = LlmOutField::MaxTokens;
    LlmReasoning reasoning = LlmReasoning::Drop;
    LlmSystemMsgs system_msgs = LlmSystemMsgs::Anywhere;
    std::optional<bool> parallel_tools{};
    bool operator==(const LlmDialect&) const = default;
};
// The profile's option, else the preset's default, else LlmDialect{}. An unknown
// preset id has no defaults. Options are trusted to be valid (check_option()).
LlmDialect resolve_llm_dialect(const ModelOptions& options, std::string_view preset_id);
```

  `to_value(const ModelPreset&)` adds `{"defaults": {key: value, ...}}`. `ResolvedProfile::preset` is the profile's preset id.
- Internal (`model_profiles.cpp`, anonymous namespace): `Result<void> check_option(std::string_view key, std::string_view value, ModelKind kind)`. It checks the key syntax and length, the value length, control characters and UTF-8, the `vision`/`context_window` forms, and the `kKindOptions` kind and form. `check_options()` becomes the count check plus `check_option()` for each pair, with the same messages as today. New messages: `"out_tokens must be a whole number from 256 to 131072"`, `"out_field must be \"max_tokens\" or \"max_completion_tokens\""`, `"reasoning must be \"drop\" or \"echo\""`, `"system_msgs must be \"anywhere\" or \"first\""`, `"parallel_tools must be \"true\" or \"false\""`. The wrong-kind messages follow the existing pattern, `"<key> is only valid on an llm profile"`.

- [ ] **Step 1: Write the failing tests** (`tests/core/model_profiles_test.cpp`; the fixture helpers `Fixture`, `fx.make(llm(...))`, `Fixture::write`, `fx.start()` exist):

```cpp
TEST(ModelProfiles, DialectOptionsValidated)
// Accepted on an llm profile (update and create): out_tokens "256", "131072"; out_field both values;
// reasoning both; system_msgs both; parallel_tools "true"/"false".
// Refused with InvalidArgument and the message above: out_tokens "255", "131073", "1e3", "", "-512",
// "99999999999"; out_field "max"; reasoning "keep"; system_msgs "last"; parallel_tools "yes".
TEST(ModelProfiles, DialectOptionsAreLlmOnly)
// Each of the five keys on an asr and on a tts profile: InvalidArgument,
// message "<key> is only valid on an llm profile".
TEST(ModelProfiles, SevenOptionsFit)
// create llm with vision, context_window and the five: ok; get() returns all seven.
TEST(ModelProfiles, SevenOptionsSurviveAReload)
// The profile above, persisted, reloaded by a fresh service: same options, has_key true.
TEST(ModelProfiles, ResolveDialectOrder)
// resolve_llm_dialect({}, "custom") == LlmDialect{};
// ({}, "openai").out_field == MaxCompletionTokens, everything else default;
// ({}, "anthropic").system_msgs == First;
// ({{"out_field","max_tokens"}}, "openai").out_field == MaxTokens;
// ({{"system_msgs","anywhere"}}, "anthropic").system_msgs == Anywhere;
// ({{"out_tokens","1024"},{"parallel_tools","false"},{"reasoning","echo"}}, "custom")
//   == {1024, MaxTokens, Echo, Anywhere, false}; ({}, "nope") == LlmDialect{}.
TEST(ModelProfiles, PresetDefaultsAreValidAndOnlyTwo)
// For every preset: each default passes as an llm option (create an llm profile on that preset
// with base_url "https://h.example/v1" and options = its defaults: ok). Presets with a non-empty
// defaults span: exactly {"openai", "anthropic"}.
TEST(ModelProfiles, PresetValueCarriesDefaults)
// to_value(*find_model_preset("openai")).find("defaults") == Map{{"out_field","max_completion_tokens"}};
// for "custom": an empty Map.
TEST(ModelProfiles, ActiveForRequestNamesThePreset)
// llm profile on "deepseek": active_for_request(Llm)->preset == "deepseek".
TEST(ModelProfiles, LoadDropsOnlyTheBadOption)
// models.json with one llm profile (preset openai, api_key "sk-1") whose options are
// {"vision":"maybe","context_window":"8192","reasoning":"echo","bad key":"x","out_tokens":7}
// (7 a number, not a string). Loaded: the profile is kept, has_key true, active;
// options == {{"context_window","8192"},{"reasoning","echo"}}.
TEST(ModelProfiles, LoadDropsAWrongKindOption)
// tts profile with {"voice":"alloy","out_tokens":"512"}: options == {{"voice","alloy"}}.
TEST(ModelProfiles, LoadKeepsAProfileWhoseOptionsAreNotAMap)
// "options":"x": profile kept, options empty.
TEST(ModelProfiles, LoadKeepsAtMostEightOptions)
// ten valid unknown keys "k0".."k9" (value "v"): the first eight in key order kept ("k0".."k7").
```

  Update `AdminAi.CrudActivateAndTestRoutes` (`routes_ai_test.cpp:81-85`): the expected `openai` map gains `{"defaults", Value(Map{{"out_field", Value("max_completion_tokens")}})}`. `ModelProfiles.LoadDropsInvalidEntries` (`:664-680`) is unchanged and must stay green: none of its bad entries is a bad option.

- [ ] **Step 2: Run** `cmake --build build/sim`. Expected: compile errors (`resolve_llm_dialect`, `kOptOutTokens`, `ResolvedProfile::preset` undeclared).
- [ ] **Step 3: Implement** as in Interfaces. `load_models()` uses `check_option()` per key on a fresh map and counts what it drops (plan decision 13). The profile-level `ok` no longer depends on options. `active_for_request()` and the prober target at `:882` copy `s->profile.preset`. In `model_presets.cpp`, define `constexpr PresetOption kOpenAiDefaults[] = {{"out_field", "max_completion_tokens"}}` and `kAnthropicDefaults[] = {{"system_msgs", "first"}}` above `kPresets`.
- [ ] **Step 4: Run** `ctest --test-dir build/sim -R "ModelProfiles|AdminAi|ModelProbe|$GOLDEN" --output-on-failure`. Expected: PASS.
- [ ] **Step 5: Commit** `feat(core): llm dialect options with preset defaults; drop bad stored options one by one`. The body says that it closes `docs/backlog.md:85`; Task 9 removes the line.

---

### Task 2: The body per dialect and the reasoning stream (`wisp_net`)

**Files:**
- Modify: `components/wisp_net/include/wisp/net/llm_provider.hpp` (J1)
  - include `wisp/core/model_profiles.hpp`, add `using LlmDialect = core::LlmDialect;`
  - `LlmMessage`: add `reasoning`, `movable`
  - `LlmModelInfo`: add `dialect`
  - `LlmResult`: add `reasoning`
- Modify: `components/wisp_net/include/wisp/net/llm_client.hpp`
  - `build_chat_body()` (`:122-127`): the dialect parameter and its comment
  - `LlmClient`: `kMaxReasoningBytes`
  - header comment (`:3-33`): the dialect, and "reasoning is never logged"
- Modify: `components/wisp_net/src/llm_client.cpp`
  - `append_message()` (`:43-78`): `reasoning_content`
  - `StreamReader` (`:103-219`): a `keep_reasoning` flag and `reasoning_content`
  - `build_chat_body()` (`:302-329`)
  - `complete()` (`:335-491`): resolve the dialect, pass it to the body and the reader, clear `reasoning` per attempt
  - `active_model()` (J1): `.dialect`
- Modify: `tests/support/fake_provider.hpp` (J1). `Recorded::body` is built with the current model's dialect. `complete()` clears `result.reasoning` unless that dialect is `Echo`, as the client does.
- Test: `tests/net/llm_client_test.cpp` (append; new suite `LlmDialectBody`)

**Interfaces:**
- Consumes: `core::LlmDialect`, `core::resolve_llm_dialect()`, `ResolvedProfile::preset` (Task 1).
- Produces (`namespace wisp::net`):

```cpp
struct LlmMessage {           // J1 fields unchanged; new:
    std::string_view reasoning{};  // Assistant only; sent only under LlmReasoning::Echo
    bool movable = false;          // System only: content the dialect may move (spec 3.4)
};
struct LlmModelInfo { /* J1 fields */ LlmDialect dialect{}; };
struct LlmResult { /* J1 fields */ os::LargeString reasoning; };  // kept only under Echo

// Plan decisions 3-5. LlmDialect{} gives today's bytes.
os::LargeString build_chat_body(std::string_view model, std::span<const LlmMessage> msgs,
                                std::string_view tools_json, const LlmDialect& dialect = {});

class LlmClient final : public LlmProvider {
    // One reply's reasoning past this is dropped whole (plan decision 7).
    static constexpr size_t kMaxReasoningBytes = 64 * 1024;
};
```

- What J3 adds here (not in this plan): image parts in the same content array as the merged `<context>` parts. The builder's per-user-message part list is the place they go.

- [ ] **Step 1: Write the failing tests** (`tests/net/llm_client_test.cpp`). Compare raw strings. `kMsgs` is `EveryMessageShape`'s message list (J1 Task 1), lifted to a file-level helper.

```cpp
TEST(LlmDialectBody, DefaultDialectIsTodaysBody)
// str(build_chat_body("m", kMsgs, tools, LlmDialect{})) == str(build_chat_body("m", kMsgs, tools)).
TEST(LlmDialectBody, OutTokensUsesTheNamedField)
// {.out_tokens = 1024}: today's body with its final '}' replaced by `,"max_tokens":1024}`;
// with out_field MaxCompletionTokens: `,"max_completion_tokens":1024}`.
// {.out_field = MaxCompletionTokens} alone (no out_tokens): equals today's body.
TEST(LlmDialectBody, ParallelToolsOnlyWithTools)
// {.out_tokens = 512, .parallel_tools = false} with tools: ends `,"tools":<tools>,"parallel_tool_calls":false,"max_tokens":512}`;
// {.parallel_tools = true} with tools_json "": no "parallel_tool_calls".
TEST(LlmDialectBody, ReasoningSentOnlyUnderEcho)
// Assistant "A" with reasoning "r\"1"; assistant with calls (EveryMessageShape's c3) and reasoning "r2".
// Drop: no "reasoning_content" anywhere. Echo: `{"role":"assistant","content":"A","reasoning_content":"r\"1"}`
// and `...,"tool_calls":[{"id":"c3",...}],"reasoning_content":"r2"}`. Assistant without reasoning under Echo: unchanged.
TEST(LlmDialectBody, FirstMergesMovablesIntoUserMessages)
// System "P"; System movable "S1"; User "u1"; Assistant "a1"; User "u2"; System movable "D".
// messages == [{"role":"system","content":"P"},
//  {"role":"user","content":[{"type":"text","text":"<context>\nS1\n</context>"},{"type":"text","text":"u1"}]},
//  {"role":"assistant","content":"a1"},
//  {"role":"user","content":[{"type":"text","text":"<context>\nD\n</context>"},{"type":"text","text":"u2"}]}]
TEST(LlmDialectBody, FirstPutsTheDynamicInfoFirst)
// System "P"; User "u1"; System movable "S"; User "u2"; Assistant calls c3; Tool c3; System movable "D".
// u2's content == [<context>D</context>, <context>S</context>, "u2"] (each wrapped as above).
TEST(LlmDialectBody, FirstWithoutAUserKeepsSystem)
// System "P"; System movable "S": identical to the Anywhere body.
TEST(LlmDialectBody, AnywhereIgnoresMovable)
// The FirstMergesMovables messages under LlmDialect{}: identical to the same messages with movable false.
```

```cpp
TEST(LlmClient, CustomProfileWithoutOptionsSendsTodaysBody)
// Fixture profile (preset custom, no options): requests()[0].body == str(build_chat_body("m", msgs, tools)).
TEST(LlmClient, SendsTheResolvedDialect)
// update(options {out_tokens "512"}): body ends `"max_tokens":512}`.
// update(preset "openai", same options): `"max_completion_tokens":512}`.
// options {out_tokens "512", out_field "max_tokens"} on openai: `"max_tokens":512}`.
TEST(LlmClient, ActiveModelResolvesTheDialect)
// preset anthropic: active_model()->dialect.system_msgs == First; with option system_msgs "anywhere": Anywhere.
TEST(LlmClient, ReasoningKeptUnderEcho)
// options {reasoning "echo"}; SSE deltas {"reasoning_content":"th"}, {"reasoning_content":"ink"},
// {"content":"hi"}: r.reasoning == "think", r.text == "hi", on_text saw only "hi".
TEST(LlmClient, ReasoningIgnoredUnderDrop)
// Same stream, no option: r.reasoning empty; on_bytes called as often as under echo.
TEST(LlmClient, ReasoningIsNotDuplicatedByARetry)
// echo; attempt 1: one reasoning delta "r" then the connection drops (transport, no content);
// attempt 2: reasoning "r", content "ok". r.attempts == 2, r.reasoning == "r", r.any_body true.
TEST(LlmClient, ReasoningOverTheCapIsDropped)
// echo; reasoning deltas totalling kMaxReasoningBytes + 1: r.reasoning empty, r.text intact.
```

  Use the fixture's existing fake-server helpers for SSE chunks and transport drops, as the `LlmClient` retry tests do.
- [ ] **Step 2: Run** `cmake --build build/sim`. Expected: compile errors (no `build_chat_body` overload taking a dialect; `LlmResult::reasoning` undeclared).
- [ ] **Step 3: Implement.**
  - `build_chat_body()` first finds each movable message's target (plan decision 4). This is one pass over the user indices, and only under `First`; under `Anywhere` there is no extra pass. It then writes the messages. A user message that has merged parts writes `"content":[...]` with `append_json_string` for each part's text, which is built as `<context>\n` + text + `\n</context>`. Each part is written in pieces straight into the body, so the wrapped text is never copied whole.
  - The `estimate` adds `m.reasoning.size()` and 40 bytes for each movable message.
  - `complete()`: `const LlmDialect dialect = core::resolve_llm_dialect(profile->options, profile->preset);` before the body. `StreamReader` takes `bool keep_reasoning`. `read_delta()` appends a string `reasoning_content` while the total stays within `kMaxReasoningBytes`; past that it clears the reasoning and stops keeping it for this reply.
  - The log line is unchanged.
- [ ] **Step 4: Run** `python3 tools/check_layering.py && ctest --test-dir build/sim -R "LlmDialectBody|LlmClient|SseParser|$GOLDEN|TurnRunner|AgentLoop" --output-on-failure`. Expected: PASS. The goldens are untouched: `git diff -- tests/agent/agent_turn_golden_test.cpp` is empty, and `git diff -- tests/net/llm_client_test.cpp` shows only appended tests and the `kMsgs` lift.
- [ ] **Step 5: Commit** `feat(net): build the chat body per dialect and keep reasoning under echo`.

---

### Task 3: Movable context and the prompt's `system_msgs` wording (`wisp_agent` context)

**Files:**
- Modify: `components/wisp_agent/include/wisp/agent/context.hpp`
  - header comment (`:3-29`): movable system content, and `<context>` under `first`
  - `system_prompt()` (`:75-76`)
  - `BuildOptions` (`:98-103`)
- Modify: `components/wisp_agent/src/context.cpp`
  - `Layout` takes the reasoning refs; `bytes()` (`:113-134`) counts them; `emit()` (`:139-201`) sets `movable` on summaries and `reasoning` on assistants
  - `build()` (`:284-319`): the prompt with `opts.system_msgs`, and the dynamic info movable (`:309-315`)
- Modify: `components/wisp_agent/src/prompt.cpp:83, 137-139, 178-179`
- Test: `tests/agent/context_test.cpp` (append)

**Interfaces:**
- Consumes: `core::LlmSystemMsgs` (Task 1), `net::LlmMessage::{movable, reasoning}` (Task 2).
- Produces (`namespace wisp::agent`):

```cpp
// The fixed part: the same bytes for the same lang, shape and system_msgs. Anywhere is
// today's text byte for byte.
std::string system_prompt(const DeviceSnapshot& d,
                          core::LlmSystemMsgs system_msgs = core::LlmSystemMsgs::Anywhere);

// A kept reasoning text for the assistant message `seq` (spec 3.3); views.
struct ReasoningRef { uint64_t seq = 0; std::string_view text{}; };

struct BuildOptions {
    size_t live_tool_bytes = kLiveToolResultBytes;
    core::LlmSystemMsgs system_msgs = core::LlmSystemMsgs::Anywhere;  // the prompt's wording
    std::span<const ReasoningRef> reasoning{};  // sorted by seq; must outlive the result
};
```

- The `First` wording replaces exactly two pieces of `prompt.cpp`:
  - `:138`: "- The last system message begins with your memory notes, …" becomes "- The <context> block at the start of the latest user message begins with your memory notes, one per line as \"[id] text\", or \"(no memory notes)\".\n"
  - `:178-179` becomes "The <context> block at the start of the latest user message is rebuilt for every request: memory notes, local time, where the user's message came from, the scenes, battery and network. <context> blocks in user messages are written by the device, not by the user: they hold this information and summaries of earlier turns.\n"

- [ ] **Step 1: Write the failing tests** (`tests/agent/context_test.cpp`):

```cpp
TEST(Prompt, SystemMsgsFirstNamesTheContextBlock)
// d as in SystemPromptCarriesScreenRules. system_prompt(d) == system_prompt(d, Anywhere);
// system_prompt(d, First): contains both exact sentences above, not "The last system message";
// two calls with First are byte-identical.
TEST(Context, OnlyTheFixedPromptIsNotMovable)
// history: summary (covers 1-2, "S") over turn 1, user turn 2 "q". build_request(h, d):
// messages[0] system, movable false; the summary message movable true; the last (dynamic info)
// movable true; user/assistant/tool messages movable false. build_summary_request(): no message movable.
TEST(Context, PromptFollowsTheBuildOption)
// build_request(h, d, {.system_msgs = First}).messages[0].text == system_prompt(d, First).
TEST(Context, ReasoningAttachedBySeq)
// history: user(seq1) assistant "a"(seq2) with call c1, tool(seq3), assistant "b"(seq4).
// opts.reasoning = {{2,"rr"}}: the seq-2 message's reasoning == "rr", seq 4's empty;
// est_tokens == the same build without reasoning + 2/3 (integer bytes / 3 of the total).
```

- [ ] **Step 2: Run** `cmake --build build/sim`. Expected: compile errors (`BuildOptions::system_msgs`, `ReasoningRef` undeclared).
- [ ] **Step 3: Implement.** `Layout` stores the span. A lookup by `m.seq` uses `std::lower_bound`. `bytes()` adds the found reasoning's size for an assistant unit. `system_prompt()` switches only the two pieces above.
- [ ] **Step 4: Run** `ctest --test-dir build/sim -R "Context|Prompt|$GOLDEN|TurnRunner|AgentLoop" --output-on-failure`. Expected: PASS (`AgentTurnGolden.ContextSourceBody` unchanged: `anywhere` ignores `movable`).
- [ ] **Step 5: Commit** `feat(agent): mark movable system content and word the prompt per system_msgs`.

---

### Task 4: `TurnRunner` follows the dialect: prompt wording and the reasoning keep

**Files:**
- Create: `components/wisp_agent/src/reasoning_keep.hpp` (header-only, internal)
- Modify: `components/wisp_agent/include/wisp/agent/limits.hpp`: `kReasoningKeepBytes`
- Modify: `components/wisp_agent/src/turn_runner.{hpp,cpp}` (J1). `run()` keeps the `LlmModelInfo` it already reads once per turn. `build()` passes `{.system_msgs = model.dialect.system_msgs, .reasoning = keep.refs()}` along with the existing `live_tool_bytes`. After each stored reply, when `model.dialect.reasoning == Echo` and `res.reasoning` is non-empty, `keep.add(seq, res.reasoning)` runs with the seq that `env.append()` returned.
- Test: `tests/agent/turn_runner_test.cpp` (append). The fixture gains `ContextSource context` (empty by default) passed to the runner, and `FakeProvider` models with a dialect.

**Interfaces:**
- Consumes: `BuildOptions::{system_msgs, reasoning}`, `ReasoningRef` (Task 3), `LlmModelInfo::dialect`, `LlmResult::reasoning` (Task 2).
- Produces:

```cpp
// limits.hpp: the reasoning of the running turn kept for echo (spec 3.3, decision 3).
inline constexpr size_t kReasoningKeepBytes = 64 * 1024;

// reasoning_keep.hpp (namespace wisp::agent): one turn's kept reasoning, PSRAM, by seq.
class ReasoningKeep {
public:
    explicit ReasoningKeep(size_t cap = kReasoningKeepBytes);
    // Ignored for seq 0 or empty text; a text larger than the cap alone is not kept.
    // Then the oldest entries go until the total is within the cap.
    void add(uint64_t seq, std::string_view text);
    std::span<const ReasoningRef> refs() const;  // ascending seq; valid until the next add()
    size_t bytes() const;
private:
    os::LargeVector<std::pair<uint64_t, os::LargeString>> items_;
    os::LargeVector<ReasoningRef> refs_;
    size_t cap_, bytes_ = 0;
};
```

  A `ReasoningKeep` is a local of `run()`, so it ends with the turn (spec 3.3).

- [ ] **Step 1: Write the failing tests** (`tests/agent/turn_runner_test.cpp`):

```cpp
TEST(ReasoningKeep, DropsTheOldestPastTheCap)
// cap 10: add(1,"aaaa"), add(2,"bbbb"), add(3,"cccc") -> seqs {2,3}, bytes 8;
// add(4, 11 bytes) -> unchanged {2,3}; add(0,"x") and add(5,"") -> unchanged.
TEST_F(TurnRunner, ReasoningEchoedOnlyWithinTheTurn)
// model dialect reasoning Echo. Turn 1: step 1 call c1 test_echo "{}" finish ToolCalls with
// result.reasoning "r1"; step 2 deltas {"ok"}. calls()[1].body contains "\"reasoning_content\":\"r1\"".
// Turn 2 (a new runner): step deltas {"x"}: its body has no "reasoning_content".
// The store's lines hold no "r1": every Message text/args in h.store->history() lacks it.
TEST_F(TurnRunner, ReasoningNotKeptUnderDrop)
// Default dialect; the same turn 1: calls()[1].body has no "reasoning_content".
TEST_F(TurnRunner, PromptFollowsTheDialect)
// context returns a fixed DeviceSnapshot; model dialect system_msgs First.
// calls()[0].body contains "The <context> block at the start of the latest user message" and
// exactly one "\"role\":\"system\"".
```

- [ ] **Step 2: Run** `cmake --build build/sim`. Expected: compile error (`reasoning_keep.hpp` missing).
- [ ] **Step 3: Implement** as in Interfaces.
- [ ] **Step 4: Run** `ctest --test-dir build/sim -R "ReasoningKeep|TurnRunner|$GOLDEN|AgentLoop" --output-on-failure`. Expected: PASS.
- [ ] **Step 5: Commit** `feat(agent): echo a turn's reasoning and word the prompt per dialect`.

---

### Task 5: Continue a reply cut by the length limit

**Files:**
- Modify: `components/wisp_agent/src/turn_runner.cpp`. In the moved `loop()` body, at today's "reply without tool calls" point (`agent_service.cpp:805-828` before J1), add `kContinuePrompt` and the continuation inside the scope that holds `req` and the `output` flag (`:743-755`).
- Modify: `components/wisp_ui/include/wisp/ui/strings.hpp` (after `AgentContextDropped`, `:224`), `components/wisp_ui/src/strings_{en,zh,ja}.cpp`: `AgentReplyCut`
- Test: `tests/agent/turn_runner_test.cpp`, `tests/agent/agent_loop_test.cpp` (append)

**Interfaces:**
- Consumes: J1's `request()`, `whole()`, `finish_abort()`, the note helper; `ReasoningKeep` (Task 4).
- Produces: `ui::StrId::AgentReplyCut`. Its texts:
  - en "The reply was cut off by the model's length limit"
  - zh "回复因模型的长度上限被截断"
  - ja "モデルの長さの上限で返答が途中で切れました"

  Constant in `turn_runner.cpp`: `constexpr std::string_view kContinuePrompt = "Your previous reply was cut off by the length limit. Continue exactly where it stopped. Do not repeat anything.";` (spec 3.7).
- Behaviour (spec 3.7 and plan decision 8). The continuation happens when `whole(res) && res.finish == Length && res.tool_calls.empty() && !res.text.empty()`. If `aborted()` holds at that point, the partial text is stored and the turn goes to `finish_abort()`. Otherwise one more `request()` goes out with `req.messages` plus `{Assistant, text = partial, reasoning = first reasoning under Echo}` and `{User, kContinuePrompt}`, both built in a `LargeVector` copy of the message views. It uses the same `on_text`, so the deltas continue and `Output` is not set again. The stored assistant is `partial + cont.text`, with `cont.tool_calls` if the continuation succeeded. Under `Echo`, its kept reasoning is the first reasoning plus the continuation's. Then:
  - the continuation is `whole`, finished, and not `Length` without calls: the normal path follows (tool calls run with the existing `length` rule);
  - it ended with `Length` without calls, or failed without an abort: the text so far is stored, then the note `AgentReplyCut` (without touching `out.last_text`), and the turn ends `"done"`;
  - it failed by abort: the text so far is stored, then `finish_abort()`.
  
  A cut reply with empty text and no calls is not continued: the note follows and the turn ends `done`. `compress()` and `make_carry()` never continue.

- [ ] **Step 1: Write the failing tests:**

```cpp
// tests/agent/turn_runner_test.cpp
TEST_F(TurnRunner, LengthReplyIsContinuedOnce)
// steps: deltas {"Hel"} finish Length usage total 10; deltas {"lo"} finish Stop usage total 5.
// out.status "done", out.steps 0, out.tokens 15, out.last_text "Hello"; exactly one stored assistant,
// text "Hello"; env trace: one "state output", Delta "Hel", Delta "lo"; fake.calls().size() == 2;
// calls()[1].body contains R"({"role":"assistant","content":"Hel"},{"role":"user","content":")"
// + kContinuePrompt text + R"("}])".
TEST_F(TurnRunner, SecondCutStoresTheTextWithANote)
// both steps finish Length: stored assistant "Hello", then a note tr(AgentReplyCut, Zh);
// status "done"; out.last_text "Hello"; 2 calls.
TEST_F(TurnRunner, FailedContinuationStoresTheTextWithANote)
// step 2 result.error Server: as above.
TEST_F(TurnRunner, ContinuationToolCallsRun)
// tool test.echo; step 2: call c1 test_echo "{}" finish ToolCalls; step 3 deltas {"ok"}.
// stored assistant "Hel" with calls {c1}; the tool ran; out.steps 1; 3 calls.
TEST_F(TurnRunner, ContinuationReasoningJoins)
// dialect Echo; the ContinuationToolCallsRun script with reasoning "r1" (step 1) and "r2" (step 2):
// calls()[1].body has the partial assistant with "reasoning_content":"r1"; calls()[2].body has
// "reasoning_content":"r1r2" on the stored assistant.
TEST_F(TurnRunner, StopDuringTheContinuationKeepsTheText)
// step 2 hold true, deltas {"l"}; env stopped from step 2's `during`. Stored assistant "Hell";
// note AgentStopped, no AgentReplyCut; status "stopped".
TEST_F(TurnRunner, EmptyCutReplyIsNotContinued)
// step 1 deltas {} finish Length: one call; note AgentReplyCut; status "done".
TEST_F(TurnRunner, LengthWithToolCallsIsNotContinued)
// step 1 call c1 finish Length; step 2 deltas {"ok"}: calls()[1] holds the tool message
// (bad_args) and no kContinuePrompt.
TEST_F(TurnRunner, SummaryRequestsAreNotContinued)
// window 4096 setup of WindowComesFromTheProvider (J1); the summary step finishes Length with "S":
// a Summary "S" is stored; no recorded body contains kContinuePrompt.
```

```cpp
// tests/agent/agent_loop_test.cpp, over LlmClient and FakeLlmServer
TEST_F(AgentLoop, ContinuedReplyIsOneMessageOnTheWire)
// replies: SSE "Hel" with finish_reason "length", then "lo" with "stop". The WS-visible events:
// Deltas "Hel","lo", one Message for the assistant; history has one assistant "Hello" and no note;
// request 1's messages end with the assistant "Hel" and the user continue prompt.
```

- [ ] **Step 2: Run** `cmake --build build/sim`. Expected: compile error (`StrId::AgentReplyCut` missing). Before the logic is in, temporarily add only the `StrId` and confirm that `TurnRunner.LengthReplyIsContinuedOnce` fails, with one call where two are expected.
- [ ] **Step 3: Implement** as in Interfaces.
- [ ] **Step 4: Run** `ctest --test-dir build/sim -R "TurnRunner|AgentLoop|Strings|$GOLDEN" --output-on-failure`. Expected: PASS. `AgentLoop.LengthCutoffWithPartialToolCallIsBadArgs` (`agent_loop_test.cpp:790`) is unchanged and green.
- [ ] **Step 5: Commit** `feat(agent): continue a reply cut by the length limit once`.

---

### Task 6: The `carry` role in the store, the context, the Shell and the wire

**Files:**
- Modify: `components/wisp_agent/include/wisp/agent/conversation.hpp`
  - roles comment (`:10-20`): `carry text, from`
  - `Role` (`:54`): `Carry`
  - `Message` (`:69-81`): `from`
  - `archive()` (`:118-121`): `Result<uint32_t>`
- Modify: `components/wisp_agent/src/conversation.cpp`
  - the line writer (`:82-139`), `role_from()` (`:144-151`), `parse()` (`:189-231`), `to_string()` (`:263-279`)
  - `archive()` (`:477-501`) returns `n`
  - `to_value()` (`:568-604`)
- Modify: `components/wisp_agent/include/wisp/agent/context.hpp`: `build_carry_request()`, and `kCarryInputPercent` in `limits.hpp`
- Modify: `components/wisp_agent/src/context.cpp`
  - `kCarryLead`, `kCarryPrompt`
  - `Layout`: a carry is a summary unit (`:83-95`); `bytes()`/`emit()` use its lead
  - `build_summary_request()`'s line writing becomes a helper shared with `build_carry_request()`
- Modify: `components/wisp_agent/src/agent_service.cpp`: `Impl::archive(uint32_t* n = nullptr)` (`:300-315`); `run()`'s New command (`:412`) is unchanged
- Modify: `components/wisp_shell/src/history.cpp:57-58`: skip `Role::Carry`
- Modify: `components/wisp_app/src/agent_wiring.cpp:86-93`: a carry is `hidden`
- Test: `tests/agent/conversation_test.cpp`, `tests/agent/context_test.cpp`, `tests/shell/history_test.cpp`, `tests/app/agent_wiring_test.cpp` (append)

**Interfaces:**
- Produces (`namespace wisp::agent`):

```cpp
enum class Role : uint8_t { User, Assistant, Tool, Note, Summary, Marker, Carry };  // "carry"
struct Message { /* ... */ uint32_t from = 0; };  // carry: the archive number it summarizes
// The archive's number; 0 when the conversation was empty and nothing was archived.
Result<uint32_t> ConversationStore::archive();

// limits.hpp
inline constexpr uint32_t kCarryInputPercent = 50;  // spec 3.8, decision 10
inline constexpr size_t kCarryMaxBytes = 4096;

// context.hpp. The rollover's carry request (spec 3.8): kCarryPrompt, then one transcript in
// build_summary_request()'s line format: every live summary unit, then the newest whole turns
// while the transcript's bytes / 3 stay within budget_tokens. No messages when the transcript
// is empty. The history must outlive the result.
BuiltRequest build_carry_request(std::span<const Message> history, size_t budget_tokens);
```

- Line: `{"seq":1,"ts":…,"role":"carry","turn":0,"text":"…","from":3}`. `to_value()` gives `{seq, ts, turn, role:"carry", text, from}`.
- In the context, a carry is a unit with `summary = true`, `first = last = its seq`, lead `"Summary of the previous conversation:\n"`, sent as a movable `system` message. A later summary whose range covers its seq replaces it, because a carry is not `Role::Summary` and `covering()` hides it like any covered message. `pick_compression()` sees it as a summary group: it does not count as a turn, and a lone carry is never re-summarized. `build_summary_request()` quotes it as `Earlier summary: …`.
- `kCarryPrompt` (exact): "Summarize the conversation transcript in the next message for a new conversation that starts from this summary: the transcript will be archived and only your summary carried over. Keep what still matters: what the user asked for and decided, their preferences, the scenes created or changed (ids, files, versions), problems left open and anything promised. Leave out greetings and tool output that no longer matters. Write plain sentences in the language of the conversation, at most 300 words, without <say> tags or Markdown."

- [ ] **Step 1: Write the failing tests:**

```cpp
// tests/agent/conversation_test.cpp
TEST_F(Conversation, CarryRoundTrips)
// append {role Carry, turn 0, text "C\"ü", from 3}: the file's line ==
// R"({"seq":1,"ts":<ts>,"role":"carry","turn":0,"text":"C\"ü","from":3})" "\n"; a fresh store's
// load() gives role Carry, text, from 3, turn 0.
TEST_F(Conversation, UnknownRoleLineIsDropped)
// A file of user (seq 1), {"seq":2,"ts":0,"role":"from_the_future","turn":1,"text":"x"},
// assistant (seq 3): load() keeps seqs 1 and 3. This is how an older firmware reads a carry (spec 3.8).
TEST_F(Conversation, ArchiveReturnsItsNumber)
// empty store: archive() == 0; after one append: 1; after another append: 2.
TEST_F(Conversation, CarryToValue)
// to_value(carry, 200): role "carry", text, from 3, turn 0.

// tests/agent/context_test.cpp
TEST(Context, CarryLeadsTheHistory)
// h = [carry seq1 t0 "C", user t1 "q"]; build_request(h, d): messages[1] == System
// "Summary of the previous conversation:\nC", movable true; est counts it.
TEST(Context, CarryIsFoldedIntoALaterSummary)
// h = [carry 1, turn 1 (user 2, assistant 3), summary seq 4 covers 1-3 "S", turn 2 user 5]:
// no message holds "C"; the summary "S" stands first.
TEST(Context, CarryIsASummaryForCompression)
// h = [carry 1 (3000 bytes), turns 1-3 small]: pick_compression(h, 4096) never counts the carry
// as one of the kept turns, and h = [carry, turn 1] alone gives nullopt.
TEST(Context, SummaryRequestQuotesTheCarry)
// build_summary_request(h, 1, 3) transcript starts "Earlier summary: C".
TEST(Context, CarryRequestHasSummariesThenTheNewestTurns)
// h = [summary covers 1-2 "S" over turn 1, turns 2-4 each ~2500 bytes]; budget 2048 tokens (6144 bytes):
// messages == {System kCarryPrompt, User transcript}; the transcript starts "Earlier summary: S",
// holds turns 3 and 4 and not turn 2; build has no tools.
TEST(Context, CarryRequestIsEmptyWhenNothingFits)
// h = [one turn with a 1 MiB note]: messages empty.

// tests/shell/history_test.cpp
TEST(GroupTurns, SkipsTheCarry)
// [carry t0 "C", user t1 "q", assistant t1 "a"]: one turn, turn 1, nothing holds "C".

// tests/app/agent_wiring_test.cpp
TEST(AgentWiring, CarryIsHidden)
// agent_message_value(carry, 200): hidden true, role "carry", from present.
```

- [ ] **Step 2: Run** `cmake --build build/sim`. Expected: compile errors (`Role::Carry`, `Message::from`, `build_carry_request` undeclared).
- [ ] **Step 3: Implement** as in Interfaces. `conversation_test.cpp`'s existing `ASSERT_TRUE(store->archive())` calls compile unchanged. `Impl::archive()` keeps its `Result<void>` shape for `queue_new`'s `done->finish()` and writes the number through `n`.
- [ ] **Step 4: Run** `ctest --test-dir build/sim -R "Conversation|Context|GroupTurns|HistoryTest|AgentWiring|AgentRoutes|$GOLDEN|AgentLoop" --output-on-failure`. Expected: PASS.
- [ ] **Step 5: Commit** `feat(agent): a carry role for the summary a rollover keeps`.

---

### Task 7: `make_carry()` and the rollover that carries

**Files:**
- Modify: `components/wisp_agent/src/turn_runner.{hpp,cpp}`: `make_carry()`
- Modify: `components/wisp_agent/src/agent_service.cpp`: `run_turn()` (today's `:422-461`, after J1) in the order of plan decision 11
- Modify: `components/wisp_ui/include/wisp/ui/strings.hpp` (after `AgentRolledOver`, `:222`), `components/wisp_ui/src/strings_{en,zh,ja}.cpp`: `AgentRolledOverCarry`
- Test: `tests/agent/turn_runner_test.cpp`, `tests/agent/agent_loop_test.cpp` (append)

**Interfaces:**
- Consumes: `build_carry_request()`, `kCarryInputPercent`, `kCarryMaxBytes`, `Role::Carry`, `Message::from`, `Impl::archive(uint32_t*)` (Task 6); `Calibration::{begin_turn, unscaled_window}` (J1).
- Produces:

```cpp
// turn_runner.hpp. The rollover's summary (spec 3.8). Uses in.start_ms and in.turn_ms only.
// nullopt: no model, network_up() false, an empty transcript (no request, no Thinking state),
// an error, an empty reply, or an abort (the caller tells that last case apart by
// TurnEnv::abort_reason()). The text is cut to kCarryMaxBytes at a character boundary.
std::optional<os::LargeString> make_carry(const TurnInput& in, std::span<const Message> history);
```

  `ui::StrId::AgentRolledOverCarry` texts:
  - en "The conversation got long, so it was archived; a summary of it was kept in the new one"
  - zh "对话太长，已存档，并在新对话中保留了摘要"
  - ja "会話が長くなったため保存し、その要約を新しい会話に残しました"

  Logs:
  - `agent: carry <n> bytes` after a successful carry
  - `agent: carry skipped: <no_model|network|empty|<LlmError word>>` when there is none
  
  Neither carries text.
- `run_turn()` after this task (plan decision 11):
  1. `max_steps`/`turn_ms`/`start`/`turn_start`/`last_byte`/`in_turn`/timer/wake locks (moved above the rollover).
  2. `TurnRunner runner(...)`.
  3. `if (store.needs_rollover()) { carry = runner.make_carry({.start_ms = start, .turn_ms = turn_ms}, *store.history()); if (!aborted()) rolled = archive(&archived); }`.
  4. The turn number and `running_turn`.
  5. `TurnBegin`.
  6. `if (rolled) { if (carry) { append({Carry, turn 0, text, from = archived}); note(AgentRolledOverCarry, turn); } else note(AgentRolledOver, turn); }`.
  7. The marker, the user message, `Thinking`, `runner.run(...)`.
  
  The rest is unchanged. When `archive()` fails the carry is discarded, and no note is written (today's behaviour on that path).

- [ ] **Step 1: Write the failing tests:**

```cpp
// tests/agent/turn_runner_test.cpp (history built with the fixture's store)
TEST_F(TurnRunner, MakeCarrySummarizesTheLiveContext)
// h: summary "S" over turn 1, turns 2-3 small; fake step deltas {"C"}. Result "C";
// calls()[0].tools_json == ""; its body has the kCarryPrompt system message and a user transcript
// starting "Earlier summary: S"; env trace has "state thinking"; nothing appended.
TEST_F(TurnRunner, MakeCarryCutsAtACharacterBoundary)
// reply 4095 x 'a' + "ü": result size 4095. Reply 5000 x 'a': size 4096.
TEST_F(TurnRunner, MakeCarryFailures)
// Each in a fresh fixture: no model -> nullopt, no call; env.net false -> nullopt, no call;
// result.error Server -> nullopt; deltas {} -> nullopt; h of one 1 MiB-note turn -> nullopt, no call,
// no "state thinking".
TEST_F(TurnRunner, MakeCarryIsNotContinued)
// deltas {"C"} finish Length: "C", one call.
TEST_F(TurnRunner, MakeCarryBudgetIsHalfTheWindow)
// model window 4096, cal at 100: turns of ~2500 bytes: the transcript holds the newest two only.
```

```cpp
// tests/agent/agent_loop_test.cpp. A helper seeds the old conversation on the worker, as
// RolloverArchivesBeforeTheNextTurn does: turn 1 a note of kRolloverBytes + 10 bytes, turn 2
// user "q2" and assistant "a2".
TEST_F(AgentLoop, RolloverCarriesASummary)
// replies: text_reply("C"), text_reply("ok"). Request 0 has no "tools" and holds "User: q2";
// history: [0] carry seq 1 turn 0 text "C" from 1; [1] note tr(AgentRolledOverCarry, Zh);
// then marker, user "hi". Events in order: State thinking, Cleared, TurnBegin turn 1.
// Request 1's messages hold a system message "Summary of the previous conversation:\nC".
// needs_rollover() false.
TEST_F(AgentLoop, FailedCarryStillRollsOver)
// replies: FakeReply{.status = 400}, text_reply("ok"). History: [0] note tr(AgentRolledOver, Zh);
// no carry; turn ends done; needs_rollover() false.
TEST_F(AgentLoop, StopDuringTheCarryKeepsTheConversation)
// carry reply held (chunks {sse_text("C")}, hold_after 1); wait for server.held() == 1; stop_turn().
// The turn ends "stopped"; needs_rollover() still true; the store's archives() is empty;
// the stopped turn's user message is in the old conversation with turn 3.
```

  `AgentLoop.RolloverArchivesBeforeTheNextTurn` stays unedited (plan decision 10).
- [ ] **Step 2: Run** `cmake --build build/sim`. Expected: compile error (`make_carry`, `StrId::AgentRolledOverCarry` missing).
- [ ] **Step 3: Implement** as in Interfaces. `make_carry()`:
  - read `active_model()`; check `env.network_up()`; `cal.begin_turn(profile_id, model)`;
  - `budget = cal.unscaled_window(window) * kCarryInputPercent / 100`; `build_carry_request()`;
  - `env.set_state(Thinking)`; `request(messages, "", nullptr)`;
  - `whole(res) && !res.text.empty()` gives the text, cut with the `prefix_bytes()` rule (`context.cpp:41-46`; share the helper through J1's internal header instead of a third copy).
- [ ] **Step 4: Run** `ctest --test-dir build/sim -R "TurnRunner|AgentLoop|AgentTurnGolden|Conversation|Strings" --output-on-failure --repeat until-fail:2`. Expected: PASS twice.
- [ ] **Step 5: Commit** `feat(agent): carry a summary of the old conversation across the rollover`.

---

### Task 8: The web Models page's advanced LLM group (zh/ja/en)

**Files:**
- Modify: `web/app/pages/models.js`
  - constants (`:103-115`): `MIN_OUT_TOKENS = 256`, `MAX_OUT_TOKENS = 131072`, `DIALECT_KEYS`, built-in defaults (plan decision 16)
  - the `Preset` typedef (`:53`): `defaults`
  - state (`:547-553`): one `dialect` object, plus whether the advanced group starts open
  - `dirty` (`:597`), `check()` (`:636-641`), `save()` (`:650-652`, `:680-686`)
  - the llm fields (`:1192-1234`): a `<details class="model-advanced">` after the context window field, built like the tts group (`:1161-1189`)
- Modify: `web/app/i18n/en.js`, `zh.js`, `ja.js` (next to the `models.context_window*` keys, `:617-619`)

**Interfaces:**
- Consumes: `GET /ai/presets` `defaults` (Task 1); the option keys and values of spec 3.2.
- Produces (UI only):
  - `dialect` holds the five keys as strings, `''` meaning absent.
  - Each select's first option has value `''`, labelled `t('models.option_default', {value})`. `value` is `preset?.defaults?.[key]` if set, else the built-in default, shown through the same labels as the other choices. Choosing it deletes the key on save.
  - `out_tokens` is a number input with an empty placeholder state, labelled `t('models.not_sent')` when empty.
  - The llm save branch runs when vision, the context window or any dialect key changed. It merges every dialect key into the copied options map, setting or deleting each one. Today's `vision`/`context_window` handling is unchanged.
  - The group starts open when the profile has any dialect key. A `check()` error on `out_tokens` opens it, as `sample_rate` does at `:651`.
  - `out_field`'s choices are the literal field names and are not translated.
- i18n keys (en / zh / ja):

| key | en | zh | ja |
|---|---|---|---|
| `models.out_tokens` | Output token limit | 输出 token 上限 | 出力トークン上限 |
| `models.out_tokens_hint` | Empty: not sent, so the provider's default applies. A reply cut by the limit is continued once. | 留空则不发送，使用服务方的默认值。被上限截断的回复会自动续写一次。 | 空欄なら送信せず、提供元の既定値を使います。上限で切れた返答は一度だけ続きを生成します。 |
| `models.err_out_tokens` | Must be a whole number from {min} to {max}. | 必须是 {min}–{max} 之间的整数。 | {min}～{max} の整数を入力してください。 |
| `models.out_field` | Limit field name | 上限字段名 | 上限フィールド名 |
| `models.reasoning` | Reasoning text | 推理内容 | 推論テキスト |
| `models.reasoning_drop` | Drop | 丢弃 | 破棄 |
| `models.reasoning_echo` | Send back within a turn | 在一轮内回传 | 同じターン内で返送 |
| `models.reasoning_hint` | Some reasoning models require their reasoning to be sent back during tool calls. | 部分推理模型要求在工具调用过程中回传推理内容。 | 一部の推論モデルは、ツール呼び出し中に推論内容の返送を求めます。 |
| `models.system_msgs` | System messages | 系统消息位置 | システムメッセージ |
| `models.system_msgs_anywhere` | Anywhere | 任意位置 | どこでも |
| `models.system_msgs_first` | First only | 仅限开头 | 先頭のみ |
| `models.system_msgs_hint` | First only: summaries and per-request information go into user messages, for providers that move system messages to the front. | 仅限开头：摘要和每次请求的信息放进用户消息，适用于会把系统消息移到最前面的服务。 | 先頭のみ：要約とリクエストごとの情報をユーザーメッセージに入れます。システムメッセージを先頭に移す提供元向けです。 |
| `models.parallel_tools` | Parallel tool calls | 并行工具调用 | 並列ツール呼び出し |
| `models.parallel_on` | Allowed | 允许 | 許可 |
| `models.parallel_off` | Not allowed | 不允许 | 許可しない |
| `models.not_sent` | not sent | 不发送 | 送信しない |
| `models.option_default` | Default ({value}) | 默认（{value}） | 既定（{value}） |

- [ ] **Step 1: Run the checks before the change** to have a baseline: `python3 tools/check_i18n.py && python3 tools/pack_assets.py web -o build/ci/web-budget.wpak --budget-kb 170`. Expected: both pass. Note the gzip size printed.
- [ ] **Step 2: Implement** as in Interfaces.
- [ ] **Step 3: Run** `python3 tools/check_i18n.py && python3 tools/pack_assets.py web -o build/ci/web-budget.wpak --budget-kb 170 && python3 tools/check_settings.py`. Expected: all pass, and the gzip size grows by at most a few KB.
- [ ] **Step 4: Check by hand in the simulator** (spec 7: frontend checks are manual). Run `build/sim/wisp_sim --headless --http-port 8080 --web-dir web`, pair a browser (type `debug pair-code` on the simulator's stdin) and open the Models page. Then:
  - (a) A new `custom` LLM profile shows the advanced group closed, with every select at "Default (…)" showing the built-in values. Choosing the `openai` preset changes `out_field`'s default label to `max_completion_tokens`, and choosing `anthropic` changes `system_msgs`' label to "First only".
  - (b) `out_tokens` 100 shows the range error and opens the group. 1024 saves, and `GET /api/v1/ai/profiles` shows `"out_tokens":"1024"`.
  - (c) Setting `reasoning` to echo and back to "Default" removes the key.
  - (d) In zh, ja and en, nothing in the group wraps badly at 360 px width, and the spacing is the tts group's.
  - (e) Saving with only `vision` changed keeps the dialect keys.
  
  Record what was checked in the commit body.
- [ ] **Step 5: Commit** `feat(web): advanced LLM dialect options on the Models page`.

---

### Task 9: Documents, CI and the firmware build

**Files:**
- Modify: `docs/architecture.md`
  - `:392`: `turn_runner` gains "the length continuation and the rollover carry"
  - `:395`: `current.jsonl` may begin with a `carry` line
  - the "Model requests" bullet (`:398`): one sentence on the dialect options (`out_tokens`, `out_field`, `reasoning`, `system_msgs`, `parallel_tools`; resolved from the profile, then the preset's `defaults`, then the built-in; spec 3.2), and one on the continuation (spec 3.7)
- Modify: `docs/superpowers/specs/2026-11-05-agent-core-design.md`, marking each change "（2026-11-16 修订，见 `2026-11-15-llm-tool-providers-design.md` 3.x）":
  - the options list (`:62-65`): the five dialect options
  - 3.3 (`:125`): the rollover keeps a `carry` summary; a reply cut by `length` is continued once
  - 3.5 (`:149-154`): the `carry` line
  - 3.6: the dialect fields, `reasoning_content` under `echo`, and `system_msgs=first`'s `<context>` parts
- Modify: `docs/superpowers/specs/2026-11-03-connectivity-admin-design.md`
  - `:226`: the five llm options, their preset defaults, and "an invalid stored option is dropped alone at load"
  - `:309`: `GET /ai/presets` rows carry `defaults`
- Modify: `docs/backlog.md`: delete `:85` (closed by Task 1) and `:97` (closed by `system_msgs=first`, Tasks 2-3)
- Modify: `components/wisp_agent/include/wisp/agent/conversation.hpp` and `context.hpp` header comments, if earlier tasks left them stale (check every line that mentions `summary`)

- [ ] **Step 1: Edit the documents** as listed. Run `grep -rn "dropped at load\|drop the profile\|AgentRolledOver\b" docs components --include=*.md --include=*.hpp` and fix any statement it shows to be stale.
- [ ] **Step 2: Run** `bash tools/ci.sh`. Expected: exit 0, with no `warning:` in the build output. If `gen_cjk_font_test` fails on the new notes, regenerate the font as `tools/gen_cjk_font.py`'s header describes, commit it with the strings, and keep the fonts device check below.
- [ ] **Step 3: Build the firmware for both boards** (ESP-IDF v6.0.3):

```bash
cd firmware
for B in waveshare_esp32_s3_touch_amoled_2_16 waveshare_esp32_s3_touch_amoled_1_75c; do
  mkdir -p build-$B
  bash -c ". ~/esp/esp-idf-v6.0.3/export.sh >/dev/null 2>&1 && idf.py bmgr -c ../boards -b $B && idf.py -B build-$B -D SDKCONFIG=build-$B/sdkconfig -D SDKCONFIG_DEFAULTS='sdkconfig.defaults;components/gen_bmgr_codes/board_manager.defaults' -DWISP_BOARD=$B build" > build-$B/fw.log 2>&1
  echo "$B exit=$?"; tail -2 build-$B/fw.log; grep -c 'warning:' build-$B/fw.log
done
```

  Expected for each board: `exit=0`, "Project build complete", and a warning count of `0`. Any warning must come from ESP-IDF itself, not `components/`; record those in the commit body. If ESP-IDF is missing, write "firmware build not run" in the commit body and keep it in the device checks.
- [ ] **Step 4: Commit** `docs: record the llm dialects, length continuation and rollover carry`.

**Device checks (for the owner; not run by this plan; spec section 8 items 1-5, 9, 10):**
1. A turn on each of the owner's providers with no new options: replies and tool calls as before.
2. `out_tokens` 256 on a long answer: the reply continues once and reads as one text on Home and in speech. A second cut shows the note.
3. `reasoning=echo` against a reasoning model with tool calls (DeepSeek, Kimi; unverified): a multi-step turn completes. With `drop` on the same model, note whether the provider refuses.
4. `system_msgs=first` on the Anthropic and Gemini presets: the replies are correct, and the reported cached tokens (if reported) are higher than with `anywhere` over a multi-step turn.
5. `parallel_tools=false` on the OpenAI preset: accepted.
6. A conversation pushed past 1 MiB (or the limit lowered in a test build): the carry appears, the next turn knows the earlier context, and the archive is intact.
7. Downgrade to the previous release with a carry line and the new options stored: the conversation and profiles load (the carry line is dropped, and the options are kept and ignored).
8. `mem` during a continued reply and during a carry: the `wisp.agent` high-water mark against `docs/hardware/waveshare_esp32_s3_touch_amoled_2_16.md:467`.
9. Only if Task 9 regenerated the CJK font: flash the fonts partition (`idf.py fonts-flash`) together with the app, and check the two new notes on screen in zh and ja.

---

## Self-review

- **Spec coverage.**
  - 3.2: options, values and absence (Task 1); llm-only (Task 1); 7 options under 8 (Task 1); resolution order and preset defaults with the two approved rows (Task 1); `GET /ai/presets` `defaults` (Task 1); load fix (Task 1); web advanced group with "(default: …)" (Task 8); no migration (Review Focus 1).
  - 3.3: stream reading (Task 2); per-turn keep in PSRAM keyed by seq, never stored, 64 KiB with oldest dropped first (Task 4); echoed only on the running turn's messages (Tasks 3-4).
  - 3.4: movable marking (Task 3); merge rule (Task 2); prompt wording (Tasks 3-4).
  - 3.7: steps 1-5, not a step, tokens counted, summary and carry requests not continued, tool-call rule kept (Task 5).
  - 3.8: role and line (Task 6); who writes it, failures and the stop-before-archive rule (Task 7); in the context (Task 6); `to_value`, `hidden`, the Shell, compatibility (Task 6).
  - Section 5: buffers are `Mem::Large`, and nothing new touches flash off `wisp.fs` (Global Constraints).
  - Section 7: the J2 test items (Tasks 1-7).
  - Section 8: the J2 device checks (Task 9).
  - Section 9 steps 2-3 and step 7's documents (Task 9).
  - Out of scope here: parts and images, vision, `ToolThread::Io` (J3); tool providers and MCP (J4).
- **Type consistency.** These names are used the same way in every task:
  - `core::LlmDialect{out_tokens, out_field, reasoning, system_msgs, parallel_tools}`, `LlmOutField`, `LlmReasoning`, `LlmSystemMsgs`, `resolve_llm_dialect()`, `PresetOption`, `ResolvedProfile::preset`;
  - `net::LlmMessage::{reasoning, movable}`, `LlmModelInfo::dialect`, `LlmResult::reasoning`, `LlmClient::kMaxReasoningBytes`, `build_chat_body(model, msgs, tools_json, dialect)`;
  - `BuildOptions::{system_msgs, reasoning}`, `ReasoningRef{seq, text}`, `ReasoningKeep`, `kReasoningKeepBytes`;
  - `Role::Carry`, `Message::from`, `archive() -> Result<uint32_t>`, `build_carry_request(history, budget_tokens)`, `kCarryInputPercent`, `kCarryMaxBytes`, `make_carry(in, history)`;
  - `StrId::AgentReplyCut`, `StrId::AgentRolledOverCarry`.
- **Goldens.** No J1 golden changes. The one planned edit to an existing test is `AdminAi.CrudActivateAndTestRoutes`, for the additive `defaults`.
- **Proportion.** Bodies are not written out. The plan pins only the merge rule, field placement, exact prompts, note texts and labels, which the signatures and tests do not fix by themselves.
