# Vision (J3): Content Parts, Image Slot, `ToolThread::Io`, `ui.screenshot` — Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A model whose profile has `vision` can see the device's screen (`ui.screenshot`) and fetched images (`http.fetch` with `as:"image"`). A model without it never receives an image part, and its requests do not change by a byte.

**Architecture:** Step 4 of spec section 9.
- `net::LlmMessage` gains content parts. `build_chat_body()` writes a user message with parts as a JSON array and base64-encodes image bytes straight into the body. Without vision it puts a text part where each image was.
- `ToolDef` gains `vision_only`, and `ToolRegistry` keeps two `tools` arrays. `ToolOutput` can carry one encoded image, and `ToolCtx` tells a tool whether the turn's model takes images.
- `TurnRunner` keeps the turn's newest image in its image slot. Each request gets one `user` message after the run of tool messages that produced an image (`attach_images()` in `context.hpp`).
- `ToolThread::Io` becomes a one-shot task per call (`agent::IoRunner`, `wisp.tool.io`, PSRAM stack, never flash).
- `stb_image_write` is vendored unmodified into `third_party/` and wrapped by `wisp::encode_jpeg()` in `wisp_base`. It is one encoder for the device and the simulator.
- `ui.screenshot` is the first `Io` tool.

**Tech Stack:** C++20 (`-fno-exceptions -fno-rtti`), C (the vendored encoder), GoogleTest, the simulator build (`build/sim`), LVGL 9's bundled TJPGD (test-side decode only), ESP-IDF v6.0.3 (firmware build check).

**Spec:** `docs/superpowers/specs/2026-11-15-llm-tool-providers-design.md` (approved 2026-11-16; all section-12 proposals adopted). This plan covers:
- sections 1.4, 1.7, 3.5, 3.6 and 3.9;
- the image half of 3.1 and the `first` interplay of 3.4;
- the J3 parts of sections 5-8;
- section 9 step 4, and the step 7 documents for it.

It builds on J1 (`docs/superpowers/plans/2026-11-16-llm-provider-seam.md`), assumed implemented: `net::LlmProvider`, `LlmModelInfo`, `TurnRunner`/`TurnEnv`, `FakeProvider`, and the goldens `LlmBodyGolden.*`/`AgentTurnGolden.*`. It also builds on J2 (dialects, `docs/superpowers/plans/2026-11-16-llm-dialects.md`), assumed to land first. Plan decision 1 lists the J2 names this plan assumes.

## Global Constraints

- **Workspace.**
  - Work in the worktree `.superpowers/worktrees/llm-providers`, branch `feat/llm-providers`.
  - Never push, merge, switch branches or `git stash`. Never add `.claude/`, `.playwright-mcp/` or `.superpowers/`.
  - Delete files only by literal absolute paths inside the worktree.
- **Builds.** Only one process may build in `build/sim` at a time. Configure once with `cmake -S simulator -B build/sim -G Ninja` if it does not exist yet. Adding the new `third_party/stb_image_write` subdirectory (Task 1) needs a re-run of that configure command.
- **Goldens stay green for non-vision paths.**
  - Run `export GOLDEN='LlmBodyGolden|AgentTurnGolden'` once per shell. The J1 and J2 goldens run in every task and are never edited.
  - A request from a profile without `vision` is byte-identical to what it was before this plan. The one exception is the deliberate `http.fetch` schema change of Task 7 (plan decision 11): no golden registers `http.fetch`, and `AgentHarness` registers only test tools.
