# Media Library and Music Playback — Design

- Date: 2026-11-19
- Status: design agreed section by section with the owner on 2026-11-19; written spec awaiting review
- Scope: a public media library on the SD card (`/sd/media`) and one system music player with a unified interface for the device UI, scenes, the agent and the admin console. Local files only; playback continues with the screen off; MP3 and WAV.
- Builds on: the foundation spec (StorageManager, the System/Library/Backup areas), the flash-thread design (`wisp.fs`/`wisp.sd.io`, `StorageRouter`), the voice spec 4.4 (sharing the output between speech and tones), the scene device spec (wake locks, standby levels, `wisp.files`, `wisp.http.download`, the pull-down panel), the state-cells design, the device-model design (caps `standby`).
- Prepares: roadmap phase I (`wisp.audio.play/record`, the `audio`/`mic` permissions) reuses the arbiter of section 5.

---

## 1. Goals and non-goals

### 1.1 Goals

1. A media library the user, scenes and the agent share: music, downloads and documents on the SD card, browsable and writable through one service.
2. One system-owned "now playing" session: a scene, the agent or the admin console starts it; it outlives the scene that started it; the pull-down panel controls it.
3. Playback continues with the screen off and stays usable with the volume keys.
4. Music gives way to alarms, speech, recording and tones, and comes back on its own when the system paused it.
5. Everything except the device decoder and the hardware itself is testable on the host.

### 1.2 Non-goals (this design)

Cover art, lyrics, AAC/FLAC/OGG, Bluetooth audio, persisted playlists, resuming playback across a reboot, streaming from the network, a built-in full-screen player (a full player UI is a scene), mixing music under other sounds (music pauses instead), resampling.

### 1.3 Decisions taken with the owner

| Topic | Decision |
|---|---|
| UI | A system playback service plus a "now playing" row in the pull-down panel (previous, play/pause, next). Full player UIs are scenes. |
| Location | SD card only, `/sd/media/{music,download,document}`. Named "media library" (媒体库) to stay apart from `wisp/library` (system data), which is unchanged. No card, no media library. |
| Index | Only `music/` is indexed; the other folders are browsed as files. |
| Content in | Copying on a computer, the admin console (upload, browse, delete), scenes and the agent. Storing music in the library is suggested, not enforced: the player also plays a scene's own private files. |
| Session | One session with a queue; modes sequence, repeat-all, repeat-one, shuffle. A new `play` replaces the queue. Playback continues after the starting scene closes. |
| Coexistence | Music pauses for alarms, speech (TTS, recording) and tones, and resumes on its own. A pause by the user is never resumed automatically. |
| Permissions | A scene declares `"media": "read"` or `"write"` in `scene.json` `permissions`, confirmed once at install like HTTP hosts. Controlling playback needs no permission; a scene can always play its own files. The agent has read/write by default without confirmation. |
| Screen off | Playback holds a `cpu` and a new `audio` wake lock; the audio output stays powered in standby. Keys behave as today (no screen-off volume keys: on the 2.16 `vol_down` is the talk key and the 1.75C has no volume keys); volume changes by voice through the agent, in settings or in a scene. |
| Architecture | Approach 1: a new portable component `wisp_media` plus `core::AudioArbiter`. |
| Decoder | Behind a portable `media::Decoder` interface: `esp_audio_codec` on the device (performance), minimp3 in the simulator and host tests. WAV is a portable parser shared by both. |
| Volume | Music shares `audio/volume` with speech. |

---

## 2. Architecture

```
          scene (wisp.media)   agent (media.*)   admin (/media/*)   panel row
                     \               |                 |              /
                      +--------------+--------+--------+-------------+
                                              |
                     media::MediaLibrary   media::MediaPlayer        (wisp_media)
                        |  index, files       |  session, queue, task wisp.media
                        |                     |  Decoder (injected factory)
                  core::StorageRouter      core::AudioArbiter         (wisp_core)
                   (wisp.sd.io)               |  Alarm > Voice > Tone > Music
                                          hal::AudioOut
```

