# Voice and Wake Responsiveness Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** Speech keeps its pace while the model is still writing, the first words after a press are not lost, and a device that just woke from deep standby records the user's first sentence instead of refusing it.

**Architecture:** Part 1 changes only `wisp_voice` (and one dependency line in `wisp_app`): a "say closed" gate for speech, a primed microphone with a pre-roll, and a bounded wait for the network before recognition. Part 2 connects those to the shell (`ConversationController`, `Home`, `TalkKey`) and replaces the two-value readiness with a checklist; it touches files that the agent-turn-reliability plan also changes, so it starts only after that branch is merged into this one.

**Tech Stack:** C++20 (`-fno-exceptions -fno-rtti`), GoogleTest, the simulator build (`build/sim`).

**Spec:** `docs/superpowers/plans/2026-11-14-architecture-optimization-roadmap.md` phase B (on branch `feat/agent-turn-reliability` until merged), amending `docs/superpowers/specs/2026-11-06-mvp-design.md` sections 4.1, 4.4, 4.5 and 6.6. Task 8 records the amendments.

## Global Constraints

- Branch `feat/voice-wake-responsiveness`. Part 1 (Tasks 1-4) is based on `main`. Part 2 (Tasks 5-8) starts after `feat/agent-turn-reliability` has been merged into this branch.
- AGENTS.md applies in full. `wisp.voice` never touches flash; buffers beyond a few KB are `Mem::Large`; strings through `ui::StrId` in zh/ja/en.
- At most two model connections at any time in a voice turn (one LLM stream, one TTS request): `ConversationTest.AtMostTwoModelConnectionsInAVoiceTurn` and the `TlsGate` limits stay as they are.
- `TonePlayer` and `PcmPlayer` never hold the `AudioOut` at once (existing rule in `voice_service.hpp`).
- Payloads of `voice.state` and the log never carry audio, the transcript or the speech text.
- Each task ends green on its ctest filter; the last task of each part runs `bash tools/ci.sh`.
- Device checks are listed for the owner and never reported as done.

## Review Focus

1. A press shorter than `kHoldStartMs` (a tap, or the press that only wakes the screen): the primed microphone is closed again and nothing is recorded, published or locked. Test in Task 2.
2. A press while speech is playing: priming is skipped (the output is in use), the hold still works through the unprimed path. Test in Task 2.
3. `stop_speaking()` or a new turn after `say_closed()`: the flag does not leak into the next turn's speech. Test in Task 1.
4. The network never comes back within the wait: the round ends with `offline`, its wake locks released, and a cancel during the wait ends it at once. Test in Task 3.
5. A device with Wi-Fi turned off by the user, or with no saved network: a hold is refused as today (no recording), with the "no Wi-Fi" hint, not "connecting". Tests in Tasks 3 and 6.

---

## Part 1: voice layer (independent of the agent-turn-reliability branch)

### Task 1: Speech continues once the `<say>` block has closed

**Files:**
- Modify: `components/wisp_voice/include/wisp/voice/say_parser.hpp`, `components/wisp_voice/src/say_parser.cpp`
- Modify: `components/wisp_voice/include/wisp/voice/voice_service.hpp`, `components/wisp_voice/src/voice_service.cpp` (`Speech`, `begin_speech`, `pump_speech` near line 598)
- Test: `tests/voice/say_parser_test.cpp`, `tests/voice/voice_service_test.cpp`

**Interfaces:**
- Produces: `SayParser::Out` gains `std::function<void()> say_end;` called once each time a `</say>` closes an open say (not for a stray `</say>`, not from `finish()`). Empty: ignored.
- Produces: `void VoiceService::say_closed();` any thread; sets `Speech::say_closed` for the open speech and wakes the task; a no-op when no speech is open. `begin_speech()` clears it.
- Rule in `pump_speech()`: `may = sent_first ? (!llm_busy_ || speech_.say_closed) : (config_.overlap_first_sentence || !llm_busy_)`.
- Update the "Speech" paragraph of `voice_service.hpp`'s header: after `say_closed()` the rest of the queue is synthesized while the model streams; still one TTS request at a time.

- [ ] **Step 1: Write the failing tests.**

