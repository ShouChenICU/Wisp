# Device Model H3: Keypad Input for System UI and the Pixel-Format Cap Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A board whose buttons carry the roles `nav_prev`/`nav_next`/`nav_ok`/`nav_back` can answer every system dialog, close the panel, leave a scene, the diagnostics or a shell sub-page and walk the settings list without touching the screen; and `pixel_format` names the buffer format the UI hands to `Display::flush()` (not the panel's native format), so a display declaring a format the UI cannot render yet degrades to headless with a boot-report note instead of drawing garbage.

**Architecture:** `UiRuntime` gets an LVGL keypad input device and one focus group whenever a button holds a nav role. A small adapter in `wisp_app` (`NavKeys`) turns the nav buttons' `down`/`up` edges from `ButtonService` into `UiRuntime::push_key()`, a thread-safe ring of 8 entries drained by the keypad's read callback on the UI thread. Before a key reaches LVGL, a hook lets `SystemLayer` keep the group scoped to what is in front (the open dialog, the error card, the panel, else the active screen), focus a dialog's safe answer, and run `nav_back` (the same exits as the back gesture, then the shell's own page back). `Display` caps gain `pixel_format`, the format of the buffers `flush()` receives; a panel whose native format differs (for example RGB888) keeps declaring `rgb565` and converts inside its driver's `flush()`. `UiRuntime::start()` returns `Unsupported` for a buffer format the UI cannot render yet (today anything but `rgb565`), and `App` records `"display": "unsupported_pixel_format"` while the rest of the device keeps running.

**Tech Stack:** C++20 (`-fno-exceptions -fno-rtti`), LVGL 9.5 (keypad indev, `lv_group_t`), GoogleTest, the simulator build (`build/sim`), SDL2 keyboard input in `simulator/main.cpp`.

**Spec:** `docs/superpowers/specs/2026-11-14-device-model-design.md` (approved 2026-11-14, section 20): sections 9.1 and 9.2, the H3 part of section 13, the keypad lines of 14.1 and 14.4, 19 ("With nav roles on two simulator keys, a system dialog is answered without the pointer"). Section 10 (headless agent) was removed at approval and is not implemented. The roadmap's section H line "keypad/encoder group" is narrowed by the spec to keypad only; the encoder stays deferred (spec 17.2 item 7).

**Owner decisions (2026-11-18):** the 2.16 nav-role test uses the local patch in the device checks (no build flag); simulator nav keys stay off unless `--nav-keys`; Home-to-page navigation, history, scene list, on-screen keyboard and sliders by keys are left to phase M (backlog); `pixel_format` is the flush buffer format, e-paper/monochrome go to roadmap phase N.

## Global Constraints

- Branch: `feat/keypad-input` (worktree `.superpowers/worktrees/keypad-input`). No commits on `main`, no push.
- AGENTS.md applies in full: `wisp::Result<T>`, no exceptions/RTTI/`dynamic_cast`, comments explain why (no review ids, no change history, no `.superpowers/` paths), layering (`tools/check_layering.py`; no device-id literals in portable code or `simulator/*.cpp`).
- Single UI thread: every `lv_*` call, `SystemLayer` member and `Shell` member stays on the UI thread. `UiRuntime::push_key()` is the only new cross-thread entry; it takes a short mutex and never blocks or allocates.
- `ButtonService` listener rules (AGENTS.md section 3, `button_service.hpp`): `NavKeys`' listener only calls `push_key()` (a non-blocking push that fails when full); it never calls LVGL, `post()`s nothing that waits, touches no flash.
- Screen shape and style guide: focus indication must stay inside the dialog card and leave a visible gap to the neighbouring button (`docs/ui-style-guide.md`, "Spacing"); checked by geometry assertions on rect 480, rounded_rect 480 and circle 466, and by the manual screenshot previews (Tasks 3 and 5).
- No new user-facing strings (no `StrId` change), no new settings (no `WISP_SETTINGS` change), no new task or stack, no internal-RAM change beyond the ring (8 entries of 2 bytes inside `UiRuntime::Impl`).
- `WISP_MANIFEST` and `WISP_BOOT_REPORT` stay single-line JSON; the only change is the boot-report note `display` (Task 1), which appears only when the pixel format is refused.
- No committed change under `boards/`. The 2.16's nav roles for the device check are a local, uncommitted patch the owner applies and reverts (section "Device checks").
- Every task ends green on `cmake --build build/sim && ctest --test-dir build/sim --output-on-failure -R '<the task's filter>'`, and `bash tools/ci.sh` is green before each commit.
- Firmware size: after Task 8 both boards build; the 2.16 app partition must stay at or below 80 % used (78.9 % on `main` before H3).
- Commits: Conventional Commits, ending with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. The AGENTS.md change is its own `docs(agents): ...` commit inside Task 8.

## Review Focus

1. **A key press that wakes the screen must not also act.** With the screen off (`UiRuntime` paused), `push_key()` refuses presses and drops their releases, and the keypad's LVGL state is reset on resume, so a held OK across standby neither clicks nor repeats. Tests in Task 2 (`KeysAreRefusedWhilePaused`, `AKeyHeldAcrossAPauseDoesNotClick`) and Task 6 (`AWakingPressDoesNotAnswerTheDialog`).
2. **Focus never lands on something that is not in front.** A button on a screen that is not loaded, or under an open dialog, is never focused; closing the dialog gives focus back to what had it. Tests in Task 3 (`FocusStaysOnTheScreenInFront`, `DialogTrapsFocusAndGivesItBack`).
3. **A dangerous dialog is never answered "yes" by a stray OK.** The safe answer (No/Deny/Cancel) is focused when a dialog opens, and on a board with touch the first key press only shows the focus. Tests in Task 3 (`DialogFocusesTheSafeAnswer`, `FirstKeyOnlyShowsTheFocus`) and Task 6 (`PowerOffDialogIsAnsweredByKeys`).
4. **Back does exactly one thing per press, in a fixed order** (dialog, error card, keyboard, panel, back handler, shell page), and does nothing on the home screen. Tests in Task 4 (`BackClosesOnePieceAtATime`, `BackOnHomeDoesNothing`).
5. **Overflow never leaves a key stuck down.** A burst of presses fills the ring; every accepted press keeps room for its release; a refused press's release is ignored. Tests in Task 2 (`RingKeepsRoomForTheRelease`, `OneKeyAtATime`).

## Deviations from the spec

Recorded per AGENTS.md section 7 ("Deviations"); all small. Task 8 writes each one into the spec as an "In the code" note under 9.1/9.2.