- **AGENTS.md applies in full.** In particular:
  - **Third-party code.** `third_party/` source is unmodified, pinned (commit and SHA-256), brought in by a script, and registered in `docs/references.md` and `THIRD_PARTY_NOTICES.md` (section 6).
  - **Layering** (`tools/check_layering.py`). Only `wisp_base` may include `stb_image_write.h` or require its component. `wisp_base` stays free of `wisp_os`, so `encode_jpeg()` writes into a caller's span.
  - **Single UI thread.** `ui.screenshot` reaches the screen only through `UiRuntime::snapshot_rgb888()`, which posts to the UI thread. No LVGL call happens off the UI thread.
  - **Flash rule.** `wisp.tool.io` never touches flash: `flash_access` keeps its `false` default and `hot_stack` stays false.
  - **Memory.** The snapshot, the scaled image, the JPEG, image copies and the body are PSRAM. The snapshot is one >16 KB `std::vector`; everything else is `os::LargeBuffer`/`LargeString`/`LargeVector`, and `ToolImage` is allocated with `os::LargeAllocator`.
  - **Tests.** TDD: write the failing test first and see it fail for the stated reason (a missing header or symbol counts). Test output and build output must be clean, with no warnings.
  - **Comments** explain why. They contain no review ids, no "Task N", no "round N" and no `.superpowers/` paths. Point at spec sections instead ("spec 3.5").
  - **Commits.** Conventional Commits. Every commit message ends with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`.
- **Aggregates.** `-Werror=missing-field-initializers` is on: every aggregate added here (`LlmPart`, `ToolImage`, `ImageInfo`, `RequestImage`, `ScreenshotOptions`, `ScreenshotToolDeps`) gives every member a default member initializer.
- **Per-task verification.**
  - Every task ends with `cmake --build build/sim 2>&1 | tee build/sim-build.log | tail -3`.
  - Then `grep -c 'warning:' build/sim-build.log` (expected `0`; `build/` is git-ignored).
  - Then the focused `ctest --test-dir build/sim -R '<filter>' --output-on-failure`.
  - The last task runs `bash tools/ci.sh` and the firmware build for both boards.
- **Device checks are never reported as done.** They are listed at the end for the owner.

## Review Focus

These are the five failures most likely to reach a user that no single feature test would catch. Each one is pinned by a test in the task named.

1. **A non-vision request changes**: an empty `parts` span emitted as `[]`, the screenshot leaking into `tools_json(false)`, or an image message inserted for a model without vision.
   - Pinned by the unchanged goldens (every task) and `LlmBodyParts.MessagesWithoutPartsAreUnchanged` (Task 2).
   - Also pinned by `ToolRegistry.WithoutVisionTheArrayIsTodays` (Task 3) and `TurnRunner.NoVisionSendsNoImage` (Task 5).
2. **A model without vision receives an image anyway** (the profile changed mid-turn, or the model calls `ui_screenshot` or `as:"image"` although they were not offered).
   - Pinned by `LlmClient.ImagesDroppedWhenTheProfileHasNoVision` (Task 2) and `ToolRegistry.VisionOnlyToolIsUnknownWithoutVision` (Task 3).
   - Also pinned by `HttpFetch.ImageWithoutVisionIsUnsupported` (Task 7).
3. **More than one image, or an image in the wrong place**: a tool message split from its siblings, an image before its tool result, two images in one request, or an image still sent in the next turn.
   - Pinned by `AttachImages.*` (Task 5).
   - Also pinned by `TurnRunner.OnlyTheNewestImageIsSent`, `TurnRunner.ParallelCallsShareOneImageMessage` and `TurnRunner.TheSlotIsEmptiedAtTurnEnd` (Task 5).
4. **An abandoned `Io` call leaks a task or blocks the agent**: never joined, a third task spawned, or a join that waits on a running encoder.
   - Pinned by `IoRunner.FinishedTasksAreJoinedByTheNextPost`, `IoRunner.TwoRunningRefuseAThird` and `IoRunner.AbandonedCallIsJoinedLater` (Task 4).
5. **A screenshot that does not show what the panel shows**: swapped channels, wrong size, visible content cropped away on the round panel, or the invisible corners not black.
   - Pinned by `Screenshot.CircleCornersAreBlack`, `Screenshot.SizePerShape` and `Screenshot.ChannelsKeepTheirOrder` (Task 6).
   - Also pinned by `ScreenshotTool.ShowsTheScreen`, which decodes the JPEG back (Task 6).

## Plan decisions (where the spec is silent, imprecise or contradicts itself)

1. **J2 names this plan builds on.** These are taken from `docs/superpowers/plans/2026-11-16-llm-dialects.md` as it stood when this plan was written; reconcile them if J2 changes before it is implemented:
   - `core::LlmDialect` (in `wisp_core`) with `core::LlmSystemMsgs system_msgs` (`Anywhere`, `First`);
   - `LlmModelInfo::dialect`;
   - `LlmMessage::movable` and `LlmMessage::reasoning`;
   - `build_chat_body(std::string_view model, std::span<const LlmMessage> msgs, std::string_view tools_json, const LlmDialect& dialect = {})`;
   - J2's `FakeProvider` building `Recorded::body` with the current model's dialect.

   J3 appends one parameter, `bool vision = false`, after `dialect`. The `first` behaviour comes from J2 plan decisions 3-4:
   - A user message that receives merged content is written as a content array. Its parts are each `{"type":"text","text":"<context>\n…\n</context>"}`, in J2's order: the movable messages after it (the dynamic info first), then those before it. The array then ends with the message's own text as a text part.
   - J3's rule for a user message with `parts`: the `<context>` parts come first in that same order, followed by the message's own parts in place of "its own text".
   - The image message is a user message like any other. When it is the latest user message, the dynamic info merges into it (spec 3.4: "into the latest user message, as its first part").
   - A user message with parts and no merged content is spec 3.5's array.

   If J2 has not landed when this plan runs, Task 2 adds only the `vision` parameter and keeps `LlmBodyParts.FirstDialectPutsContextBeforeTheParts` with `GTEST_SKIP() << "needs the system_msgs dialect (spec 3.4)"`. Whichever of the two plans lands second enables the test.
2. **The screenshot is cropped to the visible pixels' bounding box, not to `safe_rect()`. This contradicts spec 3.6 and agent-core spec 5.4.** Both say "black out what lies outside `safe_rect()` and each row's `row_span()`, crop to the safe rectangle".
   - Cropping to `safe_rect()` removes everything the blacking-out was meant to show. On the round 466 px panel the safe square is about 319 px wide, so roughly 40 % of the visible disc would be cut away, including whatever a scene puts outside the safe square. The blacking-out would then do nothing at all, because the safe rectangle is entirely visible by definition.
   - Spec goal 3 ("can see the device's screen") and agent-core spec 1.2 (the agent checks whether its UI falls into invisible areas) need the whole visible area.
   - Ruling: every pixel that `hal::visible()` says the panel cannot show is set to black, and the crop is `screenshot_area(shape)`, the bounding box of the visible pixels (the full panel for all three shapes today).
   - Switching to the literal reading is a one-line change in `screenshot_area()`, and `Screenshot.SizePerShape` pins the dimensions.
   - **Flagged for the owner.**
3. **`wisp::encode_jpeg()`, not `base::encode_jpeg()`.** `wisp_base`'s public functions live in `namespace wisp` (`base64.hpp`, `crc32.hpp`). The header is `wisp/base/image.hpp`. It writes into a caller-provided `std::span<uint8_t>` (`wisp_base` cannot use `wisp_os`'s PSRAM types) and fails `QuotaExceeded` when the JPEG does not fit. The tools map that failure to `too_large`, which enforces `kImageMaxBytes` (spec 3.5) at no extra cost.
4. **Pinned encoder: `stb_image_write.h` v1.16 at nothings/stb commit `aeb69073d7570a481245a665dc17726d68827062` (2026-06-04).**
   - Hashes: `stb_image_write.h` SHA-256 `03f49804a8cf8a28c7ce4ed870e0eb642737ef97249aec30fdd3a8dc7886ae40`, 1748 lines; the repository `LICENSE` at the same commit SHA-256 `bebfe904b14301657e4e5d655c811d51fd31b97c455b9cc2d8600d6bac6cff63`. The licence is public domain or MIT, at the user's choice.
   - Why not the newer v1.18: upstream reworked the file in some twenty commits between 2026-10-30 and 2026-11-12 (stride type, dimension validation, warning fixes). Its JPEG core has not settled, while v1.16's has been unchanged since 2021.
   - What v1.16 gives the device: `stbi_write_jpg_to_func()` makes no heap allocation, and its stack arrays are about 5 KB of floats.
   - The implementation is compiled once, as C, by the wrapper `third_party/stb_image_write/stb_image_write.c`. That is Wisp's own file next to the upstream directory, as `third_party/lua/CMakeLists.txt` is.
   - The component is named `stb_image_write`, not `stb`, to avoid a clash with any ESP component of that name.
5. **`ToolOutput` carries the image as `std::shared_ptr<const ToolImage>`**, made with `std::allocate_shared` over `os::LargeAllocator`. This keeps `ToolOutput` copyable (the registry's `CallState` and several tools copy or move it), and the control block and buffer live in PSRAM. `TurnRunner`'s image slot holds that pointer. "The bytes live in an `os::LargeBuffer` owned by the image slot" (spec 3.5) holds through the shared ownership.
6. **An image counts `kImageEstTokens = 1024` toward the request estimate**, not its base64 bytes / 3.
   - At the bytes / 3 rate, a 20 KB JPEG would count about 9 k tokens and a 128 KiB one about 58 k tokens, which is over 75 % of the default 65536 window. Every screenshot would then trigger compression.
   - Providers bill a 240 px image at well under 1 k tokens (provider documentation, unverified; device check 3).
   - The calibration (`Calibration::observe`) then compares like with like.
7. **A vision-only tool called without vision answers `unknown_tool`** from `ToolRegistry::call()`, as for a name that is not in the array the model got. `ToolCtx` gains `bool vision` (default false), set by `TurnRunner` from the `LlmModelInfo` it reads once per turn.
8. **`tools_json(bool vision = false)`.** The vision array is built lazily on its first use (under a `std::once_flag`) from the base array plus the vision-only entries. Registration is already finished before the first `call()` (`tool_registry.hpp:107`), and `add()` stays O(size of one tool). The existing callers (`tool_registry_test`, `app_agent_test`, `scene_tools_tools_test`, the heap test) keep the default, which returns today's array.
9. **IoRunner lives in `wisp_agent`** (`wisp/agent/io_runner.hpp`), next to `ToolExecutors`, so the agent tests can use it. `AgentWiring` owns one instance and declares it before `registry_`, so it outlives the registry's executor.
   - Each slot keeps its `os::Task` and a `std::shared_ptr<std::atomic<bool>> done` that the task sets as its last action. `post()` first joins every slot whose flag is set, then spawns into a free slot, or answers false (busy) when two are running.
   - `join_all()` waits for every slot; `AgentWiring::stop()` calls it after `service_->stop()`.
   - A failed `Task::spawn()` (a null handle) is also `false`.
10. **No upscaling.** On a panel narrower than 240 px the screenshot keeps the panel's width. The height is `round(crop_h * out_w / crop_w)`, at least 1.
11. **`http.fetch` gains `"image"` in its `as` enum and one sentence in its description, in the one array every model gets.** This changes the `tools` bytes once for every profile, which costs one prefix-cache miss after the upgrade. Spec 3.5 says the argument answers `unsupported` without vision, so it is offered to all models. Further rulings:
    - The image is recognised by its bytes (`FF D8 FF` or the PNG signature, `sniff_image()`), not by `content-type`, which servers often send as `application/octet-stream`.
    - Without vision the call is refused before any request is made.
    - A `ref` read with `as:"image"` works on the cached body.
    - A truncated body (`body_truncated`, over the 1 MiB response cap) or one over `kImageMaxBytes` is `too_large`; bytes that are neither format are `not_image`.
12. **Model-facing image texts are English, like the rest of the prompt:**
    - `net::kImageOmittedNoVision` = `"[image omitted: the active model does not take images]"`;
    - `agent::kImageOmittedNewer` = `"[image omitted: a newer one was taken]"`;
    - the lead text part `"Image from <tool> (call <id>):"`.

    The agent-core spec's "[截图已省略]" (3.3) is revised to these (Task 8).
13. **`ui.screenshot`'s activity label** is `StrId::ActUiScreenshot`: zh "截取屏幕", ja "画面を撮影", en "Take a screenshot". Every glyph is in the existing CJK sets (GB2312; JIS level 1), so no font image changes. `AppAgent.EveryRegisteredToolHasAnActivityLabel` walks `tools_json(true)` from now on.
14. **No version bump or `CHANGELOG.md` entry in this plan.** Phase J is released as one merge (J1 plan decision 11).

---

### Task 1: Vendor `stb_image_write`; `wisp::encode_jpeg()`, `sniff_image()`, `base64_encode_into()`

**Files:**
- Create: `tools/vendor_stb.sh`. It follows the pattern of `tools/vendor_lua.sh`:
  - constants `STB_COMMIT`, `STBIW_SHA256` and `LICENSE_SHA256` (plan decision 4);
  - `curl -fsSL` of `https://raw.githubusercontent.com/nothings/stb/${STB_COMMIT}/stb_image_write.h` and `…/LICENSE` into a `mktemp -d` directory;
  - `sha256sum -c` of both, refusing the files on a mismatch;
  - a replace of `third_party/stb_image_write/stb-${STB_COMMIT:0:7}/` with exactly those two files.
- Create (by running the script): `third_party/stb_image_write/stb-aeb6907/stb_image_write.h`, `third_party/stb_image_write/stb-aeb6907/LICENSE`
- Create: `third_party/stb_image_write/stb_image_write.c`. This is Wisp's own file: a header comment (what it is, why it is C, the "never edit `stb-aeb6907/`" rule), then `#define STB_IMAGE_WRITE_IMPLEMENTATION` and `#include "stb_image_write.h"`.
- Create: `third_party/stb_image_write/CMakeLists.txt`. It follows the pattern of `third_party/lua/CMakeLists.txt`:
  - `wisp_component(NAME stb_image_write SRCS stb_image_write.c INCLUDE_DIRS stb-aeb6907)`;
  - `target_compile_definitions(<target> PUBLIC STBI_WRITE_NO_STDIO)`: a public definition, so every includer sees the same declarations;
  - only if the build reports warnings from the upstream file, private `-Wno-<name>` options, each with a comment naming the warning.