```cpp
// say_parser_test.cpp
TEST(SayParser, SayEndFiresWhenTheTagCloses) {
    int ends = 0;
    std::string say;
    SayParser p({.say = [&](std::string_view s) { say += s; },
                 .body = [](std::string_view) {},
                 .say_end = [&] { ++ends; }});
    p.feed("<say>你好。再见。</sa");
    EXPECT_EQ(ends, 0);
    p.feed("y>正文");
    EXPECT_EQ(ends, 1);
    EXPECT_EQ(say, "你好。再见。");
}
TEST(SayParser, StrayCloseAndFinishDoNotFireSayEnd)
// feed("</say>text"), then a new parser with feed("<say>open") + finish(): ends == 0 in both.

// voice_service_test.cpp (fixture's fake TTS records request order and times)
TEST_F(VoiceServiceTest, SecondSentenceWaitsForTheLlmWithoutSayClosed)
// set_llm_busy(true); begin_speech(); speak("一。二。"); -> exactly 1 TTS request
// after the first sentence has played; set_llm_busy(false) -> the second follows.
TEST_F(VoiceServiceTest, SayClosedLetsLaterSentencesRunWhileTheLlmIsBusy)
// set_llm_busy(true); begin_speech(); speak("一。二。三。"); say_closed();
// -> 3 TTS requests in order while llm stays busy; never 2 TTS requests in flight at once.
TEST_F(VoiceServiceTest, SayClosedDoesNotLeakIntoTheNextSpeech)
// first speech: say_closed(), end_speech(), played out. Second: set_llm_busy(true);
// begin_speech(); speak("甲。乙。") -> only 1 TTS request until set_llm_busy(false).
TEST_F(VoiceServiceTest, SayClosedWithoutSpeechIsANoOp)
```

Write the commented tests with the fixture's existing helpers and exactly the stated assertions.

- [ ] **Step 2: Run** `ctest --test-dir build/sim -R 'SayParser|VoiceServiceTest' --output-on-failure`. Expected: new tests FAIL.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** the same filter. Expected: PASS, including `FirstSentenceMayOverlapTheLlm` and `NoOverlapWhenConfiguredSerial` unchanged.
- [ ] **Step 5: Commit** `feat(voice): let speech continue once the say block has closed`.

---

### Task 2: Primed microphone and pre-roll

**Files:**
- Modify: `components/wisp_voice/include/wisp/voice/limits.hpp`
- Modify: `components/wisp_voice/include/wisp/voice/capture_buffer.hpp`, `components/wisp_voice/src/capture_buffer.cpp`
- Modify: `components/wisp_voice/include/wisp/voice/voice_service.hpp`, `components/wisp_voice/src/voice_service.cpp` (`run`, `capture`)
- Test: `tests/voice/capture_buffer_test.cpp`, `tests/voice/voice_service_test.cpp`

**Interfaces:**
- Produces in `limits.hpp`: `inline constexpr uint32_t kPrerollMs = 500;` `inline constexpr uint32_t kPrimeMaxMs = 2000;`
- Produces: `void CaptureBuffer::keep_tail(uint32_t ms);` keeps the last `ms` of what was committed (moved to the front), drops the rest; a no-op when less is held.
- Produces in `VoiceConfig`: `uint32_t preroll_ms = kPrerollMs;` (0 turns priming off: `prime()` does nothing).
- Produces: `void VoiceService::prime();` and `void VoiceService::unprime();` any thread, never block.

Behaviour:
- `prime()` queues `Cmd::Prime`. The task primes only when: started, not capturing, an asr profile is active, a mic exists, and the output is idle (`!deps_.player.active()` and no tone playing). Otherwise it does nothing (the unprimed path below still works). Priming opens the mic, drops `kMicSettleMs`, then commits frames into the `CaptureBuffer`, calling `keep_tail(preroll_ms)` whenever more than `2 * preroll_ms` is held. It takes no wake lock and publishes nothing.
- Priming ends when: `unprime()` (mic closed, buffer cleared); `kPrimeMaxMs` passes (same); or `BeginCapture` arrives.
- `BeginCapture` on a primed mic (Hold mode): `keep_tail(preroll_ms)`, publish `listening`, play the start tone without waiting for it and without closing the mic, and continue the normal capture loop in the same buffer. The tone is inside the recording by design; `preroll_ms = 0` restores the old order for a board where that hurts recognition.
- `BeginCapture` unprimed: as today, except that `listening` is published after the mic has opened and settled (so the screen says "listening" only when recording has begun). Errors before that point are still reported as errors.
- The capture log line gains `preroll_ms=<n>` (0 unprimed).
- `kMinSpeechMs` applies to the recording after the pre-roll: a hold whose own part is shorter is "not heard", whatever the pre-roll holds.

