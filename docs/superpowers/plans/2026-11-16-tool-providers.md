# Tool Providers (J4a) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** `tools.find` and `tools.call` stop depending on `ToolIndex` and `SceneRuntime` and work over registered `agent::ToolProvider`s. The first provider, `SceneToolProvider`, reproduces today's scene-tool results byte for byte, so J4b's MCP provider plugs in without touching the two tools again.

**Architecture:** A new interface `agent::ToolProvider` (spec 4.1) is registered on `ToolRegistry` with `add_provider()`. The two tools move from `tools_scene_tools.cpp` to `tools_find.cpp`. `find` asks each provider in registration order and merges the results under today's limits. `call` routes a name to the one provider whose `owns()` accepts it. `SceneToolProvider` wraps `ToolIndex` and `SceneRuntime`. The fixed prompt and the two tools' descriptions name MCP servers as a second source of tools. The `tools` array's schemas do not change.

**Tech Stack:** C++20 (`-fno-exceptions -fno-rtti`), GoogleTest, the simulator build (`build/sim`), ESP-IDF v6.0.3 for the closing firmware build.

**Spec:** `docs/superpowers/specs/2026-11-15-llm-tool-providers-design.md` (approved 2026-11-16, all section 12 proposals). This plan implements section 9 step 5 (sections 4.1, 6 and 7 "ToolProvider composition") and step 7's agent-tools spec 5.3 update. J4b (`docs/superpowers/plans/2026-11-16-mcp-client.md`) depends on it.

## Global Constraints

