#!/usr/bin/env bash
set -euo pipefail

# Colab inference launcher - uses remote GPU inference via ngrok tunnel.
# Usage: ./scripts/run_stt_colab.sh
# Override the default tunnel with STT_COLAB_URL=https://xxx.ngrok-free.dev if it changes.

STT_COLAB_URL="${STT_COLAB_URL:-https://passage-capacity-wistful.ngrok-free.dev/}"

echo "Using Colab URL: $STT_COLAB_URL"

# Same VAD/segmentation params as local mode; inference runs on Colab GPU
# instead. Colab is fast enough to show stabilized partials by default.
MAX_WINDOW_SEC="${STT_MAX_WINDOW_SEC:-4.0}"
MIN_SILENCE_SEC="${STT_MIN_SILENCE_SEC:-0.5}"
PARTIAL_SEC="${STT_PARTIAL_SEC:-0.8}"
PARTIAL_AGREEMENT="${STT_PARTIAL_AGREEMENT:-2}"
BEAM_SIZE="${STT_BEAM_SIZE:-5}"
GAIN="${STT_GAIN:-0}"
SUBTITLE_HOST="${STT_SUBTITLE_HOST:-192.168.1.10}"
SUBTITLE_PORT="${STT_SUBTITLE_PORT:-5001}"
AUDIO_PORT="${STT_AUDIO_PORT:-5000}"
VAD_FILTER="${STT_VAD_FILTER:---vad-filter}"
LOSSLESS_LIVE="${STT_LOSSLESS_LIVE---lossless-live}"
JSONL="${STT_JSONL:-logs/stt_events.jsonl}"
SAVE_WAV="${STT_SAVE_WAV:-logs/board_audio.wav}"

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
mkdir -p "${REPO_DIR}/logs"
REPO_WIN_PATH="$(wslpath -w "${REPO_DIR}")"

powershell.exe -NoProfile -Command "\
Start-Process powershell.exe -ArgumentList @(\
'-NoExit',\
'-Command',\
'Set-Location -LiteralPath ''${REPO_WIN_PATH}''; python scripts/stt_receiver.py --colab-url ${STT_COLAB_URL} --host 0.0.0.0 --port ${AUDIO_PORT} --max-window-sec ${MAX_WINDOW_SEC} --min-silence-sec ${MIN_SILENCE_SEC} --partial-sec ${PARTIAL_SEC} --partial-agreement ${PARTIAL_AGREEMENT} --gain ${GAIN} --beam-size ${BEAM_SIZE} ${VAD_FILTER} ${LOSSLESS_LIVE} --jsonl ''${JSONL}''${SAVE_WAV:+ --save-wav ''${SAVE_WAV}''} --send-subtitles --subtitle-host ${SUBTITLE_HOST} --subtitle-port ${SUBTITLE_PORT}'\
)\
"