- [ ] **Step 1: Write the failing tests.**

```cpp
// capture_buffer_test.cpp
TEST(CaptureBuffer, KeepTailKeepsTheLastSamples) {
    auto b = CaptureBuffer::create(2000).value();
    // commit 1000 ms of samples valued by index
    fill_ramp(b, 16000);
    b.keep_tail(500);
    EXPECT_EQ(b.ms(), 500u);
    EXPECT_EQ(first_sample(b), 8000);  // helper: reads the PCM region's first sample
}
TEST(CaptureBuffer, KeepTailOfMoreThanHeldIsANoOp)

// voice_service_test.cpp (the fixture's fake mic counts opens/closes and serves a ramp)
TEST_F(VoiceServiceTest, PrimedHoldKeepsThePreroll)
// prime(); let 300 ms of frames pass; begin_capture(Hold); 400 ms; end_capture().
// -> mic opened once; ASR got ~700 ms (300 pre-roll + 400), minus nothing for settle
//    (settle was dropped at prime); the upload's first sample is the first frame read after settle.
TEST_F(VoiceServiceTest, PrerollIsCappedAtPrerollMs)
// prime(); 1200 ms; begin_capture(Hold); 400 ms; end -> ASR got 500 + 400 ms (+-1 frame).
TEST_F(VoiceServiceTest, UnprimeClosesTheMicAndRecordsNothing)
// prime(); 100 ms; unprime() -> mic closed, no voice.state event, no wake lock, ASR not called.
TEST_F(VoiceServiceTest, PrimeTimesOutByItself)
// prime(); advance kPrimeMaxMs + 100 -> mic closed.
TEST_F(VoiceServiceTest, PrimeIsSkippedWhileSpeechPlays)
// speech playing; prime() -> mic not opened; begin_capture(Hold) still records (unprimed path).
TEST_F(VoiceServiceTest, PrimedHoldPlaysTheToneWithoutClosingTheMic)
// tone started after listening; mic open count stays 1.
TEST_F(VoiceServiceTest, ShortHoldAfterPrimeIsNotHeard)
// prime(); 400 ms; begin_capture(Hold); 100 ms; end -> idle result "not_heard", ASR not called.
TEST_F(VoiceServiceTest, ListeningIsPublishedAfterTheMicOpens)
// unprimed: the mic's open() happens before the "listening" event.
TEST_F(VoiceServiceTest, PrerollZeroDisablesPriming)
// VoiceConfig{.preroll_ms = 0}: prime() -> mic not opened.
```

Adapt `StartToneBeforeTheMic` and `MicSettleTimeIsDropped` only as far as the new event order requires; they keep pinning the unprimed path.

- [ ] **Step 2: Run** `ctest --test-dir build/sim -R 'CaptureBuffer|VoiceServiceTest' --output-on-failure`. Expected: new tests FAIL.
- [ ] **Step 3: Implement.** Keep one frame-reading helper for the settle loop, the priming loop and the capture loop (they repeat the same read-until-full code today). Update the "Capture" paragraph of `voice_service.hpp`'s header.
- [ ] **Step 4: Run** `ctest --test-dir build/sim -R 'Voice|CaptureBuffer|Conversation|TalkKey'`. Expected: PASS.
- [ ] **Step 5: Commit** `feat(voice): prime the microphone at press and keep a pre-roll`.

---

### Task 3: Recognition waits for a network that is on its way

