# Media library: device checks

Handed to the owner. Nothing in this list has been run; every result below is an assumption until recorded. The host side is covered by the simulator tests; these checks need the boards (2.16 and 1.75C) and a real SD card.

## Preparation

1. Build and flash both boards (`idf.py app-flash`, or a full flash on a fresh board). This feature changed no glyphs, so the glyphs and fonts partitions need no reflash; if a later change touches the glyph set, also run `idf.py fonts-flash`.
2. Put test files on the card (FAT, on a computer) under `/media/music/`, `/media/download/`, `/media/document/` (the library root is `<sd root>/media`):
   - `cbr128_44k.mp3`: CBR 128 kbit/s, 44.1 kHz stereo, ID3v2 tag with cover art over 100 KB, ID3v1 tag at the end.
   - `vbr_48k.mp3`: VBR with a Xing TOC, 48 kHz, 3 to 5 minutes.
   - `mono_22k.mp3`: MPEG-2 Layer III, mono 22.05 or 24 kHz.
   - `cbr320_44k.mp3`: 320 kbit/s, 44.1 kHz stereo, at least 10 minutes (used for CPU, screen-off and seek checks).
   - `tone_44k.wav` and `tone_16k_mono.wav`: PCM16 WAVs.
   - `garbage.mp3` (random bytes), `header_then_garbage.mp3` (a valid first frame header followed by garbage), `zeroed.mp3` (all zero bytes) and `truncated.mp3` (a good file cut in half).
   - A 20 MB or larger MP3 for the upload check.
   - The simulator's test files in `tests/data/media/` can serve for the short cases.
3. Open a serial console. `mem` prints each heap, each task's stack high-water mark and each storage lane's longest wait and job. Performance numbers come from the `WISP_PERF` line (prefix plus single-line JSON); note which field carries the frame rate in the build.
4. Record before any playback: `mem` at idle, `WISP_PERF` with the home screen animating, and the app partition use from the build output.

## 1. Formats on both boards

Steps: for each board, play `cbr128_44k.mp3`, `vbr_48k.mp3`, `mono_22k.mp3`, `tone_44k.wav` and `tone_16k_mono.wav` through the admin console's Media page, or `media.play` through the agent. Let one file play to the end and let the queue move on.
Expected: correct pitch and speed (no chipmunk effect, no slow-down), duration shown matches the file, no error toast and no "not 16-bit" log line, the cover art tag does not delay the start, the trailing ID3v1 tag ends the track cleanly and the next track starts.
Record: per file and board, pass or fail, start delay, any log error.

## 2. Decoder CPU load and UI frame rate

`wisp.media` runs at the UI's priority (4), so decoding can cost UI frames.
Steps: play `cbr320_44k.mp3` while the screen animates (home screen, then a scene with animation, then the pull-down panel open). Read `WISP_PERF` in each state and compare with the idle baseline.
Expected: no audible underruns; the frame rate does not drop noticeably against the baseline agreed in the rendering spec. If it does, lower the task priority to 3 and repeat.
Record: frame rate idle and while playing in each state, underruns heard, the priority tried.

## 3. Screen-off playback

Steps: start `cbr320_44k.mp3`, let the screen turn off, and leave it for 10 minutes. Measure the supply current with the screen off while playing, and again with playback paused and the same wake state.
Expected: no dropouts for the 10 minutes, the screen stays off, current is stable (the `cpu` and `audio` wake locks `system.media` are held), and the device returns to its normal standby draw about 10 s after a pause.
Record: dropouts, current playing and paused, whether the device stayed responsive to the touch and keys.

## 4. Talk key with the screen off

Steps: with music playing and the screen off, press the talk key and speak, then release.
Expected: the screen wakes, music pauses (system pause), the turn runs, the reply is spoken, and the music resumes on its own about 800 ms after the speech ends. Repeat, but pause the music by hand first: it must stay paused afterwards. On the 2.16 `vol_down` is the talk key; the 1.75C has no volume keys, so use its talk gesture.
Record: pause and resume behaviour in both cases, any pop or click.

## 5. TTS, tone and alarm interruption

Steps: while music plays, trigger (a) a spoken agent reply, (b) a notification tone, (c) a to-do alarm that is due.
Expected: each takes the speaker within 200 ms, music pauses without a pop, the alarm is heard in full, and music resumes without a pop after each (except after a user pause). A failed resume does not leave the output open.
Record: per case, latency estimate, pops, resume behaviour.

## 6. Seeks and damaged files on the device decoder

Steps: seek `vbr_48k.mp3` to 25 %, 50 % and 90 % (web card or `media.seek`), several times in a row. Play `garbage.mp3`, `header_then_garbage.mp3`, `zeroed.mp3` and `truncated.mp3`, each alone and three in a row in a queue.
Expected: seeks land within about 1 s of the target with no long glitch and never stall (this exercises `esp_audio_simple_dec_reset` mid-stream); damaged files report `decode_error` within a moment and are skipped; three failures in a row stop playback; no crash, and no watchdog.
Record: seek error in seconds, time to report each damaged file, log lines.

## 7. Card removal while playing

Steps: start a track, pull the card, then reinsert it. Repeat while a large upload is in progress.
Expected: playback stops, the system stays responsive (touch, panel, talk key), the library reports `media_unavailable`, and after reinsertion the library rescans and a new `play` works. No hang in `stop()`, no crash.
Record: time to stop, responsiveness, the rescan time.

## 8. Internal RAM budget and stacks

Steps: run `mem` at idle, during playback, during playback with the web Media page open, and during a large web upload while playing. Repeat the worst case of `wisp/app/ram_budget.hpp` (STA+AP, two httpd instances, the WebSocket log stream, a TLS handshake) with music playing.
Expected: the minimum free internal RAM and the minimum largest block stay inside the budget; the `internal RAM: ...` line at service start is not a warning; `wisp.media` stack high-water mark stays well under its 32 KiB (stack in PSRAM, sized for minimp3's 16 KiB plus the codec). `esp_audio_codec` allocates about 28 KB per stream with plain `malloc`; blocks under 16 KB land in internal RAM, so note how much the codec takes while a track is open.
Record: the figures from `mem` in each state, the `wisp.media` high-water mark, the lane waits of `wisp.sd.io` and `wisp.fs`. Put the measured data in `docs/hardware/<board>.md`.

## 9. Large upload over Wi-Fi

Steps: from a browser on the LAN, upload the 20 MB or larger MP3 on the Media page; watch the progress bar. Try cancelling one upload, replacing a same-name file, and uploading while a track plays and while the panel is open. Also open a second upload (expect "another upload is in progress").
Expected: throughput is reasonable and progress is smooth; playback has no dropouts; the file appears in the library after the commit and plays; a cancelled upload leaves no `.part` file visible in the list; `mem` shows no httpd stack failure and internal RAM stays in budget.
Record: throughput in MB/s, `mem` during the upload, any playback dropout.

## 10. Panel row and web page on the device

Steps: with music playing, open the pull-down panel on both boards. Press prev, play/pause and next by touch, and on a board with nav keys by key. Open the admin console's Overview and Media pages over a real WebSocket (the device accepts two clients).
Expected: row layout and icons are correct on the round 466 and the 480 screens, controls act and the row updates, key focus survives a rebuild of the panel, the web now-playing card follows the device within a second.
Record: layout problems, anything that lags.

## 11. App partition size

The 2.16 app image is 81.74 % of its OTA slot with `esp_audio_codec` (about 62 KB), above the 80 % planning line; the 1.75C is at 60.86 %. Rebuild both boards at the end of the branch and record `wisp.bin` size and percentage. Decide whether a size trim is needed before release.