- **New component `wisp_media`**, placed after `wisp_voice` and before `wisp_admin` in the layer order (`wisp_base` → … → `wisp_voice` → `wisp_media` → `wisp_admin` → …). It depends on `wisp_core` and below. `tools/check_layering.py` and AGENTS.md section 3 gain the new layer.
- **Decoders**:
  - `media::Decoder` (portable interface, section 4.3).
  - `wisp_media` ships the WAV decoder and the factory type; it has no MP3 code of its own.
  - `wisp_port_esp` provides the `esp_audio_codec` MP3 decoder; `wisp_port_sim` provides the minimp3 MP3 decoder. Each platform hands its factory to `App` through `Platform::audio_decoders` (empty: MP3 is reported `unsupported_format`).
  - minimp3 (CC0) is vendored unmodified in `third_party/minimp3/` by `tools/vendor_minimp3.sh`, like Lua and stb; only `wisp_port_sim` and the `wisp_media` tests may use it. `check_layering.py` enforces this.
  - `esp_audio_codec` comes from the ESP component registry, pinned to an exact version in `firmware/main/idf_component.yml` (with any new transitive components, `tools/check_deps.py`); only `wisp_port_esp` requires it. Its licence (Espressif's modified MIT: use on Espressif chips only) is satisfied because only the ESP port links it; it is registered in `THIRD_PARTY_NOTICES.md` and `docs/references.md`.
- **`core::AudioArbiter`** in `wisp_core` replaces the output coordination now inside `VoiceService` (section 5).
- **Adapters**: the Lua binding (`wisp_scene`), the agent tools (`wisp_agent`), the REST routes (`wisp_admin`) and the panel row (`wisp_app`/`wisp_ui`) only call `MediaLibrary`/`MediaPlayer` (AGENTS.md section 3, service layer).
- **Wiring**: `App` constructs `MediaLibrary` after the storage workers and `MediaPlayer` after `TonePlayer`, the library only when the platform has an SD card medium, the player only when an `AudioOut` with role `main` exists (without a card it can still play a scene's private files on internal storage). Stop order: the player stops (task joined, locks released, arbiter lease returned) before `TonePlayer`, the voice and the storage workers; the library stops before the storage workers.

---

## 3. Media library (`media::MediaLibrary`)

### 3.1 Layout

- Root `/sd/media/` with three fixed folders: `music/`, `download/`, `document/`, created when missing each time the card is mounted. Users may nest folders freely below them.
- Without a mounted card every call returns `media_unavailable` and the library reports `available = false` (a field of its state cell, section 3.5).
- All paths in every interface are relative to `/sd/media` and use `/`.

### 3.2 Path rules

`normalize(path)` accepts a relative path of UTF-8 segments and rejects (`invalid_path`):
- an empty path, a leading `/`, a `\`, an empty segment, a `.` or `..` segment;
- a segment starting with `.` (hidden files, including `.index.json` and atomic-write temporaries);
- a control character, or a total length over 255 bytes.

Writes are further limited to paths below one of the three folders (no new top-level entries).

### 3.3 Track index

- Only `music/` is indexed, recursively; files with extension `.mp3` or `.wav` (case-insensitive) count as tracks.
- A track's **id** is its relative path, e.g. `music/Artist/Song.mp3`.
- Per track: `title`, `artist`, `album`, `duration_s` (0 when unknown), `size`, `mtime`.
  - Tags: ID3v2.3/2.4 (`TIT2`, `TPE1`, `TALB`; text encodings ISO-8859-1, UTF-16 with BOM, UTF-16BE, UTF-8) and ID3v1 as fallback. An ISO-8859-1 frame whose bytes are valid UTF-8 is read as UTF-8 (common mis-tagging); one with bytes ≥ 0x80 that are not valid UTF-8 (typically GBK) is ignored for that field, since there is no GBK table on the device. A missing title falls back to the file name without extension; a missing artist or album is empty (the UI shows a localized "Unknown artist").
  - Duration: WAV from the header; MP3 from a Xing/Info or VBRI header, otherwise estimated from the first frame's bit rate and the audio size.
- The index is stored as `/sd/media/.index.json` (`write_atomic` on the card) with a format version. A missing, unreadable or wrong-version index is discarded and rebuilt.
- **Rescan**:
  - On mount (boot or card inserted): walk `music/`, compare each file's `size` and `mtime` with the index, re-read tags only of new or changed files, drop entries whose file is gone.
  - After a write or delete through the library: update that one entry.
  - `rescan()` on request (admin console button, agent tool): the same walk.
- All card work runs on `wisp.sd.io` through `StorageRouter` (AGENTS.md section 5): scans and tag reads on the Bulk lane, interactive calls on the Interactive lane. A scan never blocks a playback read for longer than one file's tag read.

### 3.4 Service API

All calls are asynchronous (completion callback on the caller's dispatcher, or `run_sync` with a timeout for the agent and REST paths, following the existing `SceneFiles`/`core::run_files()` pattern):

| Call | Result |
|---|---|
| `list(dir)` | entries `{name, dir: bool, size, mtime}`; `dir` empty means the root (the three folders) |
| `stat(path)` | one entry |
| `read(path, offset, len)` | bytes; `len` capped at 64 KiB per call |
| `write(path, data)` | atomic write; creates parent folders; updates the index for tracks |
| `remove(path)` | a file, or an empty folder; updates the index |
| `mkdir(path)` | creates folders |
| `tracks(query)` | index entries filtered by `artist`, `album` and/or a case-insensitive substring `q` over title/artist/album; paged (`offset`, `limit` ≤ 200) |
| `rescan()` | starts a rescan; completion is visible in the state cell |
| `open_stream(path)` | a read handle for the player and for downloads served to the admin console (section 4.4) |
| `begin_write(path)` | a streaming writer (`append`, `commit`, `abort`) for uploads and downloads: writes `.tmp`, commits with the FAT two-stage rename (AGENTS.md section 8) |

### 3.5 State cell

`StateCell<MediaLibraryState>`: `available`, `scanning`, `track_count`, `revision` (bumped on every index change). Consumers re-query on a revision change; no one keeps a copy of the index.

### 3.6 Downloads into the library

- Scenes: `wisp.http.download(url, name, opts, cb)` gains `opts.media = "<path>"`; with it the body streams into the library through `begin_write` instead of the scene's private files. It needs `"media": "write"`.
- Agent: `media.download` (section 7.2).
- The existing size limits of scene downloads do not apply to library writes; the card's free space does (`io_error` when full; the original file stays intact).

### 3.7 Admin console

- REST routes (paired clients only, like `/scenes/*/files`):
  - `GET /media/files?dir=<path>` — list
  - `GET /media/files/<path>` — download (streamed)
  - `PUT /media/files/<path>` — upload (streamed through `begin_write`; any size the card holds)
  - `DELETE /media/files/<path>` — delete
  - `POST /media/folders/<path>` — mkdir
  - `GET /media/tracks?q=&artist=&album=&offset=&limit=`
  - `POST /media/rescan`
  - `GET /media/player`, `POST /media/player/{play,pause,resume,next,prev,seek,mode,stop}` (section 6)
- Web app: a "Media" page (browse by folder, upload, download, delete, new folder, rescan) and a small "Now playing" card on the home page (title, artist, pause/resume, previous, next). Text localized zh/ja/en.
- The `media.library` and `media.player` WebSocket events (section 9) keep the pages live.

---

## 4. Media player (`media::MediaPlayer`)

### 4.1 Session state

`StateCell<PlayerState>`:
- `status`: `idle` | `playing` | `paused`
- `pause_reason`: `none` | `user` | `system`
- `track`: `{id, title, artist, album, duration_s}` of the current item
- `position_s`: published at most once per second while playing, and on every seek or change
- `index`, `queue_length`, `mode`: `sequence` | `repeat_all` | `repeat_one` | `shuffle`

The session is not persisted: after boot the player is `idle` with an empty queue.

### 4.2 Queue and controls

- `play(items, start_index = 0, mode = keep)` replaces the queue and starts at `start_index`. An item is either a library id (`music/...`, or any library path to a playable file) or a scene file reference `scene:<scene id>/<file name>` (only produced by the scene binding for the calling scene, section 7.1). More than 1000 items: the rest is dropped and the result carries `truncated = true`.
- `pause()` → `paused`/`user`. `resume()` → `playing`. `stop()` → `idle`, queue cleared.
- `next()`: the next item by mode; at the end of a `sequence` queue playback stops (`idle`, queue cleared).
- `prev()`: if `position_s` > 3, restart the current item; otherwise the previous item (`repeat_all`/`shuffle` wrap around; `sequence` stays at the first item).
- `seek(seconds)`: clamped to `[0, duration]`.
- `set_mode(mode)`. `shuffle` draws a permutation (from `Platform::random`) and plays it through; every item plays once before any repeats; a new permutation is drawn when it wraps. Leaving `shuffle` keeps the current item and continues in queue order from it.
- `repeat_one` repeats the current item; `next()`/`prev()` still move by queue order.
- Scene uninstall: items `scene:<that id>/…` are removed from the queue; if the current item is one, playback moves to the next remaining item (or `idle`).
- All controls are non-blocking and thread-safe; they post a command to the player task.

### 4.3 Decoder interface

```cpp
namespace wisp::media {
struct StreamInfo { uint32_t sample_rate; uint8_t channels; uint32_t duration_ms; /* 0 = unknown */ };
class Source {   // pull-based reader over the prefetch ring (4.4)
public:
    virtual Result<size_t> read(uint8_t* dst, size_t len) = 0;  // 0 = end
    virtual Result<void> seek(uint64_t offset) = 0;
    virtual uint64_t size() const = 0;
};
class Decoder {
public:
    virtual ~Decoder() = default;
    virtual Result<StreamInfo> open(Source& src) = 0;            // parses headers
    virtual Result<size_t> decode(int16_t* pcm, size_t max_frames) = 0;  // interleaved; 0 = end
    virtual Result<void> seek_ms(uint32_t ms) = 0;
};
enum class Format : uint8_t { Mp3, Wav };
using DecoderFactory = std::function<std::unique_ptr<Decoder>(Format)>;  // nullptr: unsupported
}
```

- The format is chosen by `probe()` (magic bytes: `RIFF…WAVE`, an `ID3` header or an MPEG frame sync), not by extension alone.
- WAV: PCM 16-bit (8/24/32-bit integer and float are `unsupported_format`), 1 or 2 channels.
- MP3 seek: through the Xing table of contents when present, otherwise by bit-rate estimate followed by frame resynchronization.

### 4.4 Player task `wisp.media`

- One task, created at `start()`, stack in PSRAM (`flash_access = false`, `hot_stack = false`), priority above the scene task and below the UI and voice tasks. Size from `mem` measurements.
- It never touches the card itself: a reader keeps a 64 KiB `Mem::Large` ring filled with 16 KiB reads submitted to `wisp.sd.io` (Interactive lane), refilling at half empty. Scene files are read the same way through their `SceneFiles` path.
- Pipeline per chunk: decode → downmix to mono (`(L+R)/2`) → `AudioOut::write()`. The output is opened at the file's sample rate with 1 channel; volume goes through `AudioOut::set_volume()` from `audio/volume` (the settings cell), as speech does. A new item with a different rate closes and reopens the output; if `open()` fails the item is skipped with `unsupported_format`.
- Between two chunks the task checks its command queue and the arbiter's preemption flag, so a pause or a preemption takes effect within one chunk (≤ ~25 ms of audio).
- On a buffer underrun (card slow) it waits for data without closing the output.

### 4.5 Errors during playback

- Missing file, `decode_error`, `unsupported_format`, read error: publish `media.error` (section 9) and skip to the next item.
- Three consecutive failed items: stop (`idle`, queue cleared), `media.error` with `reason = "too_many_errors"`.
- Card removed: stop, clear the queue (`idle`), `media.error` with `reason = "card_removed"`.
- The current file deleted through the library: skip to the next item.

---

## 5. Audio arbiter (`core::AudioArbiter`)

### 5.1 Today

`VoiceService::may_play_tone()` (the `TonePlayer`'s output guard) and `VoiceService::begin_output()` share `out_mu_`: an alarm always wins (speech stops, the alarm waits up to `kAlarmWaitMs` for the player to close); a tone while speech holds the output is skipped; speech waits up to `kToneWaitMs` for a sounding tone/alarm and then gives up.

### 5.2 The arbiter

- `acquire(Kind kind, Preempt preempt = {}) -> std::optional<Lease>`; a `Lease` releases on destruction.
- Kinds and priority: `Alarm` > `Voice` (TTS, `wisp.play`, recording, loopback `wisp.loop`) > `Tone` > `Music`.
- The rules of 5.1 move into the arbiter unchanged (same waits, same outcomes); `VoiceService` and `TonePlayer` acquire through it, `out_mu_`'s output role and `may_play_tone()` go away, and the existing voice/tone tests keep passing. Recording acquires `Voice` although it does not use the speaker, so the microphone does not pick up music.
- **Music**:
  - `Music` registers a `preempt` callback with its lease. When any higher kind is acquired, the arbiter calls it; the player stops after the current chunk, closes the output and drops its lease. The arbiter waits up to **200 ms** for that, then grants the higher kind regardless (speech is never held up by music for longer).
  - The preempted player is `paused`/`system` and keeps its position.
  - When no higher lease has been held for **800 ms**, the arbiter notifies the player, which re-acquires `Music` and resumes at the kept position. The delay bridges sentence-by-sentence TTS and the tone that follows a turn.
  - A `Music` acquire while a higher lease is held fails; the player is then `paused`/`system` and waits for the same notification.
  - `pause_reason = user` (a user pause, including one during a system pause) is never resumed by the arbiter.
- Time goes through an injectable clock so the tests need no real sleeps.

---

## 6. Screen off and standby

- **New wake lock kind `WakeLockKind::Audio`** (`"audio"`). While one is held, `PowerManager` leaves every `AudioOut` device (by interface, whatever its caps `standby`) powered at `standby` and `deep_standby`; all other devices follow their caps as today, including `AudioIn`.
  - Released while in standby: the audio outputs are powered down at once per their caps.
  - Acquired while in standby: they are powered up at once.
  - The comment in `wisp/hal/audio.hpp` ("Playing or recording below Dim is not supported") becomes: playing below Dim requires an `audio` wake lock; recording below Dim is not supported.
- **The player holds** `cpu` and `audio`, both with holder `system.media`, while `playing` and while `paused`/`system`. After a user pause, a stop or the end of the queue it keeps them for **10 s** more (no power-down pop when the user resumes right away), then releases them.
- **The 30-minute reminder** (`wakelock_long`) is not posted for holder `system.media`: long music is expected and audible.
- **Wi-Fi** follows `deep_standby` as today (off); local playback needs no network. The SD card is not a standby device and stays mounted.
- **Keys with the screen off** behave as today: every key and touch wakes the screen. There is no screen-off volume key (the 2.16's `vol_down` is the talk key, `vol_up` has no handler, the 1.75C has no volume keys); with the screen off the user changes the volume by voice (the agent's `settings.set`), or after waking, in settings or a scene.
- **Notifications**: a tone without `wake` while the screen is off is still dropped (so it does not interrupt music in standby). An alarm turns the screen on, preempts music through the arbiter, and music resumes after the alarm is silenced.

---

## 7. Scene API, permission, agent tools

### 7.1 Scene API `wisp.media`

Playback (no permission):
- `wisp.media.play(items, [opts], [cb])` — `items`: a list of library ids (strings) and/or `{file = "<private file name>"}`; the binding turns the latter into `scene:<this scene's id>/<name>`, so a scene can play only its own files. `opts`: `start` (1-based), `mode`. `cb(err, {truncated = bool})`.
- `wisp.media.pause()`, `resume()`, `next()`, `prev()`, `seek(sec)`, `set_mode(m)`, `stop()`.
- `wisp.media.state()` — the current `PlayerState` as a table.
- `scene.on_media(state)` — called on every state change (position at most once per second); the cell callback only sets a pending bit that the scene task takes on its next step, like the device events.