- Modify: `simulator/CMakeLists.txt:30-31`: `add_subdirectory(${WISP_ROOT}/third_party/stb_image_write stb_image_write)` before `wisp_base`'s `add_subdirectory`, with a one-line comment. The firmware finds the component through `EXTRA_COMPONENT_DIRS` (`firmware/CMakeLists.txt:25-29`), so the firmware build needs no change. Update that comment's "third_party/lua, used only by wisp_scene" to name both components.
- Modify: `components/wisp_base/CMakeLists.txt`: add `src/image.cpp` to `SRCS` and `REQUIRES stb_image_write`.
- Create: `components/wisp_base/include/wisp/base/image.hpp`, `components/wisp_base/src/image.cpp`
- Modify: `components/wisp_base/include/wisp/base/base64.hpp`, `components/wisp_base/src/base64.cpp`
- Modify: `tools/check_layering.py:143-152, 290`. Generalise the Lua header rule into a table `THIRD_PARTY_HEADERS = {"lua.h": ("wisp_scene",), …, "stb_image_write.h": ("wisp_base",)}`, keeping the Lua message text, and set `THIRD_PARTY_REQUIRES = {"wisp_scene": {"lua"}, "wisp_base": {"stb_image_write"}}`. Update the module docstring (`:37-40`).
- Modify: `tools/check_layering_test.py`: add `test_stb_header_outside_base_is_flagged` and `test_cmake_requires_stb_only_for_base`, modelled on `:342` and `:355`.
- Modify: `tools/ci.sh:67-68`: the comment names `tools/vendor_stb.sh` next to `tools/vendor_lua.sh`. The prune of `./third_party` already covers the new directory.
- Modify: `docs/references.md`: add a row after the Lua row (`:12`): the directory, the source URL with the commit, the two SHA-256 values, "public domain or MIT, chosen: MIT", `tools/vendor_stb.sh`, the wrapper and `STBI_WRITE_NO_STDIO`, and what uses it (`wisp::encode_jpeg()`, spec 3.6).
- Modify: `THIRD_PARTY_NOTICES.md`: a `## stb_image_write` section after `## Lua` (`:138`), with the upstream `LICENSE` text (alternative A, MIT), and the line "Copyright (c) 2017 Sean Barrett" as it appears in that file.
- Create: `tests/base/image_test.cpp`; modify `tests/CMakeLists.txt` after `:43`: `wisp_test(NAME image_test SRCS base/image_test.cpp LIBS wisp_base)`
- Modify: `tests/base/base64_test.cpp`

**Interfaces:**
- Consumes: `stbi_write_jpg_to_func(stbi_write_func*, void*, int x, int y, int comp, const void* data, int quality)` (v1.16, `extern "C"` declarations under C++).
- Produces (`namespace wisp`):

```cpp
// wisp/base/image.hpp
// Baseline JPEG of tightly packed RGB888 (R, G, B per pixel, no row padding),
// through the vendored stb_image_write (third_party/stb_image_write, spec 3.6):
// 4:2:0 at quality <= 90. Writes into `out` and returns the JPEG's size.
// InvalidArgument: width or height 0 or over 65535, quality outside 1-100, or
// rgb.size() != width * height * 3. QuotaExceeded: the JPEG does not fit `out`
// (the tools' too_large). No heap allocation; about 5 KB of stack.
Result<size_t> encode_jpeg(std::span<const uint8_t> rgb, uint32_t width, uint32_t height,
                           int quality, std::span<uint8_t> out);

struct ImageInfo {
    const char* mime = "";   // "image/jpeg" | "image/png" (static strings)
    uint32_t width = 0;      // 0 when the header does not say
    uint32_t height = 0;
};
// By the bytes alone: JPEG (FF D8 FF; size from the first SOF0-SOF15 marker,
// except DHT C4, JPG C8 and DAC CC) or PNG (the 8-byte signature; size from IHDR).
// nullopt for anything else.
std::optional<ImageInfo> sniff_image(std::span<const uint8_t> bytes);

// wisp/base/base64.hpp (standard alphabet with padding, as base64_encode)
constexpr size_t base64_encoded_size(size_t n) { return (n + 2) / 3 * 4; }
// Writes exactly base64_encoded_size(data.size()) chars to `out`; no terminator.
void base64_encode_into(std::span<const uint8_t> data, char* out);
```

  `base64_encode()` is reimplemented over `base64_encode_into()`.

- [ ] **Step 1: Vendor.** Run `bash tools/vendor_stb.sh`. Expected: "verified" for both hashes and the two files under `third_party/stb_image_write/stb-aeb6907/`. Run `head -1 third_party/stb_image_write/stb-aeb6907/stb_image_write.h`. Expected: `/* stb_image_write - v1.16 - public domain - http://nothings.org/stb`.
- [ ] **Step 2: Write the failing tests:**

```cpp
// tests/base/image_test.cpp
TEST(Jpeg, EncodesMarkersAndSize)
// 16x8 gradient; out of 4096 bytes. ok; bytes [0..1] == FF D8, last two == FF D9;
// sniff_image(jpeg) == {"image/jpeg", 16, 8}.
TEST(Jpeg, SameInputSameBytes)              // two encodes of one image are equal
TEST(Jpeg, QualityChangesTheSize)           // a noisy 64x64 image: size(q=30) < size(q=90)
TEST(Jpeg, RefusesBadArguments)
// width 0, height 0, quality 0, quality 101, rgb one byte short -> InvalidArgument; out untouched.
TEST(Jpeg, TooSmallAnOutputIsQuotaExceeded) // noisy 64x64 into a 100-byte span -> QuotaExceeded
TEST(SniffImage, Png)        // signature + IHDR 300x200 -> {"image/png", 300, 200}
TEST(SniffImage, JpegWithExifBeforeSof)     // APP1 segment then SOF2 (progressive) 640x480
TEST(SniffImage, NeitherFormat)             // "GIF89a…", "<html>", 3 bytes of FF D8 FF -> mime set,
                                            // size 0 for the truncated JPEG; "" and text -> nullopt
// tests/base/base64_test.cpp
TEST(Base64, EncodeIntoMatchesEncode)
// For n in 0..7 over bytes {0,1,2,0xfb,0xff,0x10,0x80}: base64_encoded_size(n) chars written,
// equal to base64_encode(); the byte after them (a canary) unchanged.
```

```python
# tools/check_layering_test.py
def test_stb_header_outside_base_is_flagged(self):   # wisp_net including stb_image_write.h
def test_cmake_requires_stb_only_for_base(self):     # wisp_agent REQUIRES stb_image_write
```

- [ ] **Step 3: Run** `cmake -S simulator -B build/sim -G Ninja && cmake --build build/sim`. Expected: compile errors (`wisp/base/image.hpp` not found, `base64_encode_into` undeclared). Then run `python3 tools/check_layering_test.py`. Expected: the two new tests fail.
- [ ] **Step 4: Implement.**
  - `encode_jpeg()` validates its arguments. It calls `stbi_write_jpg_to_func` with a sink: a struct holding `out`, the bytes written so far and an overflow flag; past the end it sets the flag and drops the bytes. A return of 0 is `IoError`, and the overflow flag is `QuotaExceeded`.
  - `sniff_image()` walks JPEG segments by their length fields and never reads past `bytes`.
  - Then come the layering table, the CMake files and the two documents.
- [ ] **Step 5: Run** `python3 tools/check_layering.py && python3 tools/check_layering_test.py && ctest --test-dir build/sim -R 'Jpeg|SniffImage|Base64' --output-on-failure`. Expected: PASS, and `grep -c 'warning:' build/sim-build.log` prints 0. If stb warns, add the narrowest `-Wno-…` to its component (Files) and rebuild; never edit `stb-aeb6907/`. Then `git status --short third_party` lists only the new files, and `sha256sum third_party/stb_image_write/stb-aeb6907/*` matches plan decision 4.
- [ ] **Step 6: Commit** `feat(base): vendor stb_image_write and add a portable JPEG encoder`.

---

### Task 2: Content parts in `LlmMessage` and the request body

