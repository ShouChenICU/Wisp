#!/usr/bin/env bash
# Regenerates tests/data/media/ (the committed media test files) with
# ffmpeg and lame. The outputs are committed, so CI and the tests need
# neither tool; rerun this only when the set of files changes. The commands
# below are the record of how each file was made.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="${ROOT}/tests/data/media"
WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT
mkdir -p "${OUT}"

FF=(ffmpeg -nostdin -loglevel error -y -bitexact)

sine() { # <rate> <channels> <seconds> <out.wav>
    "${FF[@]}" -f lavfi -i "sine=frequency=440:sample_rate=$1:duration=$3" \
        -ac "$2" -c:a pcm_s16le "$4"
}

# PCM16 WAVs, plus an IEEE-float one the WAV reader must reject. The
# 44.1 kHz stereo file is 0.25 s and the float file 8 kHz so that all the
# test media together stay under 200 KiB (1 s of 44.1 kHz stereo PCM16 alone
# is 172 KiB).
"${FF[@]}" -f lavfi -i "sine=frequency=440:sample_rate=44100:duration=0.25" \
    -ac 2 -c:a pcm_s16le "${OUT}/sine_44k_stereo.wav"
"${FF[@]}" -f lavfi -i "sine=frequency=440:sample_rate=16000:duration=1" \
    -ac 1 -c:a pcm_s16le "${OUT}/sine_16k_mono.wav"
"${FF[@]}" -f lavfi -i "sine=frequency=440:sample_rate=8000:duration=1" \
    -ac 1 -c:a pcm_f32le "${OUT}/float32.wav"

# 44.1 kHz stereo CBR 128 kbit/s, 3 s, Xing/Info frame, ID3v2.4 with UTF-8
# text frames. lame cannot write v2.4, so ffmpeg re-muxes lame's stream
# (-c copy) and writes the tag and its own Info frame.
sine 44100 2 3 "${WORK}/a.wav"
lame --quiet -b 128 -h --cbr "${WORK}/a.wav" "${WORK}/a_raw.mp3"
"${FF[@]}" -i "${WORK}/a_raw.mp3" -c copy -write_xing 1 -id3v2_version 4 \
    -metadata title="Sine 440" -metadata artist="Wisp" -metadata album="Tests" \
    "${OUT}/sine_44k_stereo_cbr128.mp3"

# 48 kHz mono VBR, 2 s: lame's Xing frame carries a TOC; ID3v2.3 with
# UTF-16 text frames (lame's --id3v2-utf16).
sine 48000 1 2 "${WORK}/b.wav"
lame --quiet -V 4 -h --vbr-new --id3v2-utf16 \
    --tt "Sine 48k" --ta "Wisp" --tl "Tests" \
    "${WORK}/b.wav" "${OUT}/sine_48k_mono_vbr.mp3"

# 22.05 kHz mono CBR, 2 s: -t drops the Xing/Info frame, --id3v1-only
# writes no ID3v2 tag.
sine 22050 1 2 "${WORK}/c.wav"
lame --quiet -t -b 64 -h --cbr --id3v1-only \
    --tt "Sine 22k" --ta "Wisp" --tl "Tests" \
    "${WORK}/c.wav" "${OUT}/sine_22k_noxing.mp3"

# 4 KiB of a fixed byte pattern that is not an MP3 stream.
python3 -I - "${OUT}/garbage.mp3" <<'PY'
import sys
state = 0x1234567
out = bytearray()
for _ in range(4096):
    state = (state * 1103515245 + 12345) & 0x7FFFFFFF
    out.append((state >> 16) & 0xFF)
open(sys.argv[1], "wb").write(out)
PY
