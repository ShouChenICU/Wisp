# MCP Client (J4b) — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** The agent reaches tools on MCP servers (streamable HTTP, revision `2025-06-18`) through `tools.find`/`tools.call`, without those tools entering the `tools` array. The owner configures servers on the web Models page, with write-only headers. The agent can list, add (no headers), remove and enable servers itself, and the dynamic info names the configured servers.

**Architecture:** `core::McpServerService` (`wisp_core`) keeps the servers in `secrets/mcp.json`, saves through `LatestSave` on `wisp.fs`, and exposes header values only through `resolve_for_request()`. `net::McpClient` (`wisp_net`) speaks JSON-RPC over `SceneHttp`'s Agent principal. It streams each response through `on_body` and stops at the matching response (`stop_is_success`). It parses with `parse_json_large()` into PSRAM, and caches each server's session and tool list in memory. `agent::McpToolProvider` adapts the client to J4a's `ToolProvider`. The REST routes, the web card, the `mcp.*` tools and the dynamic-info line are thin adapters over the service and the provider.

**Tech Stack:** C++20 (`-fno-exceptions -fno-rtti`), ArduinoJson 7.4.3 (only in `json.cpp`), GoogleTest, the simulator build (`build/sim`), vanilla JS with Preact/htm (`web/app`), ESP-IDF v6.0.3.

**Spec:** `docs/superpowers/specs/2026-11-15-llm-tool-providers-design.md` (approved 2026-11-16, all section 12 proposals: decisions 7, 8 and 11 are the MCP ones). This plan implements section 9 step 6 (sections 4.2–4.4, the MCP parts of 5–8) and the MCP documents of step 7.

**Depends on:** J4a, `docs/superpowers/plans/2026-11-16-tool-providers.md` (`ToolProvider`, `ToolSearch`, `ToolFindFields`, `add_find_tools()`, `SceneToolProvider`, `ToolRegistry::add_provider()`). Execute J4a first.

## Global Constraints

- Work only in the worktree `/home/shouchen/Projects/wisp/.superpowers/worktrees/llm-providers`, on branch `feat/llm-providers`. Never push, merge or switch branches. Never run `git stash`. Never add `.claude/`, `.playwright-mcp/` or `.superpowers/` files.
- **Concurrent plans.** J1 (provider seam and `TurnRunner`) may run on the same branch and rewrites `llm_client.{hpp,cpp}` and `agent_service.*`.
  - This plan does not edit `agent_service.*`. In `llm_client.cpp` it touches only the one helper of Task 5 (`holds_key_piece()`, `:249-257`).
  - It includes `SseParser` from `wisp/net/llm_client.hpp` and does not change it. If J1 moved `SseParser`, include its new header.
  - Shared files: `components/wisp_app/src/agent_wiring.{hpp,cpp}` (Task 9: members, the provider and tools registration, one line in the context lambda at `:435-442`) and `components/wisp_agent/src/prompt.cpp` (`dynamic_info()` only, Task 6). Keep the edits there to exactly what the task names. When a rebase conflicts there, keep both sides.
- AGENTS.md applies in full. In particular:
  - Layering order: `wisp_base → wisp_os → wisp_hal → wisp_core → wisp_net → wisp_voice → wisp_admin → wisp_ui → wisp_scene → wisp_agent → wisp_shell → wisp_app`. `wisp_base` cannot call `os::alloc`. `wisp_core` cannot use `SceneHttp` or anything else in `wisp_net`. The portable layers include no platform headers (`esp_*`, `freertos/*`, `lwip/*`, `mbedtls/*`, `curl/*`, `sdkconfig.h`, `wisp/port/*`). `tools/check_layering.py` enforces both.
  - TDD: failing test first, run it, see it fail for the stated reason, then implement.
  - **Flash.** Only `wisp.fs` touches flash. `McpServerService::init()` runs on `wisp.fs` (or before it starts). Changes save through `LatestSave`. `McpClient`, `McpToolProvider` and the `mcp.*` tools never touch flash (they run on `wisp.agent`; `on_body` runs on `wisp.shttp`).
  - **Memory.** Lasting or large data goes in `Mem::Large`: the server list's names, URLs and header values (`os::LargeString`), the session id, tool caches, request bodies, response buffers and parsed documents (`parse_json_large()`). Only transient request copies inside `SceneHttp` and response header strings stay on the default heap, as the spec lists in section 5.
  - **Secrets.** Header values are write-only (AGENTS.md section 6; spec 4.3). No public getter, REST response, event, cell, agent tool result, error message or log line carries a value or any 8-byte piece of one. Logs name the server id, the method and the outcome only, never URLs past the host, arguments, results or headers.
  - **Untrusted content.** Server descriptions, results and error messages reach the model only through `external_data("mcp:<server id>", …)`.
  - User-facing text in zh, ja and en: device strings in `components/wisp_ui/src/strings_{zh,ja,en}.cpp`, web strings in `web/app/i18n/{zh,ja,en}.js` (checked by `tools/check_i18n.py`). New zh/ja device text must be in the committed CJK font: `tools/gen_cjk_font_test.py` runs in `ci.sh`. If it fails, regenerate with `tools/gen_cjk_font.py` and list `idf.py fonts-flash` under device checks.
  - No settings are added (no `WISP_SETTINGS` change). No event topics are added: the service's cell has no bus mirror.
  - Comments say why. No review ids, task numbers, "round N" or `.superpowers/` paths.
  - Conventional Commits. Every commit message ends with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
  - Test output clean, no warnings.
- `-Werror=missing-field-initializers`: every aggregate this plan adds gives every member a default member initializer.
- J4a's goldens stay green in every task: `export GOLDEN='SceneToolsGolden|SceneTools\.|ToolFind|ToolCall'` once per shell.
- `components/wisp_port_esp/` is compiled only by `idf.py`. Task 2 edits it, so it runs the firmware build for both boards. The command is in Task 11 Step 5.
- Each task verifies with `cmake --build build/sim` plus the focused `ctest --test-dir build/sim -R ... --output-on-failure` it names.
- Device checks are never reported as done.

## Review Focus

These are the five failures most likely to reach a user that no single feature test would catch. Each has a pinning test in the task named.

1. **A header value leaks.** Bearer tokens could escape through a route, a tool result, an error message the server echoes, or a log line. Pinned by `AdminMcp.NoResponseEverContainsAHeaderValue` (Task 8), `McpTools.ServersNeverCarryHeaderValues` (Task 7), `McpClientTest.QuotedCredentialIsWithheld` and `McpClientTest.NoLogLineCarriesAHeaderValue` (Task 5).
2. **A server that keeps its SSE stream open.** Each call would then last the whole `timeout_ms` (60 s by default) instead of ending at the response. Pinned by `McpClientTest.SseLeftOpenEndsAtTheResponse` (Task 4), on top of `SceneHttpTest.AgentStopIsSuccessKeepsStatusAndHeaders` (Task 2).
3. **A dead or slow server.** It must not make every `tools.find` wait 15 s per server, or hang the turn. Pinned by `McpToolProviderTest.FailedServerIsNotRetriedWithinAMinute` and `McpToolProviderTest.SearchBudgetBoundsSlowServers` (Task 6).
4. **Session expiry loops.** A server that answers `404` again after re-initializing must fail once, not loop. Pinned by `McpClientTest.ExpiredTwiceFailsWithoutALoop` (Task 4).
5. **Moving a server to another host keeps its credentials.** The stored header values must be cleared, and the web page must show that. Pinned by `McpServers.UpdateToAnotherOriginClearsValues` (Task 3) and `AdminMcp.OriginChangeDropsValues` (Task 8). The web card is checked by hand in Task 10 Step 4.

## Plan decisions and spec contradictions

**Contradictions with the spec, and the rulings this plan makes:**

1. **`stop_is_success` alone cannot return headers.** Spec 4.4 says a stop from `on_body` returns "the status and headers" and that this is the only change to `SceneHttp`. But both clients drop headers on a consumer stop: it is `Cancelled`, and headers are "Empty when the transfer failed" (`http.hpp:253-255`; `sim_http_client.cpp:289-293`; `esp_http_client.cpp:349-351`). The session id comes in a header. Ruling: `HttpClientRequest` gains `stop_is_end`, mirroring `truncate_at_limit` (`http.hpp:217-224`). A false from `on_body` then ends the transfer as a success, with status and headers kept. Both port clients and `FakeHttpClient` implement it (Task 2). `SceneHttp` sets it only for `stop_is_success` (Agent principal).
2. **`parse_json_large()` cannot reach `Mem::Large` from `wisp_base`.** `wisp_base` sits below `wisp_os` (spec 4.2 places the function in `wisp_base`). Ruling: it takes a `JsonAlloc` (three function pointers). `wisp_net` passes the `Mem::Large` one (`src/large_json.hpp`). `json.cpp` stays the only includer of ArduinoJson.
3. **MCP image results need J3's image slot.** Spec 4.2 and 3.5 rely on that slot, but spec 9 declares J4 independent of J1–J3. Ruling: J4b turns an `image` item into the text `[image omitted: <mime>, <n> bytes]`. Hooking MCP images into the turn's image slot belongs to J3 (or a follow-up if J3 lands first), and is recorded in `docs/backlog.md` (Task 11).
4. **`holds_key_piece()` "moves" to `wisp_net`'s internal helpers.** Spec 4.4 says so, but J1 rewrites `llm_client.cpp`. Ruling: Task 5 creates `src/secret_scrub.{hpp,cpp}` (`detail::holds_secret_piece()`) and changes `llm_client.cpp` only by deleting its local copy and including the helper, which is a trivial merge either way.
5. **The test route's categories** (spec 4.3: `dns connect tls timeout http unsupported_protocol bad_response`) do not cover `SceneHttp`'s `offline` and `other`. Ruling: both map to `connect`, and `too_large` maps to `bad_response`. Inside the agent's tool results, `offline` stays its own category (spec 4.4 lists it).
6. **`McpServerService` (core) cannot call `SceneHttp::header_syntax_ok()` (net).** Ruling: the token and value check moves to `core::http_header_syntax_ok()` in a new `wisp/core/http_rules.hpp`, and `SceneHttp::header_syntax_ok()` delegates to it (a refactor, no behaviour change). The same header takes `valid_http_url()` and `url_origin()`, which leave `model_profiles.cpp`'s anonymous namespace (`:91-140`). J2 edits `model_profiles.cpp` too, in `check_options()`, a different region.
7. **`structuredContent`.** MCP `2025-06-18` asks servers to also serialize it into a `text` item, so including both doubles the bytes. Ruling: the raw JSON is appended only when the result has no `text` item.

