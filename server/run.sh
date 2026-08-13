#!/usr/bin/env bash
set -euo pipefail

# Production launcher: board -> PC bridge -> Colab Nemotron -> firmware ACK.
# Segmentation and end-of-utterance detection run in Colab through the official
# NeMo RNNTGreedyEndpointing pipeline.
STT_STREAM_URL="${STT_STREAM_URL:-wss://passage-capacity-wistful.ngrok-free.dev/stt/stream}"

echo "Using Nemotron STT URL: $STT_STREAM_URL"

SUBTITLE_HOST="${STT_SUBTITLE_HOST:-192.168.1.10}"
SUBTITLE_PORT="${STT_SUBTITLE_PORT:-5001}"
AUDIO_PORT="${STT_AUDIO_PORT:-5000}"
JSONL="${STT_JSONL:-logs/stt_events.jsonl}"
SAVE_WAV="${STT_SAVE_WAV:-logs/board_audio.wav}"
FOREGROUND="${STT_FOREGROUND:-0}"
READY_FILE="${STT_READY_FILE:-}"
DONE_FILE="${STT_DONE_FILE:-}"
STOP_FILE="${STT_STOP_FILE:-}"
BOARD_ACK_JSONL="${STT_BOARD_ACK_JSONL:-}"
SUBTITLE_READY_TIMEOUT="${STT_SUBTITLE_READY_TIMEOUT:-15}"
SINGLE_SESSION="${STT_SINGLE_SESSION:-0}"

# Nemotron session config. Either supply the whole JSON object directly via
# STT_BACKEND_CONFIG_JSON, or set individual STT_NEMOTRON_* vars and let this script
# assemble it. Keys must match NemotronConfig.from_overrides on the server.
# STT_NEMOTRON_LATENCY_MS is the model's published operating point (algorithmic
# lookahead: 80/160/320/560/1120), NOT an end-to-end latency budget.
BACKEND_CONFIG_JSON="${STT_BACKEND_CONFIG_JSON:-}"
NEMOTRON_LATENCY_MS="${STT_NEMOTRON_LATENCY_MS:-}"
NEMOTRON_STOP_HISTORY_EOU_MS="${STT_NEMOTRON_STOP_HISTORY_EOU_MS:-}"
NEMOTRON_RESIDUE_TOKENS_AT_END="${STT_NEMOTRON_RESIDUE_TOKENS_AT_END:-}"
NEMOTRON_TARGET_LANG="${STT_NEMOTRON_TARGET_LANG:-}"

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
mkdir -p "${REPO_DIR}/logs"
REPO_WIN_PATH="$(wslpath -w "${REPO_DIR}")"

if [[ -z "${BACKEND_CONFIG_JSON}" ]]; then
    BACKEND_CONFIG_JSON="$(
        NEMOTRON_LATENCY_MS="${NEMOTRON_LATENCY_MS}" \
        NEMOTRON_STOP_HISTORY_EOU_MS="${NEMOTRON_STOP_HISTORY_EOU_MS}" \
        NEMOTRON_RESIDUE_TOKENS_AT_END="${NEMOTRON_RESIDUE_TOKENS_AT_END}" \
        NEMOTRON_TARGET_LANG="${NEMOTRON_TARGET_LANG}" \
        python3 - <<'PY'
import json, os
mapping = {
    "latency_ms": ("NEMOTRON_LATENCY_MS", int),
    "stop_history_eou_ms": ("NEMOTRON_STOP_HISTORY_EOU_MS", int),
    "residue_tokens_at_end": ("NEMOTRON_RESIDUE_TOKENS_AT_END", int),
    "target_lang": ("NEMOTRON_TARGET_LANG", str),
}
config = {}
for key, (env, kind) in mapping.items():
    raw = os.environ.get(env, "")
    if raw == "":
        continue
    config[key] = kind(raw)
print(json.dumps(config, separators=(",", ":")))
PY
    )"
fi

echo "Nemotron backend config: ${BACKEND_CONFIG_JSON}"

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
if [[ -n "${BOARD_ACK_JSONL}" ]]; then
    AUTOMATION_ARGS+=" --board-ack-jsonl '${BOARD_ACK_JSONL}'"
fi
AUTOMATION_ARGS+=" --subtitle-ready-timeout '${SUBTITLE_READY_TIMEOUT}'"
if [[ "${SINGLE_SESSION}" == "1" ]]; then
    AUTOMATION_ARGS+=" --single-session"
fi
if [[ -n "${BACKEND_CONFIG_JSON}" && "${BACKEND_CONFIG_JSON}" != "{}" ]]; then
    # Windows PowerShell strips embedded JSON quotes while constructing argv for
    # native Python. URL-safe Base64 crosses WSL -> PowerShell without quoting
    # loss; stt_stream_bridge.py decodes and validates the original JSON.
    BACKEND_CONFIG_BASE64="$(
        BACKEND_CONFIG_JSON="${BACKEND_CONFIG_JSON}" python3 - <<'PY'
import base64
import os

payload = os.environ["BACKEND_CONFIG_JSON"].encode("utf-8")
print(base64.urlsafe_b64encode(payload).decode("ascii"))
PY
    )"
    AUTOMATION_ARGS+=" --backend-config-base64 '${BACKEND_CONFIG_BASE64}'"
fi

BRIDGE_COMMAND="Set-Location -LiteralPath '${REPO_WIN_PATH}'; python -m server.runtime.bridge --stream-url '${STT_STREAM_URL}' --host 0.0.0.0 --port ${AUDIO_PORT} --jsonl '${JSONL}'${SAVE_WAV:+ --save-wav '${SAVE_WAV}'} --send-subtitles --subtitle-host ${SUBTITLE_HOST} --subtitle-port ${SUBTITLE_PORT}${AUTOMATION_ARGS}"

if [[ "${FOREGROUND}" == "1" ]]; then
    powershell.exe -NoProfile -Command "${BRIDGE_COMMAND}"
else
    powershell.exe -NoProfile -Command "Start-Process powershell.exe -ArgumentList @('-NoExit','-Command',\"${BRIDGE_COMMAND}\")"
fi
