---
name: subtitle-review
description: Review the latest STT run by running analyze_run.py and surfacing its compact report.
---

# subtitle-review skill

Review the most recent subtitle overlay STT run. The analysis script does all
heavy lifting; never read raw logs into context.

## How to run

```
python scripts/analyze_run.py [--jsonl logs/stt_events.jsonl] [--wav logs/board_audio.wav]
```

Both flags are optional and default to the paths above. Run from the repo root.
Surface the script's stdout verbatim -- that IS the report. Do not read
`logs/stt_events.jsonl` or `logs/board_audio.wav` into the conversation.

## Where logs live

| File | Contents |
|------|----------|
| `logs/stt_events.jsonl` | NDJSON; one STT event per line (fields: seq, is_final, start_sec, end_sec, text) |
| `logs/board_audio.wav` | Raw board PCM capture, S16_LE mono at 16 kHz |

## Key tunables

| Tunable | Default | Where set |
|---------|---------|-----------|
| `SUBTITLE_CLEAR_TIMEOUT_MS` | 5000 | firmware (hdl/ source) |
| `STT_MAX_WINDOW_SEC` | 4.0 | `scripts/run_stt_windows.sh` |
| `STT_MIN_SILENCE_SEC` | 0.5 | `scripts/run_stt_windows.sh` |

**Constraint:** `SUBTITLE_CLEAR_TIMEOUT_MS` must always be greater than
`STT_MAX_WINDOW_SEC * 1000` (and ideally above the max observed inter-final
gap). If it is not, the board will clear the subtitle box mid-speech. The
script's TIMING section will flag this and print a concrete suggested value.

## Workflow

1. Run `python scripts/analyze_run.py` and read its output.
2. Check each section verdict (OK / ATTENTION / TOO QUIET / CLIPPING).
3. If TIMING says clear-timeout is too low, update `SUBTITLE_CLEAR_TIMEOUT_MS`
   in firmware to the suggested value and rebuild.
4. If AUDIO says TOO QUIET, raise board gain or check AGC settings
   (see `logs/` memory: `board-audio-gain.md`).
5. Use the TRANSCRIPT SAMPLE to spot STT quality issues without dumping all
   events.