1. **The group is not LVGL's default group.** Spec 9.1 says "a default focus group". With `lv_group_set_default()`, LVGL adds every button, switch and textarea of every screen to it at creation, and `lv_group_focus_next()` does not skip objects on screens that are not loaded (`lv_group.c`, `focus_next_core()` skips only hidden and disabled objects). So `UiRuntime` creates one group for the keypad only, and `SystemLayer` fills it with what is in front (a "focus scope", Task 3), rebuilt when that changes.
2. **Any nav role creates the keypad; `nav_prev` and `nav_back` are optional, `nav_next` and `nav_ok` are needed to move focus.** Spec 9.1 requires `nav_prev`, `nav_next` and `nav_ok`. The group wraps, so `nav_next` alone reaches every item, and the 2.16 has only two keys free for the device check. `nav_back` alone works too (it needs no focus). `App` warns at boot when a nav key exists but `nav_next` or `nav_ok` is missing.
3. **One key at a time, and a release always fits.** Spec 9.1 says "a thread-safe ring of 8 entries". LVGL's keypad handles one key at a time (`indev_keypad_proc()` treats a second key's press as "pressing" of the first), and a dropped release would leave a key down. `push_key()` therefore accepts a press only while no nav key is down and two slots are free (the press and its release); a release of a key that is not down is ignored.
4. **Keys are refused while the UI is paused.** Not in the spec. Any button wakes the screen already (`PowerManager`'s listener); without this, the press that wakes would also act on whatever is focused once the UI resumes.
5. **On a board with touch, the first key shows the focus.** LVGL marks a focus with `LV_STATE_FOCUS_KEY` only when the focusing input device is a keypad (`lv_obj.c`, `LV_EVENT_FOCUSED`), and a focus set in code is attributed to the pointer when there is one. A Next/Prev/OK press while the focused object lacks `LV_STATE_FOCUS_KEY` only adds it (no move, no click). On a keys-only board the focus is shown from the start.
6. **What `nav_back` does.** Spec 9.1: "the same as the back gesture". The back gesture exists only while a scene or the diagnostics are in front, or the panel is open (shell pages go back by page drags). `SystemLayer::nav_back()` therefore does, first that applies: answer an open confirm dialog "no"; the error card's Back; hide the keyboard; close the panel; the back handler; else the shell's page back (`Shell::back()`: Wi-Fi password form or detail, choice page, sub-page to Home). On the home screen it does nothing. It acts on the press.
7. **Settings rows are focusable.** Spec 9.1 says focus scrolls the settings list. Its rows are plain clickable `lv_obj`s (`page_frame.cpp`, `make_list_row()`), not LVGL widget classes, so `make_list_row()` marks clickable rows with `ui::kKeyFocusable` (Task 5). Other shell pages (history cards, scene cards, getting from Home to a sub-page) stay with phase M's `PageStack`, as the spec says.
8. **Widgets of a scene in front are focusable.** The focus scope is the active screen whatever it is, so a scene's buttons are reached like any other; scenes get no key events (phase I's `wisp.input.on("key")`).
9. **A non-string `pixel_format` is unsupported.** Spec 9.2 defines absent as `rgb565`; a present value that is not the string `"rgb565"` (another string, a number) is refused. The boot report keeps `"mode": "headless"` beside the new `"display": "unsupported_pixel_format"`.
10. **No board change is committed.** Spec 14.4 allows assigning nav roles to the 2.16's volume keys "for the test". That assignment is a local patch in the device checks below, never committed, so H2 (which moves ids and roles into `BoardSpec`) has nothing to merge.

## Expected merge touchpoints with H2

H2 (board kit, driver adapters, target readiness) is planned in parallel. Expected overlaps, all textual and small:

- `components/wisp_app/src/app.cpp`: H2 adds the `psram` note near the boot report; H3 changes the UI start block (step 4) and adds the nav wiring and its teardown.
- `docs/architecture.md`, "Machine-readable output": both add a key to the list of `notes` keys (`psram`, `display`), in the same sentence.
- `docs/porting-guide.md`: H2 rewrites "Step 3: Write `board.cpp`" for `BoardSpec`; H3 adds the `pixel_format` cap to the capability table and a short "Navigation keys" paragraph. Rebase H3's paragraph onto H2's text if H2 lands first.
- `CHANGELOG.md` `## Unreleased` and the spec's "In the code" notes (H2: sections 6-8, 11, 13; H3: 9.1, 9.2).
- AGENTS.md section 3: H2 edits "Device model"/"Board independence"; H3 adds one sentence to "UI layout".
- The device-check patch targets today's `boards/waveshare_esp32_s3_touch_amoled_2_16/board.cpp` `kButtons` table; after H2 the same two roles go on the two volume-key slots of the 2.16's `BoardSpec`.

---

### Task 1: `pixel_format` caps and the check

