#!/usr/bin/env bash
set -euo pipefail

# Streaming Colab launcher. The board still connects to this PC for now, but
# inference/segmentation run on the Colab WebSocket server.
# Override with STT_STREAM_URL=wss://your-ngrok-host/stt/stream.

STT_STREAM_URL="${STT_STREAM_URL:-wss://passage-capacity-wistful.ngrok-free.dev/stt/stream}"

echo "Using streaming STT URL: $STT_STREAM_URL"

SUBTITLE_HOST="${STT_SUBTITLE_HOST:-192.168.1.10}"
SUBTITLE_PORT="${STT_SUBTITLE_PORT:-5001}"
AUDIO_PORT="${STT_AUDIO_PORT:-5000}"
JSONL="${STT_JSONL:-logs/stt_events.jsonl}"
SAVE_WAV="${STT_SAVE_WAV:-logs/board_audio.wav}"

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
mkdir -p "${REPO_DIR}/logs"
REPO_WIN_PATH="$(wslpath -w "${REPO_DIR}")"

powershell.exe -NoProfile -Command "\
Start-Process powershell.exe -ArgumentList @(\
'-NoExit',\
'-Command',\
'Set-Location -LiteralPath ''${REPO_WIN_PATH}''; python scripts/stt_stream_bridge.py --stream-url ${STT_STREAM_URL} --host 0.0.0.0 --port ${AUDIO_PORT} --jsonl ''${JSONL}''${SAVE_WAV:+ --save-wav ''${SAVE_WAV}''} --send-subtitles --subtitle-host ${SUBTITLE_HOST} --subtitle-port ${SUBTITLE_PORT}'\
)\
"