Library (needs `"media": "read"` or `"write"`):
- `wisp.media.tracks(query, cb)`, `wisp.media.list(dir, cb)`, `wisp.media.stat(path, cb)`, `wisp.media.read(path, offset, len, cb)`.

Library writes (needs `"media": "write"`):
- `wisp.media.write(path, data, cb)`, `wisp.media.remove(path, cb)`, `wisp.media.mkdir(path, cb)`; `wisp.http.download(url, name, {media = "<path>"}, cb)`.

Without the permission a call fails with `permission_denied` (no prompt). The generated scene-api document (`components/wisp_scene/src/schema.cpp`) and the agent docs (`tools/gen_agent_docs.py`) cover the new API.

### 7.2 Permission

- `scene.json`: `"permissions": {"http": [...], "media": "read" | "write"}`; anything else is a manifest error.
- Confirmed where HTTP hosts are confirmed today: when the agent installs or updates a scene (`scene_workspace.cpp`), the confirmation text gains one line ("Read the media library" / "Read and write the media library", zh/ja/en) next to the new hosts. An update that adds `media` or raises `read` to `write` counts as new, like a new host. A scene installed through the admin console is the user's own action and is not confirmed (as for hosts); the admin scene list shows the `media` level next to the hosts.

### 7.3 Agent tools

