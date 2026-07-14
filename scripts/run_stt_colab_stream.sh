#!/usr/bin/env bash
set -euo pipefail

# Streaming Colab launcher. The board still connects to this PC for now, but
# inference/segmentation run on the Colab WebSocket server.
# Fixed dev Colab tunnel, overridable with the URL printed by the notebook.
STT_STREAM_URL="${STT_STREAM_URL:-wss://passage-capacity-wistful.ngrok-free.dev/stt/stream}"

echo "Using streaming STT URL: $STT_STREAM_URL"

SUBTITLE_HOST="${STT_SUBTITLE_HOST:-192.168.1.10}"
SUBTITLE_PORT="${STT_SUBTITLE_PORT:-5001}"
AUDIO_PORT="${STT_AUDIO_PORT:-5000}"
JSONL="${STT_JSONL:-logs/stt_events.jsonl}"
SAVE_WAV="${STT_SAVE_WAV:-logs/board_audio.wav}"
FOREGROUND="${STT_FOREGROUND:-0}"
READY_FILE="${STT_READY_FILE:-}"
DONE_FILE="${STT_DONE_FILE:-}"
STOP_FILE="${STT_STOP_FILE:-}"
SINGLE_SESSION="${STT_SINGLE_SESSION:-0}"
MAX_WINDOW_SEC="${STT_MAX_WINDOW_SEC:-}"
MIN_SILENCE_SEC="${STT_MIN_SILENCE_SEC:-}"
PARTIAL_SEC="${STT_PARTIAL_SEC:-}"
PARTIAL_AGREEMENT="${STT_PARTIAL_AGREEMENT:-}"
GAIN="${STT_GAIN:-}"
VAD_THRESHOLD="${STT_VAD_THRESHOLD:-}"
VAD_NEG_THRESHOLD="${STT_VAD_NEG_THRESHOLD:-}"

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
mkdir -p "${REPO_DIR}/logs"
REPO_WIN_PATH="$(wslpath -w "${REPO_DIR}")"

AUTOMATION_ARGS=""
if [[ -n "${READY_FILE}" ]]; then
    AUTOMATION_ARGS+=" --ready-file '${READY_FILE}'"
fi
if [[ -n "${DONE_FILE}" ]]; then
    AUTOMATION_ARGS+=" --done-file '${DONE_FILE}'"
fi
if [[ -n "${STOP_FILE}" ]]; then
    AUTOMATION_ARGS+=" --stop-file '${STOP_FILE}'"
fi
if [[ "${SINGLE_SESSION}" == "1" ]]; then
    AUTOMATION_ARGS+=" --single-session"
fi
if [[ -n "${MAX_WINDOW_SEC}" ]]; then
    AUTOMATION_ARGS+=" --max-window-sec '${MAX_WINDOW_SEC}'"
fi
if [[ -n "${MIN_SILENCE_SEC}" ]]; then
    AUTOMATION_ARGS+=" --min-silence-sec '${MIN_SILENCE_SEC}'"
fi
if [[ -n "${PARTIAL_SEC}" ]]; then
    AUTOMATION_ARGS+=" --partial-sec '${PARTIAL_SEC}'"
fi
if [[ -n "${PARTIAL_AGREEMENT}" ]]; then
    AUTOMATION_ARGS+=" --partial-agreement '${PARTIAL_AGREEMENT}'"
fi
if [[ -n "${GAIN}" ]]; then
    AUTOMATION_ARGS+=" --gain '${GAIN}'"
fi
if [[ -n "${VAD_THRESHOLD}" ]]; then
    AUTOMATION_ARGS+=" --vad-threshold '${VAD_THRESHOLD}'"
fi
if [[ -n "${VAD_NEG_THRESHOLD}" ]]; then
    AUTOMATION_ARGS+=" --vad-neg-threshold '${VAD_NEG_THRESHOLD}'"
fi

BRIDGE_COMMAND="Set-Location -LiteralPath '${REPO_WIN_PATH}'; python scripts/stt_stream_bridge.py --stream-url '${STT_STREAM_URL}' --host 0.0.0.0 --port ${AUDIO_PORT} --jsonl '${JSONL}'${SAVE_WAV:+ --save-wav '${SAVE_WAV}'} --send-subtitles --subtitle-host ${SUBTITLE_HOST} --subtitle-port ${SUBTITLE_PORT}${AUTOMATION_ARGS}"

if [[ "${FOREGROUND}" == "1" ]]; then
    powershell.exe -NoProfile -Command "${BRIDGE_COMMAND}"
else
    powershell.exe -NoProfile -Command "Start-Process powershell.exe -ArgumentList @('-NoExit','-Command',\"${BRIDGE_COMMAND}\")"
fi
