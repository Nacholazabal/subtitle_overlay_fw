#!/usr/bin/env bash
set -euo pipefail

# SimulStreaming / AlignAtt streaming launcher. Same board->PC->Colab->firmware
# path and the same bridge as run_stt_colab_stream.sh; only the Colab STT engine
# differs. Segmentation, VAC and AlignAtt commit decisions run in Colab, so the
# faster-whisper tuning env vars are intentionally NOT forwarded here — the
# SimulStreaming tuning travels as a single generic backend-config JSON.
STT_STREAM_URL="${STT_STREAM_URL:-wss://passage-capacity-wistful.ngrok-free.dev/stt/stream}"

echo "Using SimulStreaming STT URL: $STT_STREAM_URL"

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

# SimulStreaming session config. Either supply the whole JSON object directly via
# STT_BACKEND_CONFIG_JSON, or set individual STT_SIMUL_* vars and let this script
# assemble it. Keys must match SimulStreamingConfig.from_overrides on the server.
BACKEND_CONFIG_JSON="${STT_BACKEND_CONFIG_JSON:-}"
SIMUL_MIN_CHUNK_SEC="${STT_SIMUL_MIN_CHUNK_SEC:-}"
SIMUL_FRAME_THRESHOLD="${STT_SIMUL_FRAME_THRESHOLD:-}"
SIMUL_BEAMS="${STT_SIMUL_BEAMS:-}"
SIMUL_USE_VAC="${STT_SIMUL_USE_VAC:-}"
SIMUL_NEVER_FIRE="${STT_SIMUL_NEVER_FIRE:-}"
SIMUL_AUDIO_MAX_LEN="${STT_SIMUL_AUDIO_MAX_LEN:-}"
SIMUL_AUDIO_MIN_LEN="${STT_SIMUL_AUDIO_MIN_LEN:-}"
SIMUL_LANGUAGE="${STT_SIMUL_LANGUAGE:-}"
SIMUL_TASK="${STT_SIMUL_TASK:-}"

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
mkdir -p "${REPO_DIR}/logs"
REPO_WIN_PATH="$(wslpath -w "${REPO_DIR}")"

if [[ -z "${BACKEND_CONFIG_JSON}" ]]; then
    BACKEND_CONFIG_JSON="$(
        SIMUL_MIN_CHUNK_SEC="${SIMUL_MIN_CHUNK_SEC}" \
        SIMUL_FRAME_THRESHOLD="${SIMUL_FRAME_THRESHOLD}" \
        SIMUL_BEAMS="${SIMUL_BEAMS}" \
        SIMUL_USE_VAC="${SIMUL_USE_VAC}" \
        SIMUL_NEVER_FIRE="${SIMUL_NEVER_FIRE}" \
        SIMUL_AUDIO_MAX_LEN="${SIMUL_AUDIO_MAX_LEN}" \
        SIMUL_AUDIO_MIN_LEN="${SIMUL_AUDIO_MIN_LEN}" \
        SIMUL_LANGUAGE="${SIMUL_LANGUAGE}" \
        SIMUL_TASK="${SIMUL_TASK}" \
        python3 - <<'PY'
import json, os
mapping = {
    "min_chunk_sec": ("SIMUL_MIN_CHUNK_SEC", float),
    "frame_threshold": ("SIMUL_FRAME_THRESHOLD", int),
    "beams": ("SIMUL_BEAMS", int),
    "use_vac": ("SIMUL_USE_VAC", "bool"),
    "never_fire": ("SIMUL_NEVER_FIRE", "bool"),
    "audio_max_len": ("SIMUL_AUDIO_MAX_LEN", float),
    "audio_min_len": ("SIMUL_AUDIO_MIN_LEN", float),
    "language": ("SIMUL_LANGUAGE", str),
    "task": ("SIMUL_TASK", str),
}
config = {}
for key, (env, kind) in mapping.items():
    raw = os.environ.get(env, "")
    if raw == "":
        continue
    if kind == "bool":
        config[key] = raw.strip().lower() in ("1", "true", "yes", "on")
    else:
        config[key] = kind(raw)
print(json.dumps(config, separators=(",", ":")))
PY
    )"
fi

echo "SimulStreaming backend config: ${BACKEND_CONFIG_JSON}"

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
    # Keep JSON quotes intact across WSL -> Windows PowerShell -> native Python.
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

BRIDGE_COMMAND="Set-Location -LiteralPath '${REPO_WIN_PATH}'; python scripts/stt_stream_bridge.py --stream-url '${STT_STREAM_URL}' --host 0.0.0.0 --port ${AUDIO_PORT} --jsonl '${JSONL}'${SAVE_WAV:+ --save-wav '${SAVE_WAV}'} --send-subtitles --subtitle-host ${SUBTITLE_HOST} --subtitle-port ${SUBTITLE_PORT}${AUTOMATION_ARGS}"

if [[ "${FOREGROUND}" == "1" ]]; then
    powershell.exe -NoProfile -Command "${BRIDGE_COMMAND}"
else
    powershell.exe -NoProfile -Command "Start-Process powershell.exe -ArgumentList @('-NoExit','-Command',\"${BRIDGE_COMMAND}\")"
fi