**Files:**
- Modify: `components/wisp_net/include/wisp/net/llm_provider.hpp` (J1): `LlmPartKind`, `LlmPart`, `LlmMessage::parts`, `kImageOmittedNoVision`
- Modify: `components/wisp_net/include/wisp/net/llm_client.hpp:122-127`: `build_chat_body` gains `bool vision = false` (after J2's `dialect`); update its comment.
- Modify: `components/wisp_net/src/llm_client.cpp`:
  - `append_message()` (`:43-78`) gains a user-with-parts branch;
  - the size estimate (`:304-309`) counts each part (`48 + text` or `64 + base64_encoded_size(bytes)`);
  - `complete()` (`:346`, J1 numbering may differ) passes `core::profile_vision(profile->options)`.
- Modify: `tests/support/fake_provider.hpp` (J1): `Recorded::body` passes the current model's `vision`.
- Modify: `tests/net/llm_client_test.cpp` (append suite `LlmBodyParts` and two `LlmClient` tests)

**Interfaces:**
- Consumes: `base64_encoded_size()`/`base64_encode_into()` (Task 1); J2's `core::LlmDialect`/`core::LlmSystemMsgs` (plan decision 1).
- Produces (`namespace wisp::net`, `llm_provider.hpp`), with the names of J1's Interfaces block and spec 3.1:

```cpp
enum class LlmPartKind : uint8_t { Text, Image };

// One piece of a message's content. Views only, like LlmMessage.
struct LlmPart {
    LlmPartKind kind = LlmPartKind::Text;
    std::string_view text{};            // Text
    std::string_view mime{};            // Image: "image/jpeg" | "image/png"
    std::span<const uint8_t> bytes{};   // Image: the encoded file, not base64
};

// LlmMessage gains, after `text`:
//     std::span<const LlmPart> parts{};   // User only (spec 3.5); text is unused when set

// The text part an image becomes when the profile has no vision (spec 3.5).
inline constexpr std::string_view kImageOmittedNoVision =
    "[image omitted: the active model does not take images]";
```

  Wire form (spec 3.5, agent-core spec 3.6). A user message with non-empty `parts` is written as:

```text
{"role":"user","content":[{"type":"text","text":<escaped>},
 {"type":"image_url","image_url":{"url":"data:<mime>;base64,<b64>"}}]}
```

  The parts go in order. With `vision == false` an image part becomes `{"type":"text","text":"[image omitted: the active model does not take images]"}`. A message without parts is written exactly as today. `parts` on any other role is ignored: tool messages never carry images, as spec 3.5 requires.

- [ ] **Step 1: Write the failing tests** (`tests/net/llm_client_test.cpp`):

```cpp
TEST(LlmBodyParts, UserMessageWithPartsIsAnArray)
// User with parts {Text "look:", Image "image/jpeg" bytes {FF D8 FF D9}}, vision true:
// body contains R"({"role":"user","content":[{"type":"text","text":"look:"},)"
//   R"({"type":"image_url","image_url":{"url":"data:image/jpeg;base64,/9j/2Q=="}}]})"
TEST(LlmBodyParts, Base64IsWrittenWhole)
// A 1000-byte image (i % 251): the data URL's payload == base64_encode(bytes); exactly one
// "data:image/png;base64," in the body for mime "image/png".
TEST(LlmBodyParts, TextPartsAreEscaped)      // text part "q\"\n" -> "q\"\n" escaped as to_json does
TEST(LlmBodyParts, NoVisionReplacesImagesWithText)
// Same message, vision false (also the default): no "data:" and no "image_url"; content ==
// [{"type":"text","text":"look:"},{"type":"text","text":"[image omitted: the active model does not take images]"}]
TEST(LlmBodyParts, MessagesWithoutPartsAreUnchanged)
// The LlmBodyGolden.EveryMessageShape messages with vision true and with vision false: both
// bodies equal the golden literal (no "content":[] anywhere).
TEST(LlmBodyParts, PartsOnOtherRolesAreIgnored)
// A Tool message with parts set: written as today's tool message (text content).
TEST(LlmBodyParts, FirstDialectPutsContextBeforeTheParts)
// dialect.system_msgs = core::LlmSystemMsgs::First; messages: System "S" (fixed), User "q", Tool…, User with parts
// {Text "Image from t (call c1):", Image}, System movable "D" (dynamic info), vision true.
// The image message is the latest user message: its content array is
// [{"type":"text","text":"<context>\nD\n</context>"}, the lead text part, the image_url part].
// No "role":"system" after the first message. (Plan decision 1.)
TEST(LlmClient, ImagesDroppedWhenTheProfileHasNoVision)
// Fixture profile without options; a call whose messages carry an image part: the recorded body
// holds kImageOmittedNoVision and no "data:".
TEST(LlmClient, ImagesSentWhenTheProfileHasVision)
// update(options {vision "true"}): the body holds "data:image/jpeg;base64,".
```

- [ ] **Step 2: Run** `cmake --build build/sim`. Expected: compile errors (`LlmPart`, `parts` and `kImageOmittedNoVision` undeclared; `build_chat_body` takes no `vision`).
- [ ] **Step 3: Implement.**
  - The image payload is written by `out.resize(old + base64_encoded_size(n))`, then `base64_encode_into(bytes, out.data() + old)`. No base64 copy exists outside the body (spec 3.5).
  - Under `First`, J2's merge step hands the user-message writer the `<context>` blocks to prepend. When the message has parts, those blocks become its leading text parts and its own parts follow, in place of J2's trailing text part (plan decision 1).
  - `FakeProvider` passes `model ? model->vision : false`.
- [ ] **Step 4: Run** `ctest --test-dir build/sim -R "LlmBodyParts|LlmClient|SseParser|Base64|$GOLDEN" --output-on-failure`. Expected: PASS; `git diff --stat` shows no golden test file.
- [ ] **Step 5: Commit** `feat(net): send user messages with image parts`.

---

### Task 3: Tools by vision: `vision_only`, two arrays, `ToolCtx::vision`, `ToolOutput::image`

**Files:**
- Modify: `components/wisp_agent/include/wisp/agent/tool_registry.hpp`:
  - `ToolCtx` (`:45-53`) gains `vision`;
  - `ToolImage` is new, before `ToolOutput`;
  - `ToolOutput` (`:55-61`) gains `image`;
  - `ToolDef` (`:69-88`) gains `vision_only`;
  - `tools_json` (`:113-115`) takes `bool vision = false`;
  - new private members `vision_json_` and `vision_once_`, and the header comment's "tools_json()" sentence.
- Modify: `components/wisp_agent/src/tool_registry.cpp`:
  - `add()` keeps vision-only entries out of `json_` and appends them to a private `os::LargeString vision_tail_` (same `append_json` form, without the enclosing brackets);
  - `tools_json(bool)` (`:199`);
  - `call()` (`:215-221`): a vision-only tool with `!ctx.vision` answers `unknown_tool` (plan decision 7).
- Modify: `components/wisp_agent/include/wisp/agent/limits.hpp`: `kImageMaxBytes`, `kImageEstTokens`
- Modify: `tests/agent/tool_registry_test.cpp`, `tests/agent/tool_registry_heap_test.cpp` (one assertion)

**Interfaces:**
- Produces (`namespace wisp::agent`):

```cpp
// limits.hpp
// Largest encoded image a tool may hand the model (spec 3.5): about 171 KiB of base64.
inline constexpr size_t kImageMaxBytes = 128 * 1024;
// What one image counts toward the request estimate (plan decision 6).
inline constexpr size_t kImageEstTokens = 1024;

// tool_registry.hpp
struct ToolCtx {   // gains, after `turn`:
    // Whether the turn's model takes images (spec 3.5); tools that would produce one
    // answer "unsupported" without it.
    bool vision = false;
};
// One encoded image a tool produced (spec 3.5). Allocated in Mem::Large
// (std::allocate_shared with os::LargeAllocator); never stored.
struct ToolImage {
    const char* mime = "";     // "image/jpeg" | "image/png" (static strings)
    os::LargeBuffer bytes{};   // at most kImageMaxBytes
    uint32_t width = 0;        // 0 when unknown
    uint32_t height = 0;
};
struct ToolOutput {   // gains:
    // Set only by a tool whose text result already describes it
    // ("image":{"mime","bytes","width","height"}); the turn shows it to a vision model.
    std::shared_ptr<const ToolImage> image;
};
struct ToolDef {      // gains, after `risk`:
    // Offered only to a model with vision (spec 3.5): left out of tools_json(false), last
    // in tools_json(true), and unknown to call() without ctx.vision.
    bool vision_only = false;
};
// ToolRegistry:
// The request's "tools" array, in add() order; with `vision`, the vision-only tools follow
// (agent-core spec 3.2 item 2). Without any vision-only tool both are the same array.
const os::LargeString& tools_json(bool vision = false) const;

// Helpers (tool_registry.hpp, implemented in tool_registry.cpp):
// A ToolImage in Mem::Large; null if the buffer could not be allocated.
std::shared_ptr<ToolImage> make_tool_image(const char* mime, size_t size);
// Appends {"mime":…,"bytes":N,"width":W,"height":H} (width/height left out when 0).
void append_image_json(os::LargeString& out, const ToolImage& img);
```

- [ ] **Step 1: Write the failing tests:**

```cpp
// tests/agent/tool_registry_test.cpp
TEST(ToolRegistry, WithoutVisionTheArrayIsTodays)
// add a.b, v.shot (vision_only), c.d: tools_json(false) == tools_json() and holds a_b then c_d,
// no v_shot; byte-equal to a registry that never saw v.shot.
TEST(ToolRegistry, VisionOnlyToolsGoLast)
// tools_json(true) parses as a list of names {a_b, c_d, v_shot}; a second call returns the same
// object (same data() pointer).
TEST(ToolRegistry, NoVisionOnlyToolMeansOneArray)  // tools_json(true).data() == tools_json(false).data()
TEST(ToolRegistry, VisionOnlyToolIsUnknownWithoutVision)
// call("v_shot", "", ctx{vision false}) -> error unknown_tool, the tool did not run;
// with ctx.vision true it runs.
TEST(ToolRegistry, ImageSurvivesTheThreadHop)
// A Ui-thread tool returning ToolOutput{image = make_tool_image("image/jpeg", 4)}: the caller's
// output has the same pointer and bytes; use_count() == 1 after the call returns.
TEST(ToolRegistry, ImageJsonShape)
// append_image_json -> {"mime":"image/png","bytes":4,"width":2,"height":3}; width 0 -> no
// width/height keys.
// tests/agent/tool_registry_heap_test.cpp: the existing default-heap budget assertion also holds
// with one vision-only tool registered and tools_json(true) built.
```

- [ ] **Step 2: Run** `cmake --build build/sim`. Expected: compile errors (`vision_only`, `ToolImage`, `ToolCtx::vision` unknown).
- [ ] **Step 3: Implement.**
  - The vision array is built under `std::call_once` as `json_` minus its `]`, a `,` when both sides are non-empty, then `vision_tail_` and `]`.
  - `make_tool_image()` uses `std::allocate_shared<ToolImage>(os::LargeAllocator<ToolImage>{})` and `os::LargeBuffer::allocate(size)`, and returns null when `!bytes.ok()`.
  - The `ToolDef` comment (`:74-84`) is not changed in this task (Task 4 rewrites it).
- [ ] **Step 4: Run** `ctest --test-dir build/sim -R "ToolRegistry|ToolRegistryHeap|SceneTools|AppAgent|$GOLDEN" --output-on-failure`. Expected: PASS.
- [ ] **Step 5: Commit** `feat(agent): offer vision-only tools only to vision models`.

---

### Task 4: `ToolThread::Io` as a one-shot task (`agent::IoRunner`)

**Files:**
- Create: `components/wisp_agent/include/wisp/agent/io_runner.hpp`, `components/wisp_agent/src/io_runner.cpp`; add the `.cpp` to `components/wisp_agent/CMakeLists.txt` `SRCS`
- Modify: `components/wisp_agent/include/wisp/agent/tool_registry.hpp:74-84`. The `ToolDef::thread` comment becomes: "a tool that touches flash in any way … must declare Fs or FsInteractive (wisp.fs). Io is a one-shot task per call (`wisp.tool.io`, `IoRunner`), with its stack in PSRAM on the device. It is for blocking work that cannot watch `ctx.cancelled`, such as an encoder, and it never touches flash (AGENTS.md section 5; `WISP_ASSERT_FLASH_THREAD` enforces this). Spec 3.9." Keep the FsInteractive sentences unchanged.
- Modify: `components/wisp_app/src/agent_wiring.hpp`: `std::unique_ptr<agent::IoRunner> io_runner_;` declared before `registry_` (`:318`). Modify `components/wisp_app/src/agent_wiring.cpp`:
  - `:409-418`: `.io = [io](std::function<void()> fn) { return io->post(std::move(fn)); }`, with `io = io_runner_.get()`;
  - `stop()` (`:593-601`): `io_runner_->join_all()` after `service_->stop()`.
- Create: `tests/agent/io_runner_test.cpp`; add `wisp_test(NAME io_runner_test SRCS agent/io_runner_test.cpp LIBS wisp_agent wisp_port_sim_os TIMEOUT 30)` after `tool_registry_test` (`tests/CMakeLists.txt:511-512`)

**Interfaces:**
- Consumes: `os::Task::spawn/join` (`os.hpp:143-168`), `os::TaskConfig` (`os.hpp:95-139`), `ToolRegistry` (unchanged: it already answers `busy` when `post` is refused, `tool_registry.cpp:282-287`, and abandons on timeout or cancel, `:289-306`).
- Produces (`namespace wisp::agent`, `io_runner.hpp`):

```cpp
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
    // Waits for every task (AgentWiring::stop()).
    void join_all();
    size_t running() const;
};
```

- [ ] **Step 1: Write the failing tests** (`tests/agent/io_runner_test.cpp`):

```cpp
TEST(IoRunner, RunsOnItsOwnTask)
// Two sequential posts record std::this_thread::get_id(): both differ from the test thread's
// (each on a task of its own; ids may repeat on the host and are not compared to each other).
TEST(IoRunner, FinishedTasksAreJoinedByTheNextPost)
// post a quick job, wait until it ran, then running() == 1 until the next post();
// after that post's job finished and one more post: running() <= 1. join_all() -> running() == 0.
TEST(IoRunner, TwoRunningRefuseAThird)
// Two jobs block on a semaphore: a third post() returns false; release; after both finish
// a post() returns true.
TEST(IoRunner, RegistryAnswersBusyPastTwo)
// ToolRegistry with .io = runner; tool "t.io" (ToolThread::Io, timeout 50 ms) blocks on a
// semaphore: call 1 -> timeout, call 2 -> timeout, call 3 -> error busy; release; call 4 ok.
TEST(IoRunner, AbandonedCallIsJoinedLater)
// call -> timeout while the job still runs; release it; join_all() returns; running() == 0.
TEST(IoRunner, TaskConfigNeverTouchesFlash)
// task_config(): name "wisp.tool.io", stack 16384, priority 3, flash_access false, hot_stack false.
TEST(IoRunnerDeathTest, FlashCallFromIoAborts)
// EXPECT_DEATH: a std::thread calls os::set_flash_thread() and stays alive; the runner's job runs
// WISP_ASSERT_FLASH_THREAD(); matcher "flash access off the flash thread" (the guard's message, `tests/os/flash_guard_test.cpp:226`).
```

- [ ] **Step 2: Run** `cmake --build build/sim`. Expected: compile error (`wisp/agent/io_runner.hpp` not found).
- [ ] **Step 3: Implement** as plan decision 9 describes. Slots are a `std::array<Slot, kMaxTasks>` under an `os::Mutex`. Each spawned function runs `fn()` and then `done->store(true)`. `post()` joins a slot whose flag is set before reusing it. No join runs while the mutex is held for a task that has not finished, so `post()` never waits on a running job. Then wire `AgentWiring`.
- [ ] **Step 4: Run** `python3 tools/check_layering.py && ctest --test-dir build/sim -R "IoRunner|ToolRegistry|AgentWiring|AppAgent|$GOLDEN" --output-on-failure --repeat until-fail:3`. Expected: PASS three times. Also check that `grep -rn "ToolThread::Io" components` lists only `tool_registry.{hpp,cpp}`: no tool declares `Io` before Task 6 (spec 3.9, "What moves").
- [ ] **Step 5: Commit** `feat(agent): run ToolThread::Io tools on a one-shot task`.

---

### Task 5: The image slot in `TurnRunner`, and `attach_images()`

**Files:**
- Modify: `components/wisp_agent/include/wisp/agent/context.hpp`: `RequestImage`, `kImageOmittedNewer`, `attach_images()`; `BuiltRequest` (`:79-93`) gains `parts` and `part_text`
- Modify: `components/wisp_agent/src/context.cpp`
- Modify: `components/wisp_agent/src/turn_runner.hpp`, `turn_runner.cpp` (J1). Changes:
  - members `bool vision_ = false;` and `os::LargeVector<TurnImage> images_;`;
  - `run()` sets `vision_` from the `LlmModelInfo` it reads once and reads `tools.tools_json(vision_)`;
  - `build()` calls `attach_images()`;
  - `run_tool()` sets `ctx.vision = vision_` and takes `o.image` into the slot.
- Modify: `tests/agent/context_test.cpp` (suite `AttachImages`), `tests/agent/turn_runner_test.cpp` (J1; more `TurnRunner` tests)

**Interfaces:**
- Consumes: `LlmPart`/`parts` (Task 2), `ToolImage`/`ToolOutput::image`/`ToolCtx::vision`/`tools_json(bool)`/`kImageEstTokens` (Task 3), `FakeProvider` (J1, J2, Task 2).
- Produces (`namespace wisp::agent`, `context.hpp`):

```cpp
// What an older image of the running turn becomes in a request (spec 3.5).
inline constexpr std::string_view kImageOmittedNewer = "[image omitted: a newer one was taken]";

// One image-producing tool call of the running turn, oldest first. `bytes` empty: an
// older image, replaced by kImageOmittedNewer.
struct RequestImage {
    std::string_view tool{};      // dotted name, for the lead text
    std::string_view call_id{};
    std::string_view mime{};
    std::span<const uint8_t> bytes{};
};

// BuiltRequest gains (views in `messages` point into them; both are reserved to their
// final size before the first view is taken, so they never reallocate):
//     os::LargeVector<net::LlmPart> parts;
//     os::LargeString part_text;

// After each run of tool messages that answers one assistant message and holds a call in
// `images`, inserts one user message (spec 3.5). For each such call, in the run's order, it
// holds the text part "Image from <tool> (call <id>):" and then the image part (or the text
// part kImageOmittedNewer). Calls whose tool message is not in the request are skipped.
// est_tokens grows by the lead texts / 3 plus kImageEstTokens per image sent.
void attach_images(BuiltRequest& req, std::span<const RequestImage> images);
```

- `TurnRunner` (internal):

```cpp
struct TurnImage {
    os::LargeString tool, call_id;
    std::shared_ptr<const ToolImage> image;   // null once a newer image arrived
};
```

  When a tool returns `ok` with an `image` and `vision_` is set, the previous last entry's `image.reset()` (that frees its bytes), and the new one is appended. Without vision, or when the result is not `ok`, the image is dropped. A `TurnRunner` lives for one turn (J1), so the slot is empty at the next turn's start. Images are never stored: the tool message keeps the tool's text result.

- [ ] **Step 1: Write the failing tests:**

```cpp
// tests/agent/context_test.cpp — over hand-built BuiltRequests (system, user, assistant with
// calls, tool messages, trailing movable system message).
TEST(AttachImages, ImageFollowsItsToolRun)
// Assistant calls c1,c2; tools c1,c2; system D. images {c2 jpeg}: messages become
// [... tool c1, tool c2, user{Text "Image from t.shot (call c2):", Image}, system D].
TEST(AttachImages, ParallelCallsShareOneMessage)
// images {c1 omitted, c2 jpeg} in one run: one user message with 4 parts in call order:
// lead c1, Text kImageOmittedNewer, lead c2, Image.
TEST(AttachImages, OneMessagePerRun)
// Two assistant steps, images {c1 omitted (step 1), c3 jpeg (step 2)}: two user messages,
// each right after its own run.
TEST(AttachImages, EstimateCountsTheImageNotItsBytes)
// A 100 KiB image: est_tokens grows by kImageEstTokens + lead.size() / 3, not by 100 KiB / 3.
TEST(AttachImages, UnknownCallIsSkipped)    // call id not in the request: messages unchanged
TEST(AttachImages, NoImagesNoChange)        // empty span: messages, est_tokens unchanged
// tests/agent/turn_runner_test.cpp — FakeProvider model {"p1","m",65536,vision}; tool "test.shot"
// (ToolThread::Agent) returns result_json {"ok":true,"image":{…}} and an image of bytes FF D8 FF D9.
TEST_F(TurnRunner, ImageGoesToTheNextRequest)
// vision true; steps: call c1 test_shot, then text "ok". calls()[1].body contains, in order,
// "\"tool_call_id\":\"c1\"", then {"role":"user","content":[{"type":"text","text":"Image from
// test.shot (call c1):"},{"type":"image_url","image_url":{"url":"data:image/jpeg;base64,/9j/2Q=="}}]}
// and the stored tool message's text is the tool's text result (no "data:").
TEST_F(TurnRunner, OnlyTheNewestImageIsSent)
// calls c1 then c2 (two steps), then text: calls()[2].body has exactly one "data:" (c2's) and
// one kImageOmittedNewer after c1's tool message.
TEST_F(TurnRunner, ParallelCallsShareOneImageMessage)
// one step with calls c1,c2 (both test.shot): body of calls()[1] has tool c1, tool c2, then one
// user message; tool messages contiguous.
TEST_F(TurnRunner, NoVisionSendsNoImage)
// vision false: calls()[0].tools_json == str(tools.tools_json(false)); the tool's ctx.vision was
// false; no "data:", no "Image from" in any body.
TEST_F(TurnRunner, VisionPicksTheVisionArray)
// a vision_only tool registered; vision true: calls()[0].tools_json == str(tools.tools_json(true)).
TEST_F(TurnRunner, TheSlotIsEmptiedAtTurnEnd)
// turn 1 takes an image; turn 2 (a new TurnRunner, plain reply): its body has no "data:" and no
// "Image from"; turn 1's tool message text is in it.
TEST_F(TurnRunner, FailedToolImageIsIgnored)  // ok false with an image: no image message
```

- [ ] **Step 2: Run** `cmake --build build/sim`. Expected: compile errors (`attach_images`, `RequestImage` undeclared).
- [ ] **Step 3: Implement.**
  - `attach_images()`: count the user messages to insert and the lead text bytes, then `reserve()` `parts` and `part_text`, then insert into `messages`. Views into `arena` and the history stay valid because only `messages` changes.
  - `build()` builds the `RequestImage` list from `images_`, which already holds a slot entry for an image added in this step before the next request is built.
- [ ] **Step 4: Run** `ctest --test-dir build/sim -R "AttachImages|Context|TurnRunner|Calibration|AgentLoop|$GOLDEN" --output-on-failure --repeat until-fail:2`. Expected: PASS twice, and the golden files are untouched (`git diff -- tests/agent/agent_turn_golden_test.cpp tests/net/llm_client_test.cpp` shows only Task 2's additions in the latter).
- [ ] **Step 5: Commit** `feat(agent): show the turn's newest tool image to vision models`.

---

### Task 6: `ui.screenshot`

**Files:**
- Create: `components/wisp_agent/src/screenshot.hpp`, `components/wisp_agent/src/screenshot.cpp` (internal; add to `SRCS`)
- Modify: `components/wisp_agent/src/tools_ui.cpp` (new `add_screenshot_tool()`), `components/wisp_agent/include/wisp/agent/tools.hpp:34` (declaration and deps)
- Modify: `components/wisp_agent/include/wisp/agent/limits.hpp`: `kScreenshotWidth = 240`, `kScreenshotQuality = 70`, `kScreenshotToolMs = 10000`, `kSnapshotWaitMs = 2000`
- Modify: `components/wisp_app/src/agent_wiring.cpp:496`: after `add_ui_tools`, `if (deps_.shape.width > 0) check("screenshot", agent::add_screenshot_tool(*registry_, {.ui = deps_.ui, .shape = deps_.shape}));`
- Modify: `components/wisp_ui/include/wisp/ui/strings.hpp:554` (`ActUiScreenshot` after `ActUiTree`), `components/wisp_ui/src/strings_{zh,ja,en}.cpp` (plan decision 13), `components/wisp_shell/src/activity.cpp:61` (`{"ui.screenshot", StrId::ActUiScreenshot, false}`)
- Create: `tests/agent/screenshot_test.cpp`, `tests/support/jpeg_decode.hpp`; add `wisp_test(NAME screenshot_test SRCS agent/screenshot_test.cpp LIBS wisp_agent wisp_port_sim_os TIMEOUT 60)` with `target_include_directories(screenshot_test PRIVATE ${WISP_ROOT}/components/wisp_agent/src)` after `io_runner_test`
- Modify: `tests/app/app_agent_test.cpp:248-249`: walk `tools_json(true)`; append one test

**Interfaces:**
- Consumes: `UiRuntime::snapshot_rgb888(timeout)` (`ui_runtime.hpp:214-226`), `hal::visible()`/`ScreenShape` (`display_geometry.hpp:30-35, 64`), `encode_jpeg()` (Task 1), `IoRunner` through `ToolThread::Io` (Task 4), `make_tool_image()`/`append_image_json()` (Task 3).
- Produces:

```cpp
// screenshot.hpp (namespace wisp::agent, internal)
struct ScreenshotOptions {
    uint32_t width = kScreenshotWidth;     // never scaled up (plan decision 10)
    int quality = kScreenshotQuality;
    size_t max_bytes = kImageMaxBytes;
};
// The crop (plan decision 2): the bounding box of the pixels hal::visible() accepts.
hal::Area screenshot_area(const hal::ScreenShape& shape);
// Spec 3.6 / agent-core spec 5.4 as ruled: blacks out, in place, every pixel the panel cannot
// show; crops to screenshot_area(); scales by area average into an os::LargeBuffer; frees `rgb`;
// encodes into a kImageMaxBytes buffer, truncated to the JPEG. Errors: InvalidArgument (rgb
// size != width * height * 3), NoMemory, QuotaExceeded (too_large).
Result<std::shared_ptr<ToolImage>> make_screenshot(std::vector<uint8_t>&& rgb,
                                                   const hal::ScreenShape& shape,
                                                   const ScreenshotOptions& opts = {});

// tools.hpp
struct ScreenshotToolDeps {
    ui::UiRuntime& ui;
    hal::ScreenShape shape{};
};
// ui.screenshot (spec 3.6): no arguments, ToolThread::Io, kScreenshotToolMs, Normal, vision_only.
// Result {"ok":true,"image":{"mime":"image/jpeg","bytes":N,"width":W,"height":H}} plus the image.
// Errors: busy / timeout (the UI did not take the snapshot), snapshot_failed, no_memory, too_large.
Result<void> add_screenshot_tool(ToolRegistry& reg, ScreenshotToolDeps deps);
```

  Description (model-facing): "Takes a picture of what the screen shows now (the scene and any dialog over it; parts the panel cannot show are black), 240 px wide, and shows it to you as an image. For checking how a scene looks; ui.tree gives text and positions."

- [ ] **Step 1: Write the failing tests.** `tests/support/jpeg_decode.hpp` decodes a JPEG with LVGL's bundled TJPGD (`src/libs/tjpgd/tjpgd.h`, as `components/wisp_port_sim/ui/image_decoders.cpp` does). It returns `{width, height, rgb}` with each pixel swapped from TJPGD's B, G, R into R, G, B (`image_decoders.cpp:47`). Then the tests:

```cpp
// tests/agent/screenshot_test.cpp
TEST(Screenshot, AreaIsTheVisibleBoundingBox)
// Rect 320x240, RoundedRect 480x480 r40, Circle 466x466: screenshot_area == full panel each.
TEST(Screenshot, SizePerShape)
// white frames: Rect 320x240 -> 240x180; RoundedRect 480x480 -> 240x240; Circle 466x466 ->
// 240x240; Rect 200x100 -> 200x100 (no upscale). image->mime "image/jpeg"; sniff_image() agrees;
// bytes start FF D8 and end FF D9.
TEST(Screenshot, CircleCornersAreBlack)
// Circle 466 white: decoded pixel (2,2) and (237,2) < 16 per channel; (120,120) > 239.
TEST(Screenshot, RoundedCornersAreBlackAndEdgesKept)
// RoundedRect 480 r40 white: (0,0) black; (120,0) white (the visible top edge is kept, which the
// literal safe_rect crop would have cut).
TEST(Screenshot, ChannelsKeepTheirOrder)
// Rect 64x64 left half pure red, right half pure blue: decoded (8,32) R > 200, B < 60;
// (56,32) B > 200, R < 60.
TEST(Screenshot, TooLargeIsQuotaExceeded)  // noisy Rect 240x240, max_bytes 512 -> QuotaExceeded
TEST(Screenshot, WrongSnapshotSizeIsRefused) // rgb one row short -> InvalidArgument
// A real UiRuntime over test::FakeDisplay("display.main", 466, 466) (tests/ui/ui_runtime_test.cpp:26)
// with the screen's background set to white on the UI thread; registry .io = IoRunner.
TEST(ScreenshotTool, ShowsTheScreen)
// call("ui_screenshot", "", ctx{vision true}): ok; result_json parses to image {mime image/jpeg,
// bytes == image->bytes.size(), width 240, height 240}; decoded centre white, corner black.
TEST(ScreenshotTool, IsVisionOnlyIoAndLast)
// def->vision_only, thread Io, timeout_ms 10000, risk Normal; tools_json(false) lacks
// "ui_screenshot"; it is the last entry of tools_json(true).
TEST(ScreenshotTool, StoppedUiIsAnError)
// ui.stop() first: call -> ok false, error one of busy/timeout/snapshot_failed, no image.
// tests/app/app_agent_test.cpp
TEST(AppAgent, ScreenshotOnlyForVisionModels)
// UiNetHarness: find("ui.screenshot") != nullptr with vision_only; tools_json(false) has no
// "ui_screenshot", tools_json(true) ends with it.
```

- [ ] **Step 2: Run** `cmake --build build/sim`. Expected: compile errors (`screenshot.hpp`, `add_screenshot_tool`, `StrId::ActUiScreenshot` missing).
- [ ] **Step 3: Implement.**
  - The tool's `run()` executes on `wisp.tool.io`:
    1. `ui.snapshot_rgb888(kSnapshotWaitMs)`;
    2. error mapping: `Busy` → `busy`, `Timeout` → `timeout`, anything else → `snapshot_failed` with the message;
    3. `make_screenshot(std::move(snapshot), shape)`;
    4. error mapping: `QuotaExceeded` → `too_large`, `NoMemory` → `no_memory`;
    5. the text result built with `append_image_json()`.
  - The area average sums each source box in `uint32_t` per channel; box edges are `x * cw / ow` and `(x + 1) * cw / ow`.
  - Then come the strings and the label.
- [ ] **Step 4: Run** `python3 tools/check_i18n.py && python3 tools/gen_cjk_font_test.py && ctest --test-dir build/sim -R "Screenshot|IoRunner|AppAgent|Activity|Strings|$GOLDEN" --output-on-failure`. Expected: PASS. The two scripts confirm the new strings exist in all three languages and need no new glyph.
- [ ] **Step 5: Commit** `feat(agent): add ui.screenshot for vision models`.

---

### Task 7: `http.fetch` with `as:"image"`

**Files:**
- Modify: `components/wisp_agent/src/tools_http.cpp`:
  - `build_schema()` (`:298-317`): the `as` enum becomes `["raw","text","image"]`;
  - the description (`:451-455`) gains: " With as:\"image\" (models that take images only), a JPEG or PNG response up to 128 KiB is shown to you as an image.";
  - `Fetch::run()` (`:322-431`) checks `as:"image"` first: without `ctx.vision` it answers `tool_error("unsupported", "the active model does not take images")` before `http_tools_drop_cache()` and the request;
  - new `image_of(CacheEntry&)` next to `slice_of()` (`:229-293`), used by `run()` and `from_ref()` (`:433-441`); `from_ref` gains `ToolCtx&` for the vision check;
  - the file's header comment gains one sentence on images (spec 3.5).
- Modify: `components/wisp_agent/include/wisp/agent/tools.hpp:113` comment (if it lists `as` values)
- Modify: `tests/agent/http_tools_test.cpp` (`ctx.vision` set per test)

**Interfaces:**
- Consumes: `sniff_image()` (Task 1), `make_tool_image()`/`append_image_json()`/`kImageMaxBytes`/`ToolCtx::vision` (Task 3).
- Produces: the result for `as:"image"`, in `result_json`, has `image` in place of `offset`, `returned_bytes`, `more` and `body`:

```text
{"ok":true,"status":S,"final_url":…,"content_type":…,"headers":<external_data>,"ref":"rN",
 "total_bytes":N,"image":{"mime":…,"bytes":N,"width":W,"height":H}}
```

  The response bytes are copied into the `ToolImage`, and the cache keeps its own copy for `ref`. Errors: `unsupported` (no vision), `not_image`, `too_large` (over `kImageMaxBytes`, or `body_truncated`). Network errors are today's. The note `AgentNoteFetch` goes out on every path that made a request, as today.

- [ ] **Step 1: Write the failing tests:**

```cpp
TEST_F(HttpFetch, SchemaOffersImage)            // the registered schema's as.enum == raw,text,image
TEST_F(HttpFetch, ImageWithVisionComesBack)
// ctx.vision true; client.next = 200, a 3x2 PNG (signature + IHDR + IEND), content-type
// application/octet-stream: ok; image {mime image/png, bytes N, width 3, height 2};
// out.image->bytes equals the body; no "body" key; note set.
TEST_F(HttpFetch, ImageWithoutVisionIsUnsupported)
// ctx.vision false: error unsupported; client.requests() empty; an earlier ref still valid.
TEST_F(HttpFetch, NotAnImage)                   // text/html body -> not_image, no out.image
TEST_F(HttpFetch, ImageTooLarge)
// a JPEG of kImageMaxBytes + 1 bytes -> too_large; a body_truncated response (over the 1 MiB
// response cap) -> too_large.
TEST_F(HttpFetch, ImageFromRef)
// fetch raw first, then {"ref":"r1","as":"image"} with vision: ok, image set, one request in all.
TEST_F(HttpFetch, BinaryOmitsBody)              // existing: unchanged (as raw keeps today's shape)
```

- [ ] **Step 2: Run** `cmake --build build/sim && ctest --test-dir build/sim -R HttpFetch`. Expected: the new tests fail (`as` enum lacks `image`, so `bad_args`).
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** `ctest --test-dir build/sim -R "HttpFetch|HtmlText|AppAgent|$GOLDEN" --output-on-failure`. Expected: PASS.
- [ ] **Step 5: Commit** `feat(agent): let http.fetch hand vision models an image`.

---

### Task 8: Documents, CI and the firmware build

**Files:**
- Modify: `docs/architecture.md`:
  - a new task-table row after `wisp.agent` (`:82`): "| `wisp.tool.io` | `agent::IoRunner` (`components/wisp_agent/src/io_runner.cpp`, owned by `app::AgentWiring`) | 16 KB (`IoRunner::kStackBytes`, priority 3) | `false`, PSRAM on the device | One-shot task per `ToolThread::Io` call (spec 3.9 of the provider design): blocking work that cannot watch cancellation, today only `ui.screenshot`'s scaling and JPEG encoding; at most two at once (one the registry abandoned, one new), finished ones joined by the next call and by `AgentWiring::stop()`; never touches flash |";
  - `:393`: add ", and blocking library work declares `ToolThread::Io` (`wisp.tool.io`, a one-shot task that never touches flash)";
  - the agent section: one bullet on vision. It covers the two `tools` arrays (`ToolDef::vision_only`), `ToolCtx::vision`, the image slot in `TurnRunner` (newest image only, never stored, one `user` message after the tool run) and `LlmClient` dropping images for a profile without vision.
- Modify: `docs/superpowers/specs/2026-11-05-agent-core-design.md`:
  - 5.4 (`:312-316`): replace the bullets with the revised steps. They are: snapshot; the invisible pixels blacked out; crop to the visible bounding box; 240 px wide; JPEG quality 70 with `wisp::encode_jpeg()` (vendored `stb_image_write`, the same on the device and in the simulator); on `wisp.tool.io`; at most 128 KiB; a text result plus one `user` message with the image.
  - The section is marked "（2026-11-16 修订，见 `2026-11-15-llm-tool-providers-design.md` 3.5/3.6 与 J3 计划决定 2）".
  - 3.3 (`:135`): "截图只保留最新一张，更早的替换为 `[image omitted: a newer one was taken]`（2026-11-16 修订）".
  - 3.2 item 2 (`:117`): append "（工具定义分两份：有视觉与无视觉，2026-11-16 修订）".
- Modify: `docs/superpowers/specs/2026-11-09-agent-tools-design.md:94`: the `as` row gains "或 `image`：JPEG/PNG 原样交给有视觉的模型，最大 128 KiB（2026-11-16 修订，见 provider 设计 3.5）".
- Modify: `docs/backlog.md:18`: delete "screenshots and vision;" (closed). Keep the other 4a items.
- Modify: `README.md:170-171`, `README.zh-CN.md:170-171`, `README.ja.md:170-171`: `ui.screenshot` joins `ui.tree` in the scene row ("…and, with a vision model, look at the screen"). The `http.fetch` row gains "or hand an image to a vision model". The three READMEs change together (AGENTS.md section 5).
- Modify: `components/wisp_agent/include/wisp/agent/tool_registry.hpp` header comment: one sentence on the two arrays and `ToolOutput::image`, if Task 3 did not already add it.

- [ ] **Step 1: Edit the documents** as listed. Run `grep -rn "esp_new_jpeg\|截图已省略\|one-shot task with an internal stack" docs components` and fix any statement it shows to be stale. Only spec prose and `esp_jpeg_decoder` (the decoder, which stays) may still match.
- [ ] **Step 2: Run** `bash tools/ci.sh`. Expected: exit 0, with no `warning:` in the build part of the output.
- [ ] **Step 3: Build the firmware for both boards** (ESP-IDF v6.0.3):

```bash
cd firmware
for B in waveshare_esp32_s3_touch_amoled_2_16 waveshare_esp32_s3_touch_amoled_1_75c; do
  mkdir -p build-$B
  bash -c ". ~/esp/esp-idf-v6.0.3/export.sh >/dev/null 2>&1 && idf.py bmgr -c ../boards -b $B && idf.py -B build-$B -D SDKCONFIG=build-$B/sdkconfig -D SDKCONFIG_DEFAULTS='sdkconfig.defaults;components/gen_bmgr_codes/board_manager.defaults' -DWISP_BOARD=$B build" > build-$B/fw.log 2>&1
  echo "$B exit=$?"; tail -2 build-$B/fw.log; grep -c 'warning:' build-$B/fw.log
  grep -n 'stb_image_write' build-$B/fw.log | head -3
done
```

  Expected for each board: `exit=0`, "Project build complete", and a warning count of `0`.
  - If the count is not 0, `grep warning: build-$B/fw.log` must show none from `components/` or `third_party/`. If stb warns under the IDF toolchain's `-Wextra`, add the narrowest private `-Wno-…` in `third_party/stb_image_write/CMakeLists.txt` and rebuild.
  - Record any warning from ESP-IDF itself in the commit body (the log stays inside the git-ignored build directory).
  - Record the size of `libstb_image_write.a` and `libwisp_base.a` from `idf.py -B build-$B size-components` in the commit body: the code size (spec 5, "Flash (image)").
  - If ESP-IDF is not installed, write "firmware build not run" in the commit body and add it to the device checks.
- [ ] **Step 4: Commit** `docs: record vision, ui.screenshot and the wisp.tool.io task`.

**Device checks (for the owner; not run by this plan):**
1. **`ui.screenshot` on the 2.16 board and on the round 1.75C (466 px) board**, with a vision profile. Ask "what is on the screen?" on Home and inside an example scene. Check:
   - the model describes the screen correctly, including text near the round panel's edge (this checks plan decision 2);
   - the log's encode time, from the `tool ui.screenshot ok <ms> ms` line;
   - the JPEG size (`bytes` in the stored tool result; spec 5.4 expects 15-25 KB).

   Note whether 240 px is legible for small text on the 466 px panel (spec decision 5).
2. **`mem` during and after a screenshot:**
   - the `wisp.tool.io` stack high-water mark (16 KB allotted; the stb JPEG core alone uses about 5 KB);
   - internal free and largest block against `ram_budget.hpp`;
   - PSRAM back to its idle level after the turn (the slot is emptied).
3. **The provider's reported prompt tokens** (`tokens=` in the `llm` log line) for a request with one screenshot, against `kImageEstTokens = 1024` (plan decision 6).
4. **`http.fetch` with `as:"image"`** on a PNG and a JPEG URL with a vision model: the model describes them. With a profile without vision: `unsupported`, and no request in the `shttp` log.
5. **The same turn with a non-vision profile:** `ui_screenshot` absent from the request (the tools count in the log), and replies as before.
6. **Firmware code size** of the encoder (Task 8 step 3 numbers) against the free space of the app partition on both boards.

---

## Self-review

- **Spec coverage (J3 scope).**
  - 1.4/3.9: the `Io` meaning, `IoRunner`, joins, the two-task limit and the comment (Task 4).
  - 1.7/3.6: the encoder, the screenshot steps, PSRAM buffers, the result shape, the 10 s timeout, `vision_only` and `Io` (Tasks 1, 6).
  - 3.1: `LlmPartKind`/`LlmPart`/`parts` with J1's names (Task 2).
  - 3.4: the `first` interplay (Task 2, plan decision 1).
  - 3.5: the wire form, base64 straight into the body, no images in tool messages, one user message after the run with lead texts, the formats, the 128 KiB cap and `too_large`, one live image with the omitted text, not stored, the two arrays, `unsupported` for `as:"image"` without vision, and `LlmClient` dropping images (Tasks 2, 3, 5, 7). MCP image results belong to J4, which uses `ToolOutput::image` and `kImageOmittedNoVision` from here.
  - Section 5: PSRAM and stack (Global Constraints, device checks).
  - Section 7: the body, `TurnRunner`, `ToolThread::Io` and `ui.screenshot` tests (Tasks 2, 4-6).
  - Section 8 item 6: the device checks.
  - Section 9 step 4 and the step 7 documents: architecture, agent-core 3.2/3.3/5.4, agent-tools 3.1, references and notices, backlog `:18` (Tasks 1, 8).
- **Spec contradictions flagged:**
  - plan decision 2 (the crop; the owner should confirm);
  - plan decision 3 (namespace);
  - the agent-core spec 5.4's `esp_new_jpeg` (superseded by spec decision 4, revised in Task 8).
- **Type consistency.** These are used under the same names in every task:
  - `LlmPart{kind, text, mime, bytes}`, `LlmMessage::parts`, `build_chat_body(…, dialect, vision)`;
  - `ToolImage{mime, bytes, width, height}`, `ToolOutput::image`, `ToolDef::vision_only`, `ToolCtx::vision`, `tools_json(bool)`;
  - `RequestImage{tool, call_id, mime, bytes}`, `attach_images()`, `IoRunner::{post, join_all, running, task_config}`;
  - `make_screenshot()`, `screenshot_area()`, `encode_jpeg()`, `sniff_image()`, `base64_encode_into()`.
- **Proportion.** New code is given by signatures, rules and test assertions; bodies are not written out.
