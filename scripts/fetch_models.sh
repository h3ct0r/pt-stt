#!/usr/bin/env bash
# Download the GGUF models used by pt-stt into ./models (or $PTSTT_MODELS_DIR).
#
#   scripts/fetch_models.sh            # ASR (F32, 452 MB) + Silero VAD
#   scripts/fetch_models.sh --q8       # also make a q8_0 copy of the ASR model
#                                      # (needs build/bin/parakeet-cli)
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
DIR="${PTSTT_MODELS_DIR:-$ROOT/models}"
mkdir -p "$DIR"

ASR_URL="https://huggingface.co/Singla0009/Portuguese-FastConformer-Hybrid-GGUF/resolve/main/portuguese-fastconformer-hybrid-large.f32.gguf"
ASR_FILE="$DIR/pt-fastconformer-large.f32.gguf"
VAD_URL="https://huggingface.co/mudler/parakeet-cpp-gguf/resolve/main/silero-vad-f32.gguf"
VAD_FILE="$DIR/silero-vad-f32.gguf"

fetch() {
  local url="$1" out="$2"
  if [[ -s "$out" ]]; then
    echo "ok      $(basename "$out") (already present)"
    return
  fi
  echo "fetch   $(basename "$out")"
  curl -fL --retry 3 --progress-bar -o "$out.part" "$url"
  mv "$out.part" "$out"
}

fetch "$ASR_URL" "$ASR_FILE"
fetch "$VAD_URL" "$VAD_FILE"

if [[ "${1:-}" == "--q8" ]]; then
  CLI="$ROOT/build/bin/parakeet-cli"
  [[ -x "$CLI" ]] || { echo "error: $CLI not found, build the project first" >&2; exit 1; }
  Q8="$DIR/pt-fastconformer-large.q8_0.gguf"
  [[ -s "$Q8" ]] || "$CLI" quantize "$ASR_FILE" "$Q8" q8_0
  echo "ok      $(basename "$Q8")"
fi

echo "models in $DIR"
