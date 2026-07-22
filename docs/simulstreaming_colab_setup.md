# SimulStreaming / AlignAtt experimental STT backend

> Experimental alternative STT engine for the live-subtitle thesis. It swaps
> **only** the speech-to-text engine running in Colab. Everything else — board
> audio capture, the WebSocket bridge, the session/ACK protocol, the firmware
> overlay, the short-audio bench and its reports — is reused unchanged.

## 1. Goal of the experiment

Evaluate the [SimulStreaming](https://github.com/ufal/SimulStreaming) AlignAtt
streaming policy (PyTorch Whisper + AlignAtt + VAC) against the current
faster-whisper streaming backend, on the same three short clips, with the same
end-to-end pipeline and the same ~1.5 s latency target. SimulStreaming owns its
own incremental processing, VAC endpointing, AlignAtt commit decisions, segment
finalization and last-word truncation — so we can attribute results to *that*
algorithm rather than to our previous chunk/VAD segmentation.

## 2. Architecture

```
board audio (PCM over TCP)
  -> stt_stream_bridge.py            (unchanged bridge, on the PC/WSL)
  -> WebSocket over HTTPS/ngrok
  -> stt_simulstreaming_server.py    (NEW FastAPI server in Colab)
       - GET  /health                readiness + provenance
       - POST /stt/offline           complete-file pseudo-reference
       - WS   /stt/stream            per-connection AlignAtt session
  -> transcript protocol (session_start / session_ready / audio frames /
     transcript / session_end / session_summary / ping-pong / error)
  -> bridge -> subtitle TCP -> firmware + ACKs
  -> HDMI overlay (reconstructed logically from events; see the ACK caveat)
```

The SimulStreaming server implements the **existing** contract, so the bridge and
firmware do not know which engine is behind the WebSocket. We deliberately do
**not** connect SimulStreaming's own example TCP server to the board, and we do
**not** wrap SimulStreaming inside the faster-whisper `ChunkTranscriber`.

## 3. Differences vs faster-whisper

| Aspect | faster-whisper backend | SimulStreaming backend |
|---|---|---|
| Engine | faster-whisper / CTranslate2 | PyTorch Whisper + AlignAtt |
| Segmentation | our chunk window + Silero VAD | SimulStreaming VAC + AlignAtt |
| Incremental commit | re-transcribe growing window | AlignAtt attention-guided commit |
| Last-word truncation | n/a | CIF / `never_fire` controlled |
| `run_engine` in `/health` | `stream_server` | `simulstreaming_alignatt` |
| Launcher | `run_stt_colab_stream.sh` | `run_stt_colab_simulstream.sh` |
| Bench profile | `faster_whisper` (default) | `simulstreaming_alignatt` |
| Session tuning on the wire | `config_overrides` (whitelisted) | `backend_config` (generic JSON) |

The faster-whisper path is untouched: `./scripts/audiotestshort.sh` behaves
exactly as before.

## 4. What you need on Google Drive

```
/content/drive/MyDrive/TESIS/simulstreaming/
  models/whisper-small.pt          # OpenAI Whisper multilingual small .pt
  audio/desay-short.webm
  audio/noticiero-short.webm
  audio/rel-short.webm
  results/                         # standalone notebook outputs (optional)
```

### Model

- **OpenAI Whisper multilingual `small`** PyTorch checkpoint — **not**
  faster-whisper, **not** a CTranslate2 directory, **not** `small.en`.
- Expected SHA-256: `9ecf779972d90ba49c06d968637d720dd632c55bbf19d441fb42bf17a411e794`
- The notebook validates the SHA-256 before loading and refuses a directory, a
  non-`.pt` file, or a checksum mismatch.
- If the file is missing, notebook cell 6 downloads the official checkpoint once,
  validates it, and copies it to Drive. It never downloads silently on every run.

## 5. Upstream pin

- Repo: `https://github.com/ufal/SimulStreaming`
- Pinned commit: **`077ea37d5ab4ff98bc567e4507f140dc4e5d5ad6`** (full SHA), recorded
  in the notebook, `/health`, the run manifest and every report.

## 6. Running the Colab notebook

Open `scripts/colab_simulstreaming_server.ipynb`, set **Runtime → GPU**, then
**Run all**. The cells, in order: install deps → mount Drive → clone this repo
(`dev/simulstream`) + SimulStreaming (checkout the pinned SHA) → GPU/version check
→ validate checkpoint (SHA-256) → verify the three audios → **load model + real
warm-up (synchronous)** → start FastAPI/Uvicorn in the background → wait for
`/health` `ready` → open ngrok **only after** readiness → print backend, model,
device, upstream SHA, checkpoint SHA, params, HTTP URL and WebSocket URL → keep
running.

Readiness is never declared just because uvicorn started: the model must be
loaded and the warm-up decode finished. If loading fails, the full exception is
printed and the server is not started.

## 7. `/health` contract

```json
{
  "ready": true,
  "state": "ready",                 // loading | warming_up | ready | failed
  "run_engine": "simulstreaming_alignatt",
  "model": "small",
  "language": "es",
  "device": "cuda",
  "upstream_commit": "077ea37d5ab4ff98bc567e4507f140dc4e5d5ad6",
  "model_sha256": "9ecf7799...",
  "effective_config": { "...": "..." }
}
```

In the `failed` state, `/health` also returns a sanitized `error` string and an
`error_detail` traceback (home path stripped).

## 8. Running the WSL test

```bash
# default: the project static ngrok tunnel
./scripts/audiotestsimulstream.sh

# override the endpoint printed by the notebook
STT_STREAM_URL="wss://.../stt/stream" ./scripts/audiotestsimulstream.sh

# parameter sweep (only after one normal run works)
./scripts/audiotestsimulstream.sh --sweep

# variations
./scripts/audiotestsimulstream.sh --offline-only
./scripts/audiotestsimulstream.sh --refresh-offline
./scripts/audiotestsimulstream.sh --sweep-file scripts/sweeps/simulstreaming_initial.json
```

The bench first calls `/health` and aborts **before playing any audio** if
`run_engine` is not `simulstreaming_alignatt` — so you never measure the wrong
backend. It then builds offline proxies, launches the bridge, waits for real
readiness, waits 6 s, plays the three clips in order with 6 s gaps (and after the
last), drains cleanly, analyzes, prints a summary and writes
`logs/audio-tests/<timestamp>/report.md` + `report.json`. Runs never overwrite
each other.

## 9. Session parameters

Carried as a single generic `backend_config` JSON on the wire (not dozens of
bridge flags). Server-validated by `SimulStreamingConfig.from_overrides`.

| Param | Default | Meaning |
|---|---|---|
| `model` | `small` | fixed for this experiment |
| `language` | `es` | |
| `task` | `transcribe` | |
| `min_chunk_sec` | `1.0` | minimum audio before processing (upstream `min-chunk-size`) |
| `beams` | `1` | greedy when 1, beam search when >1 |
| `use_vac` | `true` | Voice Activity Controller (recommended upstream) |
| `frame_threshold` | `25` | AlignAtt tail margin, **in encoder frames** |
| `audio_max_len` | `30.0` | max audio buffer seconds |
| `audio_min_len` | `0.0` | skip processing below this buffer length |
| `never_fire` | `false` | if true, the last word is never truncated |

### `frame_threshold` unit (verified)

The Whisper audio frontend is identical across model sizes: 16 kHz → 80-mel with
hop 160 (100 mel frames/s) → conv stride 2 → **50 encoder frames/s**, i.e.
**1 frame = 0.02 s**. Upstream documents this for `large-v3`; it is the same for
`small`. So `frame_threshold` is a tail margin measured in 0.02 s units for the
`small` model too:

- `frame_threshold = 25` → 0.50 s margin (upstream CLI default)
- `frame_threshold = 12` → 0.24 s margin (the paper's tighter margin)

Upstream defaults verified at the pinned commit: `frame_threshold` CLI default 25,
`beams` 1, `audio_max_len` 30, `never_fire` false, VAC recommended.

## 10. Metrics and reports

Each run records: `run_engine`, upstream commit, model + SHA-256, effective config,
input paths, timestamps, adapted transcript events, ACKs, logs, offline references,
per-clip and global metrics, and the `offline_proxy` warning.

Kept from the faster-whisper bench: WER, CER, proxy accuracy, latency p50/p90/max
and % under 1.5 s, readability, reliability, events generated/sent/accepted, ACKs,
rejections, lost finals, overlay reconstruction.

Added: latency p95/p99, time-to-first-subtitle, real-time factor, per-event
inference time, update rate, partial stability, partial replacements (text
withdrawn/replaced), VAC endpoints, empty decodes, last-word truncations, and GPU
peak memory when available.

Two things are kept separate and **not** combined into one global score:

1. The **algorithm** on direct digital input inside Colab (the notebook's optional
   offline test and `/stt/offline`).
2. The **full physical pipeline** board → Colab → board (the WSL bench).

Reliability is invalidated by any handshake failure, missing ACK, rejected
transcript, lost final, protocol error, incomplete session or wrong backend.
Internally replaced/skipped partials are **not** counted as "lost audio".

### `offline_proxy` limitation

Until human transcripts exist, WER/CER use an **automatic offline reference**
produced by the same SimulStreaming engine on the complete file. Reports label
this `offline_proxy`; it is **not** a verified human reference and does not claim
real error against a human ground truth. Drop a `<clip>.txt` next to a clip to use
a human reference for that clip automatically.

## 11. Diagnosing errors

- `/health` shows `failed` with a traceback → the model did not load (wrong path,
  bad checksum, OOM). Fix before serving; the server will not spin on "connection
  refused".
- Bench aborts with "requires 'simulstreaming_alignatt'" → you are pointed at the
  faster-whisper server; use the SimulStreaming notebook URL.
- "server busy" over the WebSocket → a session is already active. Only one GPU
  session runs at a time by design; retry after it ends.
- Checkpoint mismatch → you have `small.en`, a faster-whisper directory, a
  CTranslate2 model, or a corrupt download. Re-fetch with cell 6.

## 12. Reverting to the previous backend

Nothing to undo — the faster-whisper path is unchanged. Use
`./scripts/audiotestshort.sh` (profile `faster_whisper`) and the original
`scripts/colab_streaming_server.ipynb`. The two backends keep **separate** offline
caches, so their proxies never collide.

## 13. The ACK caveat

A firmware `accepted` ACK proves the subtitle was received and queued in the
firmware; it does **not** physically certify the HDMI pixels. The overlay in the
reports is reconstructed logically from the transcript events, not captured from
the HDMI output.

## 14. Sources

- SimulStreaming: https://github.com/ufal/SimulStreaming (commit
  `077ea37d5ab4ff98bc567e4507f140dc4e5d5ad6`)
- Whisper small checkpoint SHA-256:
  `9ecf779972d90ba49c06d968637d720dd632c55bbf19d441fb42bf17a411e794`