**Files:**
- Modify: `components/wisp_hal/include/wisp/hal/display_geometry.hpp` (next to `panel_kind()`, `:105-108`), `components/wisp_hal/src/display_geometry.cpp`
- Modify: `components/wisp_hal/include/wisp/hal/display.hpp` (caps comment, `:18-31`: add `"pixel_format"`)
- Modify: `components/wisp_ui/src/ui_runtime.cpp` (`UiRuntime::start()`, before the draw buffers are allocated, `:543-560`), `components/wisp_ui/include/wisp/ui/ui_runtime.hpp` (start()'s comment)
- Modify: `components/wisp_app/src/app.cpp` (step 4, `:398-403`)
- Test: `tests/hal/display_geometry_test.cpp`, `tests/ui/ui_runtime_test.cpp`, `tests/app/app_boot_test.cpp`

**Interfaces:**
- Produces in `wisp/hal/display_geometry.hpp`:
  - `inline constexpr const char* kPixelFormatRgb565 = "rgb565";`
  - `std::string pixel_format(const Properties& caps);` — the caps value when it is a string, `"rgb565"` when the key is absent, `""` for any other type.
- Produces in `UiRuntime::start()`: returns `ErrorCode::Unsupported`, message `"unsupported pixel format '<fmt>'"`, when `hal::pixel_format(caps) != kPixelFormatRgb565`, before `lv_init()` and before any allocation (so nothing needs undoing).
- Produces in `App::start()`: on that error, `report.set_note("display", "unsupported_pixel_format")` in addition to the existing `"mode": "headless"`.

- [ ] **Step 1: Write the failing tests.**

```cpp
// tests/hal/display_geometry_test.cpp
TEST(DisplayGeometry, PixelFormatDefaultsToRgb565)
// pixel_format({}) == "rgb565"; {{"pixel_format","rgb565"}} -> "rgb565";
// {{"pixel_format","l8"}} -> "l8"; {{"pixel_format", 16}} -> "".

// tests/ui/ui_runtime_test.cpp
TEST(UiRuntime, StartRefusesAnotherPixelFormat)
// FakeDisplay with set_cap("pixel_format", "l8"): start() fails, code Unsupported,
// message contains "l8"; lv_is_initialized() is false afterwards; a second UiRuntime
// over a plain FakeDisplay then starts (nothing was left behind).
TEST(UiRuntime, StartAcceptsAnExplicitRgb565)
// set_cap("pixel_format", "rgb565") -> start() ok.

// tests/app/app_boot_test.cpp
TEST(AppBoot, UnsupportedPixelFormatBootsHeadlessWithANote)
// make_app({}); h.display->set_cap("pixel_format", "i1"); start ok;
// notes.mode == "headless", notes.display == "unsupported_pixel_format", h.app.ui() == nullptr.
```

Also extend `AppBoot.DevicesAreFoundByRoleNotById`: `notes->find("display") == nullptr`.

- [ ] **Step 2: Run** `ctest --test-dir build/sim -R 'DisplayGeometry|UiRuntime|AppBoot' --output-on-failure`. Expected: compile failure (no `pixel_format`), then the three new tests FAIL.
- [ ] **Step 3: Implement** the helper, the check in `start()` (right after the width/height check, so a bad size still reports `InvalidArgument` first) and the note in `App::start()` (`impl_->ui->start()`'s result is kept in a local so its `error().code` can be read). Add `"pixel_format"` to `display.hpp`'s caps comment: `(string, optional, default "rgb565"; the format of the buffers flush() receives, not the panel's native format: a panel with another native format, such as RGB888, declares "rgb565" and converts in its driver. rgb565 is the only buffer format the UI renders today; UiRuntime::start() returns Unsupported for any other)`.
- [ ] **Step 4: Run** the same filter. Expected: PASS.
- [ ] **Step 5: Commit** `feat(ui): pixel_format cap; an unrenderable buffer format leaves the display dark`.

---

### Task 2: The keypad input device, `push_key()` and the key hook

**Files:**
- Create: `components/wisp_ui/include/wisp/ui/key_nav.hpp` (the `NavKey` enum only in this task; Task 3 adds the rest), add nothing to `CMakeLists.txt` yet
- Modify: `components/wisp_ui/include/wisp/ui/ui_runtime.hpp`, `components/wisp_ui/src/ui_runtime.cpp` (`Impl` `:83-190`, `start()` `:666-680`, `stop()` `:761`, `set_paused()`, `ui_main()`'s resume branch)
- Test: `tests/ui/ui_runtime_test.cpp`

**Interfaces:**
- Produces in `wisp/ui/key_nav.hpp`: `enum class NavKey : uint8_t { Prev, Next, Ok, Back };`
- Produces in `UiConfig`: `bool keypad = false;` — create the keypad input device and its group.
- Produces in `UiRuntime`:
  - `static constexpr size_t kKeyRing = 8;`
  - `bool push_key(NavKey key, bool pressed);` — any thread, never blocks or allocates. Returns false (and queues nothing) when: there is no keypad; the UI is paused; a press while any nav key is down (one key at a time); a press with fewer than two free slots; a release of a key that is not down. A refused press leaves the key up, so its release is refused too.
  - `using NavKeyHook = std::function<bool(NavKey key, bool pressed)>;` and `void set_nav_key_hook(NavKeyHook hook);` — thread-safe like `set_touch_activity_cb()` (copied out under a mutex, called outside it). The hook runs on the UI task, in the keypad's read callback, for each entry before LVGL sees it; returning true consumes the entry.
  - `lv_group_t* nav_group() const;` — UI thread only; the keypad's group, or nullptr without a keypad (and after `stop()`).
- Behaviour of the read callback (`Impl::keypad_read_cb`): pops entries in order; each one first goes to the hook; a consumed entry, and every `Back` entry (Back has no LVGL key), is skipped; the first other entry fills `data->key` (`Prev` → `LV_KEY_PREV`, `Next` → `LV_KEY_NEXT`, `Ok` → `LV_KEY_ENTER`) and `data->state`, and sets `data->continue_reading` while entries remain. With the ring empty it reports the last key and state it reported (so a held key auto-repeats through LVGL), released at first.
- `start()` creates the keypad indev (`LV_INDEV_TYPE_KEYPAD`) after the pointer indev, so `lv_indev_get_next(nullptr)` is still the pointer, plus `lv_group_create()` bound with `lv_indev_set_group()`; it is not `lv_group_set_default()` (deviation 1). Failure to create either is `NoMemory` through `fail_after_lv_init`. `stop()` clears both pointers (lv_deinit frees them).
- `set_paused(true)` empties the ring and the down state under the ring's lock. The UI task, when it leaves the paused branch, calls `lv_indev_reset(keypad, nullptr)` and resets the reported last state to released, before its first `lv_timer_handler()`.

- [ ] **Step 1: Write the failing tests** (helpers: a `UiRuntime` over `FakeDisplay` with `UiConfig{.keypad = true}`; `wait_until()` from `support/wait.hpp`).

```cpp
TEST(UiRuntime, NoKeypadWithoutTheFlag)
// default UiConfig: push_key(Next, true) == false; nav_group() (on the UI thread) == nullptr;
// only one indev exists.
TEST(UiRuntime, KeypadMovesFocusAndClicks)
// keypad: on the UI thread make buttons a, b on the active screen and lv_group_add_obj() them
// to nav_group(); focus a. push_key(Next,true), push_key(Next,false) -> focused == b;
// push_key(Ok,true), push_key(Ok,false) -> b's CLICKED counter == 1; a's == 0.
TEST(UiRuntime, ThePointerIsStillTheFirstIndev)
// keypad + FakeTouch: lv_indev_get_type(lv_indev_get_next(nullptr)) == LV_INDEV_TYPE_POINTER.
TEST(UiRuntime, OneKeyAtATime)
// push_key(Next,true) ok; push_key(Ok,true) false; push_key(Ok,false) false;
// push_key(Next,false) ok; push_key(Ok,true) ok.
TEST(UiRuntime, RingKeepsRoomForTheRelease)
// hold the UI task: post() a job that waits on a semaphore, so nothing drains the ring.
// Pairs 1-3 of Next fill 6 slots; the 4th press (exactly 2 free) and its release are
// accepted (ring full); a 5th press is refused and so is its release. Give the semaphore,
// wait for the drain: a press is accepted again. With 3 buttons in the group (wrap) and
// focus on the first, focus ends on index 4 % 3 == 1.
TEST(UiRuntime, HookSeesEveryEntryAndCanConsumeIt)
// hook records (key, pressed) and consumes Next presses and releases: after Next and Ok
// pairs, the record is {Next,1},{Next,0},{Ok,1},{Ok,0}; the focus has not moved; Ok clicked.
TEST(UiRuntime, BackNeverReachesLvgl)
// hook not consuming: push Back pair -> hook saw both; no LV_EVENT_KEY on the focused object.
TEST(UiRuntime, KeysAreRefusedWhilePaused)
// set_paused(true): push_key(Next,true) == false; set_paused(false): accepted.
TEST(UiRuntime, AKeyHeldAcrossAPauseDoesNotClick)
// focus a counting button; push_key(Ok,true), wait until the read callback took it
// (hook observed it); set_paused(true); set_paused(false); push_key(Ok,false) is refused
// (cleared); after 300 ms the button's CLICKED and LONG_PRESSED counters are 0.
```

- [ ] **Step 2: Run** `ctest --test-dir build/sim -R UiRuntime --output-on-failure`. Expected: compile failure, then FAIL.
- [ ] **Step 3: Implement.** The ring is a fixed `std::array<KeyEntry, kKeyRing>` with head/count, a `NavKey`-indexed `down` bitmask and an `os::Mutex`; `push_key()` reads `paused` and the keypad pointer through atomics. Document the four refusal rules in the header next to `push_key()`, and in the file comment that the keypad's group is filled by its owner (`SystemLayer`, Task 3).
- [ ] **Step 4: Run** `ctest --test-dir build/sim -R 'UiRuntime|SystemLayer' --output-on-failure` (SystemLayer's tests rely on the pointer being the first indev). Expected: PASS.
- [ ] **Step 5: Commit** `feat(ui): keypad input device fed by push_key()`.

---

### Task 3: Focus scope, focus style and dialogs

**Files:**
- Modify: `components/wisp_ui/include/wisp/ui/key_nav.hpp`; Create: `components/wisp_ui/src/key_nav.cpp` (add to `wisp_ui`'s sources in `components/wisp_ui/CMakeLists.txt`)
- Modify: `components/wisp_ui/src/theme.cpp` (`Styles`, `init_styles()`, `apply_cb()` `:193-268`), `components/wisp_ui/src/theme_internal.hpp` if `Styles` is declared there
- Modify: `components/wisp_ui/include/wisp/ui/system_layer.hpp`, `components/wisp_ui/src/system_layer.cpp` (`open_confirm()` `:876-905`, `open_dialog()` `:907`, `close_confirm()` `:914`, `show_error()` `:571`, `hide_error()`, `open_panel()`/`discard_panel()`, destructor `:216`)
- Test: `tests/ui/system_layer_test.cpp`, `tests/ui/theme_test.cpp`

**Interfaces:**
- Consumes: `NavKey`, `UiRuntime::nav_group()`, `UiConfig::keypad` (Task 2). Tests may also create a group with `lv_group_create()` on the UI thread and hand it to `set_nav_group()` directly.
- Produces in `wisp/ui/key_nav.hpp`:
  - `inline constexpr lv_obj_flag_t kKeyFocusable = LV_OBJ_FLAG_USER_2;` (the shell's `detail::kNoSwipe` is `USER_1`; `page_frame.cpp` gains a `static_assert` that they differ).
  - `bool key_focusable(lv_obj_t* obj);` — true for an object carrying `kKeyFocusable`, or a clickable object whose class is a group default (`lv_obj_is_group_def()`) other than the message box and its backdrop (`lv_msgbox_class`, `lv_msgbox_backdrop_class`).
  - `void collect_focusable(lv_obj_t* root, std::vector<lv_obj_t*>& out);` — pre-order over `root`'s descendants (not `root` itself), skipping hidden subtrees; a focusable object's own children are not visited (a button's label).
  - `void make_key_focusable(lv_obj_t* obj);` — sets `kKeyFocusable` and adds the theme's row focus style at `LV_STATE_FOCUS_KEY` (Task 5 uses it).
- Produces in the theme: style `focus_ring` at `LV_STATE_FOCUS_KEY` on buttons (incl. msgbox header/footer buttons), switches, sliders, checkboxes, dropdowns, rollers and textareas: `outline_width 2`, `outline_pad 2`, `outline_color theme::kAccentStrong`, `outline_opa LV_OPA_COVER`. Style `focus_row` (used by `make_key_focusable()`): `bg_color theme::kSurface3`, `bg_opa LV_OPA_COVER`, `radius theme::kRadius`; on a two-tone panel (`!hal::panel_is_colour(display_panel(disp))`) instead `border_width 2`, `border_color theme::kText`.
- Produces in `SystemLayer`:
  - `void set_nav_group(lv_group_t* group);` — null turns key navigation off; set once after construction (App, Task 6), cleared before destruction.
  - `bool on_nav_key(NavKey key, bool pressed);` — the `UiRuntime` hook body. On a press: rescope (below); `Back` is Task 4's (here: consumed, nothing done); for Next/Prev/Ok, with an empty scope consume; when the focused object lacks `LV_STATE_FOCUS_KEY`, add it (and `LV_STATE_FOCUSED`), scroll it into view and consume; otherwise return false. A release is consumed exactly when its press was.
  - Scope root, first that applies: the open dialog box (`confirm_box_`); the error card; the panel's content box (while it rests open); else `lv_screen_active()`. Rescope collects `collect_focusable(root)` and rebuilds the group only when that list differs from the group's (order included); a rebuild keeps the focused object, and its `LV_STATE_FOCUS_KEY`, when it is still in scope. Otherwise the scope's initial object is focused: a confirm dialog's No button; an error card's Back button; else, for the screen, the object focused there before a modal took the scope (when still valid and in scope), else the first.
  - `open_confirm()`, `open_dialog()`, `close_confirm()`, `show_error()`, `hide_error()` and the panel's open/close rescope at once, so a dialog opens with its safe answer focused.
  - New member `confirm_no_` beside `confirm_yes_`.

- [ ] **Step 1: Write the failing tests** (in `SystemLayerTest`: `start()` gains a `bool keypad = false` parameter that builds the runtime with `UiConfig{.keypad = true}`, calls `layer->set_nav_group(ui->nav_group())` and installs `layer->on_nav_key` as the runtime's hook, cleared in `TearDown()` before the layer goes; a helper `nav(NavKey k)` pushes the press and the release through `ui->push_key()` and waits until the ring is drained (a hook-side counter), so every test goes through the real key path).

```cpp
TEST_F(SystemLayerTest, DialogFocusesTheSafeAnswer)
// open_confirm(...) -> lv_group_get_focused(group) is the No button (second button of the row).
TEST_F(SystemLayerTest, FirstKeyOnlyShowsTheFocus)
// with FakeTouch: open_confirm; No lacks LV_STATE_FOCUS_KEY; nav(Next) -> No has it and is
// still focused; nav(Next) -> Yes focused; nav(Ok) -> on_answer(true) once.
TEST_F(SystemLayerTest, OkOnTheSafeAnswerSaysNo)
// open_confirm; nav(Next) (shows); nav(Ok) -> on_answer(false); confirm_open() == false.
TEST_F(SystemLayerTest, DialogTrapsFocusAndGivesItBack)
// screen buttons a, b; nav to b (focused, shown); open_confirm -> group holds exactly the
// dialog's two buttons; Next/Next cycles Yes/No only; answer -> group holds a, b and b is
// focused with LV_STATE_FOCUS_KEY.
TEST_F(SystemLayerTest, FocusStaysOnTheScreenInFront)
// screen S1 with button a (active), screen S2 (not loaded) with button c: Next any number of
// times never focuses c; lv_screen_load(S2) then Next focuses c, never a.
TEST_F(SystemLayerTest, ErrorCardFocusesBack)
// show_error -> focused is the Back button; nav(Next) shows; nav(Ok) calls on_back once.
TEST_F(SystemLayerTest, HiddenAndDisabledButtonsAreSkipped)
// a, b (hidden), c (LV_STATE_DISABLED), d -> Next sequence from a: d, a.
```

Geometry and previews, on rect 480, rounded_rect 480 and circle 466 (`ShapeCase`):

```cpp
TEST_P(SystemLayerShapes, FocusRingStaysInsideTheDialog)
// open_confirm with a long zh text; focus each button with nav(); grow the button's coords by
// outline_pad + outline_width (4 px): inside the msgbox's coords, inside box_visible(shape),
// and at least theme::kGapXs (4 px) from the other button's coords.
TEST_P(SystemLayerPreview, DISABLED_FocusedDialog)
// WISP_KEYNAV_SCREENSHOTS=<dir>: writes confirm-<shape>-no.png and confirm-<shape>-yes.png
// (snapshot_rgb888() through the existing PNG writer, as the settings preview does).
```

`tests/ui/theme_test.cpp`:

```cpp
TEST(Theme, KeyFocusRingOnButtons)
// a button with LV_STATE_FOCUS_KEY: outline width 2, pad 2, colour kAccentStrong, opa cover;
// without the state: outline width 0.
TEST(Theme, KeyFocusableRowFillsOnFocus)
// make_key_focusable(row) on an amoled panel: bg kSurface3 under LV_STATE_FOCUS_KEY;
// on a "mono" panel: border width 2 instead; flag kKeyFocusable set.
```

- [ ] **Step 2: Run** `ctest --test-dir build/sim -R 'SystemLayer|Theme' --output-on-failure`. Expected: compile failure, then FAIL.
- [ ] **Step 3: Implement** `key_nav.cpp`, the two styles, `set_nav_group()`/`on_nav_key()`/rescope in `system_layer.cpp`. Extend the file comment of `system_layer.hpp` with one paragraph on key navigation (scope order, the safe answer, first key shows the focus).
- [ ] **Step 4: Run** the same filter, then the preview once: `mkdir -p /tmp/wisp-keynav && WISP_KEYNAV_SCREENSHOTS=/tmp/wisp-keynav build/sim/tests/system_layer_test --gtest_also_run_disabled_tests --gtest_filter='*Preview*'`. Look at all six PNGs: the ring is fully visible on every shape, does not touch the other button, and the dialog's text and buttons are unchanged from today's. Expected: PASS and clean images.
- [ ] **Step 5: Commit** `feat(ui): system dialogs take keypad focus on their safe answer`.

---

### Task 4: `nav_back` in the system layer and the shell's page back

**Files:**
- Modify: `components/wisp_ui/include/wisp/ui/system_layer.hpp`, `components/wisp_ui/src/system_layer.cpp`
- Modify: `components/wisp_shell/include/wisp/shell/shell.hpp`, `components/wisp_shell/src/shell.cpp` (`start()`, `stop()`, new `back()`)
- Test: `tests/ui/system_layer_test.cpp`, `tests/shell/shell_test.cpp`

**Interfaces:**
- Consumes: `on_nav_key()` (Task 3).
- Produces in `SystemLayer`:
  - `bool nav_back();` — UI thread. First that applies, and only that one: a confirm dialog from `open_confirm()` is answered no (as its No button: closed, then `on_answer(false)`); the error card's Back (`hide_error()`, then `on_back`); the keyboard hides; the panel, resting open, slides shut (moving: nothing, returns true); the back handler runs (a copy, as `observe()` does); the fallback runs. A form from `open_dialog()` is not answered here: it falls through to the back handler or the fallback (its owner closes it). Returns false when nothing applied.
  - `void set_nav_back_fallback(std::function<void()> fallback);` — what `nav_back()` does when nothing of the layer's applies; empty: nothing.
  - `on_nav_key(Back, true)` calls `nav_back()`; both edges of Back are consumed.
- Produces in `Shell`:
  - `bool back();` — UI thread. Ignored (false) while a page drag or slide runs. Else, first that applies: `connections_->back()` (password form, then detail); a choice page closes (`close_choice()`); a sub-page goes to Home (`show(Page::Home)`); on Home, false.
  - `start()` sets the layer's fallback to post `back()` (guarded by `alive_`, like `set_diag_back_handler()`: the call arrives inside the keypad's read callback); `stop()` clears it.

- [ ] **Step 1: Write the failing tests.**

```cpp
// tests/ui/system_layer_test.cpp
TEST_F(SystemLayerTest, BackClosesOnePieceAtATime)
// back handler and fallback counters set; show the keyboard on a textarea, open_confirm:
// nav_back() -> on_answer(false), keyboard still up, handler 0; nav_back() -> keyboard gone;
// nav_back() -> handler 1; clear the handler; nav_back() -> fallback 1.
TEST_F(SystemLayerTest, BackClosesTheErrorCardWithItsBack)
// show_error: nav_back() -> error_visible() false, on_back 1, handler 0.
TEST_F(SystemLayerTest, BackClosesThePanelOnly)
// panel open and settled: nav_back() -> panel slides shut (panel_settled(), !panel_open()),
// back handler 0.
TEST_F(SystemLayerTest, BackSkipsAForm)
// open_dialog(): nav_back() -> fallback 1, confirm_open() still true.
TEST_F(SystemLayerTest, BackKeyGoesBack)
// through push_key(Back, true/false): handler 1; the focused button got no LV_EVENT_KEY.

// tests/shell/shell_test.cpp (ShellFixture)
TEST_F(ShellTest, BackKeyLeavesASubPage)
// show(Settings); layer.nav_back(); drain posts -> page() == Home, page_screen() == nullptr.
TEST_F(ShellTest, BackKeyClosesAChoicePageFirst)
// show(Settings); open_choice(...); nav_back -> choice_page() == nullptr, page() == Settings;
// nav_back -> Home.
TEST_F(ShellTest, BackOnHomeDoesNothing)
// on Home: back() == false; the home screen is still active; no page log line.
TEST_F(ShellTest, BackKeyClosesTheDiagnostics)
// open_diagnostics(); nav_back -> diagnostics_open() false (through the shell's back handler,
// not the fallback).
```

- [ ] **Step 2: Run** `ctest --test-dir build/sim -R 'SystemLayer|ShellTest' --output-on-failure`. Expected: FAIL.
- [ ] **Step 3: Implement.** In `shell.hpp`'s file comment, add the key path to the "Threading" paragraph (a back key's work is posted, like a back swipe's).
- [ ] **Step 4: Run** the same filter plus `-R 'ConnectionsPage|PageDrag'`. Expected: PASS.
- [ ] **Step 5: Commit** `feat(shell): the back key leaves dialogs, scenes and pages`.

---

### Task 5: Settings rows take key focus

**Files:**
- Modify: `components/wisp_shell/src/page_frame.cpp` (`make_list_row()`, clickable branch `:245-252`), `components/wisp_shell/src/page_frame.hpp` (`static_assert(kNoSwipe != ui::kKeyFocusable)`)
- Test: `tests/shell/settings_page_test.cpp`

**Interfaces:**
- Consumes: `ui::make_key_focusable()`, `SystemLayer::set_nav_group()`/`on_nav_key()` (Task 3).
- Produces: every clickable list row of the sub-pages (settings, choice, connections) is focusable; switches and sliders inside rows are focusable by their class already. If the circle's last row is not fully visible after its focus scrolled it in, the fix is in `key_nav.cpp`: a focus callback (`lv_group_set_focus_cb`) that, after LVGL's `scroll_to_view`, scrolls the nearest scrollable ancestor by the amount the object's top or bottom row lies outside `hal::row_span()` of the display's shape; no change to the page layout.

- [ ] **Step 1: Write the failing tests** (`SettingsPageTest` with the `PageHarness`; on the UI thread create a group with `lv_group_create()` and `layer().set_nav_group(g)`; `next()` = `on_nav_key(Next, true/false)` plus an `lv_group_focus_next(g)` when not consumed, then let animations finish).

```cpp
TEST_F(SettingsPageTest, RowsAreKeyFocusable)
// every row with an LV_EVENT_CLICKED handler on the page has kKeyFocusable; the group, after
// one rescope, holds them in screen order.
class SettingsKeyReach : public SettingsPageTest, public WithParamInterface<PreviewShape>
TEST_P(SettingsKeyReach, FirstAndLastRowsAreFullyVisibleWhenFocused)
// for rect 480, rounded_rect 480, circle 466: Next until the last focusable is focused ->
// box_visible(shape, its coords); one more Next wraps to the first -> box_visible too.
TEST_P(SettingsPreview, DISABLED_KeyFocus)
// WISP_SETTINGS_SCREENSHOTS: settings-<shape>-focus-first.png and -focus-last.png.
```

- [ ] **Step 2: Run** `ctest --test-dir build/sim -R 'SettingsPage|SettingsKeyReach' --output-on-failure`. Expected: FAIL.
- [ ] **Step 3: Implement** (the one-line call in `make_list_row()`, the `static_assert`; the focus callback only if the circle case fails).
- [ ] **Step 4: Run** the same filter, then the preview: `WISP_SETTINGS_SCREENSHOTS=/tmp/wisp-settings build/sim/tests/shell_settings_page_test --gtest_also_run_disabled_tests --gtest_filter='*SettingsPreview*'`. Check the focused rows on the three shapes: the fill covers the row within the card, the trailing value and chevron stay centred, nothing is clipped. Expected: PASS.
- [ ] **Step 5: Commit** `feat(shell): settings rows are reachable by keys`.

---

### Task 6: `NavKeys` in `wisp_app` and the App wiring

**Files:**
- Create: `components/wisp_app/src/nav_keys.hpp`, `components/wisp_app/src/nav_keys.cpp` (add to `wisp_app`'s sources)
- Modify: `components/wisp_app/src/app_impl.hpp` (member `std::unique_ptr<NavKeys> nav_keys;` and the Lifetime note), `components/wisp_app/src/app.cpp` (step 4 `:382-412`; `App::stop()` before the shell/duties block `:995` and before `system_layer.reset()` `:1010`)
- Modify: `components/wisp_hal/include/wisp/hal/roles.hpp` (table line: `nav_next` and `nav_ok` move focus, `nav_prev` and `nav_back` optional)
- Test: `tests/app/nav_keys_test.cpp` (new; `wisp_test(NAME nav_keys_test SRCS app/nav_keys_test.cpp LIBS wisp_app wisp_port_sim_os ...)`, include dir `components/wisp_app/src` like `app_boot_test`), `tests/app/app_boot_test.cpp`

**Interfaces:**
- Consumes: `ButtonService::add_listener()/remove_listener()/emit_for_test()`, `ButtonEdge`, `DeviceRegistry::by_role<hal::Button>()`, `hal::role::kNav*`, `UiRuntime::push_key()/set_nav_key_hook()/nav_group()`, `SystemLayer::set_nav_group()/on_nav_key()`.
- Produces in `nav_keys.hpp` (namespace `wisp::app`):
  - `bool has_nav_keys(const core::DeviceRegistry& registry);` — any button holds a nav role (sets `UiConfig::keypad`).
  - `class NavKeys { public: NavKeys(core::DeviceRegistry&, core::ButtonService&, ui::UiRuntime&); ~NavKeys(); };` — the constructor resolves the four roles to ids once (`FixedString<32>`), logs `WLOG_W("app", "nav keys without nav_next and nav_ok: focus cannot move")` when either is missing and `"<id> holds a nav role and <power|talk>"` for such a key, and adds a listener that, for an edge of one of those ids, calls `push_key(key, kind == Down)` for `Down`/`Up` and ignores `Short`/`Long`. The destructor removes the listener.
- App: `ui_config.keypad = has_nav_keys(registry)`. After the `SystemLayer` is built: on the UI thread `system_layer->set_nav_group(ui->nav_group())`, then `ui->set_nav_key_hook([layer](NavKey k, bool p) { return layer->on_nav_key(k, p); })`, then `nav_keys = make_unique<NavKeys>(...)` when `ui_config.keypad`. `stop()`: `nav_keys.reset()` first in the UI teardown, then `ui->set_nav_key_hook({})` and, on the UI thread, `set_nav_group(nullptr)` before `system_layer.reset()`.

- [ ] **Step 1: Write the failing tests.**

```cpp
// tests/app/nav_keys_test.cpp: a registry with FakeButtons "button.a" (nav_next),
// "button.b" (nav_ok), "button.c" (no role); a started ButtonService (or emit_for_test only);
// a UiRuntime with keypad and a hook recording entries.
TEST(NavKeys, DownAndUpBecomeKeys)
// emit_for_test("button.a", Down), (..., Up) -> hook records {Next,1},{Next,0}.
TEST(NavKeys, ShortLongAndOtherButtonsAreIgnored)
// Short/Long of button.a and Down/Up of button.c -> nothing recorded.
TEST(NavKeys, HasNavKeys)
// registry without nav roles -> false; with button.a -> true.
TEST(NavKeys, WarnsWithoutNextOrOk)
// only nav_back: the log ring holds "nav keys without nav_next and nav_ok: focus cannot move".
TEST(NavKeys, WarnsWhenANavKeyTalks)
// role list ["talk","nav_ok"] -> "button.x holds a nav role and talk".
TEST(NavKeys, StopsListeningWhenDestroyed)
// destroy NavKeys; ButtonService's destructor reports no listener left (no abort).

// tests/app/app_boot_test.cpp: MakeAppOptions gains `bool nav_keys = false;`, FakeBoard then
// registers "button.nav_next" (nav_next), "button.nav_ok" (nav_ok), "button.nav_back"
// (nav_back). A helper press(id) emits Down then Up through h.app.buttons().emit_for_test().
TEST(AppBoot, PowerOffDialogIsAnsweredByKeys)
// nav_keys, Platform::request_power_off counts calls. Long of the power key -> confirm_open();
// press("button.nav_ok") -> dialog closed, power-off count 0 (keys-only board: the safe answer
// is focused and shown from the start). Long again; press nav_next; press nav_ok -> count 1.
TEST(AppBoot, BackKeyClosesTheDiagnostics)
// open the diagnostics through the shell on the UI thread; press("button.nav_back") ->
// diagnostics_open() false.
TEST(AppBoot, AWakingPressDoesNotAnswerTheDialog)
// open the power-off dialog; press nav_next (shows the focus on No), press nav_next (Yes
// focused); put the power manager into standby as the existing standby tests do; press
// nav_ok -> the screen wakes (level Active) and power-off count stays 0; if the dialog
// survived standby, it is still open and Yes is still focused.
TEST(AppBoot, NoKeypadWithoutNavRoles)
// default board: ui()->nav_group() == nullptr (read on the UI thread).
```

- [ ] **Step 2: Run** `ctest --test-dir build/sim -R 'NavKeys|AppBoot' --output-on-failure`. Expected: compile failure, then FAIL.
- [ ] **Step 3: Implement.** The `app_impl.hpp` Lifetime note says: `nav_keys` goes before the shell and the system layer (its listener pushes into the UI; the hook reaches the layer).
- [ ] **Step 4: Run** `ctest --test-dir build/sim --output-on-failure -R 'NavKeys|AppBoot|PowerManager|SystemDuties|TalkKey'`. Expected: PASS.
- [ ] **Step 5: Commit** `feat(app): nav-role buttons drive the system UI`.

---

### Task 7: Simulator nav keys

**Files:**
- Modify: `components/wisp_port_sim/include/wisp/port/sim/sim_input.hpp` (`SimButtonKind` `:44`, the button channel array, a public `key_nav()`), `components/wisp_port_sim/dev/sim_button.cpp` (`pressed()`), `components/wisp_port_sim/include/wisp/port/sim/sim_board.hpp` (`SimBoardConfig`), `components/wisp_port_sim/sim_board.cpp` (`:110-121`)
- Modify: `simulator/sim_options.hpp`, `simulator/sim_options.cpp` (`--nav-keys`), `simulator/main.cpp` (`to_board_config()`, `handle_event()` `:603-660`, the usage text and the file comment's key list)
- Test: `tests/sim/sim_board_test.cpp`, `tests/sim/sim_options_test.cpp`

**Interfaces:**
- Produces: `enum class SimButtonKind { Power, VolUp, VolDown, NavPrev, NavNext, NavOk, NavBack };`; `void SimInput::key_nav(SimButtonKind kind, bool down)` (and the `at_us` overload, like `key_power`); `bool SimInput::button_pressed(SimButtonKind)` becomes public if `SimButton::pressed()` needs it.
- `SimBoardConfig::nav_keys` (default false). With it, `SimBoard::init()` registers `button.nav_prev`, `button.nav_next`, `button.nav_ok`, `button.nav_back` (model `"sdl-key"`, `EdgeEvent`, roles `nav_prev`/`nav_next`/`nav_ok`/`nav_back`). Without it the device list is unchanged (the smoke test's expected list stays as is).
- `--nav-keys` sets it. SDL keys (press and release, `repeat == 0` for presses): Page Up → `NavPrev`, Page Down and Tab → `NavNext`, Return and keypad Enter → `NavOk`, Escape and Backspace → `NavBack`. The arrow keys keep the tilt.

- [ ] **Step 1: Write the failing tests.**

```cpp
TEST(SimOptions, NavKeys)
// parse {"--nav-keys"} -> opts.nav_keys; default false.
TEST(SimBoard, NavKeysAreButtonsWithNavRoles)
// config.nav_keys: by_role<hal::Button>("nav_next")->info().id == "button.nav_next" (and the
// other three); input->key_nav(NavOk, true) -> that button's pressed() true; without the flag
// no button has a nav role and list<hal::Button>().size() is today's 3.
```

- [ ] **Step 2: Run** `ctest --test-dir build/sim -R 'SimOptions|SimBoard' --output-on-failure`. Expected: FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** the same filter, then by hand: `build/sim/simulator/wisp_sim --nav-keys --shape circle`; open Settings with a drag, press Page Down a few times (the ring shows on the first press, then moves; the list scrolls), Escape (back to Home); open the power-off dialog with a long `p`, Return (dialog closes, "No"). Repeat with `--shape rect`. Note what was seen in the commit body. Expected: as described.
- [ ] **Step 5: Commit** `feat(sim): nav keys on the keyboard with --nav-keys`.

---

### Task 8: Documents, full check and firmware size

**Files:**
- Modify: `docs/superpowers/specs/2026-11-14-device-model-design.md`: "In the code" notes under 9.1 (deviations 1-8) and 9.2 (deviation 9), and under 14.4's H3 line (deviation 10).
- Modify: `docs/architecture.md`: "Button input and standby" gains a paragraph on nav keys (`NavKeys` listener → `push_key()`, the ring rules, refused while paused); "`wisp_shell` (system shell)" a line on `Shell::back()`; "Machine-readable output": the `display` note (value `"unsupported_pixel_format"`) and caps `pixel_format`.
- Modify: `docs/porting-guide.md`: the `Display` row of the capability table (`pixel_format`, optional, only `"rgb565"`; it is the buffer format `flush()` receives, so a panel with another native format such as RGB888 declares `rgb565` and converts in its driver's `flush()`, costing a conversion buffer and more bus bandwidth; e-paper and monochrome panels need roadmap phase N); a short "Navigation keys" paragraph after the button roles in "Step 3": give `nav_next` and `nav_ok` (and optionally `nav_prev`, `nav_back`) to keys no other role uses; what keys can do in this version (system dialogs, back, settings rows; no Home-to-page navigation, no on-screen keyboard).
- Modify: `docs/testing.md`: the `WISP_KEYNAV_SCREENSHOTS` preview command beside the settings preview.
- Modify: `CHANGELOG.md`: add `## Unreleased` at the top if absent, with Features: "Boards with navigation keys (roles `nav_prev`/`nav_next`/`nav_ok`/`nav_back`) answer system dialogs, go back and walk the settings list without touch; the simulator has them with `--nav-keys`"; and "A display whose buffer format the UI cannot render yet (anything but RGB565) is left dark and the boot report notes `display`; the rest of the device keeps working."
- Modify: `docs/backlog.md`: keys from Home to the sub-pages and on the history and scene list pages (phase M); the on-screen keyboard and sliders by key; the 2.16 device check below once run.
- Modify: `AGENTS.md` section 3 "UI layout": "System UI stays usable with navigation keys: a clickable control that is not an LVGL widget class (a list row) is marked with `ui::make_key_focusable()`, and modal pieces are opened through `ui::SystemLayer`, which scopes key focus to them." (own commit `docs(agents): system UI stays reachable by keys`).

- [ ] **Step 1:** Edit the documents above.
- [ ] **Step 2:** Run `bash tools/ci.sh`. Expected: exit 0, no warnings in the test output.
- [ ] **Step 3:** Build both boards (ESP-IDF 6.0.3), for B in `waveshare_esp32_s3_touch_amoled_2_16` and `waveshare_esp32_s3_touch_amoled_1_75c`:

```bash
cd firmware && bash -c ". ~/esp/esp-idf-v6.0.3/export.sh >/dev/null 2>&1 && idf.py bmgr -c ../boards -b $B && idf.py -B build-$B -D SDKCONFIG=build-$B/sdkconfig -D SDKCONFIG_DEFAULTS='sdkconfig.defaults;components/gen_bmgr_codes/board_manager.defaults' -DWISP_BOARD=$B build"
```

Expected: both succeed; the "Smallest app partition" line of the 2.16 shows at most 80 % used (record the percentage and the binary-size delta against `main` in the commit body). `firmware/dependencies.lock` unchanged.
- [ ] **Step 4: Commit** `docs: keypad input and the pixel-format cap`, then the AGENTS.md commit.

**Device checks for the owner (not run by the agent; never reported as verified):**

Build and flash as usual (`idf.py app-flash`; no glyph or font change, so no `fonts-flash`). On both boards, with the committed firmware:
1. Boot report: no `display` note; manifest unchanged (no nav roles on either board).
2. Touch, power key, talk key and dialogs behave as before (no focus ring appears when no key has a nav role).
3. `mem` idle against `ram_budget.hpp`: unchanged (no new task; the ring is 16 bytes).

On the 2.16 only, with a local, uncommitted patch to `boards/waveshare_esp32_s3_touch_amoled_2_16/board.cpp` `kButtons` (after H2: the same roles on the two volume-key slots of its `BoardSpec`). Revert with `git checkout -- boards/waveshare_esp32_s3_touch_amoled_2_16/board.cpp` afterwards.

Configuration A (dialogs; talking by key is off during this test):

```diff
-        {kPinBtnVolUp, true, "button.vol_up", "gpio-button", ""},
+        {kPinBtnVolUp, true, "button.vol_up", "gpio-button", wisp::hal::role::kNavNext},
-        {kPinBtnVolDown, true, "button.vol_down", "gpio-button", wisp::hal::role::kTalk},
+        {kPinBtnVolDown, true, "button.vol_down", "gpio-button", wisp::hal::role::kNavOk},
```

4. Boot log: no "focus cannot move" warning; manifest shows the two roles.
5. Long-press the power key: the power-off dialog opens. Vol+ once: a ring appears on "No" (touch board: first key only shows it). Vol- : the dialog closes, the device stays on.
6. Long-press power again; Vol+ twice (ring on "Yes"); Vol- : the device powers off.
7. Settings → System → Factory reset (by touch): the dialog opens; Vol+ then Vol- answers "No"; the reset is cancelled (web page or log shows `cancelled`).
8. Open Settings by touch; Vol+ repeatedly: the ring walks the rows and the list scrolls; the last row is fully visible; one more press wraps to the first row, fully visible. Vol- on "Language" opens the choice page.
9. Screen off with a short power press; press Vol- : the screen wakes and nothing is clicked.

Configuration B (back):

```diff
-        {kPinBtnVolUp, true, "button.vol_up", "gpio-button", ""},
+        {kPinBtnVolUp, true, "button.vol_up", "gpio-button", wisp::hal::role::kNavBack},
```

10. Boot log shows "nav keys without nav_next and nav_ok: focus cannot move".
11. Open Settings, then a choice page: Vol+ returns to Settings, Vol+ again to Home; Vol+ on Home does nothing.
12. Open a scene: Vol+ sends it back, as the back swipe does. Open Hardware diagnostics: Vol+ closes them. Pull the panel down: Vol+ closes it.
13. Hold Vol- to talk: voice still works in this configuration.
