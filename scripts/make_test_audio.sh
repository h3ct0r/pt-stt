#!/usr/bin/env bash
# Regenerate the Portuguese test fixtures in tests/fixtures (macOS only: uses `say`).
# All files are 16 kHz mono 16-bit WAV. The fixtures are committed, so CI does
# not need to run this.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
OUT="$ROOT/tests/fixtures"
VOICE="${VOICE:-Luciana}"
TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT
mkdir -p "$OUT"

command -v say >/dev/null || { echo "error: needs macOS 'say'" >&2; exit 1; }
command -v ffmpeg >/dev/null || { echo "error: needs ffmpeg" >&2; exit 1; }

TEXT="O robô deve virar à esquerda e seguir em frente por dois metros."

say -v "$VOICE" -o "$TMP/speech.aiff" "$TEXT"
to_wav() { ffmpeg -loglevel error -y -i "$1" -ar 16000 -ac 1 -c:a pcm_s16le "$2"; }

# speech.wav: the sentence alone.
to_wav "$TMP/speech.aiff" "$OUT/speech.wav"

# speech_pause.wav: 0.5 s silence, sentence, 3 s silence (silence-timeout test).
ffmpeg -loglevel error -y -i "$OUT/speech.wav" \
  -af "adelay=500:all=1,apad=pad_dur=3" -ar 16000 -ac 1 -c:a pcm_s16le "$OUT/speech_pause.wav"

# silence.wav: 6 s of very quiet noise (no-speech test).
ffmpeg -loglevel error -y -f lavfi -i "anoisesrc=d=6:c=white:a=0.002:r=16000" \
  -ac 1 -c:a pcm_s16le "$OUT/silence.wav"

echo "$TEXT" > "$OUT/speech.txt"
ls -l "$OUT"
