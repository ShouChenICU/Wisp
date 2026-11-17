# LLM and Tool Provider Interfaces — Design

- Date: 2026-11-15
- Status: approved by the owner on 2026-11-16 (all proposals of section 12 accepted)
- Scope: phase J of `docs/superpowers/plans/2026-11-14-architecture-optimization-roadmap.md`. The agent talks to the model through an internal `net::LlmProvider` seam, with `LlmClient` as its chat-completions implementation; per-profile dialect options cover the known differences between OpenAI-compatible providers; messages carry content parts and the profile's `vision` option gets a producer (`ui.screenshot`); a reply cut by `finish_reason: "length"` is continued once; tools outside the fixed `tools` array come from `agent::ToolProvider`s (scene tools, and an MCP client over streamable HTTP); `ToolThread::Io` becomes a one-shot task; the 1 MiB rollover carries a summary into the new conversation; `TurnRunner` leaves `AgentService::Impl`. The wire protocol stays OpenAI chat completions (AGENTS.md section 1): the provider interface is a seam inside the firmware, not a plan for other protocols.
- Builds on: the agent-core spec (`2026-11-05-agent-core-design.md`: 3.2 context layout, 3.3 context length, 3.5 persistence, 3.6 `LlmClient`, 5.4 screenshots, 6.3 untrusted content), the agent-tools spec (`2026-11-09-agent-tools-design.md`: 3.2 `SceneHttp`'s agent principal, 5 scene tools), the MVP spec (`2026-11-06-mvp-design.md`: screenshots deferred, `vision` kept), the connectivity spec 5.3 (model profiles, write-only keys), the settings-model shortcuts spec (`2026-11-13-settings-model-shortcuts-design.md`: what the device shows of profiles), the flash-thread design (only `wisp.fs` touches flash), and the agent turn reliability plan (`2026-11-14-agent-turn-reliability.md`: retries, overflow recovery, live tool results; it left the rollover summary to this phase).
- Serves: phase K (voice protocol interfaces follow the same "interface plus one adapter for today's request shape" pattern), phase L (the provider interfaces are what the composition root wires; `AgentDeps` loses its `ModelProfileService`), phase M (`SceneToolHost` behind `SlotHost` is the scene tool provider's back end).
- Evidence: the code on `feat/llm-providers` at `06279ef`. Every `file:line` below refers to it. Statements about provider behaviour (what DeepSeek, Moonshot, Anthropic's or OpenAI's endpoints accept) come from those providers' public documentation as I remember it and are **not verified** against live endpoints; they are marked "unverified" where they matter, and section 8 lists the live checks.

---

## 1. Background and problem

### 1.1 How a turn talks to the model today

- **Types.** `net::LlmClient` (`llm_client.hpp:152-179`) has one entry point, `LlmResult complete(const LlmCall&)` (`:173`). `LlmMessage` is a role, one `text` view, tool calls and a tool-call id, all views (`llm_client.hpp:61-66`). `LlmCall` carries the messages, the `tools` JSON text, a timeout for the whole call, a `RequestWatch`, and callbacks for text, bytes, tool progress and cancellation (`:102-120`). `LlmResult` carries the error as a string (`"no_model"`, `"unauthorized"`, `"rate_limited"`, `"server"`, `"http"`, `"transport"`, `"cancelled"`, `"bad_stream"`, `"context_length"`, `:75-79`), the text, the tool calls, `finish`, usage, attempts and the provider's scrubbed error code (`:74-93`).
- **Who calls it.** `AgentDeps` holds the concrete `net::LlmClient&` and `core::ModelProfileService&` (`agent_service.hpp:183-184`). `AgentWiring` builds the client over the platform's `HttpClient` (`agent_wiring.cpp:403`). The only caller is `AgentService::Impl::request()` (`agent_service.cpp:582-616`), used by the turn loop (`:744`) and by context compression (`:639`, a request without tools).
- **Thread and connection.** Everything runs on `wisp.agent` (16 KB stack, `flash_access = false`, `agent_service.hpp:212`, `agent_service.cpp:1022-1030`). `complete()` blocks; each attempt is one `HttpClient::request()`, and on the device each request makes and frees its own `esp_http_client` (`esp_http_client.cpp:151, 225`), so a model connection exists only while a request runs. TLS handshakes go through the process-wide `TlsGate` (one at a time, `tls_gate.hpp:3-22`).
- **Request JSON.** `build_chat_body()` (`llm_client.cpp:302-329`) writes `{"model","stream":true,"stream_options":{"include_usage":true},"messages":[...],"tools":...}` into an `os::LargeString`; nothing else from the profile reaches the body. Each message's `content` is one JSON string (`append_message()`, `:43-78`). The profile, key included, is read on every call (`:338`).
- **Reply parsing.** `SseParser` splits events (`llm_client.hpp:133-150`); `StreamReader::read_delta()` reads only `delta.content` and `delta.tool_calls` (`llm_client.cpp:154-187`); every other delta field, `reasoning_content` included, is ignored, though its bytes still count for stall detection (`on_bytes`, `:391`).
- **Request layout.** `context.cpp` builds the messages: system prompt first, the history (summaries as `system` messages at the place of the turns they cover, `context.cpp:151-152`), then the dynamic info as one trailing `system` message (`context.cpp:309-314`; `context.hpp:5-18`). The system prompt tells the model so in words: "The last system message begins with your memory notes" (`prompt.cpp:138`, `:178`).
- **Memory.** The body, the reply text and tool arguments are `os::LargeString` (PSRAM) (`llm_client.hpp:29-30`); each SSE event is parsed on its own with `from_json` (`llm_client.cpp:115`), briefly a small default-heap tree (a backlog entry on the per-event `from_json` tree).

### 1.2 Provider-specific behaviour

| # | Behaviour | Today | Evidence |
|---|---|---|---|
| P1 | Output token limit (`max_tokens` or `max_completion_tokens`) | never sent; the provider's default applies | `llm_client.cpp:313-327` |
| P2 | Reasoning text (`reasoning_content`) | dropped while streaming; never sent back. Some providers are documented to require it to be echoed during a tool-call loop and others to refuse it (unverified) | `llm_client.cpp:154-187` |
| P3 | Where non-leading system content goes | dynamic info is a trailing `system` message, summaries are mid-history `system` messages; a gateway that hoists system messages to the front (Anthropic's compatibility layer is documented to, unverified) moves the dynamic info ahead of the history and defeats the prefix cache (the backlog recorded the summary half; closed by 3.4) | `context.cpp:151-152, 309-314` |
| P4 | `parallel_tool_calls` | never sent; the loop runs whatever calls arrive one after another | `agent_service.cpp:830-846` |
| P5 | Context window | profile option `context_window`, read once per turn | `model_profiles.hpp:108-111`, `agent_service.cpp:561-567` |
| P6 | Vision | profile option `vision`, validated (`model_profiles.cpp:275-278`), probed from OpenRouter's model list (`model_probe.cpp:405-425`), edited on the web (`models.js:547, 680-684`), shown as a badge (`chat.js:526`, `agent_wiring.cpp:79`); **nothing reads it to change a request** | grep for `profile_vision` finds only `agent_wiring.cpp:79` |
| P7 | Error recognition | `context_length` from `error.code` or the 400/413 message text | `llm_client.cpp:263-283` |
| P8 | Presets | 13 rows of id, name, kinds, base URL, key required; no behaviour | `model_presets.cpp:19-35` |

Profile options are a flat string map, at most 8 keys of at most 15 bytes, values at most 64 bytes (`model_profiles.hpp:97-102, 257-262`); unknown keys are kept as they are (`:101`); `check_options()` validates the known ones (`model_profiles.cpp:261-294`). A stored profile whose option fails validation is dropped at load, key included (the backlog entry on dropped stored options).

### 1.3 Tools today

- The `ToolRegistry` holds the built-in tools; its `tools_json()` is fixed after registration and sent with every request (`tool_registry.hpp:113-115`, `tool_registry.cpp:201-213`). The loop reads it once per turn (`agent_service.cpp:679`). Tools are registered once in `AgentWiring` (`agent_wiring.cpp:495-542`).
- Scene tools never enter the `tools` array (agent-tools spec 1.3): `ToolIndex` (`tool_index.hpp`) holds every installed scene's declared tools in PSRAM, refreshed by `SceneToolIndexObserver` on `wisp.fs` (`agent_wiring.cpp:323-393`). Two built-in tools reach it: `tools.find` (query, scene filter, 20 detailed or 200 listed, `tools_scene_tools.cpp:72-108`, `tool_index.hpp:42-43`) and `tools.call` (looks the name up, validates the arguments against the declared schema, calls `SceneRuntime::call_tool()`, wraps the result as external data and adds the note "调用 <scene> · <tool>", `tools_scene_tools.cpp:110-161`). Both are `ToolThread::Agent` and depend on `ToolIndex` and `SceneRuntime` directly. A scene tool's full name is `<scene id>.<tool>`; scene ids are `[a-z0-9_-]` (`scene_manifest.cpp:380-390`).
- The fixed prompt mentions only scenes as a source of extra tools (`prompt.cpp:134-135`).
- `ToolRegistry::call()` runs an `Agent` tool inline, with no timeout it can enforce (`tool_registry.cpp:249-251`); other threads are posted to and waited for with timeout and cancellation (`:253-306`).

### 1.4 `ToolThread::Io`

`ToolThread` has `Agent, Ui, Fs, Io, FsInteractive` (`tool_registry.hpp:43`). The comment says a tool that touches flash "must declare Fs (wisp.fs) or Io (a one-shot task with an internal stack)" (`tool_registry.hpp:74-79`). The wiring posts `Io` to the flash worker (`agent_wiring.cpp:409, 413`), and no tool declares `Io` (grep of `ToolThread::` at HEAD). The documented meaning is also wrong under the current rule: only `wisp.fs` may touch internal flash (AGENTS.md section 5), so a one-shot task touching flash is forbidden whatever its stack.

### 1.5 The 1 MiB rollover

`run_turn()` archives the conversation when `current.jsonl` is past `kRolloverBytes` (1 MiB, `limits.hpp:57`, `conversation.cpp:437`) before the turn begins (`agent_service.cpp:423`) and writes the note `AgentRolledOver` ("The conversation got long, so it was archived and a new one started", `strings_en.cpp:268-269`; `agent_service.cpp:453`). The new conversation starts empty: the model loses everything, live summaries included. Archives keep the record (`kArchivesKept = 5`, `limits.hpp:56`).

### 1.6 `finish_reason: "length"`

The stream's `finish_reason` becomes `LlmFinish::Length` (`llm_client.cpp:95-100`). A reply without tool calls that ends this way is stored and the turn ends `done` (`agent_service.cpp:805-828`), so a cut-off answer looks finished. With tool calls, the last call's arguments are treated as cut and answered with `bad_args` without running it (`:842-845`, `:862-864`); the model usually retries the call. Since P1 sends no limit, `length` comes only from the provider's own default.

### 1.7 What "vision" does

Nothing beyond P6: no tool produces an image, `LlmMessage` has no image part, and the agent-core spec's `ui.screenshot` (agent-core spec 3.2 item 2, 4 table, 5.4) was deferred by the MVP spec (`2026-11-06-mvp-design.md:134`; `docs/backlog.md:18`). Building blocks exist: `UiRuntime::snapshot_rgb888()` (`ui_runtime.hpp:214-226`), a JPEG decoder on the device (`esp_jpeg_decoder.hpp`), a PNG writer in the simulator only (`png_writer.hpp:29`). There is no JPEG encoder in the tree.

### 1.8 `AgentService::Impl`

`Impl` (`agent_service.cpp:106-1012`, about 900 lines) holds the task and its command queue, the listeners, the status cell, confirmation, the Wi-Fi subscription, the check timer and abort flags, archiving and clearing, the turn framing, and the whole turn: request building with compression and overflow recovery (`loop()`, `:663-848`), summarizing (`compress()`, `:634-661`), the provider call under the turn's controls (`request()`, `:582-616`), tool dispatch and result recording (`run_tool()`, `:850-943`), and the estimate calibration (`:998-1007`). The turn cannot be tested without the task, the store and a fake HTTP server (`tests/support/fake_llm.hpp:47`).

## 2. Goals and non-goals

### 2.1 Goals

1. The agent depends on an interface (`net::LlmProvider`), not on `LlmClient`; a fake provider can drive the turn in tests.
2. Every provider difference P1-P4 is a named, validated profile option with a default equal to today's behaviour, so a stored profile behaves as before until someone changes it.
3. A model with `vision` can see the device's screen (and fetched or MCP-returned images); a model without it never receives an image part.
4. A reply cut by the length limit is continued once.
5. Tools from more than one source are found and called through one interface; an MCP server's tools reach the agent without entering the `tools` array.
6. `ToolThread::Io` means what its name says, and the flash rule holds for it.
7. A rollover keeps a summary of the old conversation.
8. The turn is a class of its own with a test seam.

### 2.2 Non-goals

- Other wire protocols (Anthropic Messages, Gemini native, OpenAI Responses). The provider interface makes one possible later; nothing here builds one.
- Restricting the agent's outbound HTTP or LAN reach, or adding confirmations for tool calls (roadmap "Out of scope"; owner's autonomy ruling). MCP calls need no confirmation.
- Changing what the device's settings page shows of profiles (settings-model shortcuts spec 3: name, model, test, activate).
- The daily token budget (`day_tokens`, agent-core spec 1.5): still deferred.

## 3. Design: the LLM side

### 3.1 `net::LlmProvider`

New header `components/wisp_net/include/wisp/net/llm_provider.hpp`; the message and call types move there from `llm_client.hpp` (which includes it).

```cpp
namespace wisp::net {

enum class LlmError : uint8_t {
    None, NoModel, Unauthorized, RateLimited, Server, Http,
    Transport, Cancelled, BadStream, ContextLength,
};
// The strings LlmResult::error holds today ("" for None), for logs.
const char* to_string(LlmError e);

enum class LlmPartKind : uint8_t { Text, Image };

// One piece of a message's content. Views only, like LlmMessage.
struct LlmPart {
    LlmPartKind kind = LlmPartKind::Text;
    std::string_view text{};             // Text
    std::string_view mime{};             // Image: "image/jpeg" | "image/png"
    std::span<const uint8_t> bytes{};    // Image: the encoded file, not base64
};

struct LlmMessage {
    LlmRole role = LlmRole::User;
    std::string_view text{};                     // as today; used when parts is empty
    std::span<const LlmPart> parts{};            // User only (3.5)
    std::span<const LlmToolCall> tool_calls{};   // Assistant only
    std::string_view tool_call_id{};             // Tool only
    std::string_view reasoning{};                // Assistant only; sent only under reasoning=echo (3.3)
    // Where a system message stands: the fixed prompt (first), or content
    // the dialect may move (summaries, the carry, the dynamic info).
    bool movable = false;                        // System only
};

struct LlmModelInfo {
    std::string profile_id, model;
    uint32_t context_window = 0;   // profile_context_window()
    bool vision = false;           // profile_vision()
    LlmDialect dialect;            // 3.2, resolved
};

class LlmProvider {
public:
    virtual ~LlmProvider() = default;
    // The active profile as the next complete() will use it; nullopt when
    // there is none. Memory only (no flash, no network); any thread.
    virtual std::optional<LlmModelInfo> active_model() const = 0;
    // One request, retries included (as LlmClient::complete() today).
    // Blocks; one call at a time per provider object; callbacks run on
    // the calling thread from inside the stream.
    virtual LlmResult complete(const LlmCall& call) = 0;
};

}  // namespace wisp::net
```

- `LlmCall` keeps every field of `llm_client.hpp:102-120`. `LlmResult` keeps its fields; `error` becomes `LlmError` (the agent's string comparisons at `agent_service.cpp:645, 776, 792-798` become enum comparisons; log lines keep the same words through `to_string`). It gains `os::LargeString reasoning` (the reply's reasoning text, kept only under `reasoning=echo`).
- **Cancellation** is unchanged: `cancelled()` (also ends retry waits) and `watch` with `on_armed` (`llm_client.hpp:107-113`). **Error mapping** is `LlmClient`'s today (`llm_client.hpp:19-27`, `llm_client.cpp:413-461`), now in the enum.
- **Threads.** `active_model()` from any thread (it reads `ModelProfileService::get()`, memory under a mutex, `model_profiles.hpp:38-40`). `complete()` on `wisp.agent` only. Neither touches flash.
- **`LlmClient`** becomes `class LlmClient final : public LlmProvider`; its constructor, retries, scrubbing and log line stay. `active_model()` resolves the dialect (3.2) from the active profile and its preset.
- **`AgentDeps`** holds `net::LlmProvider& llm` and loses `core::ModelProfileService& models` (`agent_service.hpp:183-184`): `active_llm()` (`agent_service.cpp:561-567`) becomes `deps.llm.active_model()`. `AgentApiAdapter` keeps its own `ModelProfileService` reference for the `model` field of `GET /agent/conversation` (`agent_wiring.cpp:71-82`).
- Layering: `wisp_net` already sits below `wisp_agent`; the new header includes only `wisp_core`'s `model_profiles.hpp` and `wisp_os`.

### 3.2 Dialect options

Five new `llm` profile options. Each is absent by default, and absent means today's behaviour. All are `llm`-only: `check_options()` refuses them on other kinds, as `kKindOptions` does for the voice options (`model_profiles.cpp:253-259`).

| Option key | Values | Absent | Effect on the request |
|---|---|---|---|
| `out_tokens` | decimal 256-131072 | not sent | adds the output limit field with this value |
| `out_field` | `max_tokens` \| `max_completion_tokens` | `max_tokens` | the name of that field; meaningful only with `out_tokens` |
| `reasoning` | `drop` \| `echo` | `drop` | `echo`: keeps `reasoning_content` of the running turn's assistant messages and sends it back on them (3.3) |
| `system_msgs` | `anywhere` \| `first` | `anywhere` | `first`: only the first message may be `system`; movable system content is merged into a user message (3.4) |
| `parallel_tools` | `true` \| `false` | not sent | adds `"parallel_tool_calls":<bool>` (only when the request has tools) |

- Keys are at most 15 bytes (`model_profiles.hpp:261`). An `llm` profile can then hold 7 known options (`vision`, `context_window` and these five) under the 8-key limit (`:260`). The limit stays 8: an older firmware loading a profile with more options than it allows would drop the profile and its key (backlog `:85` behaviour), so raising it would make a downgrade lose profiles.
- **Resolution** (`LlmDialect`, a plain struct of the five values): the profile's option, else the preset's default, else the table above. Preset defaults are a new optional span in `ModelPreset` (`model_profiles.hpp:81-91`) and in `GET /api/v1/ai/presets` as `"defaults": {...}` (additive). At HEAD every preset would ship with no defaults (today's behaviour); open decision 2 proposes two.
- **Validation fix.** `load_models()` drops only the invalid option of a stored profile and logs one warning, instead of dropping the profile with its key (closes the backlog entry on dropped stored options). This matters more once there are seven options.
- **Admin web page.** The `llm` form on the Models page gains an "Advanced" group (the page already has one for `tts`, `models.js:551`): a number field for `out_tokens`, selects for the other four, each with an "(default: …)" choice that removes the key and shows the resolved default from the preset. The save code already merges options into the whole map (`models.js:680-684`); the new keys follow it. Labels in zh/ja/en (`web/app/i18n/*.js`).
- **Device.** The settings-model shortcuts show name, model, test and activate only (settings-model shortcuts spec 3.1); they do not show these options, and this phase does not add them. The agent cannot change profiles (agent-core spec 1.4); unchanged.
- **Migration.** None: stored profiles have none of the keys, so their requests do not change. Unknown keys are kept (`model_profiles.hpp:101`), so a downgrade keeps the options and ignores them.

### 3.3 Reasoning echo

- The stream reader also reads `delta.reasoning_content` (a string); under `reasoning=drop` it is counted for stall detection only (as today); under `echo` it is appended to `LlmResult::reasoning`.
- `TurnRunner` keeps the reasoning of each assistant message of the **running turn** in memory (PSRAM), keyed by the message's seq; it is never written to `current.jsonl`. `build_request()` sets `LlmMessage::reasoning` on those messages; earlier turns never carry it.
- Cap: `kReasoningKeepBytes = 64 KiB` for the turn; past it, the oldest messages' reasoning is dropped first (open decision 3).
- A turn ended by a restart is `interrupted` and never resumed (`conversation.hpp:99-101`), so losing in-memory reasoning at a reboot changes nothing.
- Shown nowhere: the screen, the web and the conversation file never see reasoning text (out of scope, section 10).

### 3.4 `system_msgs=first`

`context.cpp` marks every system message after the first as `movable` (the summaries, the carry, the dynamic info). `build_chat_body()` applies the dialect:

- `anywhere`: as today, each is its own `system` message.
- `first`: each movable message becomes a text part of a user message, wrapped `<context>…</context>`: a summary or the carry is merged into the next user message after it; the dynamic info (the last message) into the latest user message, as its first part. When no user message follows a summary (it cannot happen in a turn, since the running turn's user message is always present), it goes into the latest one.

The fixed prompt must describe where the dynamic info is (`prompt.cpp:138`, `:178`): `system_prompt()` takes the resolved `system_msgs` and says "the last system message" or "the `<context>` block at the start of the latest user message". The prompt stays byte-stable for a given language, shape and dialect.

Prefix-cache cost of `first`: the latest user message changes with every request, so the cached prefix ends before it; with `anywhere` it ends before the trailing system message. Within a long tool loop the difference is the running turn's messages after its user message, which are re-read on every step. That is the price on providers that would otherwise hoist system messages to the front, where the whole history would miss the cache.

### 3.5 Content parts and images

**Wire form.** A user message with parts is sent as `"content":[{"type":"text","text":…},{"type":"image_url","image_url":{"url":"data:<mime>;base64,…"}}]` (agent-core spec 3.6). The builder base64-encodes the bytes straight into the body (`os::LargeString`), so the image is never held twice in base64. A message without parts is sent as a string, as today.

**Tool messages carry no images.** In chat completions a `tool` message's content is text (unverified for every gateway; it is the safe form). A tool that produces an image therefore returns a text result, and the request builder emits, after the run of `tool` messages that answered one assistant message, one `user` message holding the images of that run as parts, each preceded by a text part `Image from <tool> (call <id>):`. Parallel calls keep their tool messages contiguous, as the API requires.

**What an image is on this device.**

- Source: `ui.screenshot` (3.6); `http.fetch` with `as: "image"` (open decision 6); an MCP tool's `image` content (4.4).
- Formats: `image/jpeg` and `image/png`, sent as received or encoded; nothing is transcoded.
- Size: at most `kImageMaxBytes = 128 KiB` encoded (about 171 KiB of base64 in the body). Larger: the tool fails with `too_large`.
- Memory: the bytes live in an `os::LargeBuffer` (PSRAM) owned by `TurnRunner`'s image slot.
- One live image: the slot holds the newest image of the running turn only (agent-core spec 3.3, last bullet: "only the latest screenshot is kept"). An older tool result's image is replaced in the request by the text part `[image omitted: a newer one was taken]`. The slot is emptied at the end of the turn.
- Not stored: `current.jsonl` keeps the tool's text result, which says `"image":{"mime":"image/jpeg","bytes":21345,"width":240,"height":240}`; later turns see that text only. REST and WebSocket shapes do not change.

**Without vision.** `TurnRunner` reads `active_model().vision` once per turn.

- `ui.screenshot` is a vision-only tool: `ToolDef` gains `bool vision_only`; `ToolRegistry` keeps two arrays, `tools_json(bool vision)`, the vision-only tools last (agent-core spec 3.2 item 2). The turn sends the one that matches; within a model's turns the array is stable.
- `http.fetch` with `as:"image"` and an MCP image result answer `unsupported` / a text part `[image omitted: the active model does not take images]`.
- If the profile changes between reading the flag and the request, `LlmClient` drops image parts when its own resolved profile has no vision and puts that text part in their place: the request never carries an image a model cannot take.

### 3.6 `ui.screenshot`

- Arguments: none. Thread: `ToolThread::Io` (3.9). Timeout 10 s (agent-core spec 4 table). Risk `Normal`. `vision_only`.
- Steps: `UiRuntime::snapshot_rgb888()` (posts to the UI thread and waits, `ui_runtime.hpp:214-226`); on the Io task: black out what lies outside each row's `row_span()` (agent-core spec 5.4), crop to the bounding box of the visible pixels, scale to 240 px wide (area average), encode JPEG quality 70; put the bytes in the image slot.
  - （2026-11-16 修订：原写「裁剪到 `safe_rect()`」。圆屏的 `safe_rect()` 会切掉约 40% 的可见面积，所以只涂黑看不见的像素，裁剪到可见像素的外接矩形；实施前已告知负责人。）
- Encoder: open decision 4. Proposal: `stb_image_write` (public domain or MIT), vendored unmodified under `third_party/`, one portable `base::encode_jpeg(rgb, w, h, quality, out)` in `wisp_base`, the same on the device and in the simulator; registered in `docs/references.md` and `THIRD_PARTY_NOTICES.md` (AGENTS.md section 6).
- Memory, all PSRAM: the snapshot (a `std::vector<uint8_t>` of width × height × 3: 651 KiB for a 466 × 466 panel; over 16 KB, so `malloc` places it in PSRAM, AGENTS.md section 8), the scaled image (240 × 240 × 3 = 169 KiB, `Mem::Large`), the JPEG (typically 15-25 KB per agent-core spec 5.4, not measured on this panel). The snapshot is freed before encoding.
- Result: `{"ok":true,"image":{"mime":"image/jpeg","bytes":N,"width":240,"height":H}}`.

### 3.7 `finish_reason: "length"`

A reply that ends with `LlmFinish::Length` **and no tool calls** is continued once:

1. The partial text is kept by `TurnRunner`, not stored yet; its deltas already reached the listeners.
2. A second request goes out with the usual messages, then the partial reply as an `assistant` message, then a `user` message `kContinuePrompt`: "Your previous reply was cut off by the length limit. Continue exactly where it stopped. Do not repeat anything." Neither of the two is stored.
3. Its deltas go to the listeners as `Delta` events, so the screen and `SayParser` continue the same text.
4. The stored assistant message is the partial text followed by the continuation, with the continuation's tool calls if it made any (they run as usual).
5. If the continuation also ends with `length`, or fails, the text so far is stored, a note `AgentReplyCut` ("The reply was cut off by the model's length limit", zh/ja/en) follows, and the turn goes on as for a normal reply (it ends `done` without tool calls).

Not a step (no tool call), counted in the turn's tokens. With tool calls the existing rule stays: the last call is answered `bad_args` (`agent_service.cpp:842-845`), and the model's next request is the continuation. Summary and carry requests are not continued (their text is used as it is, as `whole()` already treats `Length` as complete, `agent_service.cpp:620-625`).

### 3.8 Rollover carries a summary

New message role `carry` in `ConversationStore`: `{"seq","ts","role":"carry","turn":0,"text","from":<archive n>}`.

- **Who writes it.** `AgentService::Impl::run_turn()`, at the start of the turn that finds `needs_rollover()` (`agent_service.cpp:423`), after the turn's wake locks are taken and its state is `Thinking`, before `turn_begin`:
  1. `TurnRunner::make_carry(history)` sends one request without tools: a fixed prompt (`kCarryPrompt`: like `kSummaryPrompt`, `context.cpp:29-35`, but "for a new conversation that starts from this summary", at most 300 words) and a transcript of the live context built by `build_summary_request()`'s rules (`context.cpp:378-430`): every live summary, then the newest whole turns while the scaled estimate stays under 50 % of the window.
  2. `archive()` as today.
  3. Append the carry (seq 1, turn 0) with the text cut to `kCarryMaxBytes = 4096` at a character boundary, then the note `AgentRolledOverCarry` ("The conversation got long, so it was archived; a summary of it was kept in the new one").
- **Failures.** No model, a refused key, no network, an error or an empty reply: archive without a carry and write today's `AgentRolledOver` note; the turn goes on. A stop or a limit during the carry request ends the turn as usual (`stopped`, `time_limit`) **before** archiving, so the next turn tries again.
- **In the context.** A carry is a live summary unit at the start of the history (lead "Summary of the previous conversation:"), movable (3.4), and `pick_compression()` may fold it into a later summary like any summary (`context.hpp:115-123`).
- **Elsewhere.** `to_value()` emits it (role `carry`, `text`, `from`); `agent_message_value()` marks it `hidden` like a summary (`agent_wiring.cpp:86-93`); the web chat already renders no unknown or hidden role (`chat.js:531-549`); the Shell's history grouping must skip it as it skips summaries (`history.cpp:56-58`).
- **Compatibility.** Old files have no carry line and load as before. An older firmware reading a file with a carry drops that line (`role_from()` knows no `carry`, so `parse()` returns nullopt, `conversation.cpp:144-151, 181-182`) and keeps the rest.

### 3.9 `ToolThread::Io`

- New meaning: a one-shot task per call, `flash_access = false`, stack in PSRAM on the device (the port's choice from the declaration, `os.hpp:106-128`), for blocking work that cannot watch `ctx.cancelled` (a library call such as an encoder). `ToolRegistry::call()` already waits with timeout and cancellation and abandons the call (`tool_registry.cpp:289-306`); on `Agent` the same work would hold `wisp.agent` past its timeout (`:249-251`).
- `AgentWiring`'s `.io` executor (`agent_wiring.cpp:413`) becomes an `IoRunner`: it spawns `wisp.tool.io` (16 KB stack, priority 3) per call; `os::Task` must be joined (`os.hpp:148-158`), so finished tasks are joined by the next spawn and by `AgentWiring::stop()`; at most 2 run at once (one abandoned, one new), else the call is refused `busy` (the registry's existing answer, `tool_registry.cpp:282-287`).
- The comment at `tool_registry.hpp:74-79` changes to: a tool that touches flash declares `Fs` or `FsInteractive`; `Io` never touches flash (`WISP_ASSERT_FLASH_THREAD` already enforces this at run time, AGENTS.md section 5).
- What moves: nothing moves off `wisp.fs`, because no tool declares `Io` today; `ui.screenshot` is the first `Io` tool.

### 3.10 `TurnRunner`

New internal class in `components/wisp_agent/src/turn_runner.{hpp,cpp}`.

```cpp
// What the service lends a turn. Implemented by AgentService::Impl.
struct TurnEnv {
    virtual uint64_t append(Message m) = 0;           // on the storage worker, waits
    virtual void note(ui::StrId id) = 0;              // a note of this turn
    virtual void note_text(std::string_view t) = 0;
    virtual void set_state(AgentState s, std::string_view tool = {}) = 0;
    virtual void emit(const AgentEvent& e) = 0;
    virtual bool aborted() = 0;                       // stop, network, stall, time
    virtual Abort abort_reason() const = 0;
    virtual bool network_up() const = 0;
    virtual ui::Lang lang() const = 0;
    virtual TurnControls& controls() = 0;             // watch, ticket, last byte, in_request
};

struct TurnInput { uint32_t turn, serial, max_steps; uint64_t start_ms, turn_ms; Source source; };

class TurnRunner {
public:
    TurnRunner(net::LlmProvider& llm, ToolRegistry& tools, ConversationStore& store,
               ContextSource context, Calibration& cal, TurnEnv& env);
    Outcome run(const TurnInput& in);                       // the loop
    std::optional<os::LargeString> make_carry(std::span<const Message> history);
};
```

- **TurnRunner owns** one turn's work: building each request (context, dialect-aware), compression and the overflow recovery (today's `loop()` and `compress()`), the provider call under the turn's controls (`request()`), the length continuation (3.7), tool dispatch and result recording (`run_tool()`, `skip_calls()`), the image slot (3.5), the reasoning keep (3.3), the carry request (3.8), `finish_abort()`'s mapping of an abort to a status and note.
- **`AgentService::Impl` keeps** the task and the command queue, `start()`/`stop()`, `submit()`/`stop_turn()`, listeners and `emit()`, the status cell and `set_state()`, confirmation (`ask_user()`, `confirm_fn()`), the Wi-Fi subscription, the check timer and the abort atomics (`on_tick()`, `trigger()`, `aborted()`), turn framing (markers, `TurnBegin`/`TurnEnd`, wake locks, the done notice), archive, new conversation and clear, and `Calibration` (the estimate scale and its model, which outlive a turn, `agent_service.cpp:998-1007`; keyed by profile id and model instead of model alone, closing the backlog entry on the calibration key).
- A `TurnRunner` is constructed per turn on `wisp.agent`'s stack (a few pointers; its buffers are `Mem::Large`), so its state cannot leak into the next turn.
- The public `AgentService` API and every `AgentEvent` are unchanged.

## 4. Design: the tool side

### 4.1 `agent::ToolProvider`

New header `components/wisp_agent/include/wisp/agent/tool_provider.hpp`.

```cpp
namespace wisp::agent {

struct ToolInfo {
    os::LargeString name;         // full name, as tools.call takes it
    os::LargeString description;  // already wrapped as external data
    os::LargeString parameters;   // JSON Schema text; "" in list mode
    os::LargeString origin;       // scene name or server name, for the note
    bool available = true;        // scene running / server reachable
};

class ToolProvider {
public:
    virtual ~ToolProvider() = default;
    // "scene", "mcp": for logs and result fields.
    virtual std::string_view id() const = 0;
    // Whether a full name has this provider's syntax (not whether it exists).
    virtual bool owns(std::string_view full_name) const = 0;
    // Appends at most `max` matches (details: with parameters). `scope`:
    // a scene id or server id, "" for all. Every query word must occur in
    // the name or description (ToolIndex's rule, tool_index.hpp:31-32).
    virtual void search(std::string_view query, std::string_view scope, size_t max,
                        bool details, ToolCtx& ctx, os::LargeVector<ToolInfo>& out) = 0;
    virtual size_t count() const = 0;
    virtual std::optional<ToolInfo> describe(std::string_view full_name, ToolCtx& ctx) = 0;
    // Validated against the provider's schema by the provider itself.
    virtual ToolOutput call(std::string_view full_name, const Value& args, ToolCtx& ctx) = 0;
};

}  // namespace wisp::agent
```

- **Threads.** Every method runs on `wisp.agent` (from `tools.find`/`tools.call`, which stay `ToolThread::Agent`). A provider that waits on the network bounds its own wait by `ctx.deadline_ms` and `ctx.cancelled`, as `http.fetch` does (`tools_http.cpp:381-396`). No method touches flash.
- **Composition.** `ToolRegistry` gains `add_provider(ToolProvider&)` (before the first call, like `add()`). `tools.find` and `tools.call` move from `tools_scene_tools.cpp` to `tools_find.cpp` and work over the registered providers: `find` asks each in registration order (scenes first, then MCP) and keeps today's limits for the merged list (20 with details, 200 listed, `total` the sum of `count()`); `call` routes to the one provider whose `owns()` accepts the name, else `not_found`.
- **Names.** Scene tools keep `<scene id>.<tool>` (no `:`); MCP tools are `<server id>:<tool>` (`:` is never in a scene id, `scene_manifest.cpp:380-390`, nor in a server id, 4.3). The syntaxes cannot collide. Built-in tool names are not reachable through `tools.call`; a scene `device` exporting `status` is `device.status` through `tools.call`, as today.
- **Activity labels.** The agent's state and `ToolStart` stay `tools.call` (the registered tool), as today; the note after a successful call is `AgentNoteToolCall` "调用 <origin> · <tool>" (`tools_scene_tools.cpp:155-158`) with the provider's `origin`.
- **Scene provider.** `SceneToolProvider` wraps `ToolIndex` and `SceneRuntime` with today's behaviour, results and errors (`tools_scene_tools.cpp:39-161`).
- **Prompt.** `prompt.cpp:134-135` becomes "Scenes and connected MCP servers may export tools: …". The `tools` array does not change (agent-tools spec 1.3), so the prefix cache is untouched by MCP.

### 4.2 MCP client: protocol subset

`net::McpClient` in `wisp_net` (it uses `SceneHttp`, which lives there); `agent::McpToolProvider` adapts it.

- **Transport:** streamable HTTP, protocol revision `2025-06-18` (accepting a server that answers `2025-03-26`, the revision that introduced this transport; any other answer is `unsupported_protocol`). Every message is an HTTP `POST` of one JSON-RPC 2.0 message to the server's URL with `Accept: application/json, text/event-stream` and `Content-Type: application/json`. No batching, no `GET` stream, no `DELETE` at shutdown.
- **Methods used:** `initialize` (`clientInfo {"name":"wisp","version":<firmware version>}`, `capabilities {}`), `notifications/initialized`, `tools/list` (following `nextCursor`), `tools/call`. Not used: resources, prompts, completion, logging, sampling, elicitation, roots. Server requests and notifications that arrive in a response stream are ignored (the client declares no capability that would invite them).
- **Responses:** `application/json` (one JSON-RPC response) or `text/event-stream` (events parsed with `SseParser`, `llm_client.hpp:133-150`, until the response whose `id` matches).
- **Sessions:** the `mcp-session-id` response header of `initialize` (at most 256 bytes) is sent back on every later request, with `MCP-Protocol-Version: <negotiated>`. A `404` on a request that carried a session means the session expired: re-initialize once and retry that request once. A session lives in memory only; a reboot starts new ones.
- **Initialization** is lazy: the first `search`, `describe` or `call` that needs a server initializes it and lists its tools.
- **Tool list cache:** per server, in PSRAM, refreshed when older than `kMcpListTtlMs = 10 min` at a search, after a `call` answered "unknown tool" (JSON-RPC `-32602` or a matching message; then the call is not retried), and when the server's configuration changes. At most `kMcpMaxTools = 128` tools per server; a description cut to 1024 bytes; a schema over 16 KiB, or a name outside `[A-Za-z0-9_.-]{1,64}`, leaves that tool out with one log line.
- **Arguments:** when the tool's `inputSchema` passes `schema_supported()` (`tool_registry.hpp:154-156`), arguments are validated locally (`validate_args()`), as for scene tools; otherwise they go to the server unvalidated (the server validates; its error comes back as `mcp_error`).
- **Results:** `content` items in order: `text` items joined; `image` items (`image/jpeg`, `image/png`, at most `kImageMaxBytes`) become the turn's image (3.5) when the model has vision, else the omitted-image text part; `resource_link` gives its URI and name; `audio` and embedded resources give `[<type> omitted]`. `structuredContent` is included as JSON. `isError: true` is a tool failure. The tool result is `{"ok":true,"result":<external_data("mcp:<server id>", text)>}` or `{"ok":false,"error":"tool_error","message":<wrapped>}`, the shape of scene tools (`tools_scene_tools.cpp:142-154`).
- **Parsing memory.** A `tools/list` page of 128 tools as a `Value` tree would be thousands of small default-heap allocations, which land in internal RAM (AGENTS.md section 8; the tool registry once cost 54 KB this way, hardware doc `waveshare_esp32_s3_touch_amoled_2_16.md:471`). `wisp_base` gains `parse_json_large()`: an ArduinoJson document with a `Mem::Large` allocator behind a small read-only view (`find`, `at`, `size`, `string`, `raw()` to re-serialize a subtree into an `os::LargeString`); `json.cpp` stays the only file that includes ArduinoJson (`json.cpp:5-8`). The MCP client uses it for every response; each tool's schema is kept as text, as the registry does (`tool_registry.hpp:24-26`).

### 4.3 MCP servers: configuration

- **Service:** `core::McpServerService` in `wisp_core`, modelled on `ModelProfileService`: `/secrets/mcp.json` in the System area, `{"version":1,"servers":[{id,name,url,enabled,timeout_ms,headers:[{name,value}]}]}`, saved through `LatestSave` on `wisp.fs` (`model_profiles.hpp:41-45`), loaded at boot on `wisp.fs`, cleared by a factory reset. A `StateCell` of `{count, enabled, revision}` (no URLs, no headers) that the MCP client follows to drop sessions and caches.
- **Fields:** `id` `[a-z0-9_-]{1,16}`, unique (chosen from the name on create, fixed after); `name` 1-32 bytes; `url` `http(s)` with a host (the model profile's URL rule, `model_profiles.cpp:94-120`); `enabled`; `timeout_ms` 1000-120000, default 60000 (`AgentHttpLimits::kMaxTimeoutMs`, `scene_http.hpp:190`); `headers` at most 8, name an RFC 9110 token, value at most 1024 bytes without CR, LF or NUL (`SceneHttp::header_syntax_ok()`, `scene_http.hpp:327-331`). At most `kMcpMaxServers = 8`.
- **Secrets.** Header values are write-only, as API keys are (AGENTS.md section 6; `model_profiles.hpp:18-24`): nothing returned by a public method, route, event or log carries a value or any part of it; reads show `{"name":"Authorization","has_value":true}`. A value belongs to the origin of the URL it was saved with: moving `url` to another origin without new values clears them (the model profiles' rule, `model_profiles.hpp:26-33`). Only `McpClient` gets the values, through an internal `resolve_for_request(id)` (like `active_for_request()`, `model_profiles.hpp:317-319`).
- **Who configures.** The owner on the web admin page; the agent through tools (open decision 7). REST: `GET`/`POST /api/v1/mcp/servers`, `PUT`/`DELETE /api/v1/mcp/servers/{id}`, `POST /api/v1/mcp/servers/{id}/test` (initialize and list; answers `{ok, error, latency_ms, protocol, tools:[names]}`, the error a category: `dns connect tls timeout http unsupported_protocol bad_response`). Paired like the model routes (`routes_ai.cpp:289-297`). The web page: an "MCP servers" card on the Models page (open decision 7), with the same write-only field pattern as API keys.
- **Agent tools** (proposal, open decision 7): `mcp.servers` (list: id, name, URL, enabled, tool count, last error, header names; never header values), `mcp.add {name, url}` (no headers), `mcp.remove {id}`, `mcp.enable {id, enabled}`. No confirmation (autonomy ruling). `ToolThread::Agent`; the writes are memory changes whose file save is queued on `wisp.fs` by `LatestSave`, as for profiles.
- **Dynamic info** gains one line, "MCP: <id> (<n> tools), …" or nothing without servers (open decision 11), so the model knows a server exists without a `tools.find`; the fixed prompt does not change with the configuration.

### 4.4 MCP client: transport, limits and failures

- **Through `SceneHttp`'s agent principal** (`scene_http.hpp:12-57`): `principal = Agent`, `scene_id = kAgentHttpId`, interactive lane `wisp.shttp`. No https-only or host allowlist (the agent's reach is not restricted); `max_redirects = 2` (a hop to another origin drops `Authorization`, `scene_http.hpp:50-52`), but `0` for a server with any stored header, since every stored value belongs to the origin it was saved for (4.3) and `SceneHttp` drops only the authorization headers: such a server's 3xx is `mcp_unavailable` `http 3xx`; response cap `kMcpMaxResponse = 512 KiB` for `tools/list` pages and 1 MiB (`AgentHttpLimits::kMaxResponse`) for `tools/call`.
- **Streaming.** A server answering with `text/event-stream` should close the stream after the response; one that keeps it open would hold the request until its deadline. The client therefore reads the body as it arrives (`SceneHttpRequest::on_body`, which `SceneHttp::execute()` already streams whenever it is set, `scene_http.cpp:773, 859-880`, on the lane chosen by `download` alone, `:342-344`) and stops once the matching response has been parsed. Today a deliberate stop reads as error `"other"` (`scene_http.cpp:920-925`); `SceneHttpRequest` gains `bool stop_is_success` (Agent principal only), so that a stop from `on_body` returns the status and headers with error `""`. This is the only change to `SceneHttp`.
- **Timeouts:** `initialize` and each `tools/list` page 15 s; `tools/call` the server's `timeout_ms`; every wait also ends at `ctx.deadline_ms` and on `ctx.cancelled()`, which calls `SceneHttp::cancel_scene(kAgentHttpId)` as `http.fetch` does (`tools_http.cpp:391-394`). The `tools.call` registration timeout stays 125 s (`tools_scene_tools.cpp:25`).
- **Failure results** (never a confirmation, never a silent retry beyond the session rule): `mcp_unavailable` (dns, connect, tls, timeout, `offline`, a non-2xx status; `message` names the category and status), `mcp_error` (a JSON-RPC error: code and message, the message scrubbed (below) and wrapped as external data), `tool_error` (`isError`), `bad_response` (not JSON-RPC, no matching id, too large), `not_found`, `bad_args`, `cancelled`, `timeout`. `tools.find` lists a server that failed to initialize or list under `"unavailable":[{"server":<id>,"error":<category>}]` and goes on with the others.
- **Scrubbing.** A server's error message may quote a credential. `holds_key_piece()` (`llm_client.cpp:249-257`) moves to `wisp_net`'s internal helpers; a message that holds any header value, or 8 or more bytes of one, is replaced by `"(message withheld: it quoted a credential)"`. The check covers only the part of the text that is kept (the message cap; for a tool result, the agent's tool payload cap `kHttpMaxMaxBytes`) and is one pass over it against a table of the values' 8-byte windows, so a long text cannot make it slow.
- **Logs:** `SceneHttp` already logs one line per agent request with method, host, status and time (`scene_http.hpp:55-57`); `McpClient` adds `mcp: <server id> <method> <ok|error:<code>> <ms> ms`, never arguments, results or headers.
- **Untrusted content.** Descriptions, results and error messages are wrapped by `external_data("mcp:<server id>", …)` (`tool_registry.hpp:161-167`), and the injection rules of the fixed prompt apply (`prompt.cpp:128-133`).

## 5. Costs

**Internal RAM.**

- The provider interface: a vtable pointer. `LlmDialect` resolution: a small struct on `wisp.agent`'s stack (PSRAM) per call.
- Images, reasoning, the MCP tool cache, MCP responses and the server list: PSRAM (`Mem::Large`, `parse_json_large()`).
- Transient default-heap allocations, internal RAM because they are small: `SceneHttp`'s copies of the URL and headers for the request in flight (bounded by `AgentHttpLimits`, `scene_http.hpp:133-135`; a few hundred bytes for MCP), the response header strings (bounded at 8 KiB by `keep_response_header()`, `http.hpp:188-196`), the per-event `from_json` trees of `SseParser` events (as for the LLM today). `McpServerService`'s in-memory list: names and URLs as `std::string`, at most 8 × (32 + 2048) bytes worst case, about 1 KB typical; proposal: hold them in `os::LargeString` from the start.
- `wisp.tool.io`: its stack in PSRAM; its task control block wherever the port allocates it (not verified; a few hundred bytes while it runs).
- Expected net change at idle: under 1 KB. Re-measured with `mem` on the device (section 9).

**PSRAM.**

- Screenshot, transient: the RGB888 snapshot (651 KiB on a 466 × 466 panel), then the 169 KiB scaled copy and the JPEG. Held for the turn: the JPEG (at most 128 KiB, typically 15-25 KB).
- Reasoning: at most 64 KiB per turn. Continuation: the partial reply until it is stored.
- MCP: per server the tool cache (at most 128 tools × (1 KiB description + schema) — a few hundred KiB in the worst case with large schemas; typically tens of KiB), a response being parsed (at most 1 MiB body plus its document), the session id.
- The carry request's transcript: at most half the window's estimate in bytes (about 98 KB at the default 65536-token window).

**Task stacks.**

- `wisp.agent` (16 KB): today's measured high-water leaves 10112 B during a spoken turn (hardware doc `:467`). `TurnRunner::run()` adds one frame level; the continuation and the carry request reuse `request()`; MCP waits like `http.fetch`. The plan measures the high-water mark after J; no stack change is proposed.
- New `wisp.tool.io`: 16 KB proposed for the JPEG encoder (stb's baseline encoder uses a few KB of stack arrays; not measured), PSRAM, at most two at once.
- `wisp.shttp` (8192 bytes, `scene_http.hpp:282-285`): `on_body` now parses SSE for MCP on this task; the parser's state is in PSRAM (`SseParser` holds `os::LargeString`s, `llm_client.hpp:145-146`). The plan measures its high-water mark.

**TLS connections.** The gate serializes handshakes (`tls_gate.hpp:3-22`); with mbedTLS allocations in PSRAM, two sessions at once (LLM and TTS) were measured within the budget (`docs/hardware/waveshare_esp32_s3_touch_amoled_2_16.md:467`; `firmware/sdkconfig.defaults:150-164`). J adds no new kind of concurrent session: MCP requests go through `wisp.shttp`, the lane `http.fetch` already uses, one request at a time, and a tool runs while no model request is open (each model request makes and frees its own client, `esp_http_client.cpp:151, 225`). The worst overlap stays what `http.fetch` already allows: TTS still speaking, one agent request on `wisp.shttp`, a scene download on `wisp.shttp.dl`. The continuation and the carry are ordinary model requests.

**Flash thread.** No new flash access off `wisp.fs`: `McpServerService` loads on `wisp.fs` and saves through `LatestSave`; images and reasoning are never stored; the carry is one more conversation append through `run_io_interactive` (`agent_service.cpp:229-243`); `ToolThread::Io` never touches flash (3.9).

**Per-request JSON.** Dialect fields: tens of bytes. An image: at most about 171 KiB of base64, in the requests of the turn that made it. Reasoning echo: at most 64 KiB. MCP: nothing in the `tools` array; `tools.find` results like scene tools'. The body is one `os::LargeString` (`llm_client.cpp:302-329`); its size is otherwise bounded by the window, as today.

**Flash (image).** The JPEG encoder (stb's is one header of a few thousand lines; code size not measured), the MCP client, the provider seam; the web page gains the MCP card and the advanced LLM group (a few KB of the 170 KB gzip budget, `docs/architecture.md:272`).

**Latency.** A continuation or a carry costs one more model request when it happens; `system_msgs=first` costs cached prefix (3.4).

## 6. Compatibility

Unchanged:

- Stored profiles: same file and keys; no new option is written by the upgrade; requests from a stored profile are byte-identical to today's (golden test, section 7). A downgrade keeps the new options and ignores them.
- Stored conversations: every existing role and key; old files load as before.
- `GET`/`PUT /ai/profiles` shapes (options remain a flat map), `model.profiles` event, the device's model shortcuts.
- WebSocket `agent` frames and `GET /agent/conversation`; `AgentEvent`; `agent.state`.
- Every built-in tool name, argument and result; `tools.find`/`tools.call` names, arguments, result shapes and errors for scene tools.

Changed (additive unless stated):

- New profile options (3.2); `GET /ai/presets` gains `defaults`; `load_models()` drops a bad option instead of a profile (a behaviour change, only for hand-edited files).
- New message role `carry` (3.8): visible in `GET /agent/conversation` as a hidden message; older firmware drops its line.
- New tool `ui.screenshot` (vision models only); `http.fetch` gains `as: "image"` (open decision 6); new `mcp.*` tools (open decision 7).
- `tools.find` results may hold MCP tools (names with `:`) and an `unavailable` list; `tools.call` accepts MCP names.
- New routes `/api/v1/mcp/servers…`; new file `/secrets/mcp.json`.
- Notes `AgentReplyCut`, `AgentRolledOverCarry` (zh/ja/en).
- A reply cut by `length` is no longer stored as finished.
- Internal: `AgentDeps`, `LlmResult::error`'s type, `ToolThread::Io`'s executor, `ToolRegistry::tools_json(bool)`.

## 7. Testing

GoogleTest in the simulator, failing test first (AGENTS.md section 5).

- **Request bodies (golden):** `build_chat_body()` for a stored profile without new options equals today's bytes (captured at `06279ef` before any change); each dialect option on and off; `system_msgs=first` with summaries, a carry and the dynamic info; parts with an image (base64 correct, `data:` URL); images after a run of parallel tool messages; image dropped for a non-vision profile; reasoning echoed only on the running turn's messages.
- **Stream reading:** `reasoning_content` kept under `echo`, ignored under `drop`, both count as bytes.
- **Profiles:** each option's validation and wrong-kind refusal; resolution order (option, preset default, built-in); a stored bad option dropped alone, key kept; 7 options fit.
- **TurnRunner with a `FakeProvider`** (scripted `LlmResult`s, records each `LlmCall`): length continuation (once; text concatenated; second `length` adds the note; continuation with tool calls runs them; summary requests not continued); carry (written after archive with seq 1, text cut at 4096 bytes on a character boundary; failure writes the old note; a stop during the carry ends the turn before archiving); image slot (newest kept, older replaced by text, emptied at turn end, vision-only array chosen by the flag); reasoning cap; calibration keyed by profile and model. The existing `AgentLoop` tests keep passing over `LlmClient` and `FakeLlmServer` (`tests/agent/agent_loop_test.cpp`).
- **Conversation:** carry round-trips through `current.jsonl`; a file with a carry line parsed by a store that does not know the role drops only that line (a test of `parse()` with an unknown role, which is today's behaviour); the Shell's history skips carries.
- **ToolProvider composition:** merged `find` order and limits, `total`; routing by syntax; `not_found`; the scene provider reproduces today's `tools.find`/`tools.call` results byte for byte (goldens from `tests/agent/scene_tools_tools_test.cpp`).
- **MCP client** against a fake server behind `FakeHttpClient` and a real `SceneHttp`: initialize (both accepted revisions, an unsupported one), session header round trip, `404` re-initialize once, JSON and SSE answers, an SSE stream left open after the response (ends on the response, not the deadline), pagination, tool limits and schema cut, local validation on and off, every result content type, `isError`, JSON-RPC errors with a quoted credential (scrubbed), timeouts, cancellation, `offline`, `parse_json_large()` allocations land in `Mem::Large` (the heap test pattern of `tests/agent/tool_registry_heap_test.cpp`).
- **`McpServerService`:** validation, write-only values never in any getter, route or event, the origin rule, persistence through a fake worker, factory reset.
- **Admin routes:** no header value in any response; the test route's categories.
- **`ToolThread::Io`:** one task per call, flash assertion on a flash call from it, abandoned task joined later, the two-task limit answers `busy`.
- **`ui.screenshot`:** on the simulator, a known screen gives a JPEG with SOI/EOI markers, the expected dimensions per shape (rect, rounded, circle), black outside what the panel cannot show (2026-11-16 修订：不再按 `safe_rect()`，见 3.6; decoded back with the simulator's JPEG decoder), and fails `too_large` past the cap.
- **Logs:** no test log line contains a header value, an API key, reasoning or tool content.

The web page's new controls are checked by hand in the simulator (frontend tests are out of the roadmap's scope).

## 8. Device checks (not run; for the owner)

1. A turn on each of the owner's providers with no new options: replies and tool calls as before (the request bytes are tested on the host).
2. `out_tokens` small (for example 256) on a long answer: the reply continues once, reads as one text on Home and in speech, and a second cut shows the note.
3. `reasoning=echo` against a reasoning model with tool calls (DeepSeek and Kimi documentation describe this need; unverified): a multi-step turn completes; with `drop` on the same model, note whether the provider refuses.
4. `system_msgs=first` against the Anthropic and Gemini presets: replies correct, and the provider's reported cached tokens (if it reports them) higher than with `anywhere` over a multi-step turn.
5. `parallel_tools=false` on the OpenAI preset: accepted.
6. `ui.screenshot` on the 2.16 board and the round 1.75 board: the model describes the screen correctly; encode time and JPEG size; `wisp.tool.io` stack high-water mark.
7. An MCP server on the LAN over `http://` and one on the internet over `https://` with a bearer header: `test`, `tools.find`, `tools.call`; a server restart in between (session `404` and re-initialize).
8. `mem` at idle and during an MCP call while TTS speaks: internal free and largest block against `ram_budget.hpp`; `wisp.agent` and `wisp.shttp` high-water marks.
9. A conversation pushed past 1 MiB (or the limit lowered in a test build): the carry appears, the next turn knows the earlier context, the archive is intact.
10. Downgrade to the previous release with a carry line and new options stored: the conversation and profiles load.

## 9. Migration and recommended plans

Broad order; each step leaves `bash tools/ci.sh` green.

1. **Seam, no behaviour change.** `llm_provider.hpp`, `LlmError`, `LlmClient : LlmProvider`, `AgentDeps` on the interface, `TurnRunner` extracted with `TurnEnv`, `Calibration` keyed by profile and model, `FakeProvider`. Goldens of request bodies and of the loop's events captured first.
2. **Dialect.** Options, validation, the load fix, preset defaults mechanism, body building per dialect, the prompt's `system_msgs` wording, the web advanced group.
3. **Length continuation and rollover carry.** `carry` role in the store, context and Shell; the two notes.
4. **Content parts and vision.** Parts in `LlmMessage`, image slot, tool arrays by vision, `ToolThread::Io` executor, the JPEG encoder in `third_party/`, `ui.screenshot`, `http.fetch` `as:"image"`.
5. **Tool providers.** `ToolProvider`, `SceneToolProvider`, generic `tools.find`/`tools.call`, prompt wording.
6. **MCP.** `parse_json_large()`, `SceneHttp`'s `stop_is_success`, `McpServerService`, `McpClient`, `McpToolProvider`, routes, web card, `mcp.*` tools, dynamic-info line.
7. **Documents** in the commits they describe: `docs/architecture.md` (the agent's threads table, `wisp.tool.io`, new routes and file), the agent-core spec (3.2, 3.3, 3.6, 5.4 marked revised by this spec), the agent-tools spec 5.3 (providers), the connectivity spec's route table, `docs/references.md` and `THIRD_PARTY_NOTICES.md` (encoder), `docs/backlog.md` (close `:18` screenshots, `:85`, `:97`, `:231`).

The phase is too large for one plan. Recommended split:

| Plan | Steps | Depends on | Notes |
|---|---|---|---|
| J1 Provider seam and TurnRunner | 1 | — | refactor only; every later LLM plan builds on it |
| J2 Dialects, continuation and carry | 2, 3 | J1 | one plan: they share the request builder and the context tests |
| J3 Vision | 4 | J1 (J2 for `system_msgs` interplay with the image user message; can follow either order with a rebase) | carries the `Io` change, its first user |
| J4 Tool providers and MCP | 5, 6 | none (independent of J1-J3) | may split further: J4a providers (small), J4b MCP client and configuration (the largest piece) |

J4 can run in its own worktree in parallel with J1-J3; the only shared file is `prompt.cpp` (one sentence) and `AgentWiring`.

## 10. Out of scope

- Other wire protocols (Anthropic Messages, Gemini native, the OpenAI Responses API, Ollama's native API) and a second provider implementation; per-request fallback across profiles.
- Extra request options that are not dialect differences: temperature, top_p, seed, response format, `stream_options` switching, provider-specific prompt-cache hints (`cache_control`), custom request headers for gateways (for example OpenRouter's attribution headers). Each would be one more option row later.
- Showing reasoning text anywhere.
- MCP: OAuth 2.1 authorization, the `GET` stream and server-initiated notifications (`tools/list_changed`), resources, prompts, sampling, elicitation, roots, stdio and SSE-only (2024-11-05) servers, MCP servers configured per scene.
- Images from the web chat or the microphone side, a camera, images returned by scene tools, image tools for scenes (`ui.image` from the agent is phase I).
- A thumbnail of the screenshot in the web chat.
- `day_tokens` and usage statistics.
- Restricting or confirming the agent's network access (owner's ruling).

## 11. Changes to the roadmap's decisions

| Roadmap (phase J) | This spec | Why |
|---|---|---|
| `complete(const LlmCall&) -> LlmResult` | the same, plus `active_model()`; `LlmResult::error` becomes an enum | the agent reads the window, model and vision from the provider instead of holding `ModelProfileService` (3.1) |
| "where the dynamic info goes (trailing system message or merged into the last user message)" | one option `system_msgs` (`anywhere`/`first`) that moves every movable system message: dynamic info, summaries, the carry | the mid-history summaries have the same problem (the backlog entry on summaries, closed by 3.4); one rule keeps the request shape consistent (3.4) |
| "the profile's vision option becomes real" | real through `ui.screenshot` (agent-core spec 5.4), `http.fetch` `as:"image"` and MCP image results | an image part needs a producer; the screenshot is the one the agent-core spec already designed |
| `ToolProvider` (`search`, `describe`, `call`) | plus `id()`, `owns()`, `count()` | routing by name syntax and `tools.find`'s `total` need them (4.1) |
| MCP "through `SceneHttp`'s agent principal" | the same, plus one `SceneHttp` flag (`stop_is_success`) | an SSE answer that is not closed would otherwise last until the deadline (4.4) |
| `ToolThread::Io` "a one-shot task" | a one-shot task that never touches flash, PSRAM stack | the old comment's "for tools that touch flash, with an internal stack" contradicts the rule that only `wisp.fs` touches flash (1.4, 3.9) |
| "rollover carries a summary" | a `carry` role written before the archive's new conversation starts; a failed carry still rolls over | a rollover must not be blocked by the network (3.8) |
| (not listed) | invalid stored options dropped one by one | seven options make the old "drop the profile" rule costlier (3.2) |

## 12. Decisions

Approved by the owner on 2026-11-16: every proposal below is adopted as written. The alternatives stay recorded for a later change.

1. **Dialect option set.** *Proposal: the five options of 3.2, with `system_msgs` covering all movable system content.* If wrong: an option is one table row and one branch in the body builder; renaming one later needs a load-time alias for stored profiles.
2. **Preset defaults.** *Proposal: ship the mechanism; set `out_field = max_completion_tokens` on `openai` and `system_msgs = first` on `anthropic`; every other preset keeps the built-in defaults until a device check (section 8, items 3-5) shows otherwise.* Both are from provider documentation, unverified. If wrong: a provider refuses or mis-orders requests for profiles on that preset that do not set the option themselves; the fix is one preset row, and a profile can override it at once.
3. **Reasoning echo.** *Proposal: default `drop`; `echo` keeps at most 64 KiB per turn, oldest dropped first.* If wrong: a provider that needs all of a long turn's reasoning refuses a late request of the turn (an `http` error ending it); raising the cap costs PSRAM and request size only.
4. **JPEG encoder.** *Proposal: `stb_image_write` in `third_party/`, one portable path on the device and the simulator.* Alternative: `esp_new_jpeg`'s encoder on the device (the decoder is already linked, `esp_jpeg_decoder.hpp`; possibly faster, not measured) and stb in the simulator, two paths with different output. If wrong: encoding takes longer on the device (bounded by the tool's 10 s timeout); switching is local to `base::encode_jpeg()`.
5. **Screenshot size.** *Proposal: 240 px wide, quality 70 (agent-core spec 5.4).* A 466 px round panel's small text may be hard to read at 240. If wrong: the model misreads small text; 320 px costs about 1.8 times the bytes.
6. **`http.fetch` `as: "image"`.** *Proposal: yes, JPEG and PNG passed through up to `kImageMaxBytes`.* If wrong: an unneeded argument; removing it is a schema change of one tool.
7. **Who configures MCP servers.** *Proposal: the web Models page gets an "MCP servers" card (with write-only headers); the agent gets `mcp.servers`, `mcp.add` (no headers), `mcp.remove` and `mcp.enable`, without confirmation.* The agent can then connect a header-less server the user names by voice; secrets stay a web task. If wrong: four tools to remove, or a separate web page instead of a card; nothing stored changes.
8. **`SceneHttp` change for SSE.** *Proposal: `stop_is_success` for the Agent principal (4.4).* Alternative: rely on servers closing the stream (the MCP spec says they should). If wrong: a non-closing server costs each call its whole timeout.
9. **Continuation form.** *Proposal: partial reply as an assistant message plus a user instruction (3.7), on every provider.* Some providers offer assistant-prefix continuation (unverified per provider), which continues more cleanly but is not portable. If wrong: occasional repeated words at the seam; a per-preset option could switch later.
10. **Carry rules.** *Proposal: input at most half the window; output at most 4096 bytes; a failed carry still archives with today's note.* If wrong: a carry too thin or too long; both are constants.
11. **MCP line in the dynamic info.** *Proposal: yes, server ids and tool counts.* If wrong: a few dozen bytes per request with no benefit; one line to remove.