**Files:**
- Modify: `components/wisp_voice/include/wisp/voice/limits.hpp`, `voice_service.hpp`, `components/wisp_voice/src/voice_service.cpp` (`capture`)
- Modify: `components/wisp_app/src/voice_wiring.hpp`, `voice_wiring.cpp`, and the place in `components/wisp_app/src/app.cpp` (near line 1206) that builds `network_up`
- Test: `tests/voice/voice_service_test.cpp`, `tests/app/voice_wiring_test.cpp` (or the existing app test that covers `VoiceWiring`)

**Interfaces:**
- Produces in `limits.hpp`: `inline constexpr uint32_t kNetWaitMs = 8000;`
- Produces in `VoiceDeps`: `std::function<bool()> network_expected;` true when Wi-Fi is enabled by the user and at least one network is saved (so a connection is being made or will be). Empty: false.
- Produces in `VoiceWiringDeps`: the same `network_expected`, filled in `app.cpp` from the in-memory Wi-Fi snapshot that `network_up` reads (no `WifiService::status()` call, no flash).

Behaviour in `capture()`:
- At the start: offline and not expected → `Offline` error as today. Offline but expected → record anyway.
- Before the ASR request: if offline, stay in `transcribing` and wait up to `kNetWaitMs` for `network_up()`, checking `capture_cancelled()` every 100 ms; still offline → `Offline`; cancelled → the round ends without an error.
- The round's wake locks (network included) are already held from `begin_capture()`, which is what brings Wi-Fi back from deep standby.

- [ ] **Step 1: Write the failing tests.**

```cpp
TEST_F(VoiceServiceTest, OfflineAndNotExpectedIsRefusedAtOnce)
// network_up=false, network_expected=false: error "offline", mic never opened.
TEST_F(VoiceServiceTest, RecordsWhileTheNetworkIsOnItsWay)
// up=false, expected=true: hold 500 ms, release; network comes up 2 s later
// -> ASR called once, transcript delivered; states listening, transcribing, idle.
TEST_F(VoiceServiceTest, GivesUpWhenTheNetworkNeverComes)
// up stays false: after kNetWaitMs the error is "offline", ASR never called, locks released.
TEST_F(VoiceServiceTest, CancelDuringTheNetworkWaitEndsTheRound)
// cancel_capture() 1 s into the wait -> idle within 200 ms, no error event, locks released.
```

- [ ] **Step 2: Run** `ctest --test-dir build/sim -R VoiceServiceTest --output-on-failure`. Expected: FAIL.
- [ ] **Step 3: Implement**, then the wiring and its test (`network_expected` false with Wi-Fi disabled or no saved network, true with a saved network while disconnected).
- [ ] **Step 4: Run** `ctest --test-dir build/sim -R 'Voice|App'`. Expected: PASS.
- [ ] **Step 5: Commit** `feat(voice): wait for a connecting network before recognition`.

---

### Task 4: Part 1 check

- [ ] **Step 1:** Run `bash tools/ci.sh`. Expected: exit 0, clean output.
- [ ] **Step 2:** Add an `Unreleased` section to `CHANGELOG.md` listing Tasks 1-3 (create the section if this branch does not have it yet).
- [ ] **Step 3: Commit** `docs(changelog): note the voice responsiveness changes`.

---

## Part 2: shell (after `feat/agent-turn-reliability` is merged into this branch)

Before Task 5: `git merge feat/agent-turn-reliability` (or rebase onto `main` once it has landed there), rebuild, run `ctest`.

### Task 5: The controller reports the closed say and primes at press

**Files:**
- Modify: `components/wisp_shell/include/wisp/shell/conversation.hpp`, `components/wisp_shell/src/conversation.cpp`
- Modify: `components/wisp_shell/src/talk_key.cpp`, `components/wisp_shell/include/wisp/shell/talk_key.hpp`
- Modify: `components/wisp_shell/src/gestures.cpp` and its caller in `home.cpp` (the wisp press)
- Test: `tests/shell/conversation_test.cpp`, `tests/shell/talk_key_test.cpp`, `tests/shell/gestures_test.cpp`