**Choices the spec leaves open:**

8. **URLs are kept as given.** A trailing `/` can matter to an MCP endpoint, so it is not stripped, unlike the profiles' base URL. A URL with user info (`user:pass@host`) is refused (`invalid_argument`, "put credentials in a header"), since URLs are readable through routes and `mcp.servers`.
9. **Reserved header names.** `Accept`, `Content-Type`, `Mcp-Session-Id`, `MCP-Protocol-Version`, `Host`, `Content-Length`, `Transfer-Encoding` and `Connection` (any case) are refused, because the client sets them or `SceneHttp` drops them.
10. **Ids.** On create, the name is lowercased, every run of characters outside `[a-z0-9_-]` becomes one `-`, leading and trailing `-` are trimmed, and the result is cut to 16 bytes. If that leaves nothing, the id is `mcp`. A taken id gets `-2`, `-3`, … with the base shortened to stay within 16. The id never changes after create.
11. **Header update semantics** (web and REST). `headers` replaces the whole list. An entry without `value` keeps the stored value of the same name (case-insensitive) if the origin is unchanged. Otherwise the entry is dropped. An entry with a `value` sets it (`""` is refused: drop the entry instead).
12. **Failed servers back off.** A server whose initialize or list failed is not contacted again for `kMcpRetryAfterMs = 60 s`. Meanwhile `tools.find` lists it as `unavailable` with the remembered category, and `tools.call` to it answers `mcp_unavailable` without a request.
13. **`tools.find` search budget.** `McpToolProvider::search()` gives all servers together at most `kMcpSearchBudgetMs = 20 s` (and never past `ctx.deadline_ms`). Servers not reached in time are `unavailable` with `timeout`. Without this, eight dead servers would cost 8 × 15 s inside one tool call that the registry cannot time out (`tool_registry.cpp:249-251`).
14. **Lists are bounded:** at most `kMcpMaxListPages = 16` pages per `tools/list`, as well as `kMcpMaxTools = 128` tools.
15. **`timeout` vs `mcp_unavailable`.** The tool error `timeout` means the turn's deadline (`ctx.deadline_ms`) ended the wait. A request that ran past the server's own `timeout_ms` (or the 15 s initialize/list limit) is `mcp_unavailable` with category `timeout`.
16. **Unknown tool after `-32602`.** The list is refreshed. If the tool is gone, the answer is `not_found`, otherwise `mcp_error` with the code. The call is not retried.
17. **Disabled or unknown server in `tools.call`:** `not_found` ("MCP server <id> is disabled" / "no MCP server <id>").
18. **Dynamic info line** (spec decision 11): `MCP: <id> (<n> tools), <id> (not connected yet), <id> (unavailable: <category>)`, enabled servers only, in configuration order. It is placed after the `Network:` line, and absent when no server is enabled.
19. **The `mcp.*` tools** are registered, and the MCP provider added, only when the platform has `SceneHttp` (the same condition as `http.fetch`).
20. **Web placement.** (Revised 2026-11-17: at the owner's request the card became its own page, `#/mcp`, a sub-page of Models; see the providers spec 4.3.) The "MCP servers" card renders below the chat tab's panel (`#/models/llm`), in its own module `web/app/pages/mcp_servers.js` (the Models page is already 1252 lines).
21. **Session state is per server and in memory**, keyed by id and the server's `revision`. A configuration change of that server (any `revision` change) drops its session and cache before the next use. The service's cell (`{count, enabled, revision}`) is read with `get()` on each use; no subscription is needed.

---

### Task 1: `parse_json_large()`

**Files:**
- Modify: `components/wisp_base/include/wisp/base/json.hpp`, `components/wisp_base/src/json.cpp`
- Create: `components/wisp_net/src/large_json.hpp` (the `Mem::Large` `JsonAlloc`)
- Create: `tests/base/json_large_test.cpp`. Modify `tests/CMakeLists.txt` after `:40`: `wisp_test(NAME json_large_test SRCS base/json_large_test.cpp LIBS wisp_base)`. It is its own binary: it replaces global `operator new`, as `tool_registry_heap_test.cpp` does.

**Interfaces:**
- Consumes: ArduinoJson 7.4.3's `ArduinoJson::Allocator` (`allocate`/`deallocate`/`reallocate`), `deserializeJson` with `DeserializationOption::NestingLimit`.
- Produces (`namespace wisp`, `json.hpp`):

```cpp
struct JsonAlloc {
    void* (*allocate)(size_t) = nullptr;
    void (*deallocate)(void*) = nullptr;
    void* (*reallocate)(void*, size_t) = nullptr;
};

// A read-only view into a LargeJson; valid while that document lives.
// Copying a view copies two pointers.
class JsonView {
public:
    enum class Type : uint8_t { Null, Bool, Int, Double, String, Array, Object };
    Type type() const;
    JsonView find(std::string_view key) const;   // Null view: not an object, or absent
    JsonView at(size_t i) const;                 // Null view past the end
    size_t size() const;                         // members of an array/object, else 0
    std::string_view string() const;             // "" unless String; points into the document
    std::optional<int64_t> integer() const;      // Int only
    bool boolean(bool fallback = false) const;
    // Compact JSON of this subtree, in pieces, without a temporary copy.
    void write(void (*sink)(void* ctx, const char* p, size_t n), void* ctx) const;
    template <class S> void append_to(S& out) const;  // calls write() with S::append
private:
    // An ArduinoJson JsonVariantConst, which is two pointers (static_assert in json.cpp).
    alignas(void*) unsigned char v_[2 * sizeof(void*)] = {};
};

class LargeJson {
public:
    LargeJson(LargeJson&&) noexcept;
    LargeJson& operator=(LargeJson&&) noexcept;
    ~LargeJson();
    JsonView root() const;
private:
    void* impl_ = nullptr;  // {Allocator adapter, JsonDocument}, itself placed with the JsonAlloc
};

// InvalidArgument on a parse error (ArduinoJson's text), QuotaExceeded past
// max_depth (as from_json(s, max_depth)), NoMemory when the allocator fails.
// Every byte of the document comes from `mem`.
Result<LargeJson> parse_json_large(std::string_view text, const JsonAlloc& mem,
                                   uint8_t max_depth = 32);
```

  `components/wisp_net/src/large_json.hpp`: `inline const JsonAlloc& large_json_alloc();` built over `os::alloc(n, Mem::Large)`, `os::dealloc`, `os::realloc(p, n, Mem::Large)`.

- [ ] **Step 1: Write the failing tests** (suite `JsonLarge`; a counting `JsonAlloc` over `malloc` counts allocations and frees; the replaced `operator new` counts blocks while a `thread_local` flag is set, as in `tool_registry_heap_test.cpp:28-60`):

```cpp
TEST(JsonLarge, ReadsObjectsArraysAndScalars)
// {"a":1,"b":"x","c":[true,null,2.5],"d":{"e":"f"}}: find("a").integer()==1; find("b").string()=="x";
// find("c").size()==3, at(0).boolean()==true, at(1).type()==Null, at(2).type()==Double; find("d").find("e").string()=="f".
TEST(JsonLarge, MissingKeysAndWrongTypesGiveNullViews)
// find("zz"), at(9), find on an array, at on an object -> Type::Null; string() of an Int -> "".
TEST(JsonLarge, AppendToReserializesASubtree)
// std::string out; root().find("d").append_to(out); out == R"({"e":"f"})"; whole root equals the compact input.
TEST(JsonLarge, InvalidJsonIsInvalidArgument)
TEST(JsonLarge, DeeperThanTheLimitIsQuotaExceeded)   // 33 nested arrays with the default 32
TEST(JsonLarge, StringsOver64KiBKeepTheirLength)     // a 70000-byte string value
TEST(JsonLarge, EveryAllocationGoesThroughTheAllocator)
// parse a 200-member object; counting alloc: allocations > 0; operator-new blocks counted during
// parse, find, at, string and append_to(std::string reserved beforehand) == 0;
// after the LargeJson is destroyed, allocations == frees.
TEST(JsonLarge, AllocatorFailureIsNoMemory)          // allocate returns nullptr
```

- [ ] **Step 2: Run** `cmake --build build/sim`. Expected: build FAIL (`parse_json_large` undeclared).
- [ ] **Step 3: Implement** in `json.cpp`. The adapter derives from `ArduinoJson::Allocator` and forwards to the `JsonAlloc`. `impl_` points at a struct holding the adapter and the `JsonDocument` constructed with it, the struct's own storage coming from `mem.allocate`. Pass the input as `(const char*, size_t)` so strings are copied into the document. `JsonView` stores a `JsonVariantConst` by placement new (`static_assert(sizeof(JsonVariantConst) <= sizeof v_)`). Write `large_json.hpp` in `wisp_net`.
- [ ] **Step 4: Run** `cmake --build build/sim && ctest --test-dir build/sim -R 'JsonLarge|Json\.' --output-on-failure`. Expected: PASS.
- [ ] **Step 5: Commit** `feat(base): parse_json_large() with an injected allocator`.

---

### Task 2: `stop_is_end` in the HTTP clients, `stop_is_success` in `SceneHttp`

**Files:**
- Modify: `components/wisp_net/include/wisp/net/http.hpp:204-247` (`HttpClientRequest::stop_is_end`), `components/wisp_port_sim/net/sim_http_client.cpp:268-297`, `components/wisp_port_esp/net/esp_http_client.cpp:347-352` (and its result path), `tests/support/fake_http.hpp:158-170`
- Modify: `components/wisp_net/include/wisp/net/scene_http.hpp:209-244` (`SceneHttpRequest::stop_is_success`) and the header comment (`:115-120`), `components/wisp_net/src/scene_http.cpp` (`execute()`, `:771-925`)
- Test: `tests/sim/sim_http_client_test.cpp`, `tests/net/scene_http_test.cpp`

**Interfaces:**
- Produces:

```cpp
// HttpClientRequest
// With on_body: a false return from on_body ends the transfer as a
// success -- transport None, status and headers kept, body empty -- the
// way truncate_at_limit does. Without it, false is Cancelled. SceneHttp
// sets it for SceneHttpRequest::stop_is_success only.
bool stop_is_end = false;

// SceneHttpRequest (Agent principal with on_body only; ignored otherwise)
// on_body returning false ends the request with error "", the status and
// the headers: a streamed answer (SSE) that the caller has finished
// reading is not an error (spec 4.4).
bool stop_is_success = false;
```

- [ ] **Step 1: Write the failing tests.**

```cpp
// sim_http_client_test.cpp (loopback server, as StreamsBodyHeadersAndCancels at :188)
TEST(SimHttpClient, StopIsEndKeepsStatusAndHeaders)
// server: 200, header "Mcp-Session-Id: s-1", 64 KiB body; on_body returns false on the first piece.
// stop_is_end: transport None, status 200, header "mcp-session-id" == "s-1", body empty.
// Same request without it: transport Cancelled, headers empty (today's contract).

// scene_http_test.cpp (FakeHttpClient responder: 200, headers {{"mcp-session-id","s-1"},
// {"content-type","text/event-stream"}}, a 10 KiB body)
TEST_F(SceneHttpTest, AgentStopIsSuccessKeepsStatusAndHeaders)
// Agent principal, on_body returns false at once, stop_is_success: error "", status 200,
// headers carry mcp-session-id, body empty.
TEST_F(SceneHttpTest, AgentStopWithoutStopIsSuccessIsOther)          // error "other"
TEST_F(SceneHttpTest, StopIsSuccessIsIgnoredForTheScenePrincipal)   // error "other"
TEST_F(SceneHttpTest, StopIsSuccessDoesNotHideACancel)
// cancel_scene(kAgentHttpId) from before_piece: done() never called (as for any cancel).
TEST_F(SceneHttpTest, StopIsSuccessDoesNotHideTheDeadline)
// before_piece advances the clock past timeout_ms: error "timeout".
```

- [ ] **Step 2: Run** `cmake --build build/sim && ctest --test-dir build/sim -R 'SimHttpClient|SceneHttpTest' --output-on-failure`. Expected: build FAIL (unknown members).
- [ ] **Step 3: Implement.**
  - Sim client: a `t.stopped` flag set when `on_body` returns false under `stop_is_end`, treated like `t.truncated` in the result (`:272-288`).
  - ESP client: on a false return under `stop_is_end`, leave the read loop as the truncated path does (`:340-352`) and keep status and headers.
  - `FakeHttpClient`: on a false return under `stop_is_end`, return `{None, status, "", headers}`.
  - `SceneHttp::execute()`:
    - set `req.stop_is_end = is_agent && job.stop_is_success && streamed`;
    - record `consumer_stopped` when `job.on_body` itself returned false;
    - with transport None and `job.cancelled` set, keep today's "other". The flags for timeout, `too_large` and the error-body cut stay checked first, as now.
  - Update the header comment's `on_body` paragraph (`:115-120`) to describe the flag.
- [ ] **Step 4: Run** the Step 2 command, then `ctest --test-dir build/sim -R 'SceneHttp|HttpFetch|LlmClient' --output-on-failure`. Expected: PASS.
- [ ] **Step 5: Firmware build** for both boards (Task 11 Step 5's command). Expected: both build, no warnings. If ESP-IDF is unavailable, write "firmware build not run" in the commit body.
- [ ] **Step 6: Commit** `feat(net): let an agent request end at its reader's stop with status and headers`.

---

### Task 3: `McpServerService`

**Files:**
- Create: `components/wisp_core/include/wisp/core/http_rules.hpp`, `components/wisp_core/src/http_rules.cpp`
- Modify: `components/wisp_core/src/model_profiles.cpp:91-140` (use `valid_http_url()` and `url_origin()`; delete the moved code), `components/wisp_net/src/scene_http.cpp` (`header_syntax_ok()` delegates to `core::http_header_syntax_ok()`)
- Create: `components/wisp_core/include/wisp/core/mcp_servers.hpp`, `components/wisp_core/src/mcp_servers.cpp`
- Modify: `components/wisp_core/CMakeLists.txt` (two sources)
- Create: `tests/core/mcp_servers_test.cpp`. Modify `tests/CMakeLists.txt` after `:60`: `wisp_test(NAME mcp_servers_test SRCS core/mcp_servers_test.cpp LIBS wisp_core wisp_port_sim_os)`.

**Interfaces:**
- Consumes: `StorageManager` (System area, `write_atomic`), `LatestSave` (`latest_save.hpp:45-58`), `StateCell<T>(EventBus&, T)` (`state_cell.hpp:239`), `ModelRandomFn`-style random (none needed: ids come from names), the `fake_io_queue.hpp` test helper.
- Produces (`namespace wisp::core`):

```cpp
// http_rules.hpp
bool valid_http_url(std::string_view url);       // exactly normalize_base_url()'s acceptance, no stripping
std::string url_origin(std::string_view url);    // origin_of()'s result, moved
bool url_has_userinfo(std::string_view url);
bool http_header_syntax_ok(std::string_view name, std::string_view value);  // RFC 9110 token; no CR, LF, NUL

// mcp_servers.hpp
inline constexpr size_t kMcpMaxServers = 8;
inline constexpr size_t kMcpMaxHeaders = 8;
inline constexpr size_t kMcpMaxIdBytes = 16;
inline constexpr size_t kMcpMaxNameBytes = 32;
inline constexpr size_t kMcpMaxUrlBytes = 2048;
inline constexpr size_t kMcpMaxHeaderValueBytes = 1024;
inline constexpr uint32_t kMcpMinTimeoutMs = 1000;
inline constexpr uint32_t kMcpMaxTimeoutMs = 120000;   // AgentHttpLimits::kMaxTimeoutMs
inline constexpr uint32_t kMcpDefaultTimeoutMs = 60000;

struct McpHeaderInfo { os::LargeString name{}; bool has_value = false; };
struct McpServerInfo {            // never a header value
    std::string id{};
    os::LargeString name{};
    os::LargeString url{};
    bool enabled = true;
    uint32_t timeout_ms = kMcpDefaultTimeoutMs;
    std::vector<McpHeaderInfo> headers{};
    uint32_t revision = 0;        // bumped by every change of this server
};
struct McpHeaderInput { std::string name{}; std::optional<std::string> value{}; };
struct McpServerInput {
    std::string name{}; std::string url{}; bool enabled = true;
    std::optional<uint32_t> timeout_ms{}; std::vector<McpHeaderInput> headers{};  // values required
};
struct McpServerUpdate {
    std::optional<std::string> name{}, url{}; std::optional<bool> enabled{};
    std::optional<uint32_t> timeout_ms{}; std::optional<std::vector<McpHeaderInput>> headers{};
};
struct McpServerTarget {          // internal: for McpClient only
    std::string id{}; os::LargeString name{}; os::LargeString url{}; uint32_t timeout_ms = 0;
    std::vector<std::pair<os::LargeString, os::LargeString>> headers{};  // with values
    uint32_t revision = 0;
};
struct McpServersState {
    uint32_t count = 0, enabled = 0, revision = 0;
    bool operator==(const McpServersState&) const = default;
};
// POST /mcp/servers/{id}/test's answer. error "" or: dns connect tls timeout http
// unsupported_protocol bad_response.
struct McpProbe {
    bool ok = false; std::string error{}; int http_status = 0; uint32_t latency_ms = 0;
    std::string protocol{}; std::vector<os::LargeString> tools{};
};
using McpProber = std::function<McpProbe(const McpServerTarget&)>;
enum class McpFailureKind { None, LimitReached };

class McpServerService {
public:
    McpServerService(StorageManager& storage, EventBus& bus, LatestSave::SubmitIo submit_io = {});
    Result<void> init();                               // flash: on wisp.fs or before it starts
    std::vector<McpServerInfo> list() const;           // configuration order
    std::optional<McpServerInfo> get(std::string_view id) const;
    Result<McpServerInfo> create(const McpServerInput& in, McpFailureKind* why = nullptr);
    Result<McpServerInfo> update(std::string_view id, const McpServerUpdate& u);
    Result<void> remove(std::string_view id);
    Result<McpProbe> test(std::string_view id) const;  // Unsupported without a prober
    void set_prober(McpProber prober);                 // nullptr clears it
    std::optional<McpServerTarget> resolve_for_request(std::string_view id) const;  // McpClient only
    const StateCell<McpServersState>& cell() const;
    void set_on_persist_failed(std::function<void()> fn);
    void clear();                                      // factory reset: forget all, delete the file
};
Value to_value(const McpServerInfo& s);   // {id,name,url,enabled,timeout_ms,headers:[{name,has_value}]}
```

  The file is `secrets/mcp.json` in the System area: `{"version":1,"servers":[{"id","name","url","enabled","timeout_ms","headers":[{"name","value"}]}]}`. It is written with `write_atomic` inside the `LatestSave` job. The class comment repeats the model profiles' secret rules (`model_profiles.hpp:18-33`) for header values.

- [ ] **Step 1: Write the failing tests** (suite `McpServers`; fixture: temp System area, `EventBus`, `FakeIoQueue` for saves):

```cpp
TEST(McpServers, CreateValidatesEveryField)
// name "" and 33 bytes; url "ftp://x", "http://", "https://u:p@h/mcp" (userinfo); timeout 999 and 120001;
// 9 headers; header name "a b"; value with "\r\n"; 1025-byte value; value ""; reserved names
// "accept", "Content-Type", "MCP-Session-Id", "mcp-protocol-version", "host" -> InvalidArgument each, nothing stored.
// Valid: timeout absent -> 60000; url kept byte for byte, trailing '/' included.
TEST(McpServers, IdComesFromTheNameAndNeverChanges)
// "My Tools!" -> "my-tools"; again -> "my-tools-2"; "天气" -> "mcp"; a 40-char ASCII name -> 16 bytes;
// update(name="Other") keeps the id.
TEST(McpServers, AtMostEightServers)        // ninth: QuotaExceeded, why == LimitReached
TEST(McpServers, ReadsNeverCarryHeaderValues)
// value "tok-SECRET-123456": list(), get(), to_value() JSON hold the name and has_value true, never the
// value or any 8-byte piece; resolve_for_request() returns it.
TEST(McpServers, UpdateKeepsAValueWhenTheEntryHasNone)
TEST(McpServers, UpdateToAnotherOriginClearsValues)
// http://a.example/mcp -> https://b.example/mcp with headers [{name:"Authorization"}] -> header dropped;
// with a new value -> kept; same origin, other path -> kept.
TEST(McpServers, CellCountsEnabledAndRevision)
// create two, disable one: {2,1,r}; revision grows per change; a refused change leaves the cell unchanged.
TEST(McpServers, RevisionIsPerServer)       // updating A bumps A's revision only
TEST(McpServers, SavesThroughTheWorkerAndReloads)
// changes queue one job each (newest wins); after running them a new service's init() reads the same
// list and resolve_for_request() gives the same values.
TEST(McpServers, ABadStoredEntryIsDroppedAlone)
// file with one valid and one invalid entry (bad url) -> one server, one warning without the url.
TEST(McpServers, CorruptFileStartsEmpty)
TEST(McpServers, ClearForgetsAndDeletesTheFile)
TEST(McpServers, TestRunsTheProberWithTheValues)
// no prober: Unsupported; fake prober gets a target with the header value and returns McpProbe{ok:true}.
TEST(McpServers, NoLogLineCarriesAValue)
// a CaptureSink (wisp::log::add_sink) over create/update/load/save: no line contains the value.
TEST(HttpRules, MatchesTheProfilesRules)
// valid_http_url and url_origin on the cases model_profiles_test covers; http_header_syntax_ok on
// "X-Key"/"v", "a b"/"v", "k"/"a\nb".
```

- [ ] **Step 2: Run** `cmake --build build/sim`. Expected: build FAIL (headers missing).
- [ ] **Step 3: Implement** `http_rules` first, and switch `model_profiles.cpp` and `SceneHttp::header_syntax_ok()` to it. Then implement the service, modelled on `ModelProfileService`: one mutex, validate everything before changing anything, `publish` by `cell.update()`. Store names, URLs and values as `os::LargeString`. Use decisions 8–11 for URLs, reserved names, ids and header updates.
- [ ] **Step 4: Run** `cmake --build build/sim && ctest --test-dir build/sim -R 'McpServers|HttpRules|ModelProfiles|SceneHttpTest' --output-on-failure`. Expected: PASS.
- [ ] **Step 5: Commit** `feat(core): McpServerService with write-only header values`.

---

### Task 4: `McpClient`: sessions, streams and tool lists

**Files:**
- Create: `components/wisp_net/include/wisp/net/mcp_client.hpp`, `components/wisp_net/src/mcp_client.cpp`, `components/wisp_net/src/mcp_wire.hpp`, `components/wisp_net/src/mcp_wire.cpp` (request bodies; the response reader: JSON-or-SSE sniffing, `SseParser`, id matching)
- Modify: `components/wisp_net/CMakeLists.txt:7-10`
- Create: `tests/support/fake_mcp_server.hpp`, `tests/net/mcp_client_test.cpp`, `tests/net/mcp_client_heap_test.cpp`. Modify `tests/CMakeLists.txt` after `:346`: `mcp_client_test` and `mcp_client_heap_test` (LIBS `wisp_net wisp_core wisp_port_sim_os`, TIMEOUT 60).

**Interfaces:**
- Consumes: Task 1's `parse_json_large()`/`large_json_alloc()`; Task 2's `stop_is_success`; Task 3's `McpServerService::resolve_for_request()`, `list()`, `cell()`, `McpProbe`, `McpProber`; `SceneHttp::submit()`/`cancel_scene()` with `HttpPrincipal::Agent`, `kAgentHttpId`, `max_redirects = 2`; `SseParser` (`llm_client.hpp:129-150`).
- Produces (`namespace wisp::net`, `mcp_client.hpp`):

```cpp
inline constexpr std::string_view kMcpProtocol = "2025-06-18";
inline constexpr std::string_view kMcpProtocolOld = "2025-03-26";
inline constexpr uint32_t kMcpRequestTimeoutMs = 15000;   // initialize, notifications/initialized, each tools/list page
inline constexpr uint64_t kMcpListTtlMs = 10 * 60 * 1000;
inline constexpr uint64_t kMcpRetryAfterMs = 60 * 1000;
inline constexpr size_t kMcpMaxTools = 128;
inline constexpr size_t kMcpMaxListPages = 16;
inline constexpr size_t kMcpMaxDescriptionBytes = 1024;
inline constexpr size_t kMcpMaxSchemaBytes = 16 * 1024;
inline constexpr size_t kMcpMaxSessionIdBytes = 256;
inline constexpr size_t kMcpMaxResponse = 512 * 1024;                    // initialize, tools/list
inline constexpr size_t kMcpMaxCallResponse = AgentHttpLimits::kMaxResponse;

enum class McpError : uint8_t { None, Unavailable, RpcError, ToolError, BadResponse, NotFound,
                                Cancelled, Timeout, UnsupportedProtocol };
// "", "mcp_unavailable", "mcp_error", "tool_error", "bad_response", "not_found", "cancelled",
// "timeout", "unsupported_protocol"
const char* to_string(McpError e);

struct McpFailure {
    McpError error = McpError::None;
    std::string category{};      // Unavailable: dns connect tls timeout offline http other
    int http_status = 0;
    int64_t rpc_code = 0;
    os::LargeString message{};   // scrubbed (Task 5); server text only for RpcError/ToolError
};
struct McpTool { os::LargeString name{}, description{}, input_schema{}; };
struct McpWait { uint64_t deadline_ms = 0; std::function<bool()> cancelled{}; };  // 0: none
struct McpListResult { McpFailure failure{}; os::LargeVector<McpTool> tools{}; };
struct McpCallResult { McpFailure failure{}; bool is_error = false; os::LargeString text{}; };  // Task 5
struct McpServerStatus {
    std::string id{}; os::LargeString name{}; bool enabled = true;
    std::optional<size_t> tools{};   // nullopt: never listed (or dropped by a change)
    std::string error{};             // the last failure's category while backing off, else ""
};
struct McpClientConfig {
    std::string client_version{};
    std::function<uint64_t()> now_ms = os::now_ms;
};

// Blocking; one caller at a time (wisp.agent). Never from wisp.shttp, whose
// lane it waits on. status() is memory only.
class McpClient {
public:
    McpClient(SceneHttp& http, core::McpServerService& servers, McpClientConfig config = {});
    McpListResult list_tools(std::string_view server_id, const McpWait& wait, bool refresh = false);
    McpCallResult call_tool(std::string_view server_id, std::string_view tool,
                            std::string_view args_json, const McpWait& wait);   // Task 5
    std::vector<McpServerStatus> status() const;
    // Initialize and list once, sharing nothing with any client's cache (the test route).
    static core::McpProbe probe(SceneHttp& http, const core::McpServerTarget& target,
                                std::string_view client_version);
};
core::McpProber make_mcp_prober(SceneHttp& http, std::string client_version);
```

  Protocol, as spec 4.2 fixes it:
  - Every request is a `POST` of one JSON-RPC 2.0 message with `Accept: application/json, text/event-stream` and `Content-Type: application/json`, plus the stored headers. After initialize it also carries `Mcp-Session-Id` (when the server gave one) and `MCP-Protocol-Version: <negotiated>`.
  - `initialize` has params `{"protocolVersion":"2025-06-18","capabilities":{},"clientInfo":{"name":"wisp","version":<client_version>}}`. Then `notifications/initialized` is sent; any 2xx is accepted. Then `tools/list`, following `nextCursor`.
  - The reader sniffs the first non-blank byte. On `{`, it collects the body up to the cap and parses it after `done`. Otherwise it parses SSE events on `wisp.shttp`, ignores requests and notifications, and returns false from `on_body` once a response with the request's id has been parsed.
  - The reader is held by a `shared_ptr` that the `on_body` closure shares, so a cancelled request cannot outlive it.
  - Waits poll the done semaphore every 20 ms, like `http.fetch` (`tools_http.cpp:381-396`). On `wait.cancelled()` the client calls `cancel_scene(kAgentHttpId)` and returns `Cancelled`. Past `wait.deadline_ms` it does the same and returns `Timeout`.
  - Log one line per request: `mcp: <id> <method> ok|error:<code> <ms> ms`.

  `FakeMcpServer : net::HttpClient` (`tests/support/fake_mcp_server.hpp`) runs under a real `SceneHttp`. It parses each request's JSON-RPC body and records the method, id, headers and URL of every request. It honours `on_body`, `stop_is_end`, `cancelled` and `watch` exactly as `FakeHttpClient` does. Its knobs: `protocol` answered (default `2025-06-18`); `session_id` (default `"s-1"`, `""` for none); `sse` (answer as `text/event-stream`, with one `notifications/message` event before the response); `keep_open` (after the response, keep sending `":\n\n"` pieces every 1 ms until stopped); `tools` and `page_size`; `on_call` (a function from name and arguments to the `result` JSON or a JSON-RPC error); `expire_sessions` (how many session-carrying requests answer 404 next); `status` / `transport` overrides; `hold`.

- [ ] **Step 1: Write the failing tests** (suite `McpClientTest`; fixture: `FakeMcpServer`, a started `SceneHttp` over it, an `McpServerService` with server `files` at `http://mcp.local/mcp` and header `Authorization: Bearer tok-SECRET-123456`, a manual clock in `McpClientConfig::now_ms`):

```cpp
TEST_F(McpClientTest, InitializeSendsTheProtocolAndClientInfo)
// list_tools("files"): requests in order initialize, notifications/initialized, tools/list;
// the first carries protocolVersion "2025-06-18", clientInfo {"name":"wisp","version":"1.2.3"},
// both Accept and Content-Type, the Authorization header; the later ones carry
// Mcp-Session-Id "s-1" and MCP-Protocol-Version "2025-06-18".
TEST_F(McpClientTest, AcceptsTheOlderRevision)          // answer 2025-03-26 -> ok; later header says 2025-03-26
TEST_F(McpClientTest, RefusesAnyOtherRevision)          // "2024-11-05" -> UnsupportedProtocol, no tools/list sent
TEST_F(McpClientTest, NoSessionIdSendsNone)
TEST_F(McpClientTest, SessionIdOver256BytesIsBadResponse)
TEST_F(McpClientTest, ExpiredSessionReinitializesOnceAndRetries)
// cached list dropped by refresh=true; expire_sessions=1 -> 404, re-initialize, retry: ok; 2 initialize total.
TEST_F(McpClientTest, ExpiredTwiceFailsWithoutALoop)
// expire_sessions=2 -> Unavailable "http" 404; exactly 2 initialize and 2 tools/list requests.
TEST_F(McpClientTest, ReadsSseAndSkipsNotifications)
TEST_F(McpClientTest, SseLeftOpenEndsAtTheResponse)
// sse + keep_open, server timeout 60000: list_tools returns ok in under 2000 ms of real time.
TEST_F(McpClientTest, FollowsNextCursor)                // page_size 2, 5 tools -> 3 tools/list requests, 5 tools
TEST_F(McpClientTest, ListLimits)
// 130 valid tools -> 128; a 2000-byte description -> cut to <= 1024 at a UTF-8 boundary;
// a 17 KiB schema and the name "a b" -> left out, one log line each naming the tool.
TEST_F(McpClientTest, ListIsCachedForTenMinutes)        // second call: no request; clock +10 min: tools/list again (no initialize)
TEST_F(McpClientTest, ConfigChangeDropsSessionAndCache) // update url -> next call initializes again
TEST_F(McpClientTest, DisabledServerIsNotContacted)     // NotFound, zero requests
TEST_F(McpClientTest, TransportFailuresAreUnavailable)
// transport Dns/Connect/Tls/Timeout and status 500 -> Unavailable with category dns/connect/tls/timeout/http(500);
// SceneHttp deferring -> "offline".
TEST_F(McpClientTest, FailureIsRememberedForAMinute)    // status() error "connect"; next call: no request until +60 s
TEST_F(McpClientTest, CancelStopsTheRequest)            // hold; cancelled() flips -> Cancelled; fake saw_cancel
TEST_F(McpClientTest, DeadlineEndsTheWait)              // hold; deadline_ms near -> Timeout
TEST_F(McpClientTest, OversizeListPageIsBadResponse)    // a page over 512 KiB
TEST_F(McpClientTest, NotJsonRpcIsBadResponse)          // "[]" and a response with another id
TEST_F(McpClientTest, ProbeReportsToolsAndLeavesNoState)
// probe(): ok, protocol, tool names, latency_ms; then status() shows tools nullopt (no cache filled).
TEST_F(McpClientTest, ProbeMapsCategories)              // offline/other -> connect; too_large -> bad_response; 2024 revision -> unsupported_protocol

// mcp_client_heap_test.cpp (replaced operator new, as tool_registry_heap_test.cpp)
TEST(McpClientHeap, ToolCacheStaysOffTheDefaultHeap)
// list 128 tools with 1 KiB descriptions: live default-heap bytes counted on the test thread
// after list_tools() returns <= 1024.
```

- [ ] **Step 2: Run** `cmake --build build/sim`. Expected: build FAIL (`mcp_client.hpp` missing).
- [ ] **Step 3: Implement** `mcp_wire` (bodies into `os::LargeString`; the reader), then `McpClient`:
  - Per-server state: a session id, the negotiated protocol, the tool cache, `listed_at`, the last failure and `failed_at`, and the server `revision` seen. It lives in an `os::LargeVector` guarded by a mutex that is never held across a wait.
  - `list_tools` follows decisions 12, 14 and 21.
  - `probe()` runs the same steps on a temporary state.
  - `call_tool` returns `NotFound` until Task 5.
- [ ] **Step 4: Run** `cmake --build build/sim && ctest --test-dir build/sim -R 'McpClient|SceneHttpTest' --output-on-failure`. Expected: PASS.
- [ ] **Step 5: Commit** `feat(net): MCP client sessions and tool lists over streamable HTTP`.

---

### Task 5: `McpClient::call_tool`, results and scrubbing

**Files:**
- Create: `components/wisp_net/src/secret_scrub.hpp`, `components/wisp_net/src/secret_scrub.cpp`
- Modify: `components/wisp_net/src/llm_client.cpp:249-257` (delete `holds_key_piece()`, call `detail::holds_secret_piece()`; nothing else), `components/wisp_net/src/mcp_client.cpp`, `mcp_wire.cpp`, `components/wisp_net/CMakeLists.txt`
- Test: `tests/net/mcp_client_test.cpp` (append), `tests/net/llm_client_test.cpp` (unchanged; must stay green)

**Interfaces:**
- Consumes: Task 4's client, reader and fake.
- Produces:

```cpp
// secret_scrub.hpp (namespace wisp::net::detail)
// Whether `text` holds `secret` whole, or any run of 8 or more bytes of it.
bool holds_secret_piece(std::string_view text, std::string_view secret);
inline constexpr std::string_view kWithheldMessage = "(message withheld: it quoted a credential)";
```

  `call_tool(server, tool, args_json, wait)`:
  - It ensures the list (cached or fresh). A tool missing from it is `NotFound`, with no request.
  - It sends `tools/call` `{"name":tool,"arguments":<args_json>}` with the server's `timeout_ms` and the cap `kMcpMaxCallResponse`.
  - Result content items are joined with `"\n"` in order:
    - `text`: the text;
    - `image`: `[image omitted: <mime>, <n> bytes]`, where n is the decoded size from the base64 length (decision 3);
    - `resource_link`: `[resource: <name> <uri>]`;
    - `audio`: `[audio omitted]`;
    - `resource`: `[resource omitted]`.
  - `structuredContent` is appended as raw JSON only when there is no `text` item (decision 7).
  - `isError: true` sets `is_error` (the provider maps it to `tool_error`).
  - A JSON-RPC error gives `RpcError` with code and message. Code `-32602`, or a message that says the tool is unknown, refreshes the list once and follows decision 16.
  - Every server-supplied message passes `holds_secret_piece()` against each header value of that server, and on a hit becomes `kWithheldMessage`.

- [ ] **Step 1: Write the failing tests.**

```cpp
TEST_F(McpClientTest, CallSendsNameAndArguments)        // params == {"name":"read","arguments":{"path":"/a"}}
TEST_F(McpClientTest, CallJoinsContentInOrder)
// [text "a", image png (base64 of 300 bytes), resource_link {name "n", uri "u"}, audio, resource, text "b"]
// -> "a\n[image omitted: image/png, 300 bytes]\n[resource: n u]\n[audio omitted]\n[resource omitted]\nb"
TEST_F(McpClientTest, StructuredContentOnlyWithoutText)
TEST_F(McpClientTest, IsErrorIsReported)                // is_error true, text kept
TEST_F(McpClientTest, RpcErrorCarriesCodeAndMessage)    // -32000 "disk full"
TEST_F(McpClientTest, QuotedCredentialIsWithheld)
// message "bad token tok-SECRET-123456" and "...SECRET-1..." (8+ bytes of it) -> kWithheldMessage;
// "invalid" (no 8-byte piece) kept.
TEST_F(McpClientTest, UnknownToolRefreshesTheListAndIsNotRetried)
// -32602 and the tool gone from the new list -> NotFound; exactly one tools/call request.
TEST_F(McpClientTest, UnknownToolStillListedIsRpcError)
TEST_F(McpClientTest, ToolNotInTheListIsNotFoundWithoutACall)
TEST_F(McpClientTest, CallUsesTheServersTimeout)        // timeout_ms 1000, hold -> Unavailable "timeout" after ~1 s
TEST_F(McpClientTest, CallResponseOver1MiBIsBadResponse)
TEST_F(McpClientTest, NoLogLineCarriesAHeaderValue)
// CaptureSink over list + call + error paths: no line contains "tok-SECRET-123456", "/mcp" path, the
// arguments or the result text; each request logged once as "mcp: files <method> ...".
TEST(SecretScrub, FindsWholeAndEightBytePieces)
```

- [ ] **Step 2: Run** `cmake --build build/sim && ctest --test-dir build/sim -R 'McpClient|SecretScrub' --output-on-failure`. Expected: FAIL (call_tool returns NotFound; `secret_scrub.hpp` missing).
- [ ] **Step 3: Implement** as described above.
- [ ] **Step 4: Run** `ctest --test-dir build/sim -R 'McpClient|SecretScrub|LlmClient' --output-on-failure`. Expected: PASS.
- [ ] **Step 5: Commit** `feat(net): MCP tool calls with scrubbed server messages`.

---

### Task 6: `McpToolProvider` and the dynamic-info line

**Files:**
- Create: `components/wisp_agent/include/wisp/agent/mcp_tool_provider.hpp`, `components/wisp_agent/src/mcp_tool_provider.cpp`
- Modify: `components/wisp_agent/include/wisp/agent/tool_index.hpp` (declare `match_query`), `components/wisp_agent/src/tool_index.cpp:21-48` (`matches_query()` calls it), `components/wisp_agent/CMakeLists.txt`
- Modify: `components/wisp_agent/include/wisp/agent/context.hpp:52-66` (`DeviceSnapshot::mcp`), `components/wisp_agent/src/prompt.cpp` (`dynamic_info()`, after the `Network:` line at `:234-236`; nothing else)
- Create: `tests/agent/mcp_tool_provider_test.cpp` (+ `tests/CMakeLists.txt`, LIBS `wisp_agent wisp_port_sim_os`, TIMEOUT 60); Modify: `tests/agent/context_test.cpp`

**Interfaces:**
- Consumes: J4a's `ToolProvider`, `ToolSearch`, `ToolSourceIssue`, `ToolFindFields`, `add_find_tools()`; Task 4/5's `McpClient`; Task 3's `McpServerService::list()`; `validate_args()`, `schema_supported()`, `tool_error()`, `external_data()` (`tool_registry.hpp:152-167`); `detail::external_data_large()`; `ui::tr_fill_n(StrId::AgentNoteToolCall, …)`.
- Produces:

```cpp
// tool_index.hpp: ToolIndex's rule (every whitespace-separated word occurs in name or
// description; ASCII case-insensitive), shared with the MCP provider.
bool match_query(std::string_view query, std::string_view name, std::string_view description);

// context.hpp, DeviceSnapshot
std::vector<std::string> mcp;  // "<id> (<n> tools)" ... (decision 18); empty: no line

// mcp_tool_provider.hpp
inline constexpr uint64_t kMcpSearchBudgetMs = 20000;
struct McpProviderConfig { uint64_t search_budget_ms = kMcpSearchBudgetMs; std::function<uint64_t()> now_ms = os::now_ms; };
class McpToolProvider final : public ToolProvider {
public:
    McpToolProvider(net::McpClient& client, core::McpServerService& servers, McpProviderConfig config = {});
    std::string_view id() const override;              // "mcp"
    ToolFindFields find_fields() const override;       // {"server_name", "available"}
    bool owns(std::string_view full_name) const override;  // contains ':'
    void search(std::string_view query, std::string_view scope, size_t max, bool details,
                ToolCtx& ctx, ToolSearch& out) override;
    size_t count() const override;                     // cached tools of enabled servers
    std::optional<ToolInfo> describe(std::string_view full_name, ToolCtx& ctx) override;
    ToolOutput call(std::string_view full_name, const Value& args, ToolCtx& ctx) override;
    std::vector<std::string> summary_lines() const;    // decision 18, memory only
    std::vector<net::McpServerStatus> status() const;  // the client's, for mcp.servers
};
```

  Mapping:
  - A tool's full name is `<server id>:<tool>`. The description is `external_data("mcp:<id>", …)`. `parameters` is the schema text in details mode. `origin` is the server's name.
  - In `search`, the servers outside `scope` are skipped. A failed or backing-off server adds `{id, category}` to `out.unavailable`, and so does one the budget did not reach (`timeout`).
  - In `call`:
    - `args` must be an object (`bad_args`, field `""`);
    - when `schema_supported()` accepts the parsed schema, `validate_args()` runs first;
    - success is `{"ok":true,"result":<external_data("mcp:<id>", text)>}` with the note `AgentNoteToolCall` {server name, tool};
    - `is_error` is `tool_error` with the wrapped text;
    - `RpcError` is `mcp_error` with the wrapped message and extra `{"code":<n>}`;
    - `Unavailable` is `mcp_unavailable` with the message `"<category>"` or `"http <status>"`;
    - `BadResponse`, `NotFound`, `Cancelled`, `Timeout` and `UnsupportedProtocol` keep their names.

- [ ] **Step 1: Write the failing tests** (suite `McpToolProviderTest`; fixture: Task 4's fake, `SceneHttp`, service and client, plus a `ToolRegistry` with this provider and `add_find_tools`; servers `files` "Files" and `web` "Web"):

```cpp
TEST_F(McpToolProviderTest, FindListsServerToolsWithPrefixedNames)
// tools_find {"query":"read"} -> name "files:read", description starts "<data source=\"mcp:files\">",
// "server_name":"Files", "available":true, parameters object.
TEST_F(McpToolProviderTest, FindFiltersByScope)                  // {"scene":"web"} -> only web:* tools
TEST_F(McpToolProviderTest, UnreachableServerIsListedUnavailable)
// web fails connect -> "unavailable":[{"server":"web","error":"connect"}], files' tools still listed.
TEST_F(McpToolProviderTest, FailedServerIsNotRetriedWithinAMinute)
TEST_F(McpToolProviderTest, SearchBudgetBoundsSlowServers)
// both servers hold; search_budget_ms 300 -> both unavailable "timeout", real time < 1500 ms.
TEST_F(McpToolProviderTest, CallValidatesLocallyWhenTheSchemaIsSupported)  // bad_args field "path", no tools/call sent
TEST_F(McpToolProviderTest, CallSendsUnvalidatedWhenTheSchemaIsUnsupported) // schema with "oneOf" -> request sent
TEST_F(McpToolProviderTest, CallResultShapeAndNote)
// {"ok":true,"result":"<data source=\"mcp:files\">hello</data>"}; note zh "调用 Files · read", en "Call Files · read".
TEST_F(McpToolProviderTest, CallFailuresMapToToolErrors)
// connect -> mcp_unavailable "connect"; RPC -32000 -> mcp_error with code; isError -> tool_error;
// disabled server -> not_found; "nope:x" -> not_found.
TEST_F(McpToolProviderTest, CountIsCachedToolsOfEnabledServers)
TEST_F(McpToolProviderTest, SummaryLines)
// before any use: {"files (not connected yet)", "web (not connected yet)"}; after a list of 3 and a failure:
// {"files (3 tools)", "web (unavailable: connect)"}; a disabled server is absent.
TEST(MatchQuery, SameRuleAsToolIndex)                       // "TEMPERATURE city" matches as in SceneTools

// context_test.cpp
TEST(Context, DynamicInfoListsMcpServers)
// d.mcp = {"files (3 tools)", "web (not connected yet)"} -> the line
// "MCP: files (3 tools), web (not connected yet)\n" right after the Network line.
TEST(Context, DynamicInfoWithoutMcpServersIsUnchanged)      // empty mcp: no "MCP:" anywhere
```

- [ ] **Step 2: Run** `cmake --build build/sim`. Expected: build FAIL.
- [ ] **Step 3: Implement** the provider, `match_query()` and the `dynamic_info()` line.
- [ ] **Step 4: Run** `cmake --build build/sim && ctest --test-dir build/sim -R "McpToolProvider|MatchQuery|Context|ToolIndex|$GOLDEN" --output-on-failure`. Expected: PASS.
- [ ] **Step 5: Commit** `feat(agent): MCP tools through tools.find and tools.call`.

---

### Task 7: The `mcp.*` agent tools

**Files:**
- Create: `components/wisp_agent/src/tools_mcp.cpp`; Modify: `components/wisp_agent/include/wisp/agent/tools.hpp`, `components/wisp_agent/CMakeLists.txt`
- Modify: `components/wisp_ui/include/wisp/ui/strings.hpp` (after `AgentNoteToolCall`, `:255`, and with the `Act*` ids), `components/wisp_ui/src/strings_{zh,ja,en}.cpp`, `components/wisp_shell/src/activity.cpp:20-65` (four rows)
- Create: `tests/agent/mcp_tools_test.cpp` (+ `tests/CMakeLists.txt`)

**Interfaces:**
- Consumes: Task 3's service (`list`, `create`, `update`, `remove`); Task 6's `McpToolProvider::status()` (tool counts and last errors).
- Produces:

```cpp
struct McpToolDeps {
    core::McpServerService& servers;
    McpToolProvider& provider;
};
// mcp.servers, mcp.add, mcp.remove, mcp.enable (spec 4.3, decision 7): ToolThread::Agent,
// no confirmation; header values are never read or set here.
Result<void> add_mcp_tools(ToolRegistry& reg, McpToolDeps deps);
```

  | Tool | Schema | Risk | Result / note |
  |---|---|---|---|
  | `mcp.servers` | `{}` | Normal | `{"ok":true,"servers":[{"id","name","url","enabled","tools":<n or null>,"error":<category or null>,"headers":[<names>]}],"max":8}` |
  | `mcp.add` | `{"name":{"type":"string","maxLength":32},"url":{"type":"string"}}`, both required | Note | `{"ok":true,"server":{"id","name","url","enabled":true}}`; `bad_args` with the service's message, `limit_reached`; note `AgentNoteMcpAdded` {name} |
  | `mcp.remove` | `{"id":string}` required | Note | `{"ok":true}`; `not_found`; note `AgentNoteMcpRemoved` {name} |
  | `mcp.enable` | `{"id":string,"enabled":boolean}` required | Note | `{"ok":true,"enabled":b}`; `not_found`; note `AgentNoteMcpEnabled` / `AgentNoteMcpDisabled` {name} |

  New `StrId`s: `AgentNoteMcpAdded`, `AgentNoteMcpRemoved`, `AgentNoteMcpEnabled`, `AgentNoteMcpDisabled`, `ActMcpServers`, `ActMcpAdd`, `ActMcpRemove`, `ActMcpEnable`. Wording follows the neighbouring rows (for example zh `添加了 MCP 服务器「{}」`, ja `MCP サーバー「{}」を追加`, en `Added MCP server "{}"`). Activity rows: `mcp.servers` no target; `mcp.add`, `mcp.remove`, `mcp.enable` with target (the name or the id).

- [ ] **Step 1: Write the failing tests** (suite `McpTools`; service with server `files` holding header `Authorization: Bearer tok-SECRET-123456`):

```cpp
TEST(McpTools, ServersNeverCarryHeaderValues)   // result has "Authorization" in headers, never the value
TEST(McpTools, AddCreatesAnEnabledServerWithoutHeaders)
// args {"name":"Docs","url":"https://docs.example/mcp","headers":[{"name":"X","value":"v"}]} ->
// stored server has no headers (extra property ignored); note zh "添加了 MCP 服务器「Docs」".
TEST(McpTools, AddValidationIsBadArgs)          // url "ftp://x" -> bad_args, nothing stored
TEST(McpTools, AddPastEightIsLimitReached)
TEST(McpTools, RemoveAndEnable)                 // notes in zh, ja, en; unknown id -> not_found
TEST(McpTools, RunOnTheAgentThreadWithoutConfirmation)
// each def: thread Agent, risk != Confirm.
```

- [ ] **Step 2: Run** `cmake --build build/sim`. Expected: build FAIL.
- [ ] **Step 3: Implement** the tools, the strings and the activity rows.
- [ ] **Step 4: Run** `cmake --build build/sim && ctest --test-dir build/sim -R 'McpTools|Activity|Strings' --output-on-failure && python3 tools/gen_cjk_font_test.py`. Expected: PASS. If the font test fails, run `python3 tools/gen_cjk_font.py` as its message says, commit the regenerated font with this task, and note `idf.py fonts-flash` under device checks (Task 11).
- [ ] **Step 5: Commit** `feat(agent): mcp.servers, mcp.add, mcp.remove and mcp.enable tools`.

---

### Task 8: Admin routes `/api/v1/mcp/servers…`

**Files:**
- Create: `components/wisp_admin/src/routes_mcp.cpp`
- Modify: `components/wisp_admin/src/admin_internal.hpp:74` (`void add_mcp_routes(std::vector<Route>& routes);  // /mcp/*`), `components/wisp_admin/src/admin_server.cpp:200` (call it), `components/wisp_admin/include/wisp/admin/admin_server.hpp:85-94` (`core::McpServerService* mcp = nullptr;` with a comment: null, every `/mcp` route answers 503 `unavailable`), `components/wisp_admin/CMakeLists.txt`
- Modify: `tests/admin/admin_fixture.hpp:52-163` (an `McpServerService` member with a fake prober, passed as `.mcp`)
- Create: `tests/admin/routes_mcp_test.cpp`; Modify: `tests/CMakeLists.txt:396` (add `routes_mcp_test` to the admin `foreach`)

**Interfaces:**
- Consumes: Task 3's service and `to_value(McpServerInfo)`; `service_error()`, `bad_request()`, `percent_decode()` (`admin_internal.hpp:137-146`); the model routes' pattern (`routes_ai.cpp`).
- Produces (auth and CSRF as for the model routes, `routes_ai.cpp:289-297`):

| Route | Body | Answer |
|---|---|---|
| `GET /mcp/servers` | — | `{"servers":[to_value…],"max":8}` |
| `POST /mcp/servers` | `{name, url, enabled?, timeout_ms?, headers?:[{name,value}]}` | 201, the server |
| `PUT /mcp/servers/{id}` | `{name?, url?, enabled?, timeout_ms?, headers?:[{name,value?}]}` (decision 11) | 200, the server |
| `DELETE /mcp/servers/{id}` | — | 204, as `DELETE /ai/profiles/{id}` |
| `POST /mcp/servers/{id}/test` | — | `{ok, error:<category or null>, latency_ms, protocol, tools:[names]}`; 503 without a prober |

  Unknown fields give 400 `invalid_argument`, a ninth server 409 `limit_reached`, an unknown id 404.

- [ ] **Step 1: Write the failing tests** (suite `AdminMcp`):

```cpp
TEST(AdminMcp, CrudRoutes)                     // create (201, has_value true), list, update, delete (204), 404 after
TEST(AdminMcp, NeedsATokenAndCsrf)             // as AdminAi.NeedsATokenAndCsrf
TEST(AdminMcp, UnknownFieldIs400)
TEST(AdminMcp, LimitReachedIs409)
TEST(AdminMcp, NoResponseEverContainsAHeaderValue)
// value "tok-SECRET-123456": every route's body, errors and the test included, holds neither the value
// nor any 8-byte piece of it.
TEST(AdminMcp, OriginChangeDropsValues)        // PUT url to another host with headers [{name}] -> headers []
TEST(AdminMcp, TestRouteReportsTheProbe)       // fake prober ok -> {ok:true, error:null, protocol, tools}; each category passes through
TEST(AdminMcp, UnavailableWithoutTheService)   // AdminDeps.mcp null -> 503
```

- [ ] **Step 2: Run** `cmake --build build/sim`. Expected: build FAIL.
- [ ] **Step 3: Implement** `routes_mcp.cpp`.
- [ ] **Step 4: Run** `cmake --build build/sim && ctest --test-dir build/sim -R 'AdminMcp|AdminAi|Routes' --output-on-failure`. Expected: PASS.
- [ ] **Step 5: Commit** `feat(admin): MCP server routes with write-only headers`.

---

### Task 9: App wiring

**Files:**
- Modify: `components/wisp_app/src/app_impl.hpp:86` (`core::McpServerService mcp{storage, bus, io_submitter()};` right after `models`), `components/wisp_app/include/wisp/app/app.hpp:367` (`core::McpServerService& mcp();`), `components/wisp_app/src/app.cpp`:
  - `:352-361`: load with `run_io`, as the models are, and set `set_on_persist_failed`;
  - before `:640`: when `scenes != nullptr && scenes->http() != nullptr`, call `impl_->mcp.set_prober(net::make_mcp_prober(*scenes->http(), impl_->platform.firmware_version))`;
  - `:1003-1006`: `impl_->mcp.set_prober(nullptr)` right after `impl_->net.reset()`;
  - `:1240-1251`: `.mcp = &impl->mcp, .firmware_version = impl->platform.firmware_version`.
- Modify: `components/wisp_app/src/factory_reset.cpp:124` (`app.mcp().clear();` after `app.models().clear();`)
- Modify: `components/wisp_app/src/net_wiring.hpp:107-135` (`core::McpServerService* mcp = nullptr;`), `components/wisp_app/src/net_wiring.cpp:158-169` (`.mcp = deps_.mcp`), `app.cpp:640-651` (`.mcp = &impl_->mcp`)
- Modify: `components/wisp_app/src/agent_wiring.hpp:178-195` (deps `core::McpServerService* mcp = nullptr; std::string firmware_version{};`). The members `std::unique_ptr<net::McpClient> mcp_client_; std::unique_ptr<agent::McpToolProvider> mcp_tools_;` are declared before `registry_` (`:318`), next to J4a's `scene_tools_`.
- Modify: `components/wisp_app/src/agent_wiring.cpp`:
  - the context lambda (`:435-442`) gains `if (mcp_tools) d.mcp = mcp_tools->summary_lines();`;
  - after J4a's scene provider registration: when `deps_.mcp && deps_.scene_http`, build the client and the provider, then `registry_->add_provider(*mcp_tools_)` and `check("mcp", agent::add_mcp_tools(...))`.
- Modify: `docs/architecture.md:48` (boot loads `secrets/mcp.json` on the storage thread too), `:129` (factory reset clears `secrets/mcp.json`)
- Test: `tests/app/factory_reset_test.cpp`, `tests/app/app_agent_test.cpp`, `tests/app/app_boot_test.cpp`

**Interfaces:**
- Consumes: Tasks 3–8.
- Produces: `App::mcp()`.

- [ ] **Step 1: Write the failing tests.**

```cpp
TEST(FactoryReset, ClearsMcpServers)
// app.mcp().create({.name="Files",.url="http://mcp.local/mcp",.headers={{"Authorization","Bearer t"}}});
// drain the worker; <System>/secrets/mcp.json exists; after the reset it does not, and app.mcp().list() is empty.
TEST(AppBoot, McpServersLoadAtBoot)
// pre-write a valid secrets/mcp.json in the System area; after start, app.mcp().list() has the server.
TEST(AppAgent, McpToolsAreRegistered)
// agent_tools() has mcp_servers, mcp_add, mcp_remove, mcp_enable (EveryRegisteredToolHasAnActivityLabel
// then covers their labels).
TEST(AppAgent, McpServersReachTheDynamicInfo)
// UiNetHarness with opts.http_client = a FakeLlmServer kept by the test (as voice_wiring_test.cpp does);
// create an llm profile and an MCP server "files"; submit a turn; the first request's last message
// contains "MCP: files (not connected yet)". No MCP request is made (lazy).
```

- [ ] **Step 2: Run** `cmake --build build/sim && ctest --test-dir build/sim -R 'FactoryReset|AppBoot.McpServers|AppAgent' --output-on-failure`. Expected: FAIL (`App::mcp` undeclared).
- [ ] **Step 3: Implement** the wiring as listed under Files, and the two `architecture.md` sentences.
- [ ] **Step 4: Run** `ctest --test-dir build/sim -R "FactoryReset|AppBoot|AppAgent|AppNet|AgentWiring|$GOLDEN" --output-on-failure`. Expected: PASS.
- [ ] **Step 5: Commit** `feat(app): wire MCP servers into the agent, the admin routes and the factory reset`.

---

### Task 10: Web "MCP servers" card

**Files:**
- Create: `web/app/pages/mcp_servers.js` (`export function McpServersCard()`)
- Modify: `web/app/pages/models.js` (import, and render `<${McpServersCard} />` after `KindPanel` when `kind === 'llm'`, `:283-293`), `web/app/i18n/{en,zh,ja}.js` (`mcp.*` keys), `web/styles/*` only if an existing class does not fit

**Interfaces:**
- Consumes: Task 8's routes through `api()` (`web/app/api.js`); `Card`, `dialog`, `toast` components; the write-only key field pattern of `models.js` (`:8-12`, `:568-570`, `:687-706`, `:905`).
- Produces: UI only.

  The card contains:
  - A title and the count "n of 8".
  - One row per server: name, id, URL, an enabled switch (PUT `{enabled}`), header names each marked set or unset, and the Test, Edit and Delete buttons. Delete asks for confirmation with the existing dialog.
  - The test outcome: protocol and tool count with the names, or the error category in words (`dns connect tls timeout http unsupported_protocol bad_response`, one i18n key each).
  - A form with name, URL, enabled, timeout in seconds (1–120, default 60), and header rows of a name plus a value input:
    - the value is a password-type input;
    - when a value is stored, its placeholder is "unchanged" and the value is sent only when typed (decision 11);
    - "remove" drops a row;
    - when the URL's origin changes, rows that only have stored values show "will be cleared" before saving;
    - after saving, a header that came back without a value raises the same toast the models page shows for a dropped key.
  - An empty state, a loading state, a load error with Retry, and a disabled "Add" at 8 servers.
  - Every string in zh, ja and en. No value is ever logged, stored in `localStorage` or kept in state after the request.

- [ ] **Step 1: Write the strings first** (en, zh, ja with the same keys). Run `python3 tools/check_i18n.py`. Expected: FAIL until all three files have every new key, then PASS.
- [ ] **Step 2: Implement** the card and its placement.
- [ ] **Step 3: Run** `python3 tools/check_i18n.py && python3 tools/pack_assets.py web -o build/ci/web-budget.wpak --budget-kb 170`. Expected: both pass, within the gzip budget.
- [ ] **Step 4: Check by hand in the simulator** (frontend tests are out of scope, spec 7). Run `cmake --build build/sim && build/sim/wisp_sim --http-port 8900`, pair, and open `#/models/llm` at desktop width and at 390 px wide. Check, and fix before going on:
  - the empty state, add, the long-URL wrap, the header rows and the 8-server disabled state;
  - a test against `http://127.0.0.1:9/mcp`, which shows "connect";
  - the origin-change notice;
  - at least 8 px between buttons (`docs/ui-style-guide.md`), no horizontal scroll, and dark mode.
- [ ] **Step 5: Commit** `feat(web): MCP servers card on the Models page`.

---

### Task 11: Documents and the full check

**Files:**
- Modify: `docs/architecture.md`:
  - the `wisp.agent` row (`:82`): MCP calls wait here like `http.fetch`;
  - the `wisp.shttp` row (`:69`): MCP requests and the SSE reading of their responses run here; never flash.
- Modify: `docs/superpowers/specs/2026-11-03-connectivity-admin-design.md`:
  - the route table (`:308-315`): add the five `/mcp/servers…` rows after the model rows, with the bodies of Task 8;
  - "恢复出厂设置的范围" (factory reset scope, `:324`): add the MCP servers and their headers.
- Modify: `docs/superpowers/specs/2026-11-09-agent-tools-design.md` 5.3: a bullet naming the `mcp.*` tools and the dynamic-info line (revised by the providers spec 4.3).
- Modify: `docs/backlog.md`, with two new sections at the end:
  - "## MCP client: device checks not run" — spec section 8 items 7 and 8, and `idf.py fonts-flash` if Task 7 regenerated the font;
  - "## MCP client: deferred" — MCP image results into the turn's image slot (needs J3), OAuth, the `GET` stream and `tools/list_changed`, resources and prompts. These are spec 10's MCP items that someone may ask for.

- [ ] **Step 1: Write the documents** listed above.
- [ ] **Step 2: Run** `bash tools/ci.sh`. Expected: exit 0, no warnings.
- [ ] **Step 3: Grep for leaks:** `ctest --test-dir build/sim -R 'Mcp|AdminMcp' --output-on-failure -V 2>&1 | grep -c 'tok-SECRET'`. Expected: `0`.
- [ ] **Step 4: Run the internal-RAM budget test:** `ctest --test-dir build/sim -R 'RamBudget' --output-on-failure`. Expected: PASS. This is the host-side model only; the device numbers are a device check.
- [ ] **Step 5: Firmware build**, both boards, one after the other, no warnings:

```bash
cd firmware
for B in waveshare_esp32_s3_touch_amoled_2_16 waveshare_esp32_s3_touch_amoled_1_75c; do
  bash -c ". ~/esp/esp-idf-v6.0.3/export.sh >/dev/null 2>&1 && idf.py bmgr -c ../boards -b $B && idf.py -B build-$B -D SDKCONFIG=build-$B/sdkconfig -D SDKCONFIG_DEFAULTS='sdkconfig.defaults;components/gen_bmgr_codes/board_manager.defaults' -DWISP_BOARD=$B build" || break
done
```

  Expected: both builds succeed with no warnings.
- [ ] **Step 6: Commit** `docs: MCP servers in the architecture, the route table and the backlog`.

No version bump here: the phase's merge to `main` sets `WISP_BASE_VERSION` and `CHANGELOG.md` (AGENTS.md section 7).

## Device checks (not run; for the owner)

From spec section 8, the MCP items:

1. **Two real servers.** Use one MCP server on the LAN over `http://` and one on the internet over `https://` with a bearer header. On each, run `Test` on the web card, ask the agent for something that needs `tools.find` then `tools.call`, and restart the server between two calls (expect a session `404` and one re-initialize in the log, `mcp: <id> initialize ok`).
2. **`mem` at idle and during an MCP call while TTS speaks.** Compare internal free and largest block against `ram_budget.hpp`, and note the `wisp.agent` and `wisp.shttp` high-water marks (SSE parsing now runs on `wisp.shttp`). Record the results in `docs/hardware/<board>.md`.
3. **A server answering SSE that keeps the stream open.** A call returns at once instead of after `timeout_ms`. This checks `stop_is_end` in `esp_http_client`.
4. **Agent by voice.** "Connect the MCP server at <url>" goes through `mcp.add`, with no confirmation dialog and the note on screen. The next turn's tools come from it.
5. **After a factory reset**, `secrets/mcp.json` is gone and the card is empty.
6. **If Task 7 regenerated the CJK font**, run `idf.py fonts-flash` and check that the new notes render in zh and ja.