- Work only in the worktree `/home/shouchen/Projects/wisp/.superpowers/worktrees/llm-providers`, on branch `feat/llm-providers`. Never push, merge or switch branches. Never run `git stash`. Never add `.claude/`, `.playwright-mcp/` or `.superpowers/` files.
- **Concurrent plans.** J1 (provider seam and `TurnRunner`) may be executed on the same branch. This plan does not touch `AgentService`, `LlmClient`, `context.cpp` or `agent_service.*`. The files it shares with J1–J3 are `components/wisp_agent/src/prompt.cpp` (the one sentence at `:134-135`), `components/wisp_app/src/agent_wiring.{hpp,cpp}` (the scene tools registration at `:535-537` and two members) and `tool_registry.{hpp,cpp}` (J3 adds `tools_json(bool)`; this plan only adds `add_provider()`/`providers()`). Keep the edits in those files to exactly what each task names. When a rebase conflicts there, keep both sides.
- AGENTS.md applies in full. In particular:
  - Layering order: `wisp_base → wisp_os → wisp_hal → wisp_core → wisp_net → wisp_voice → wisp_admin → wisp_ui → wisp_scene → wisp_agent → wisp_shell → wisp_app`. Each layer includes only its own or earlier layers (`tools/check_layering.py`). The portable layers include no platform headers (`esp_*`, `freertos/*`, `lwip/*`, `curl/*`, `sdkconfig.h`, `wisp/port/*`, ...).
  - TDD: write the failing test first, run it, see it fail for the stated reason, then implement.
  - Lasting data goes in `Mem::Large` (`os::LargeString`, `os::LargeVector`). A plain `std::string`/`std::vector` under 16 KB lands in internal RAM on the device (AGENTS.md section 8). `tool_registry_heap_test` stays green: `tools.find` adds at most 512 peak default-heap bytes.
  - No tool touches flash. `tools.find`/`tools.call` and every `ToolProvider` method run on `wisp.agent` (`ToolThread::Agent`).
  - Comments say why the code is as it is. No review ids, task numbers, "round N" or `.superpowers/` paths (`tools/ci.sh` greps for them).
  - Conventional Commits. Every commit message ends with the line `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
  - Test output clean, no warnings.
- `-Werror=missing-field-initializers` is on: every aggregate this plan adds gives every member a default member initializer.
- **Compatibility (spec 6):** `tools.find`/`tools.call` keep their names, argument schemas (`kFindSchema`, `kCallSchema`), result shapes and errors for scene tools. The goldens of Task 1 must pass unchanged in every later task. Export `GOLDEN='SceneToolsGolden|SceneTools\.'` once per shell; the `-R` filters below use it.
- Each task verifies with `cmake --build build/sim` plus the focused `ctest --test-dir build/sim -R ... --output-on-failure` it names. If `build/sim` does not exist, configure it first: `cmake -S simulator -B build/sim -G Ninja`.
- Device checks are never reported as done.

## Review Focus

These are the five failures most likely to reach a user that no feature test would catch on its own. Each has a pinning test in the task named.

1. **Scene tool results change bytes.** The model has seen today's shapes for months, and the web chat's tool view parses them. Pinned by `SceneToolsGolden.*` (Task 1), which must stay unchanged through Task 3.
2. **A name in neither syntax.** `tools_call {"name":"weather"}` (no `.`, no `:`) must answer exactly as today, `not_found` "no scene tool named weather". It must not answer a new generic message. Pinned by `SceneToolsGolden.CallNameWithoutADot` (Task 1), and by `SceneTools.ProviderOwnsEveryNameWithoutAColon` (Task 3).
3. **A provider returns more than it was asked for.** The merged list must still stop at 20 (details) or 200 (listed). Pinned by `ToolFind.MergedListKeepsTheLimitsWhenAProviderOverfills` (Task 2).
4. **`args` missing or not an object, for an unknown tool.** `not_found` must still come before `bad_args`, as today. Pinned by `SceneToolsGolden.UnknownToolWithBadArgsIsNotFound` (Task 1).
5. **Merging leaks to the default heap.** A 200-entry listing must stay in PSRAM. Pinned by the existing `tool_registry_heap_test`, which Task 3 ports to the provider API without loosening its bounds.

## Plan decisions

Section 4.1 of the spec leaves these choices to the plan, or they follow from the code:

1. **Provider-specific field names in `find` details.** Today a detailed entry is `{name, description, parameters, scene_name, running}`. To keep that byte for byte and still give MCP entries meaningful names, `ToolProvider` gains one method beyond spec 4.1: `virtual ToolFindFields find_fields() const` (`{"scene_name","running"}` for scenes; J4b uses `{"server_name","available"}`). `ToolInfo` is unchanged.
2. **`unavailable` channel.** Spec 4.4 needs `tools.find` to report servers that failed. `search()` therefore writes into a `ToolSearch` (`tools` plus `unavailable`) instead of a bare `LargeVector<ToolInfo>`. `"unavailable":[{"server":<source>,"error":<category>}]` is emitted only when non-empty, so scene-only results do not change.
3. **Ownership by syntax.** `SceneToolProvider::owns()` is "no `:` in the name", not "has a `.`". A name in neither syntax therefore still reaches the scene provider and gets today's `not_found` message. The generic "no tool named X" answer only appears when no registered provider owns the name (no scene provider registered).
4. **`call()` gets the raw `args` value.** The generic `tools.call` passes `args`'s `Value` (a null `Value` when absent) to the provider. The provider does its own lookup first and its own object check second, which keeps `not_found` ahead of `bad_args` as today. The provider builds the whole `ToolOutput`, note included (spec 4.1 "Activity labels").
5. **Limits are global.** `find` passes each provider the room left (`max - got`), so later providers are skipped once the list is full. The generic code also truncates a provider that overfills. `total` (list mode only, as today) is the sum of every provider's `count()`.
6. **`scope` keeps the argument name `scene`.** Spec 4.1's `scope` is the existing optional `scene` argument; the schema does not change (spec 6). The tool description says it takes a scene id or an MCP server id.
7. **`describe()`** is implemented as spec 4.1 defines it, but no generic code calls it yet. Its callers are tests here, and later `SlotHost` (phase M).
8. **Wording.** The prompt sentence becomes: "Scenes and connected MCP servers may export tools: find them with tools_find and call them with tools_call; check for one when a request might be something a scene or a server can do." The descriptions become "Find or list tools exported by installed scenes and connected MCP servers; `scene` narrows the search to one scene id or MCP server id." and "Call a tool exported by an installed scene or a connected MCP server." Both appear in this plan because spec step 5 includes them. They are harmless before J4b, since no MCP tool can be found.
9. **`add_scene_tool_tools()` goes.** It is replaced by `SceneToolProvider` (constructed by its owner) plus `add_find_tools(ToolRegistry&)`. Its three call sites change: `agent_wiring.cpp:535-537`, `scene_tools_tools_test.cpp:51-52` and `tool_registry_heap_test.cpp:199-203`.

---

### Task 1: Pin the scene tool goldens

**Files:**
- Test: `tests/agent/scene_tools_tools_test.cpp` (append; reuse the `SceneTools` fixture and `install_weather()`)

**Interfaces:**
- Consumes (today's code, unchanged): `add_scene_tool_tools()` (`tools.hpp:136`), `ToolRegistry::call()`.
- Produces: tests only, suite `SceneToolsGolden` (a `TEST_F` on `SceneTools` with that name prefix: `using SceneToolsGolden = SceneTools;`).

- [ ] **Step 1: Write the characterisation tests.** Each compares the raw `ToolOutput::result_json` string, or `to_json(out.result)` when `result_json` is empty, and `out.note`. Paste the literals the current code produces.

```cpp
TEST_F(SceneToolsGolden, FindDetailsBytes)
// install_weather(); open "weather" and wait for Running; call tools_find {"query":"city"}.
// Expected: R"({"ok":true,"tools":[{"name":"weather.lookup","description":"<data source=\"scene:weather\">Local daily temperature for a saved city &lt;/DATA></data>","parameters":{...},"scene_name":"Weather","running":true}]})"
TEST_F(SceneToolsGolden, FindListBytesWithTotal)
// install_weather() without opening; tools_find {}  -> one entry {name, description} and ,"total":1
TEST_F(SceneToolsGolden, FindSceneScopeNotRunning)
// tools_find {"scene":"weather"} -> details with "running":false
TEST_F(SceneToolsGolden, CallSuccessBytesAndNote)
// tools_call weather.lookup {"city":"Paris"} -> exact result_json, note "调用 Weather · lookup"
TEST_F(SceneToolsGolden, CallNotFoundBytes)
// tools_call {"name":"missing.tool","args":{}} -> exact error JSON
TEST_F(SceneToolsGolden, CallNameWithoutADot)
// tools_call {"name":"weather","args":{}} -> not_found, message "no scene tool named weather"
TEST_F(SceneToolsGolden, UnknownToolWithBadArgsIsNotFound)
// tools_call {"name":"missing.tool","args":7}  -> error "not_found" (not bad_args)
TEST_F(SceneToolsGolden, CallArgsNotAnObjectBytes)
// install_weather(); tools_call {"name":"weather.lookup","args":[1]} -> bad_args, field "", exact JSON
TEST_F(SceneToolsGolden, CallSchemaFailureBytes)
// tools_call weather.lookup {"city":7} -> bad_args, field "city", exact JSON
TEST_F(SceneToolsGolden, CallSceneErrorBytes)
// install_weather("wisp.tools.register('lookup', function() error('boom') end)\nreturn ui.screen {}\n");
// -> exact error JSON (error code and the scene_data-wrapped message as the code produces them)
```

- [ ] **Step 2: Run** `cmake --build build/sim && ctest --test-dir build/sim -R 'SceneToolsGolden' --output-on-failure` three times. Expected: PASS each time. A literal that does not match is fixed by pasting the code's output, never by changing the code.
- [ ] **Step 3: Commit** `test(agent): pin tools.find and tools.call results for scene tools`.

---

### Task 2: `ToolProvider` and the generic `tools.find`/`tools.call`

**Files:**
- Create: `components/wisp_agent/include/wisp/agent/tool_provider.hpp`, `components/wisp_agent/src/tools_find.cpp`
- Modify: `components/wisp_agent/include/wisp/agent/tool_registry.hpp:98-140` (add `add_provider`, `providers`, member `providers_`), `components/wisp_agent/src/tool_registry.cpp`, `components/wisp_agent/include/wisp/agent/tools.hpp` (declare `add_find_tools`), `components/wisp_agent/CMakeLists.txt:13` (add `src/tools_find.cpp`)
- Create: `tests/agent/tool_find_test.cpp`; Modify: `tests/CMakeLists.txt` after `:516` (`wisp_test(NAME tool_find_test SRCS agent/tool_find_test.cpp LIBS wisp_agent wisp_port_sim_os TIMEOUT 30)`)

**Interfaces:**
- Consumes: `ToolCtx`, `ToolOutput`, `tool_error()`, `ToolRegistry::add()` (`tool_registry.hpp`), `append_json_string()` (`wisp/base/json.hpp`), `detail::append_number()` (`src/tools_json.hpp`), `kFindMax`/`kListMax` (`tool_index.hpp:42-43`).
- Produces (`namespace wisp::agent`):

```cpp
// tool_provider.hpp
struct ToolInfo {
    os::LargeString name{};         // full name, as tools.call takes it
    os::LargeString description{};  // already wrapped as external data
    os::LargeString parameters{};   // JSON Schema text; "" in list mode
    os::LargeString origin{};       // scene name or server name, for the note and details
    bool available = true;          // scene running / server reachable
};
struct ToolSourceIssue {
    os::LargeString source{};  // a server id
    os::LargeString error{};   // a category: dns connect tls timeout http unsupported_protocol bad_response ...
};
struct ToolSearch {
    os::LargeVector<ToolInfo> tools{};
    os::LargeVector<ToolSourceIssue> unavailable{};
};
// The keys a provider's detailed find entries use for origin and available.
struct ToolFindFields {
    std::string_view origin{};
    std::string_view available{};
};
class ToolProvider {
public:
    virtual ~ToolProvider() = default;
    virtual std::string_view id() const = 0;                      // "scene", "mcp"
    virtual ToolFindFields find_fields() const = 0;
    virtual bool owns(std::string_view full_name) const = 0;
    virtual void search(std::string_view query, std::string_view scope, size_t max, bool details,
                        ToolCtx& ctx, ToolSearch& out) = 0;     // appends at most max to out.tools
    virtual size_t count() const = 0;                             // memory only
    virtual std::optional<ToolInfo> describe(std::string_view full_name, ToolCtx& ctx) = 0;
    virtual ToolOutput call(std::string_view full_name, const Value& args, ToolCtx& ctx) = 0;
};

