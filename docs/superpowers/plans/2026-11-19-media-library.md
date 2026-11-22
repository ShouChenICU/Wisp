# Media Library and Music Playback Implementation Plan

> **For agentic workers:** REQUIRED SUB-SKILL: Use superpowers:subagent-driven-development (recommended) or superpowers:executing-plans to implement this plan task-by-task. Steps use checkbox (`- [ ]`) syntax for tracking.

**Goal:** A public media library on the SD card (`/sd/media/{music,download,document}`) with a track index, and one system music player (queue, modes, MP3/WAV, continues with the screen off, gives way to alarms/speech/tones and resumes), reachable through one service API from scenes (`wisp.media`), the agent (`media.*`), the admin console (`/media/*`) and a "now playing" row in the pull-down panel.

**Architecture:** A new portable component `wisp_media` (between `wisp_voice` and `wisp_admin`) holds `MediaLibrary` (paths, index, file operations on `wisp.sd.io` through `StorageRouter`) and `MediaPlayer` (session cell, queue, task `wisp.media` that pulls file data through a prefetch ring and decodes through an injected `media::Decoder`). MP3 decoding is a platform service: `esp_audio_codec` in `wisp_port_esp`, vendored minimp3 in `wisp_port_sim`; WAV is portable. A new `core::AudioArbiter` takes over the speech/tone output rules now inside `VoiceService` and adds music as the lowest, preemptible kind with delayed auto-resume. A new wake lock kind `audio` keeps `AudioOut` devices powered in standby. Lua bindings, agent tools, REST routes, the web page and the panel row are thin adapters.

**Tech Stack:** C++20 (`-fno-exceptions -fno-rtti`), GoogleTest, the simulator (`build/sim`, SDL audio), ESP-IDF 6.0 with `espressif/esp_audio_codec` 2.6.2, minimp3 (CC0, vendored), Lua 5.5 bindings, preact/htm web app.

**Spec:** `docs/superpowers/specs/2026-11-19-media-library-design.md` (agreed section by section 2026-11-19; written spec approved by the owner 2026-11-19, with the amendment commit "no screen-off volume keys; confirm media where hosts are confirmed").

## Global Constraints

- Branch `feat/media-library` (worktree `.superpowers/worktrees/media-library`). No commits on `main`, no push, no merge, no release without asking the owner.
- AGENTS.md applies in full: `wisp::Result<T>`, no exceptions/RTTI/`dynamic_cast`, comments explain why only (no review ids, no change history, no `.superpowers/` paths), `tools/check_layering.py`, `tools/check_settings.py`, `tools/check_topics.py`, `tools/check_i18n.py`, `tools/check_deps.py`.
- Layer order becomes `wisp_base → wisp_os → wisp_hal → wisp_core → wisp_net → wisp_voice → wisp_media → wisp_admin → wisp_ui → wisp_scene → wisp_agent → wisp_shell → wisp_app`. `wisp_media` requires only `wisp_base wisp_os wisp_hal wisp_core` (not `wisp_net`/`wisp_voice`).
- Third-party: minimp3 lives unmodified in `third_party/minimp3/`; only `wisp_port_sim` and tests may include `minimp3.h`. `esp_audio_codec` is pinned `"2.6.2"` in `firmware/main/idf_component.yml`; only `wisp_port_esp` lists `espressif__esp_audio_codec`. Both registered in `THIRD_PARTY_NOTICES.md` and `docs/references.md` with their licences (CC0-1.0; Espressif modified MIT, Espressif chips only).
- Threads: only `wisp.sd.io` (or `wisp.fs` when there is no `wisp.sd.io`) touches card files; the player task `wisp.media` touches no file and no flash (`flash_access = false`, `hot_stack = false`, stack in PSRAM). Large buffers (`ring`, decode output, index) use `os::alloc(Mem::Large)` / `os::Large*` types.
- Paths in every interface are relative to `/sd/media`; the absolute root is a constructor parameter (`<sd root>/media`), never hard-coded in a test.
- Error wire names (spec section 8): `media_unavailable`, `not_found`, `invalid_path`, `permission_denied`, `unsupported_format`, `decode_error`, `busy`, `io_error`. REST status 503, 404, 400, 403, 415, 422, 503, 500.
- Numbers from the spec, verbatim: queue cap 1000; `prev` restart threshold 3 s; position published at most once per second; consecutive-failure stop at 3; ring 64 KiB with 16 KiB reads, refill at half empty; `read` cap 64 KiB per call; `tracks` `limit` ≤ 200; `media.read_text` ≤ 32 KiB; arbiter preemption grant 200 ms; auto-resume delay 800 ms; wake-lock linger after user pause/stop/end 10 s; holder `system.media`.
- Topics: `media.player`, `media.library`, `media.error`; payloads carry ids and states only, never file contents.
- User-facing strings localized zh/ja/en (device `StrId` tables and web `i18n/{en,zh,ja}.js`); new CJK glyphs go through the glyph subset tool.
- Every task ends green on `cmake --build build/sim && ctest --test-dir build/sim --output-on-failure -R '<task filter>'`, and `bash tools/ci.sh` is green before each commit. Test output stays free of warnings.
- Commits: Conventional Commits ending with `Co-Authored-By: Claude Opus 5.5 <noreply@anthropic.com>`. AGENTS.md changes are their own `docs(agents): ...` commit in the task that triggers them.
- Deletions only by literal absolute paths inside the worktree or the scratchpad; never bare `git stash`, never `git clean -fdx`; never add `.claude/`, `.playwright-mcp/` or `.superpowers/`.

## Review Focus

1. **Music never holds speech or an alarm back.** A preempt callback that does not return in time (player task blocked in `AudioOut::write()`) must not delay the higher kind past 200 ms, and the late player must not reopen the output afterwards. Test in Task 6 (`StuckMusicIsGrantedPastAfter200ms`) and Task 9 (`PreemptedWhileWriteBlocksNeverReopens`).
2. **A user pause is final.** Pause during a system pause, or a pause arriving between the arbiter's resume notification and the player's re-acquire, must never resume. Tests in Task 6 (`UserPauseDuringSystemPauseIsNotResumed`) and Task 9 (`PauseRacingTheResumeNotificationWins`).
3. **Card removal mid-read never hangs or crashes the player.** A pending ring refill whose job fails or never runs (router refused while `wisp.sd.io` drains) ends the item and stops playback, and `stop()` joins promptly. Test in Task 9 (`CardRemovedWithAReadInFlightStops`, `StopJoinsWithARefillRefused`).
4. **Hostile tags and headers.** ID3 frames with sizes past the buffer, unsynchronisation, zero-length frames, a WAV `data` chunk larger than the file, an MP3 of only garbage: parsers return errors or fallbacks, never read out of bounds. Tests in Task 3 (`Id3FrameSizePastTheEndIsIgnored`, `WavDataChunkLongerThanFileIsClamped`) and Task 4 (`GarbageIsNotAnMp3`).
5. **Wake locks never leak.** Every exit path (stop, end of queue, card removed, three failures, `MediaPlayer::stop()` at shutdown, scene uninstall emptying the queue) releases `cpu` and `audio` after at most the 10 s linger, and shutdown releases them at once. Test in Task 9 (`EveryExitReleasesTheLocks`).

## Rulings on spec details (recorded per AGENTS.md section 7)