The agent has read/write access without confirmation.

| Tool | Purpose |
|---|---|
| `media.tracks` | query the index (`q`, `artist`, `album`, `offset`, `limit`) |
| `media.list` | list a folder |
| `media.read_text` | read a UTF-8 text file (≤ 32 KiB; binary files are refused) |
| `media.write_text` | write a UTF-8 text file |
| `media.remove` | delete a file or empty folder |
| `media.download` | download a URL into a library path (streamed; returns size) |
| `media.play` | `items`, `start`, `mode` |
| `media.control` | `action`: pause, resume, next, prev, seek (`seconds`), mode (`mode`), stop |
| `media.state` | the player state and the library's `available`/`track_count` |

The system prompt says music is best saved under `music/` (a suggestion, not a rule).

### 7.4 Pull-down panel row

- Shown below the panel header while the player is not `idle`: title and artist on the left (one line each, ellipsized), three icon buttons on the right (previous, play/pause, next) with at least `theme::kGapSm` between them, centred vertically in the row.
- Follows the player cell on the UI dispatcher; buttons call the service only (no scene opens). Laid out with `row_span()`/`safe_rect()`, checked on all three screen shapes; key-focusable (`ui::make_key_focusable()`). Text localized zh/ja/en.

---

## 8. Error codes

