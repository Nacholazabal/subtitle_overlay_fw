#!/usr/bin/env bash
set -euo pipefail

MODEL="${STT_MODEL:-tiny}"
CHUNK_SEC="${STT_CHUNK_SEC:-2.0}"
SUBTITLE_HOST="${STT_SUBTITLE_HOST:-192.168.1.10}"
SUBTITLE_PORT="${STT_SUBTITLE_PORT:-5001}"
AUDIO_PORT="${STT_AUDIO_PORT:-5000}"

REPO_WIN_PATH="$(wslpath -w "$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)")"

powershell.exe -NoProfile -Command "\
Start-Process powershell.exe -ArgumentList @(\
'-NoExit',\
'-Command',\
'Set-Location -LiteralPath ''${REPO_WIN_PATH}''; python scripts/stt_receiver.py --model ${MODEL} --host 0.0.0.0 --port ${AUDIO_PORT} --chunk-sec ${CHUNK_SEC} --send-subtitles --subtitle-host ${SUBTITLE_HOST} --subtitle-port ${SUBTITLE_PORT}'\
)\
"