// tool_registry.hpp, class ToolRegistry
// Not thread-safe, like add(): register every provider before the first
// call(). Not owned; must outlive the registry's last call(). Searched and
// routed in registration order.
void add_provider(ToolProvider& provider);
std::span<ToolProvider* const> providers() const;
// private: os::LargeVector<ToolProvider*> providers_;

// tools.hpp
// tools.find and tools.call (agent-tools spec 5.3, this spec 4.1) over
// reg.providers(): ToolThread::Agent, ToolRisk::Note, tools.call's
// registration timeout 125000 ms.
Result<void> add_find_tools(ToolRegistry& reg);
```

- [ ] **Step 1: Write the failing tests** in `tool_find_test.cpp`. The test-local `FakeProvider : ToolProvider` is configured with `id`, `find_fields`, the separator it owns (`'.'`-only or `':'`), a list of `ToolInfo`s, `count`, an optional `overfill` (returns `max + 5` entries) and `unavailable` entries. It records each `search()` call (query, scope, max, details) and each `call()` (name, `to_json(args)`), and `call()` returns `{"ok":true,"from":<id>}`. Registry: `ToolRegistry reg{ToolExecutors{}}`, two providers added, then `add_find_tools(reg)`.

```cpp
TEST(ToolFind, AsksProvidersInRegistrationOrderAndMerges)
// a: 2 tools, b: 3 tools; tools_find {"query":"x"} -> names a1,a2,b1,b2,b3 in that order;
// a.search max == 20, b.search max == 18; both details == true; scope "" passed through.
TEST(ToolFind, ListModeTotalIsTheSumOfCounts)
// a.count 250, b.count 7; tools_find {} -> 200 entries max overall, "total":257; details == false.
TEST(ToolFind, MergedListKeepsTheLimitsWhenAProviderOverfills)
// a overfills: details mode -> exactly 20 entries; b.search never called.
TEST(ToolFind, DetailsUseEachProvidersFieldNames)
// a {"scene_name","running"}, b {"server_name","available"} with available=false ->
// a's entry has "scene_name" and "running":true; b's has "server_name" and "available":false.
TEST(ToolFind, UnavailableAppearsOnlyWhenSomeProviderReportsIt)
// no issues -> result has no "unavailable" key;
// b reports {source "srv", error "timeout"} -> "unavailable":[{"server":"srv","error":"timeout"}] after "tools" (and after "total" in list mode).
TEST(ToolFind, ScopeIsPassedToEveryProvider)
// tools_find {"scene":"srv"} -> both providers saw scope "srv", details == true.
TEST(ToolCall, RoutesByOwnerAndPassesArgs)
// a owns names without ':', b names with ':'; tools_call {"name":"srv:echo","args":{"k":1}}
// -> b.call("srv:echo", {"k":1}); a.call not called; result {"ok":true,"from":"b"}.
TEST(ToolCall, MissingArgsReachTheProviderAsNull)
// tools_call {"name":"x.y"} -> a.call got a null Value.
TEST(ToolCall, NoOwnerIsNotFound)
// only b registered; tools_call {"name":"x.y","args":{}} -> error "not_found", message "no tool named x.y".
TEST(ToolRegistry, ProvidersKeepRegistrationOrder)
// add_provider(a), add_provider(b): providers() == {&a, &b}.
```

- [ ] **Step 2: Run** `cmake --build build/sim`. Expected: build FAIL (`tool_provider.hpp` missing, `add_find_tools` undeclared).
- [ ] **Step 3: Implement** the header, `add_provider()`/`providers()` and `tools_find.cpp`. The `tools.find` body streams entries into `out.result_json` (an `os::LargeString`), as `tools_scene_tools.cpp:89-106` does today. Per entry: `"name"`, `"description"`, then in details mode `"parameters"` (raw text), `"<origin key>"` (JSON string) and `"<available key>"` (`true`/`false`). Then `]`, `,"total":N` in list mode, `,"unavailable":[...]` when non-empty, and `}`. Give the two new tools the descriptions of decision 8 and the existing `kFindSchema`/`kCallSchema` text unchanged. Do not register them anywhere yet: `tools_scene_tools.cpp` still registers the old pair, so the tests above use their own registry only.
- [ ] **Step 4: Run** `cmake --build build/sim && ctest --test-dir build/sim -R 'ToolFind|ToolCall|ToolRegistry|tool_registry' --output-on-failure`. Expected: PASS.
- [ ] **Step 5: Commit** `feat(agent): ToolProvider interface and provider-based tools.find/tools.call`.

---

### Task 3: `SceneToolProvider` replaces the scene-only tools

**Files:**
- Create: `components/wisp_agent/include/wisp/agent/scene_tool_provider.hpp`, `components/wisp_agent/src/scene_tool_provider.cpp` (moves the logic of `tools_scene_tools.cpp:27-161`)
- Delete: `components/wisp_agent/src/tools_scene_tools.cpp`
- Modify: `components/wisp_agent/CMakeLists.txt:13` (swap the source), `components/wisp_agent/include/wisp/agent/tools.hpp:129-136` (remove `SceneToolDeps` and `add_scene_tool_tools`)
- Modify: `components/wisp_app/src/agent_wiring.hpp` (member `std::unique_ptr<agent::SceneToolProvider> scene_tools_;` declared **before** `registry_` at `:318`, so it is destroyed after the registry), `components/wisp_app/src/agent_wiring.cpp:535-537`
- Modify tests: `tests/agent/scene_tools_tools_test.cpp:51-52` (fixture builds a `SceneToolProvider` member, `add_provider`, `add_find_tools`), `tests/agent/tool_registry_heap_test.cpp:196-203` (same, for both registries)

**Interfaces:**
- Consumes: Task 2's `ToolProvider`, `ToolSearch`, `ToolFindFields`, `add_find_tools()`, `ToolRegistry::add_provider()`; `ToolIndex::search/find/size` (`tool_index.hpp:30-35`); `SceneRuntime::status()`/`call_tool()`; `detail::external_data_large()` (`tool_io.hpp`); `ui::tr_fill_n(StrId::AgentNoteToolCall, ...)`.
- Produces:

```cpp
namespace wisp::agent {
class SceneToolProvider final : public ToolProvider {
public:
    SceneToolProvider(ToolIndex& index, scene::SceneRuntime& runtime);
    std::string_view id() const override;              // "scene"
    ToolFindFields find_fields() const override;       // {"scene_name", "running"}
    bool owns(std::string_view full_name) const override;  // no ':' in the name
    void search(std::string_view query, std::string_view scope, size_t max, bool details,
                ToolCtx& ctx, ToolSearch& out) override;
    size_t count() const override;                      // index.size()
    std::optional<ToolInfo> describe(std::string_view full_name, ToolCtx& ctx) override;
    ToolOutput call(std::string_view full_name, const Value& args, ToolCtx& ctx) override;
};
}  // namespace wisp::agent
```

- [ ] **Step 1: Write the failing tests** (append to `scene_tools_tools_test.cpp`, after switching the fixture to the new API; the goldens and existing `SceneTools.*` tests are the main check):

```cpp
TEST_F(SceneTools, ProviderOwnsEveryNameWithoutAColon)  // suite SceneTools, so $GOLDEN runs it
// SceneToolProvider p(index, *rt): owns("weather.lookup"), owns("weather"), owns("memory.add") true;
// owns("srv:echo") false; id() == "scene".
TEST_F(SceneTools, ProviderDescribeHasParametersAndRunningState)
// install_weather(); describe("weather.lookup") -> name, wrapped description, parameters text with
// "required", origin "Weather", available false (not open); describe("weather.nope") -> nullopt.
TEST_F(SceneTools, ProviderSearchListModeLeavesParametersEmpty)
// search("", "", 200, false, ...) -> parameters "" for every entry.
```

- [ ] **Step 2: Run** `cmake --build build/sim`. Expected: build FAIL (`SceneToolProvider` undeclared).
- [ ] **Step 3: Implement** `SceneToolProvider` by moving today's bodies:
  - `search` maps `IndexedTool` to `ToolInfo`, computing `available` only in details mode. The state test is the one at `tools_scene_tools.cpp:56-61`: Running, Background or Headless.
  - `call` does the `find()`, then the object check, schema validation, `call_tool()`, wrapping and note, as `tools_scene_tools.cpp:119-159` does. Messages and order are unchanged.

  Delete `tools_scene_tools.cpp`. In `AgentWiring`, construct `scene_tools_` before the `check(...)` calls. Replace the "scene tools" check with `registry_->add_provider(*scene_tools_); check("find tools", agent::add_find_tools(*registry_));`, keeping its place in the registration order so the `tools` array keeps its order.
- [ ] **Step 4: Run** `cmake --build build/sim && ctest --test-dir build/sim -R "$GOLDEN|ToolFind|ToolCall|tool_registry_heap|AppScenes|AgentWiring" --output-on-failure`. Expected: PASS, with no golden literal edited and the heap test's bounds unchanged.
- [ ] **Step 5: Commit** `refactor(agent): scene tools behind SceneToolProvider`.

---

### Task 4: Prompt wording, documents and the full check

**Files:**
- Modify: `components/wisp_agent/src/prompt.cpp:134-135` (the one sentence of decision 8; nothing else in this file)
- Modify: `tests/agent/context_test.cpp:664-666` (expected sentence)
- Modify: `docs/superpowers/specs/2026-11-09-agent-tools-design.md` section 5.3 (`:216-247`)
- Modify: `docs/architecture.md:82` (the `wisp.agent` row: `tools.find`/`tools.call` search and call registered `ToolProvider`s on this task)

**Interfaces:**
- Consumes: Task 2's descriptions (already in `tools_find.cpp`).
- Produces: none.

- [ ] **Step 1: Update the test first.** In `context_test.cpp`, the expected text becomes decision 8's sentence verbatim, and add `EXPECT_FALSE(has(prompt, "Scenes may export tools:"));`.
- [ ] **Step 2: Run** `ctest --test-dir build/sim -R 'Context' --output-on-failure`. Expected: FAIL on the new sentence.
- [ ] **Step 3: Change** `prompt.cpp:134-135` to the new sentence (two string literals, same line breaks).
- [ ] **Step 4: Documents.**
  - Agent-tools spec 5.3: add a paragraph "Providers (revised by `2026-11-15-llm-tool-providers-design.md` 4.1)". It says `tools.find`/`tools.call` search and route over `agent::ToolProvider`s in registration order (scenes, then MCP servers). Scene names stay `<scene id>.<tool>` and MCP names are `<server id>:<tool>`. Limits are shared. Detailed MCP entries use `server_name`/`available`, and an `unavailable` list may follow.
  - The "提示词" (prompt) bullet: the sentence now names MCP servers.
  - `docs/architecture.md:82`: one clause, as listed above.
- [ ] **Step 5: Full check.** Run `bash tools/ci.sh`. Expected: exit 0, no warnings. Then build the firmware for both boards, one after the other (no warnings expected):

```bash
cd firmware
for B in waveshare_esp32_s3_touch_amoled_2_16 waveshare_esp32_s3_touch_amoled_1_75c; do
  bash -c ". ~/esp/esp-idf-v6.0.3/export.sh >/dev/null 2>&1 && idf.py bmgr -c ../boards -b $B && idf.py -B build-$B -D SDKCONFIG=build-$B/sdkconfig -D SDKCONFIG_DEFAULTS='sdkconfig.defaults;components/gen_bmgr_codes/board_manager.defaults' -DWISP_BOARD=$B build" || break
done
```

  If ESP-IDF v6.0.3 is not installed, say so in the commit body ("firmware build not run").
- [ ] **Step 6: Commit** `feat(agent): name MCP servers as a tool source in the prompt` (the docs go in the same commit: `docs` changes describe this behaviour).

## Device checks (not run; for the owner)

None specific to J4a. The scene tool paths are covered on the host. The existing `tools.find`/`tools.call` flow on the device (a scene exporting a tool, asked for by voice) is worth one smoke try after flashing.