- **R1 `decode_error`** has no `wisp::ErrorCode`. Task 1 adds `ErrorCode::Corrupt` (`"corrupt"`) to `wisp_base`; `media::wire_name(const Error&)` maps `Corrupt → "decode_error"`, `StorageUnavailable → "media_unavailable"`, `NotFound → "not_found"`, `InvalidArgument → "invalid_path"`, `PermissionDenied → "permission_denied"`, `Unsupported → "unsupported_format"`, `Busy → "busy"`, everything else → `"io_error"`.
- **R2 Decoder factory** reaches `App` as `Platform::audio_decoder` (`std::function<std::unique_ptr<media::Decoder>(media::Format)>`, empty = no MP3). `wisp_app` already depends on `wisp_media`, so the type is visible there; the spec's name `Platform::audio_decoders` is singular in code.
- **R3 Volume:** the player passes `audio/volume` to `AudioOut::set_volume()` on open and on each settings-cell change, as `SimAudioOut` scales in the device; `PcmPlayer` keeps its software scaling. The player reads the volume through a `std::function<uint8_t()>` given at construction (same lambda shape as `voice_wiring.cpp`).
- **R4 Media permission confirmation** only on the agent install/update path (`scene_workspace.cpp`), as amended in spec 7.2; the admin scene list shows `"media"`.
- **R5 Scene file playback on internal storage** reads through `SceneFiles::read()` (which routes by the scene's medium); library items read through `MediaLibrary`. Both are hidden behind one `media::ItemReader` (Task 9) so the player does not know which.

---

### Task 1: `wisp_media` component, layering, path rules and error names

**Files:**
- Create: `components/wisp_media/CMakeLists.txt`, `components/wisp_media/include/wisp/media/path.hpp`, `components/wisp_media/src/path.cpp`, `components/wisp_media/include/wisp/media/errors.hpp`, `components/wisp_media/src/errors.cpp`
- Modify: `components/wisp_base/include/wisp/base/error.hpp` and its `to_string` (add `Corrupt`), `tools/check_layering.py` (`COMPONENTS`, `LAYER_INCLUDE_PREFIX`), `tools/check_layering_test.py` if it lists the order, `simulator/CMakeLists.txt` (`add_subdirectory` after `wisp_voice`), `components/wisp_app/CMakeLists.txt` (`REQUIRES` gains `wisp_media`), `AGENTS.md` section 3 layer order, `docs/architecture.md` layer list
- Test: `tests/media/path_test.cpp`, `tests/media/errors_test.cpp`, `tests/CMakeLists.txt`

**Interfaces:**
- Produces:
  - `namespace wisp::media { Result<std::string> normalize(std::string_view rel); Result<std::string> normalize_for_write(std::string_view rel); bool is_track_name(std::string_view name); inline constexpr std::string_view kFolders[] = {"music", "download", "document"}; }`
  - `const char* wire_name(const Error& e);` `int http_status(const Error& e);` (R1 mapping; status per Global Constraints)
  - `ErrorCode::Corrupt` with `to_string` `"corrupt"`.

- [ ] **Step 1: Write failing tests** `MediaPath.AcceptsNestedUtf8` (`"music/周杰伦/晴天.mp3"` round-trips), `MediaPath.RejectsEachForbiddenForm` (table: `""`, `"/music/a"`, `"music\\a"`, `"music//a"`, `"music/./a"`, `"music/../a"`, `".index.json"`, `"music/.a.mp3"`, `"music/a\x01"`, 256-byte path — each `InvalidArgument`), `MediaPath.EmptyDirMeansRootForList` (`normalize("")` is rejected but `normalize_dir("")` → `""`; add `Result<std::string> normalize_dir(std::string_view)`), `MediaPath.WritesStayInsideTheThreeFolders` (`normalize_for_write("x.txt")`, `("music")` rejected; `("document/n.txt")` ok), `MediaPath.TrackExtensionIsCaseInsensitive` (`a.MP3`, `b.wav` yes; `c.flac`, `mp3` no), `MediaErrors.WireNamesAndStatus` (every R1 row and its status).
- [ ] **Step 2: Run** `ctest --test-dir build/sim -R 'MediaPath|MediaErrors'` — fails to build (no component).
- [ ] **Step 3: Implement** the component with `wisp_component(NAME wisp_media SRCS src/path.cpp src/errors.cpp INCLUDE_DIRS include REQUIRES wisp_base wisp_os wisp_hal wisp_core)`; update the layer order everywhere listed above.
- [ ] **Step 4: Run** the filter and `python3 tools/check_layering.py` (and its test) — pass.
- [ ] **Step 5: Commit** `feat(media): add wisp_media layer with path rules and error names`; then `docs(agents): add wisp_media to the layer order` (AGENTS.md alone).

### Task 2: Vendor minimp3 and generate test media

**Files:**
- Create: `tools/vendor_minimp3.sh` (pinned upstream commit + SHA-256 per file, like `tools/vendor_stb.sh`), `third_party/minimp3/CMakeLists.txt`, `third_party/minimp3/minimp3.c` (single `#define MINIMP3_IMPLEMENTATION` TU, like `stb_image_write.c`), `third_party/minimp3/minimp3-<short sha>/{minimp3.h,minimp3_ex.h,LICENSE}`, `tools/gen_media_test_data.sh`, `tests/data/media/` (generated files, committed)
- Modify: `tools/check_layering.py` (`THIRD_PARTY_HEADERS["minimp3.h"] = ("wisp_port_sim",)`, `THIRD_PARTY_REQUIRES["wisp_port_sim"]` gains `"minimp3"`; tests exempt as for Lua), `simulator/CMakeLists.txt` (`add_subdirectory(third_party/minimp3)`; not added to the firmware — `firmware/CMakeLists.txt` `EXTRA_COMPONENT_DIRS` includes `third_party`, so the component is kept out of the firmware with `EXCLUDE_COMPONENTS minimp3` in `firmware/CMakeLists.txt`), `THIRD_PARTY_NOTICES.md`, `docs/references.md`
- Test: `tests/media/test_data_test.cpp`

**Interfaces:**
- Produces: CMake target `minimp3` (host only); test files under `tests/data/media/` with a header `tests/support/media_data.hpp` exposing `std::string media_data(std::string_view name)` (absolute path via a compile definition, the way other tests find data):
  - `sine_44k_stereo_cbr128.mp3` (3 s, 440 Hz, Xing/Info header, ID3v2.4 UTF-8 tags title "Sine 440", artist "Wisp", album "Tests")
  - `sine_48k_mono_vbr.mp3` (2 s, Xing TOC, ID3v2.3 UTF-16 tags)
  - `sine_22k_noxing.mp3` (2 s, no Xing/VBRI, ID3v1 only)
  - `sine_44k_stereo.wav` (1 s, PCM16), `sine_16k_mono.wav` (1 s), `float32.wav` (1 s, IEEE float, for rejection)
  - `garbage.mp3` (4 KiB of a fixed pseudo-random pattern)
  Each generated with `ffmpeg`/`lame` (`gen_media_test_data.sh` records exact commands; files are committed so CI needs neither tool). Total under 200 KiB.

- [ ] **Step 1: Write failing test** `MediaTestData.FilesExistAndAreSmall` (each file present, total < 200 KiB).
- [ ] **Step 2: Run** — fails.
- [ ] **Step 3: Vendor** minimp3 with the script, add the CMake wrapper, generate the data, register licences.
- [ ] **Step 4: Run** the filter, `python3 tools/check_layering.py`, and an ESP configure (`idf.py reconfigure` in a scratch build dir) to prove `minimp3` is excluded from the firmware.
- [ ] **Step 5: Commit** `build(third_party): vendor minimp3 for the simulator and add media test data`.

### Task 3: Tag and header parsing (ID3, MP3 info, WAV)

**Files:**
- Create: `components/wisp_media/include/wisp/media/tags.hpp`, `src/id3.cpp`, `src/mp3_info.cpp`, `src/wav.cpp`
- Test: `tests/media/tags_test.cpp`

**Interfaces:**
- Produces (pure functions over bytes; no I/O):
  - `struct TrackTags { std::string title, artist, album; };`
  - `size_t id3v2_size(std::span<const uint8_t> head);` (bytes to skip, 0 if none; `head` ≥ 10 bytes)
  - `TrackTags parse_id3v2(std::span<const uint8_t> tag);` `TrackTags parse_id3v1(std::span<const uint8_t> last128);`
  - `struct Mp3Info { uint32_t sample_rate; uint8_t channels; uint32_t duration_ms; bool has_toc; std::array<uint8_t,100> toc; uint64_t audio_offset, audio_bytes; };`
  - `Result<Mp3Info> parse_mp3_info(std::span<const uint8_t> after_id3, uint64_t file_size, uint64_t audio_offset);` (first frame header, Xing/Info/VBRI, else CBR estimate)
  - `struct WavInfo { uint32_t sample_rate; uint8_t channels; uint64_t data_offset, data_bytes; uint32_t duration_ms; };`
  - `Result<WavInfo> parse_wav(std::span<const uint8_t> head, uint64_t file_size);` (PCM16 only; others `Unsupported`)
  - `std::string title_from_file_name(std::string_view path);`

- [ ] **Step 1: Write failing tests** against `tests/data/media/` bytes and hand-built buffers: `Id3v2Utf8Tags` (sine_44k: "Sine 440"/"Wisp"/"Tests"), `Id3v2Utf16Tags` (sine_48k), `Id3v1Fallback` (sine_22k), `Latin1ThatIsUtf8IsReadAsUtf8` (frame encoding 0 with bytes of "晴天" → "晴天"), `Latin1NonUtf8FieldIsDropped` (encoding 0, GBK bytes `C7 E7 CC EC` → empty), `Id3FrameSizePastTheEndIsIgnored` (no crash, fields parsed before it kept), `Id3UnsynchronisedTagIsRead`, `XingDuration` (sine_44k within ±50 ms of 3000), `VbrTocIsKept` (sine_48k `has_toc`), `CbrEstimateWithoutXing` (sine_22k within ±150 ms of 2000), `WavHeader` (both wavs: rate/channels/duration), `WavFloatIsUnsupported`, `WavDataChunkLongerThanFileIsClamped`, `TitleFromFileName` (`"music/a/晴天.mp3"` → `"晴天"`).
- [ ] **Step 2: Run** `ctest -R MediaTags` — fails.
- [ ] **Step 3: Implement**; every length read is checked against the span.
- [ ] **Step 4: Run** — pass.
- [ ] **Step 5: Commit** `feat(media): parse ID3 tags, MP3 info and WAV headers`.

### Task 4: Decoder interface, probe, WAV decoder, minimp3 decoder

**Files:**
- Create: `components/wisp_media/include/wisp/media/decoder.hpp`, `src/decoder.cpp` (probe), `src/wav_decoder.cpp`; `components/wisp_port_sim/include/wisp/port/sim/minimp3_decoder.hpp`, `components/wisp_port_sim/src/minimp3_decoder.cpp`
- Modify: `components/wisp_port_sim/CMakeLists.txt` (`REQUIRES` gains `wisp_media`; links `minimp3`)
- Test: `tests/media/decoder_test.cpp` (WAV, probe, a fake `Source`), `tests/sim/minimp3_decoder_test.cpp`

**Interfaces:**
- Produces: exactly the spec 4.3 block (`StreamInfo`, `Source`, `Decoder`, `Format`, `DecoderFactory`), plus:
  - `Result<Format> probe(std::span<const uint8_t> head);` (`Unsupported` otherwise)
  - `std::unique_ptr<Decoder> make_wav_decoder();`
  - `class MemorySource : public Source` (in `decoder.hpp`, used by tests and by the player for nothing else)
  - `DecoderFactory with_wav(DecoderFactory platform);` — returns a factory that serves `Format::Wav` itself and forwards `Mp3` to `platform` (nullptr when empty)
  - `std::unique_ptr<media::Decoder> wisp::port::sim::make_minimp3_decoder();`
- `decode()` output is interleaved int16 with `StreamInfo::channels` channels; `max_frames` counts frames. `seek_ms` for MP3 uses the Xing TOC when present, else byte estimate, then resyncs on the next frame header (minimp3 does the sync).

- [ ] **Step 1: Write failing tests:** `Probe.DetectsEachFormat` (wav, ID3-prefixed mp3, bare-sync mp3), `Probe.GarbageIsNotAnMp3`, `WavDecoder.DecodesAllFrames` (44.1k stereo: 44100 frames ± 0), `WavDecoder.SeekLandsOnTheFrame`, `WithWav.ForwardsMp3OnlyWhenPlatformHasOne`; `Minimp3Decoder.DecodesSineAtRate` (44100 Hz, 2 ch, ≈3 s of frames, peak amplitude > 8000), `Minimp3Decoder.MonoVbr48k`, `Minimp3Decoder.SeekByTocAndByEstimate` (seek to 1000 ms then decoded remainder ≈ 2000/1000 ms ± 100), `Minimp3Decoder.CorruptMiddleSkipsToNextFrame` (bytes 2000..2400 zeroed: decode continues, no error), `Minimp3Decoder.GarbageIsCorrupt` (`open` → `ErrorCode::Corrupt`).
- [ ] **Step 2: Run** `ctest -R 'Probe|WavDecoder|WithWav|Minimp3Decoder'` — fails.
- [ ] **Step 3: Implement.** minimp3: feed through `mp3dec_decode_frame` from a 16 KiB input window refilled from `Source::read`.
- [ ] **Step 4: Run** — pass.
- [ ] **Step 5: Commit** `feat(media): decoder interface with WAV and the simulator's minimp3 decoder`.

### Task 5: MediaLibrary — index, scan, file operations, state cell

**Files:**
- Create: `components/wisp_media/include/wisp/media/library.hpp`, `src/library.cpp`, `src/index_store.cpp` (JSON (de)serialization of the index, format version 1, ArduinoJson as elsewhere in `wisp_core`)
- Modify: `components/wisp_media/CMakeLists.txt` (`HOST_LIBS`/`IDF_REQUIRES` for ArduinoJson as `wisp_core` does)
- Test: `tests/media/library_test.cpp`

**Interfaces:**
- Consumes: `core::StorageManager` (`write_atomic`, `read_file`, `read_file_large`, `has_sd()`, `sd_cell()`), `core::StorageRouter` (`submit`, `run_sync`, `IoLane`, `Medium::Sd`), `core::StateCell`, `core::EventBus`.
- Produces:
  - `struct MediaEntry { std::string name; bool dir; uint64_t size; int64_t mtime; };`
  - `struct Track { std::string id, title, artist, album; uint32_t duration_s; uint64_t size; int64_t mtime; };`
  - `struct TrackQuery { std::string q, artist, album; uint32_t offset = 0, limit = 50; };` `struct TrackPage { std::vector<Track> items; uint32_t total; };`
  - `struct MediaLibraryState { bool available = false; bool scanning = false; uint32_t track_count = 0; uint32_t revision = 0; };`
  - `struct MediaLibraryDeps { core::StorageManager& storage; core::StorageRouter* router; core::EventBus* bus; std::string root; /* "<sd root>/media" */ };`
  - `class MediaLibrary { explicit MediaLibrary(MediaLibraryDeps); void start(); void stop(); const core::StateCell<MediaLibraryState>& state() const;`
    - async (completion on `wisp.sd.io`; callers post onward): `void list(std::string dir, std::function<void(Result<std::vector<MediaEntry>>)>); void stat(...); void read(std::string path, uint64_t off, size_t len, std::function<void(Result<os::LargeBuffer>)>); void write(std::string path, os::LargeBuffer data, std::function<void(Result<void>)>); void remove(...); void mkdir(...); void tracks(TrackQuery, std::function<void(Result<TrackPage>)>); void rescan();`
    - streaming: `class Writer { Result<void> append(std::span<const uint8_t>); Result<void> commit(); void abort(); };` `Result<std::unique_ptr<Writer>> begin_write(std::string path);` (must be called and used on `wisp.sd.io`; `write`/uploads/downloads submit their chunks there)
    - `Result<os::LargeBuffer> read_now(const std::string& path, uint64_t off, size_t len);` (on `wisp.sd.io` only; used by the player's reader and REST download stream)
    - `std::optional<Track> track(const std::string& id) const;` (memory only)
    - `void set_on_removed(std::function<void(const std::string& path)>);` (player skips a deleted current file)
  - `}` Every entry point returns `StorageUnavailable` without a card and `InvalidArgument` for a bad path (Task 1 rules).
- Behaviour: `start()` follows `sd_cell()`; on online: create the three folders, load `.index.json`, walk `music/` on the Bulk lane comparing `size`/`mtime`, re-read tags of changed files (read head 64 KiB for ID3v2 + MP3 info, tail 128 bytes for ID3v1), save the index with `write_atomic` once per scan; on offline: clear the in-memory index, `available = false`. `revision` bumps on every index change. Publishes `media.library` (Task 10 adds the topic constants; this task publishes through a `std::function<void(const MediaLibraryState&)>` hook left empty in tests).

- [ ] **Step 1: Write failing tests** using a temp-dir `StorageManager` with an SD root and a real `StorageRouter`/SD worker (follow `tests/core/storage_router_test.cpp` and `tests/core/scene_files_test.cpp` set-up): `CreatesFoldersOnMount`, `NoCardIsUnavailable`, `ScanIndexesTracksWithTags` (copy the three mp3 + two wav test files into `music/x/`), `RescanReadsOnlyChangedFiles` (count tag reads through a test hook), `RemovedFileLeavesTheIndex`, `CorruptIndexIsRebuilt`, `OldVersionIndexIsRebuilt`, `WriteAndRemoveUpdateOneEntry`, `TracksFilterAndPage` (q case-insensitive, artist, album, limit capped at 200), `ReadIsCappedAt64KiB`, `WriteOutsideTheFoldersIsRejected`, `HiddenFilesAreNotListed`, `WriterCommitsAtomically` (abort leaves no file; commit replaces), `CardRemovedClearsTheIndex`.
- [ ] **Step 2: Run** `ctest -R MediaLibrary` — fails.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** — pass.
- [ ] **Step 5: Commit** `feat(media): media library with track index and file operations`.

### Task 6: AudioArbiter and the voice/tone migration

**Files:**
- Create: `components/wisp_core/include/wisp/core/audio_arbiter.hpp`, `components/wisp_core/src/audio_arbiter.cpp`
- Modify: `components/wisp_core/include/wisp/core/tone_player.hpp` + `src/tone_player.cpp` (replace `set_output_guard` with `set_arbiter(AudioArbiter*)`), `components/wisp_voice/include/wisp/voice/voice_service.hpp` + `src/voice_service.cpp` (`begin_output`, `may_play_tone`, `out_mu_`: acquire `Voice` through the arbiter; recording acquires `Voice` for the capture), `components/wisp_app/src/voice_wiring.cpp` (no guard; pass the arbiter), `components/wisp_app/src/app.cpp` (own the arbiter, before `TonePlayer`), the header comments of all three
- Test: `tests/core/audio_arbiter_test.cpp`; existing `tests/core/tone_player_test.cpp`, `tests/voice/voice_service_test.cpp` keep passing (adapt only their set-up to the arbiter; assertions unchanged)

**Interfaces:**
- Produces:
  - `enum class AudioKind : uint8_t { Music, Tone, Voice, Alarm };` (ascending priority)
  - `class AudioArbiter { public: struct Config { uint32_t preempt_grant_ms = 200; uint32_t resume_delay_ms = 800; uint32_t alarm_wait_ms; uint32_t tone_wait_ms; }; AudioArbiter(Config, std::function<uint64_t()> now_ms = os::now_ms);`
    - `class Lease { /* move-only; ~Lease releases */ bool valid() const; AudioKind kind() const; };`
    - `Lease acquire(AudioKind kind, const std::function<bool()>& stopped = {});` — blocking rules of spec 5.1 for Alarm/Voice/Tone (Alarm calls the registered stop for Voice holders and waits up to `alarm_wait_ms`; Tone fails at once while Voice/Alarm is held; Voice waits up to `tone_wait_ms` while Tone/Alarm is held); invalid lease = refused.
    - `Lease try_acquire_music(std::function<void()> preempt);` — fails while any higher lease is held.
    - `void set_voice_stop(std::function<void()>);` (VoiceService registers `stop_speaking`) 
    - `void set_resume_listener(std::function<void()>);` (called on the arbiter's timer thread when no higher lease has been held for `resume_delay_ms`, only after a music preemption or a refused music acquire)
    - `void tick();` (drives the resume delay; `App` calls it from its existing 100 ms-class timer or an `os::Timer` owned by the arbiter — choose an owned `os::Timer` started by `start()`, stopped by `stop()`; tests call `tick()` with a fake clock and a non-started arbiter) `void start(); void stop(); bool held(AudioKind) const; }`
  - Music preemption: on any higher `acquire`, the arbiter calls the music lease's `preempt` (outside its lock) and waits until that lease is released or `preempt_grant_ms` passes, then grants. A music lease that outlived its grant is marked revoked; releasing it later is harmless.
- `kAlarmWaitMs`/`kToneWaitMs` keep their current values (moved from `VoiceService` into the `Config` defaults set by `voice_wiring.cpp`).

- [ ] **Step 1: Write failing tests:** `AudioArbiter.AlarmStopsVoiceAndWins`, `AudioArbiter.ToneIsRefusedWhileVoiceHolds`, `AudioArbiter.VoiceWaitsOutATone`, `AudioArbiter.VoiceGivesUpBehindAnAlarm` (the three existing rules, same timings), `AudioArbiter.MusicIsPreemptedByEachHigherKind` (preempt called once, higher granted after release), `AudioArbiter.StuckMusicIsGrantedPastAfter200ms`, `AudioArbiter.ResumeAfter800msWithoutHigher`, `AudioArbiter.NewHigherLeaseRestartsTheResumeDelay`, `AudioArbiter.MusicRefusedWhileHigherIsHeldThenResumed`, `AudioArbiter.NoResumeWithoutAPreemption`, `AudioArbiter.UserPauseDuringSystemPauseIsNotResumed` (here: the listener fires, the test player state ignores it — the rule lives in the player; this test pins that the arbiter fires exactly once).
- [ ] **Step 2: Run** `ctest -R 'AudioArbiter'` — fails.
- [ ] **Step 3: Implement** the arbiter, then migrate `TonePlayer` and `VoiceService` (delete `may_play_tone`, `out_mu_`'s output role and `set_output_guard`).
- [ ] **Step 4: Run** `ctest -R 'AudioArbiter|TonePlayer|VoiceService|PcmPlayer|VoiceWiring'` and the full suite — pass with the existing assertions unchanged.
- [ ] **Step 5: Commit** `refactor(core): move audio output sharing into AudioArbiter` (behaviour-preserving) and, if separate, `feat(core): preemptible music in the audio arbiter`.

### Task 7: `audio` wake lock and standby power

**Files:**
- Modify: `components/wisp_core/include/wisp/core/wakelock.hpp` + `src/wakelock.cpp` (`WakeLockKind::Audio`, `"audio"`), every listing of kinds (scene `bind_power`, admin wakelock route, web if it maps kinds — grep `to_string(WakeLockKind`/`"screen"`), `components/wisp_app/src/power_manager.cpp` (`ordered_devices`/`walk_devices` skip `AudioOut` while an audio lock is held; lock change during standby powers audio outputs up/down at once through the existing lock-eval job), `components/wisp_app/src/system_notices.cpp` (`post_wakelock_reminders` skips holder `system.media`), `components/wisp_hal/include/wisp/hal/audio.hpp` (caps comment per spec 6)
- Test: `tests/core/wakelock_test.cpp`, `tests/app/power_manager_test.cpp`, `tests/app/system_notices_test.cpp` (or where `post_wakelock_reminders` is tested)

**Interfaces:**
- Produces: `WakeLockKind::Audio`; `PowerManager` behaviour only.

- [ ] **Step 1: Write failing tests:** `WakeLock.AudioKindName`, `PowerManager.AudioLockKeepsAudioOutPoweredInStandby` (fake AudioOut with `standby: off` sees no `set_power(Standby)`; fake AudioIn and display still do), `PowerManager.AudioLockAcquiredInStandbyPowersOutputUp`, `PowerManager.AudioLockReleasedInStandbyPowersOutputDown`, `PowerManager.AudioLockDoesNotKeepTheScreenOn`, `SystemNotices.NoReminderForSystemMedia` (a `system.media` cpu lock held 31 min posts nothing; another holder still does).
- [ ] **Step 2: Run** the filter — fails.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** — pass.
- [ ] **Step 5: Commit** `feat(power): audio wake lock keeps audio outputs powered in standby`.

### Task 8: Play queue logic

**Files:**
- Create: `components/wisp_media/include/wisp/media/play_queue.hpp`, `src/play_queue.cpp`
- Test: `tests/media/play_queue_test.cpp`

**Interfaces:**
- Produces (pure, single-threaded; the player owns it under its lock):
  - `enum class PlayMode : uint8_t { Sequence, RepeatAll, RepeatOne, Shuffle };` `const char* to_string(PlayMode); std::optional<PlayMode> play_mode_from(std::string_view);`
  - `inline constexpr size_t kMaxQueue = 1000; inline constexpr uint32_t kPrevRestartS = 3;`
  - `class PlayQueue { struct SetResult { bool truncated; }; SetResult set(std::vector<std::string> items, size_t start); void set_mode(PlayMode, std::function<uint32_t()> rand); std::optional<size_t> current() const; const std::string& item(size_t) const; size_t size() const;`
    - `std::optional<size_t> advance_auto();` (track ended: RepeatOne → same; Sequence end → nullopt)
    - `std::optional<size_t> next();` `std::optional<size_t> prev(uint32_t position_s);` (returns same index to restart when `position_s > 3`)
    - `size_t remove_prefix(std::string_view prefix);` (scene uninstall: removes items starting with `"scene:<id>/"`; returns removed count; adjusts current, which moves to the next remaining item if it was removed)
    - `void clear(); }`
  - Shuffle uses a permutation drawn from `rand`; wrapping draws a new one whose first element differs from the last played when size > 1.

- [ ] **Step 1: Write failing tests:** `PlayQueue.SetReplacesAndTruncatesAt1000`, `PlayQueue.SequenceStopsAtTheEnd`, `PlayQueue.RepeatAllWraps`, `PlayQueue.RepeatOneRepeatsOnAutoButNextMoves`, `PlayQueue.ShufflePlaysEachOncePerCycle` (size 10, 30 advances: each item exactly 3 times, no immediate repeat at wraps), `PlayQueue.LeavingShuffleContinuesInOrderFromCurrent`, `PlayQueue.PrevRestartsAfter3s`, `PlayQueue.PrevAtFirstInSequenceStays`, `PlayQueue.RemovePrefixMovesCurrent`, `PlayQueue.RemovePrefixEmptiesTheQueue`.
- [ ] **Step 2: Run** `ctest -R PlayQueue` — fails.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** — pass.
- [ ] **Step 5: Commit** `feat(media): play queue with sequence, repeat and shuffle modes`.

### Task 9: MediaPlayer — session, task, reader ring, arbiter and wake locks

**Files:**
- Create: `components/wisp_media/include/wisp/media/player.hpp`, `src/player.cpp`, `src/item_reader.cpp` (+ `include/wisp/media/item_reader.hpp`)
- Test: `tests/media/player_test.cpp` (uses `tests/support/fake_audio.hpp`, the minimp3 factory from Task 4, a real `MediaLibrary` on a temp SD, a fake clock)

**Interfaces:**
- Consumes: Tasks 3, 4, 5, 6, 7, 8; `core::WakeLockRegistry`; `core::SceneFiles` (R5); `hal::AudioOut`.
- Produces:
  - `struct TrackInfo { std::string id, title, artist, album; uint32_t duration_s = 0; };`
  - `enum class PlayerStatus : uint8_t { Idle, Playing, Paused };` `enum class PauseReason : uint8_t { None, User, System };` (+ `to_string` each)
  - `struct PlayerState { PlayerStatus status; PauseReason pause_reason; TrackInfo track; uint32_t position_s; size_t index, queue_length; PlayMode mode; };`
  - `class ItemReader { /* resolves "music/…" via MediaLibrary::read_now and "scene:<id>/<name>" via SceneFiles::read, always on the storage thread that owns the medium; */ static Result<std::unique_ptr<ItemReader>> open(const std::string& item, ...); };` — implementer chooses internals; the player sees a `media::Source` over the ring.
  - `struct MediaPlayerDeps { std::shared_ptr<hal::AudioOut> out; core::AudioArbiter& arbiter; core::WakeLockRegistry& locks; MediaLibrary* library; core::SceneFiles* scene_files; core::StorageRouter* router; DecoderFactory decoders; std::function<uint8_t()> volume; std::function<uint32_t()> random; std::function<uint64_t()> now_ms = os::now_ms; std::function<void(const std::string& reason, const std::string& item)> on_error; };`
  - `class MediaPlayer { struct Config { size_t ring_bytes = 64 * 1024; size_t read_bytes = 16 * 1024; uint32_t linger_ms = 10000; size_t stack_bytes; int priority; }; MediaPlayer(MediaPlayerDeps, Config = {}); Result<void> start(); void stop();`
    - `PlayQueue::SetResult play(std::vector<std::string> items, size_t start, std::optional<PlayMode> mode);` `void pause(); void resume(); void next(); void prev(); void seek(uint32_t s); void set_mode(PlayMode); void stop_playback();`
    - `void forget_scene(std::string_view scene_id);` `void on_card_removed();` `void on_file_removed(const std::string& path);` `void on_volume_changed();`
    - `const core::StateCell<PlayerState>& state() const;`
    - `static os::TaskConfig task_config(const Config&);` (`name = "wisp.media"`, `flash_access = false`, `hot_stack = false`, priority above the scene task and below `wisp_ui`/the voice tasks: read their values from `scene_runtime`/`PcmPlayer::task_config` and pick between) `}`
  - Errors go to `on_error(reason, item)` with reasons `not_found`, `decode_error`, `unsupported_format`, `io_error`, `too_many_errors`, `card_removed`.
- Behaviour (spec 4, 5, 6): commands go through an `os::Queue`; the task decodes chunk by chunk (≈ 1152 frames), downmixes to mono, writes; checks commands and the preempt flag between chunks; position published at most once per second; three consecutive failures stop; wake locks `cpu`+`audio` held while `Playing` or `Paused/System`, released `linger_ms` after `Paused/User`/`Idle`, at once in `stop()`; on the arbiter's resume listener, resumes only if `pause_reason == System` at the moment the command is processed on the task.

- [ ] **Step 1: Write failing tests:** `MediaPlayer.PlaysAWavToTheEnd` (fake out receives ≈ 44100 mono frames at rate 44100), `MediaPlayer.PlaysMp3AndDownmixes`, `MediaPlayer.ReopensOnRateChange` (wav 44.1k then 16k: two opens), `MediaPlayer.SkipsUnsupportedAndCorrupt` (`float32.wav`, `garbage.mp3`, then a good item plays; two `on_error` calls), `MediaPlayer.StopsAfterThreeFailures`, `MediaPlayer.OutputRefusingTheRateSkips` (fake out fails `open` for 16000), `MediaPlayer.PositionTicksOncePerSecond`, `MediaPlayer.SeekJumps`, `MediaPlayer.PrevRestartsAfter3s`, `MediaPlayer.UserPauseHoldsPositionAndReleasesLocksAfterLinger`, `MediaPlayer.PreemptPausesSystemAndResumes` (a `Voice` lease taken and dropped → `Paused/System` then `Playing` after the delay, position kept), `MediaPlayer.PauseRacingTheResumeNotificationWins`, `MediaPlayer.PreemptedWhileWriteBlocksNeverReopens` (fake out `write` blocks 500 ms; voice granted at 200 ms; out not reopened by music until resume), `MediaPlayer.PlaysASceneFile` (`scene:demo/a.wav` through `SceneFiles`), `MediaPlayer.ForgetSceneDropsItsItems`, `MediaPlayer.CardRemovedWithAReadInFlightStops`, `MediaPlayer.StopJoinsWithARefillRefused`, `MediaPlayer.DeletedCurrentFileSkips`, `MediaPlayer.EveryExitReleasesTheLocks` (stop, end, card removed, three failures, `stop()`, forget-scene emptying), `MediaPlayer.VolumeFollowsTheSetting`.
- [ ] **Step 2: Run** `ctest -R MediaPlayer` — fails.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** the filter repeatedly (`--repeat until-fail:20`) — pass, no flakes.
- [ ] **Step 5: Commit** `feat(media): system music player with queue, arbiter and wake locks`.

### Task 10: App wiring, topics, simulator decoder

**Files:**
- Modify: `components/wisp_app/include/wisp/app/app.hpp` (`Platform::audio_decoder`, R2), `components/wisp_app/src/app.cpp` (construct `MediaLibrary` after `SceneWiring::start_storage()` when `storage.has_sd()`; `MediaPlayer` after `TonePlayer` when an `AudioOut` with role `main` exists; publish events; scene lifecycle handler `State::Deleted` → `player->forget_scene(id)`; `sd_cell` offline → `player->on_card_removed()`; library `set_on_removed` → `player->on_file_removed`; settings cell volume → `on_volume_changed`; stop order: `player->stop()` right after the status shell stops (before the voice, so no lease is held when `VoiceService` goes) and the library's `stop()` just before `scenes->stop()` (before the storage workers); the arbiter stops after `TonePlayer`; accessors `App::media_library()`, `App::media_player()`), `components/wisp_core/include/wisp/core/topics.hpp` (`kMediaPlayer = "media.player"`, `kMediaLibrary = "media.library"`, `kMediaError = "media.error"`, kinds CellMirror/CellMirror/Telemetry, forwarded true), `components/wisp_admin/src/ws_hub.cpp` (`kForwardedPrefixes` gains `"media."`), `simulator/main.cpp` (`platform.audio_decoder = media::with_wav(wisp::port::sim::make_minimp3_decoder factory)`), `docs/architecture.md` (boot step, stop order, task table row `wisp.media`, topics table, `Platform` field)
- Test: `tests/app/media_wiring_test.cpp` (App-level, like other `tests/app/*` that boot `App` with a sim platform)

**Interfaces:**
- Produces: `App::media_library() -> media::MediaLibrary*` (null without a card medium), `App::media_player() -> media::MediaPlayer*` (null without audio out); event payloads per spec 9 (`media.player`: `status, pause_reason, track_id, index, mode`; `media.library`: `available, scanning, track_count`; `media.error`: `reason, track_id`).

- [ ] **Step 1: Write failing tests:** `MediaWiring.BuildsWithCardAndAudio`, `MediaWiring.NoCardStillHasAPlayer`, `MediaWiring.DeletedSceneLeavesTheQueue`, `MediaWiring.PublishesPlayerEventsNotPerPositionTick`, `MediaWiring.StopReleasesEverything` (no leftover subscriptions: `~EventBus`/`~UiRuntime` misuse checks stay silent), plus `Topics` consistency test and `WsHub` forwarded-list test updated.
- [ ] **Step 2: Run** `ctest -R 'MediaWiring|Topics|WsHub'` — fails.
- [ ] **Step 3: Implement**; run the simulator by hand once with a card dir containing `media/music/*.mp3` and confirm audible playback (`--` flags as in `docs/testing.md`), note it in the report.
- [ ] **Step 4: Run** the filter, `python3 tools/check_topics.py`, full `ctest` — pass.
- [ ] **Step 5: Commit** `feat(app): wire the media library and player`.

### Task 11: ESP decoder with esp_audio_codec

**Files:**
- Create: `components/wisp_port_esp/include/wisp/port/esp/esp_mp3_decoder.hpp`, `components/wisp_port_esp/media/esp_mp3_decoder.cpp`
- Modify: `components/wisp_port_esp/CMakeLists.txt` (`SRCS`, `REQUIRES wisp_media`, `IDF_REQUIRES espressif__esp_audio_codec`), `firmware/main/idf_component.yml` (`espressif/esp_audio_codec: "2.6.2"` with a one-line comment), `firmware/dependencies.lock` (from a fresh resolve; any new transitive registry component pinned too — `tools/check_deps.py` must pass), `firmware/main/main.cpp` (`platform.audio_decoder = media::with_wav(make_esp_mp3_decoder factory)`), `THIRD_PARTY_NOTICES.md`, `docs/references.md`; an `.lf` linker fragment only if the codec places large code in IRAM (check `idf.py size-components` IRAM before/after; keep IRAM growth under 2 KiB or move it like `esp_new_jpeg.lf`)
- Test: none on host (the ESP port has no host tests for codecs); verification is both board builds plus the device checks.

**Interfaces:**
- Produces: `std::unique_ptr<media::Decoder> wisp::port::esp::make_esp_mp3_decoder();` implementing the Task 4 contract with the codec's simple-decoder API (read the installed component's `esp_audio_simple_dec.h` in `firmware/managed_components/espressif__esp_audio_codec/include/` for the exact calls; register only the MP3 decoder; output int16 interleaved; `open` fills `StreamInfo` from the first decoded frame's info; `seek_ms` re-positions the `Source` with the same TOC/estimate logic as Task 3 (`parse_mp3_info`) and resets the decoder; corrupt input → `ErrorCode::Corrupt` only if no frame decodes in the first 64 KiB).

- [ ] **Step 1: Pin and resolve** in a fresh detached worktree build dir (`idf.py set-target esp32s3`, `idf.py bmgr -c ../boards -b waveshare_esp32_s3_touch_amoled_2_16`, `idf.py reconfigure`); commit the lock change with the pin.
- [ ] **Step 2: Implement** the decoder.
- [ ] **Step 3: Build** both boards (`…_2_16` and `…_1_75c`); record app partition use (2.16 must stay ≤ 80 %) and IRAM in the report.
- [ ] **Step 4: Run** `python3 tools/check_deps.py`, `python3 tools/check_layering.py`, `bash tools/ci.sh`.
- [ ] **Step 5: Commit** `feat(port-esp): MP3 decoding with esp_audio_codec`.

### Task 12: Scene API `wisp.media`, the `media` permission and downloads into the library

**Files:**
- Create: `components/wisp_scene/src/bind_media.cpp`
- Modify: `components/wisp_core/include/wisp/core/scene_manifest.hpp` + `src/scene_manifest.cpp` (`enum class MediaAccess : uint8_t { None, Read, Write }; MediaAccess media = MediaAccess::None;` parsed from `permissions.media`; other values are manifest errors), `components/wisp_scene/include/wisp/scene/scene_api.hpp` (`media::MediaLibrary* media_library; media::MediaPlayer* media_player;` + `install_media` decl), `bind_scene.cpp` (`install_media`), `CMakeLists.txt` (source; `REQUIRES wisp_media`), `scene_runtime.{hpp,cpp}` (`kMedia = 16` pending bit; `DeviceEvent::Media`; follows the player cell → `scene.on_media(state)`), `bind_http.cpp` (`check_opts` accepts `media = "<path>"`; `do_download` streams into `MediaLibrary::begin_write` when set; requires `Write`), `components/wisp_scene/src/schema.cpp` (API entries; regenerate `docs/scene-api.md` from `build/sim/scene-api.md`), `components/wisp_agent/src/scene_workspace.cpp` (confirmation line: new `StrId::AgentConfirmMediaRead`/`AgentConfirmMediaWrite`, asked when `media` is new or raised, R4), `components/wisp_admin/src/routes_scenes.cpp` (`scene_item` emits `"media"`), `web/app/pages/scenes.js` (shows it), `components/wisp_ui/include/wisp/ui/strings.hpp` + `src/strings_{en,zh,ja}.cpp`, `web/app/i18n/{en,zh,ja}.js`, `components/wisp_app/src/app.cpp` (fill `SceneApi` media pointers), `python3 tools/gen_agent_docs.py` output `components/wisp_agent/src/docs_data.cpp`, `components/wisp_agent/src/prompt.cpp` (Lua modules line gains `wisp.media`)
- Test: `tests/core/scene_manifest_test.cpp`, `tests/scene/bind_media_test.cpp` (on `scene_runtime_fixture.hpp`), `tests/scene/bind_http_test.cpp`, `tests/agent/scene_workspace_test.cpp`, `tests/admin/routes_scenes_test.cpp`, `tests/scene/schema_test.cpp` (`DocIsUpToDate`), `tests/ui/strings_test.cpp`

**Interfaces:**
- Consumes: Tasks 5, 9, 10.
- Produces: the Lua surface of spec 7.1 exactly (`play`, `pause`, `resume`, `next`, `prev`, `seek`, `set_mode`, `stop`, `state`, `tracks`, `list`, `stat`, `read`, `write`, `remove`, `mkdir`; `scene.on_media(state)`; `wisp.http.download(url, name, {media = path}, cb)`). Error strings are `wire_name()` values. State table fields: `status`, `pause_reason`, `id`, `title`, `artist`, `album`, `duration`, `position`, `index` (1-based), `length`, `mode`.

- [ ] **Step 1: Write failing tests:** `SceneManifest.MediaPermission` (none/read/write; `"rw"` rejected), `BindMedia.PlaybackNeedsNoPermission`, `BindMedia.ReadNeedsReadOrWrite`, `BindMedia.WriteNeedsWrite`, `BindMedia.FileItemResolvesToThisSceneOnly` (`{file="a.mp3"}` → `scene:<own id>/a.mp3`; no way to name another scene), `BindMedia.OnMediaFiresOnChange` (position-only changes at most once per second), `BindMedia.StateTable`, `BindHttp.DownloadIntoTheLibrary` (needs write; file lands in `music/`; index updated), `BindHttp.DownloadIntoTheLibraryDeniedWithoutWrite`, `SceneWorkspace.ConfirmsNewMediaPermission` and `…RaisedToWrite` and `…UnchangedIsNotAsked`, `RoutesScenes.ListShowsMedia`, `Schema.DocIsUpToDate`.
- [ ] **Step 2: Run** `ctest -R 'SceneManifest|BindMedia|BindHttp|SceneWorkspace|RoutesScenes|Schema|Strings'` — fails.
- [ ] **Step 3: Implement**; binding calls are asynchronous with callbacks as in `bind_files`/`bind_notify` (follow their longjmp rules).
- [ ] **Step 4: Run** the filter, `python3 tools/gen_agent_docs.py --check`, `python3 tools/check_i18n.py` — pass.
- [ ] **Step 5: Commit** `feat(scene): wisp.media API and the media permission`.

### Task 13: Agent tools `media.*`

**Files:**
- Create: `components/wisp_agent/src/tools_media.cpp`
- Modify: `components/wisp_agent/include/wisp/agent/tools.hpp` (`Result<void> add_media_tools(ToolRegistry&, MediaToolDeps)`), `components/wisp_agent/CMakeLists.txt` (source; `REQUIRES wisp_media`), `components/wisp_app/src/agent_wiring.cpp` (register when the library or player exists), `components/wisp_agent/src/prompt.cpp` (one fixed line per language: music is best saved under `music/` of the media library; keep the prompt byte-stable otherwise), `components/wisp_agent/src/docs.cpp`/README tool lists if they enumerate tools
- Test: `tests/agent/tools_media_test.cpp`, `tests/agent/prompt_test.cpp`

**Interfaces:**
- Produces: tools of spec 7.3 with JSON schemas:
  - `media.tracks {q?, artist?, album?, offset?, limit?}` → `{items:[{id,title,artist,album,duration}], total}`
  - `media.list {dir?}` → `{entries:[{name,dir,size}]}`
  - `media.read_text {path}` → `{text}` (≤ 32 KiB; non-UTF-8 → `unsupported_format`)
  - `media.write_text {path, text}` → `{}`; `media.remove {path}` → `{}`
  - `media.download {url, path}` → `{size}` (through the scene HTTP proxy's download path or `Platform::http_client`, the same client `http.fetch` uses; streamed through `MediaLibrary::begin_write`; `timeout_ms` 10 min; `ToolThread::Io`)
  - `media.play {items:[string], start?, mode?}` → `{truncated}`; `media.control {action, seconds?, mode?}` → `{}`; `media.state {}` → player state + `{available, track_count}`
  - Card tools use `ToolThread::Agent` with `run_io`/library callbacks; errors are `tool_error(wire_name(e), message)`.

- [ ] **Step 1: Write failing tests:** one per tool on the agent fixture with a temp SD (`MediaTools.TracksQuery`, `ListRoot`, `ReadTextCapsAndRejectsBinary`, `WriteTextThenRemove`, `DownloadStreamsIntoTheLibrary` (fake HTTP), `PlayAndControl`, `StateWithoutCard`), `Prompt.MentionsMediaLibraryOnce` per language.
- [ ] **Step 2: Run** `ctest -R 'MediaTools|Prompt'` — fails.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** — pass.
- [ ] **Step 5: Commit** `feat(agent): media library and player tools`.

### Task 14: Admin REST routes

**Files:**
- Create: `components/wisp_admin/src/routes_media.cpp`
- Modify: `components/wisp_admin/src/admin_internal.hpp` (`add_media_routes`, `media_stream`), `admin_server.cpp` (route table; `stream_handler()` chain gains `media_stream`), `include/wisp/admin/admin_server.hpp` (`AdminDeps::media_library`, `media_player`), `CMakeLists.txt` (source; `REQUIRES wisp_media`), `components/wisp_app/src/net_wiring.cpp` or wherever `AdminDeps` is filled
- Test: `tests/admin/routes_media_test.cpp`

**Interfaces:**
- Produces: the routes of spec 3.7 (paired only). `PUT /media/files/<path>` is a `net::BodySink` stream: requires `Content-Length` (411 otherwise), refuses when another media upload is running (409), each chunk submitted to `wisp.sd.io` through the library `Writer` with back-pressure (wait for the previous chunk's job before accepting the next), `on_abort` aborts the writer. `GET /media/files/<path>` streams with `res.stream`, chunk ≤ `min(net::kStreamChunkBytes, core::kIoChunk)`, `system.download` network wake lock as in `routes_scene_files.cpp`. Error bodies `{"error": wire_name}` with `http_status()`.

- [ ] **Step 1: Write failing tests:** `RoutesMedia.ListRootAndFolder`, `DownloadStreams`, `UploadStreamsAndIndexes` (an mp3 upload appears in `/media/tracks`), `UploadWithoutLengthIs411`, `ConcurrentUploadIs409`, `AbortedUploadLeavesNoFile`, `DeleteAndMkdir`, `BadPathIs400`, `NoCardIs503`, `UnpairedIsRefused`, `PlayerRoutes` (play/pause/next/state).
- [ ] **Step 2: Run** `ctest -R RoutesMedia` — fails.
- [ ] **Step 3: Implement.**
- [ ] **Step 4: Run** — pass.
- [ ] **Step 5: Commit** `feat(admin): media library and player routes`.

### Task 15: Web app — Media page and Now playing card

**Files:**
- Create: `web/app/pages/media.js`, `web/app/components/now_playing.js`
- Modify: `web/app/main.js` (route `/media`; reached from the System page like scenes/memory/logs via `PARENT`), the System page (entry row), the home page (card shown when the player is not idle), `web/app/api.js` (calls), `web/app/ws.js` consumers for `media.player`/`media.library`, `web/app/i18n/{en,zh,ja}.js`, `web/app/icons.js` if new icons are needed; rebuild the web asset pack the way `docs/testing.md` describes
- Test: the existing web checks (`tools/check_i18n.py`, any web unit tests under `web/`), plus a Playwright pass against the simulator's admin server (manual, in the report with screenshots at phone width 390 px and desktop)

**Interfaces:**
- Consumes: Task 14 routes, Task 10 events.
- Produces: UI only. Browse with breadcrumbs, upload (file picker, progress, one at a time), download, delete with confirmation, new folder, rescan button showing `scanning`; empty state ("No card" / "Empty folder"); the card shows title, artist, previous/pause-resume/next.

- [ ] **Step 1:** Add i18n keys for all three languages; run `python3 tools/check_i18n.py` — fails until pages use them consistently.
- [ ] **Step 2: Implement** the page and card.
- [ ] **Step 3: Verify** in the simulator with Playwright: upload an mp3, see it in the list and in tracks, play from the card, delete; no console errors; no horizontal scroll at 390 px.
- [ ] **Step 4: Run** `bash tools/ci.sh`.
- [ ] **Step 5: Commit** `feat(web): media library page and now playing card`.

### Task 16: Pull-down panel "now playing" row

**Files:**
- Modify: `components/wisp_ui/include/wisp/ui/system_layer.hpp` (`struct PanelMedia { bool shown; std::string title, artist; bool playing; };` in `PanelModel`; `PanelHandlers::media_prev/media_toggle/media_next`), `components/wisp_ui/src/notification_panel.cpp` (row below the header: `Part::Media*`, layout with `row_span()`/`safe_rect()`, icons with ≥ `theme::kGapSm` gaps, vertically centred, ellipsized labels, key-focusable), `components/wisp_app/src/status_shell.cpp` (follow the player cell on the UI dispatcher; fill `PanelMedia`; handlers call `MediaPlayer`), `components/wisp_ui/include/wisp/ui/strings.hpp` + `strings_{en,zh,ja}.cpp` (`StrId::MediaUnknownArtist`), glyph subset regenerated if new CJK glyphs (`tools/gen_cjk_font.py`), screenshot preview tool entries if previews enumerate panel states
- Test: `tests/ui/system_layer_test.cpp`, `tests/app/status_shell_test.cpp`

**Interfaces:**
- Consumes: Task 9 `PlayerState`, Task 10 `App::media_player()`.

- [ ] **Step 1: Write failing tests:** `SystemLayerTest.MediaRowInsideVisibleAreaOnEveryShape` (rect 480, rounded_rect 480, circle 466: row and its three buttons fully visible, gaps ≥ `kGapSm`, buttons centred), `SystemLayerTest.MediaRowButtonsCallHandlers`, `SystemLayerTest.MediaRowHiddenWhenIdle`, `SystemLayerTest.MediaRowIsKeyFocusable`, `SystemLayerTest.LongTitleEllipsizes`, `StatusShellTest.PanelFollowsThePlayer` (open panel updates on play/pause; unknown artist text per language).
- [ ] **Step 2: Run** `ctest -R 'SystemLayerTest|StatusShellTest'` — fails.
- [ ] **Step 3: Implement**; run the screenshot previews for the three shapes and inspect spacing/alignment/wrapping.
- [ ] **Step 4: Run** the filter and `python3 tools/check_i18n.py` — pass.
- [ ] **Step 5: Commit** `feat(ui): now playing row in the pull-down panel`.

### Task 17: Documentation and the device check list

**Files:**
- Modify: `docs/architecture.md` (anything not already covered in Tasks 1/10: the library and player sections, the arbiter replacing VoiceService's output rules, the `audio` wake lock in the power section), `docs/testing.md` (new test groups, media test data regeneration), `README.md` + `README.zh-CN.md` + `README.ja.md` (feature list line), `docs/superpowers/specs/2026-11-19-media-library-design.md` ("In the code" notes for R1–R5), `docs/backlog.md` (out-of-scope items of spec 1.2 that the owner may want later: cover art, AAC/FLAC, playlists, screen-off volume keys)
- Create: `docs/superpowers/plans/2026-11-19-media-library-device-checks.md` (the seven device checks of spec 10, each with exact steps, expected result, and where to read `WISP_PERF`/`mem` numbers; to be handed to the owner, not marked done)

- [ ] **Step 1:** Write the docs.
- [ ] **Step 2: Run** `bash tools/ci.sh --full` — pass.
- [ ] **Step 3: Commit** `docs: media library and music playback`.
