#!/usr/bin/env bash
set -euo pipefail

# Accuracy-first defaults. `tiny`/`base` garbled Spanish (tesis->tecis,
# transcripcion->"otra ficcion"); `small` fixed almost all of it on the test
# recording. Override via env vars. If inference can't keep up in real time,
# drop to STT_MODEL=base (or tiny).
MODEL="${STT_MODEL:-small}"
# VAD segmentation: phrases are cut at pauses (>= MIN_SILENCE), not at fixed
# intervals, so words are no longer split mid-boundary. MAX_WINDOW forces a cut
# if the speaker never pauses.
MAX_WINDOW_SEC="${STT_MAX_WINDOW_SEC:-4.0}"
MIN_SILENCE_SEC="${STT_MIN_SILENCE_SEC:-0.5}"
# Partials re-transcribe the whole growing window every N seconds, which is ~5x
# real-time work and made a modest PC drop audio constantly. Default OFF
# (finals-only = one transcription per phrase, comfortably real-time). Set
# STT_PARTIAL_SEC=0.7 to re-enable the live word-by-word feel if the PC has spare
# CPU (or use a faster STT_MODEL=base/tiny).
PARTIAL_SEC="${STT_PARTIAL_SEC:-0}"
BEAM_SIZE="${STT_BEAM_SIZE:-5}"
# The board USB soundcard captures ~30x too quiet; 0 = auto-normalize the level
# (fixes it without touching the board). Set a fixed multiplier if you prefer, or
# raise the board capture gain (amixer) for better SNR and set this back to 0.
GAIN="${STT_GAIN:-0}"
SUBTITLE_HOST="${STT_SUBTITLE_HOST:-192.168.1.10}"
SUBTITLE_PORT="${STT_SUBTITLE_PORT:-5001}"
AUDIO_PORT="${STT_AUDIO_PORT:-5000}"
VAD_FILTER="${STT_VAD_FILTER:---vad-filter}"
# Every emitted STT event is also written here so the run can be replayed/debugged
# offline with scripts/subtitle_debug.py. Path is relative to the repo root.
JSONL="${STT_JSONL:-logs/stt_events.jsonl}"
# Exact PCM the board sent, saved for offline accuracy checks (isolates audio
# quality from real-time drops). Set STT_SAVE_WAV= to disable.
SAVE_WAV="${STT_SAVE_WAV:-logs/board_audio.wav}"

REPO_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
mkdir -p "${REPO_DIR}/logs"
REPO_WIN_PATH="$(wslpath -w "${REPO_DIR}")"

powershell.exe -NoProfile -Command "\
Start-Process powershell.exe -ArgumentList @(\
'-NoExit',\
'-Command',\
'Set-Location -LiteralPath ''${REPO_WIN_PATH}''; python scripts/stt_receiver.py --model ${MODEL} --host 0.0.0.0 --port ${AUDIO_PORT} --max-window-sec ${MAX_WINDOW_SEC} --min-silence-sec ${MIN_SILENCE_SEC} --partial-sec ${PARTIAL_SEC} --gain ${GAIN} --beam-size ${BEAM_SIZE} ${VAD_FILTER} --jsonl ''${JSONL}''${SAVE_WAV:+ --save-wav ''${SAVE_WAV}''} --send-subtitles --subtitle-host ${SUBTITLE_HOST} --subtitle-port ${SUBTITLE_PORT}'\
)\
"