**Interfaces:**
- Consumes: `SayParser::Out::say_end`, `VoiceService::say_closed()`, `prime()`, `unprime()` (Part 1).
- Produces: `void ConversationController::press_begin();` (calls `voice.prime()` when `ready()` and no capture runs) and `void ConversationController::press_end();` (calls `voice.unprime()` unless a hold began). `hold_begin()` keeps its meaning.
- `TalkKey::on_down()` calls `press_begin()`; `on_up()` without a hold calls `press_end()`. The wisp press gesture does the same at touch-down and at a release or a move beyond the hold slop.
- `TalkKey::on_down()` no longer returns early when the screen is off: the press is tracked, and a hold of `kHoldStartMs` starts the capture (the button event already wakes the screen through `PowerManager`'s activity callback). A shorter press still only wakes the screen.
- The parser's `say_end` calls `deps_.voice.say_closed()` when `speaking_`.

- [ ] **Step 1: Write the failing tests.**

```cpp
TEST_F(ConversationTest, ClosedSayIsReportedToTheVoice)
// voice turn, deltas "<say>一。二。</say>正文": voice.say_closed called once, after the say text.
TEST_F(ConversationTest, MutedTurnDoesNotReportSayClosed)
TEST_F(ConversationTest, PressPrimesAndAShortPressUnprimes)
// press_begin() -> prime once; press_end() -> unprime once; no capture.
TEST_F(ConversationTest, PressThenHoldDoesNotUnprime)
// press_begin(), hold_begin(), hold_end(), press_end() -> unprime count 0.
TEST_F(ConversationTest, PressWhenNotReadyDoesNotPrime)
TEST_F(TalkKeyTest, HoldFromAScreenOffPressStartsTheCapture)
// screen_on()=false at down; hold kHoldStartMs -> hold_begin called.
TEST_F(TalkKeyTest, ShortPressWithTheScreenOffDoesNothingElse)
```

- [ ] **Step 2: Run** `ctest --test-dir build/sim -R 'ConversationTest|TalkKey|Gestures' --output-on-failure`. Expected: FAIL.
- [ ] **Step 3: Implement.** Update the header contract in `talk_key.hpp` ("the press that wakes the screen does only that" no longer holds for a held press).
- [ ] **Step 4: Run** `ctest --test-dir build/sim -R 'Shell|Conversation|TalkKey|Gestures|Voice'`. Expected: PASS.
- [ ] **Step 5: Commit** `feat(shell): prime the microphone at press and talk from a dark screen`.

---

### Task 6: Readiness becomes a checklist with a connecting state

**Files:**
- Modify: `components/wisp_shell/include/wisp/shell/conversation.hpp`, `components/wisp_shell/src/conversation.cpp`
- Modify: `components/wisp_shell/src/home.cpp` (`hint_text`), `components/wisp_shell/include/wisp/shell/home.hpp`
- Modify: `components/wisp_app/src/voice_wiring.cpp` (the `readiness` lambda)
- Modify: `components/wisp_ui/include/wisp/ui/strings.hpp`, `components/wisp_ui/src/strings_{zh,ja,en}.cpp`
- Test: `tests/shell/conversation_test.cpp`, `tests/shell/home_test.cpp`, the app test covering `VoiceWiring`

**Interfaces:**
- Produces:

```cpp
struct Readiness {
    enum class Net : uint8_t { Up, Connecting, Down };
    Net net = Net::Up;
    bool llm = true, asr = true, tts = true;
    bool can_talk() const { return net != Net::Down && llm && asr; }
    bool complete() const { return net == Net::Up && llm && asr && tts; }
    bool operator==(const Readiness&) const = default;
};
```

  replacing `enum class Readiness`. `to_string(Readiness)` is removed; its callers (logs, the console) print the fields.
- `Net::Connecting` = offline and `network_expected()`; `Net::Down` = offline otherwise. `tts` is true when a tts profile is active or `tts_mute` is on.
- `ConversationController::ready()` = `can_talk()`. `voice_text()` keeps refusing with `"no_wifi"` unless `net == Up`, and `"no_llm"`.
- New strings: `HomeConnecting` ("正在连接 Wi-Fi…" / "Wi-Fi に接続中…" / "Connecting to Wi-Fi…"); `HomeMissing` ("还差：%s" / "未設定：%s" / "Still needed: %s"); item names `HomeNeedLlm` ("对话模型" / "対話モデル" / "chat model"), `HomeNeedAsr` ("语音识别" / "音声認識" / "speech recognition"), `HomeNeedTts` ("语音合成" / "音声合成" / "speech"). `HomeNoLlm` is replaced by the list.
- Home hint rule: `Net::Down` → `HomeNoWifi` (as today); `Net::Connecting` → `HomeConnecting`; else if anything is missing → `HomeMissing` with the missing items joined by "、" (zh/ja) or ", " (en), plus the admin URL on its own line when idle; else the turn and voice hints as before.
- `ConversationController::tap()` stays without voice effect; Home shows the readiness hint for `kFadeAfterMs` on a wisp tap when `!complete()`, and `HomeHoldToTalk` (existing string used by the empty history page; reuse its id) when complete.

- [ ] **Step 1: Write the failing tests.**

```cpp
TEST_F(ConversationTest, HoldRecordsWhileConnecting)
// readiness {net Connecting}: hold_begin() starts a capture.
TEST_F(ConversationTest, HoldIsRefusedWhenDown)
TEST_F(ConversationTest, HoldIsRefusedWithoutAsr)
TEST_F(HomeTest, ConnectingHintReplacesNoWifi)
TEST_F(HomeTest, MissingProfilesAreListedWithTheAdminUrl)
// {llm ok, asr missing, tts missing}, lang zh, admin_url "http://192.168.1.5:8900"
// -> hint == "还差：语音识别、语音合成\nhttp://192.168.1.5:8900"
TEST_F(HomeTest, MutedDeviceDoesNotAskForTts)
TEST_F(HomeTest, TapShowsTheHintThenFades)
TEST_F(HomeTest, HintFitsTheSafeAreaOnAllShapes)
// longest case (all three missing, ja) on round 466, rounded 480 and rect: label inside safe_rect.
```

- [ ] **Step 2: Run** `ctest --test-dir build/sim -R 'ConversationTest|HomeTest|App' --output-on-failure`. Expected: FAIL to compile, then fail.
- [ ] **Step 3: Implement.** Run `python3 tools/check_i18n.py`.
- [ ] **Step 4: Run** `ctest --test-dir build/sim -R 'Shell|Conversation|Home|App|Strings'`. Expected: PASS.
- [ ] **Step 5: Commit** `feat(shell): show what is missing and record while Wi-Fi reconnects`.

---

### Task 7: Simulator walk-through

- [ ] **Step 1:** Run the simulator against `tools/fake_openai.py` with the scripted voice input the smoke tests use: a reply `<say>` of three sentences followed by a long body (speech does not pause after the first sentence); a hold started with `--wifi-fail` cleared two seconds later (the sentence is transcribed); a tap on the wisp with no asr profile (the checklist appears).
- [ ] **Step 2:** Record what was observed in the commit body of Task 8.

---

### Task 8: Documents and full check

**Files:**
- Modify: `docs/superpowers/specs/2026-11-06-mvp-design.md` (4.1 capture order, priming and pre-roll; 4.4/4.5 the say-closed rule; 6.6 readiness checklist, connecting, tap, talking from a dark screen)
- Modify: `docs/architecture.md` (`voice.state` notes if the event order is described; the `wisp.voice` task row)
- Modify: `docs/backlog.md` (remove "Home screen: without a ConversationController..." only if closed; add the device checks below), `CHANGELOG.md`

- [ ] **Step 1:** Edit the documents. **Step 2:** `bash tools/ci.sh`, exit 0. **Step 3: Commit** `docs: record the voice and wake responsiveness rules`.

**Device checks for the owner (not run by the agent):**
- Recognition accuracy with the start tone inside the recording, on both boards; if it suffers, set `VoiceConfig::preroll_ms = 0` for that board and report.
- Opening the microphone at press: no pop in the speaker, no I2S reconfiguration glitch; `mem` for `wisp.voice` stack high-water mark.
- A spoken multi-sentence reply during a long body: no pause after the first sentence; `mem` worst case with the LLM stream and a TTS request open together.
- Deep standby, then press and speak at once: the sentence is transcribed after Wi-Fi reconnects; time from release to transcript.
- Standby current with the changed `TalkKey` (the press tracked while the screen is off): unchanged.