Shared by the service, the scene API, the agent tools and REST:

| Code | Meaning |
|---|---|
| `media_unavailable` | no card mounted |
| `not_found` | no such file or track |
| `invalid_path` | rejected by 3.2 |
| `permission_denied` | the scene lacks the `media` permission it needs |
| `unsupported_format` | not MP3/WAV, unsupported WAV sample format, or the output refused the sample rate |
| `decode_error` | corrupt file |
| `busy` | the card queue is full; retry later |
| `io_error` | card read/write failure (including a full card) |

REST maps them to 503, 404, 400, 403, 415, 422, 503 and 500.

## 9. Events

- `media.player`: one per player cell change (not per position tick): `{status, pause_reason, track_id, index, mode}`.
- `media.library`: one per library cell change: `{available, scanning, track_count}`.
- `media.error`: `{reason, track_id}`.
Payloads carry ids and states only. Log lines name tracks by id.

---

## 10. Testing

Host (GoogleTest, TDD):
- **Library**: path normalization (every rejection in 3.2); scan, incremental rescan, entry update on write/remove; index rebuild from a missing/corrupt/old-version file; ID3v2 (each encoding, the UTF-8-in-Latin-1 and non-UTF-8 cases), ID3v1 fallback, file-name fallback; MP3 duration from Xing, VBRI and estimate; WAV header parsing and rejection of unsupported sample formats; `media_unavailable` without a card.
- **Player** (minimp3 decoding generated test files: a few seconds of sine wave as MP3 and WAV, committed under the tests' data, no copyrighted material): each mode; `prev`'s 3-second rule; shuffle plays each item once per cycle; `seek`; a sample-rate change between items; skipping a bad item; stopping after three; card removal; queue truncation at 1000; removal of a scene's items on uninstall.
- **Arbiter**: the existing alarm/voice/tone behaviour (the current tests move with it); music preemption, the 200 ms grant, the 800 ms resume delay, no resume after a user pause, a failed `Music` acquire waiting for the notification; on a fake clock.
- **Power**: an `audio` lock keeps audio outputs powered in standby; acquiring and releasing it during standby; no `wakelock_long` for `system.media`.
- **Scene API**: with and without each permission level; `{file = …}` resolves to the calling scene only; `on_media`; manifest parsing and the confirmation lines.
- **Agent tools and REST**: the existing tool and admin test harnesses.
- **Panel row**: layout tests on the three shapes; screenshot previews run manually.
- **Simulator**: real playback through `SimAudioOut` (to be checked at 44.1 kHz).

Device checks (handed to the owner; not claimed as verified):
1. MP3 at 44.1 kHz and 48 kHz and a WAV play correctly on both boards.
2. `esp_audio_codec` CPU load while playing.
3. Ten minutes of screen-off playback without dropouts; current draw with the screen off.
4. The talk key with the screen off still wakes and talks while music plays (music pauses, then resumes).
5. TTS, tones and an alarm interrupt music, and music resumes without pops.
6. Removing the card while playing leaves the system responsive.
7. Internal RAM budget re-measured (`mem`), per AGENTS.md section 5.

## 11. Documentation

`docs/architecture.md` (new layer, boot and stop order, task `wisp.media`, events, wake lock kind), AGENTS.md section 3 (layer order, third-party rule for minimp3), `docs/testing.md` if CI scope changes, `THIRD_PARTY_NOTICES.md` and `docs/references.md` (minimp3, esp_audio_codec), the generated scene-api and agent docs, `CHANGELOG.md` at release.

## 12. In the code

Where the implementation differs from the text above, or fixes a detail the text left open.

- **`decode_error` has no `ErrorCode` of its own.** `ErrorCode::Corrupt` (`"corrupt"`) was added to `wisp_base`; `media::wire_name(const Error&)` maps `Corrupt` to `decode_error`, `StorageUnavailable` to `media_unavailable`, `NotFound` to `not_found`, `InvalidArgument` to `invalid_path`, `PermissionDenied` to `permission_denied`, `Unsupported` to `unsupported_format`, `Busy` to `busy` and everything else to `io_error`.
- **Decoder injection.** The factory reaches `App` as `Platform::audio_decoder` (singular; `std::function<std::unique_ptr<media::Decoder>(media::Format)>`) and covers MP3 only. `App` applies `media::with_wav()` itself, so WAV plays even when the field is empty. The simulator's minimp3 decoder lives in its own component `wisp_port_sim_media` (no SDL dependency); the device's `esp_audio_codec` decoder is in `wisp_port_esp`.
- **Volume.** The player passes `audio/volume` to `AudioOut::set_volume()` when it opens the output and on each settings change; it reads the value through a `std::function<uint8_t()>` given at construction. `PcmPlayer` keeps its own software scaling.
- **Permission confirmation** for `"media"` happens only on the agent install/update path (`scene_workspace.cpp`). Installs from the admin console do not ask; the admin scene list shows `"media"`.
- **Scene files on internal storage** are read through `SceneFiles::read()`, library items through `MediaLibrary`; both sit behind `media::ItemReader`, so the player does not know which it plays.
- **Lanes and priority.** Library calls use the Normal lane of `wisp.sd.io` (its Interactive lane holds the player's reads), not Interactive as 3.3 says. `wisp.media` runs at priority 5, above the UI and at the speech player's level, not below the UI as section 4.4 says: the player writes each decoded chunk straight to the output, whose I2S DMA ring holds about 33 ms at 44.1 kHz, so a long UI frame must not delay it. The gap first heard on the 2.16 when opening the scene list turned out to be mic priming (see "No mic priming while music plays" below), not CPU: decoding a 320 kbit/s chunk takes 5–6 ms and the longest gap between writes was 7 ms. The priority stays as headroom. The arbiter keeps music and speech from playing at once, so sharing speech's level costs speech nothing (owner's ruling, 2026-11-20; kept 2026-11-21).
- **No mic priming while music plays.** Section 5 lets the mic's input lease pause music, and the voice service primes the mic at every press on the wisp (and at the talk key) before it knows the press is a hold. On the 2.16 every page drag that started on the wisp cut the music for about a second (the prime, then the arbiter's resume delay). `VoiceService::prime_mic()` now skips priming while a Music lease is held; a real hold takes the unprimed path (start tone, then the mic) and pauses the music as before, without the pre-roll.
- **Recording.** `core::AudioArbiter` takes an input lease for the microphone (reported as `Voice`). It pauses music but does not block tones or alarms, so the primed start tone and alarms keep their timing.
- **Player details.** `next`/`prev` while paused stay paused at the new item. Card removal stops any session, including one playing a scene file on internal storage. Commands are dropped with a warning when the 32-slot command queue is full. `MediaPlayerDeps` carries the `EventBus` and a state mirror hook so `media.player` is published like `media.library`.
- **Library writes.** Temporary names are `.<name>.part` and `.<name>.done` (`StorageManager` rejects `.tmp`/`.new`); leftovers are recovered on list and scan.
- **`media.read_text`** returns the first 32 KiB (cut on a character boundary) with `truncated: true` instead of an error.
- **REST errors** use the admin console's envelope `{"error":{"code":<wire name>,"message":...}}`. A request with `Content-Length` 0 answers 411 (the device reports a missing length as 0), so empty uploads are unsupported. `GET /media/player` answers 503 without a player. The router supports a trailing `**` wildcard for file paths.
- **Scene API.** There is no `DeviceEvent::Media`; `scene.on_media` has its own pending bit. `wisp.media` is not exposed to the agent's `lua.run` VM.
- **Panel.** `StatusShell` receives the player's state cell and prev/pause/resume/next functions rather than a `MediaPlayer*`.
- **Screen off.** There are no screen-off volume keys (see 1.3); the arbiter and wake locks behave as in 5 and 6.
- **Golden recorder.** The simulator's forwarded-stream recorder skips `media.*` events (the boot library events race Wi-Fi events); `media_wiring_test` checks their payloads.
- **Test data.** The committed files in `tests/data/media/` are small: the 44.1 kHz stereo WAV is 0.25 s (11025 frames) and the float WAV is 8 kHz mono, so all test media stay under 200 KiB. Expected frame counts use these real lengths. The minimp3 tests use a peak threshold of 2000 and a duration tolerance of 100 ms (encoder delay and padding are untrimmed).
- **Firmware size.** On the 2.16 the app partition is 81.74 % after adding `esp_audio_codec` (about 62 KB), above the 80 % planning line; it is tracked in the device checks.
