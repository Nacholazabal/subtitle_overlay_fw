#!/usr/bin/env python3
"""FastAPI / WebSocket server exposing the Nemotron 3.5 + NeMo streaming backend.

Implements the *same* contract as the faster-whisper and SimulStreaming
streaming servers so the existing bridge, session protocol and firmware ACK path
are reused verbatim:

    GET  /health          readiness + provenance (loading|warming_up|ready|failed)
    POST /stt/offline      complete-file pseudo-reference transcription
    WS   /stt/stream       per-connection online cache-aware NeMo session

Only the STT engine changes. ``fastapi``/``uvicorn``/``torch``/NeMo are imported
lazily so this module imports in the WSL test environment; the state machine and
session manager are plain classes exercised by unit tests with fakes.

NOTE: this module deliberately does NOT use ``from __future__ import annotations``.
FastAPI resolves route handler annotations at request time; with PEP 563 stringized
annotations, the ``request: Request`` parameter (whose ``Request`` type is imported
locally inside ``create_app``) cannot be resolved and FastAPI mis-treats it as a
required query parameter (HTTP 422). Concrete annotations avoid that.
"""

import argparse
import asyncio
import json
import os
import subprocess
import sys
import tempfile
import threading
import time
import traceback
from dataclasses import dataclass, field
from pathlib import Path


sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from scripts.stt_nemotron_backend import (
    MODEL_ID,
    NEMO_COMMIT,
    NEMO_REPO,
    RUN_ENGINE,
    TARGET_RATE,
    NemotronConfig,
    SharedNemotronModel,
    runtime_provenance,
    transcribe_offline_float32,
)
from scripts.stt_simulstreaming_backend import pcm_s16le_to_float32
from scripts.stt_stream_protocol import (
    MESSAGE_ERROR,
    MESSAGE_PING,
    MESSAGE_PONG,
    MESSAGE_SESSION_END,
    MESSAGE_SESSION_READY,
    MESSAGE_SESSION_SUMMARY,
    decode_audio_frame,
    decode_json_message,
    encode_json_message,
    make_error,
    validate_backend_config,
)


STATE_LOADING = "loading"
STATE_WARMING_UP = "warming_up"
STATE_READY = "ready"
STATE_FAILED = "failed"

# Session-level overrides a client may NOT set: they would swap the loaded
# checkpoint or the decoder family out from under a shared, already-warmed model.
LOCKED_OVERRIDES = ("model_id", "decoder_type", "compute_dtype", "use_amp", "device", "device_id")


@dataclass
class ServerConfig:
    host: str = "0.0.0.0"
    port: int = 8765
    device: str = "cuda"
    warmup_sec: float = 1.0
    warmup_audio_path: str | None = None
    warmup_speech_sec: float = 12.0
    backend: NemotronConfig = field(default_factory=NemotronConfig)


def _sanitize_error(exc: BaseException) -> str:
    """Human-useful but path/secret-light error string for /health failed state."""
    text = f"{type(exc).__name__}: {exc}"
    home = os.path.expanduser("~")
    if home and home != "/":
        text = text.replace(home, "~")
    return text[:600]


class BackendState:
    """Loading/warming/ready/failed state machine for the shared pipeline.

    ``run_loader`` is what a background thread calls; it is injectable so tests can
    drive every transition without torch or NeMo. /health only reports ready once
    the checkpoint is loaded, moved to CUDA, pinned to the configured language and
    attention context, warmed up with a real decode, and proven able to create a
    session. On any exception the state is ``failed`` with a sanitized description
    plus a useful traceback, so nothing is hidden behind an endless
    "connection refused"."""

    def __init__(self, config: ServerConfig, loader=None):
        self.config = config
        self._loader = loader or self._default_loader
        self._lock = threading.Lock()
        self._status = STATE_LOADING
        self._error = None
        self._error_detail = None
        self.shared_model = None
        self.started_monotonic = time.monotonic()
        self.ready_monotonic = None
        self.load_sec = None
        self.warmup_summary = None

    def _default_loader(self):
        started = time.monotonic()
        model = SharedNemotronModel(self.config.backend)
        self._set_status(STATE_WARMING_UP)
        speech_audio = None
        if self.config.warmup_audio_path:
            canary_path = Path(self.config.warmup_audio_path).expanduser()
            if not canary_path.is_file():
                raise FileNotFoundError(f"Nemotron streaming canary not found: {canary_path}")
            speech_audio = decode_audio_bytes_to_float32(canary_path.read_bytes())
            max_samples = int(TARGET_RATE * max(float(self.config.warmup_speech_sec), 0.1))
            speech_audio = speech_audio[:max_samples]
        self.warmup_summary = model.warmup(self.config.warmup_sec, speech_audio=speech_audio)
        # Readiness also means "a session can actually be created".
        model.build_session(source_rate=TARGET_RATE)
        self.load_sec = round(time.monotonic() - started, 2)
        return model

    def _set_status(self, status):
        with self._lock:
            self._status = status

    @property
    def status(self):
        with self._lock:
            return self._status

    def is_ready(self):
        return self.status == STATE_READY

    def run_loader(self):
        """Synchronously execute the loader and record the outcome. Meant to be run
        on a background thread from ``start_background``."""
        try:
            self._set_status(STATE_LOADING)
            model = self._loader()
            with self._lock:
                self.shared_model = model
                self._status = STATE_READY
                self.ready_monotonic = time.monotonic()
        except BaseException as exc:  # noqa: BLE001 - surfaced via /health
            detail = "".join(traceback.format_exception(type(exc), exc, exc.__traceback__))
            with self._lock:
                self._status = STATE_FAILED
                self._error = _sanitize_error(exc)
                self._error_detail = detail
            print(f"Nemotron model load FAILED:\n{detail}", flush=True)

    def start_background(self):
        thread = threading.Thread(target=self.run_loader, name="nemotron-loader", daemon=True)
        thread.start()
        return thread

    def provenance_payload(self) -> dict:
        if self.shared_model is not None:
            try:
                return self.shared_model.provenance()
            except Exception as exc:  # noqa: BLE001 - provenance must not break /health
                return {"provenance_error": _sanitize_error(exc)}
        provenance = {
            "run_engine": RUN_ENGINE,
            "model_id": self.config.backend.model_id,
            "nemo_commit": NEMO_COMMIT,
            "nemo_repo": NEMO_REPO,
            "target_lang": self.config.backend.target_lang,
            "decoder_type": self.config.backend.decoder_type,
            "att_context_size": list(self.config.backend.att_context_size),
            "lookahead_ms": self.config.backend.latency_ms,
            "sample_rate_hz": TARGET_RATE,
        }
        provenance.update(runtime_provenance())
        return provenance

    def health_payload(self) -> dict:
        status = self.status
        backend = self.config.backend
        payload = {
            # ``status: ok`` keeps the faster-whisper-era health_check() happy while
            # ``ready``/``run_engine`` are the Nemotron-specific truth.
            "status": "ok" if status == STATE_READY else status,
            "ready": status == STATE_READY,
            "state": status,
            "run_engine": RUN_ENGINE,
            "model": backend.model_id,
            "language": backend.target_lang,
            "task": "transcribe",
            "device": self.config.device,
            "nemo_commit": NEMO_COMMIT,
            "effective_config": backend.as_effective_config(),
            "run_config": backend.run_config(realtime=True, transport="websocket"),
            "provenance": self.provenance_payload(),
            "uptime_sec": round(time.monotonic() - self.started_monotonic, 2),
        }
        if self.load_sec is not None:
            payload["model_load_sec"] = self.load_sec
        if self.warmup_summary is not None:
            payload["streaming_canary"] = self.warmup_summary
        if status == STATE_FAILED:
            payload["error"] = self._error
            payload["error_detail"] = (self._error_detail or "")[:4000]
        return payload


class SingleSessionManager:
    """At most one active GPU session. A second concurrent connection is rejected
    with a clear busy error rather than silently sharing mutable NeMo cache state."""

    def __init__(self):
        self._lock = threading.Lock()
        self._active = False

    def try_acquire(self) -> bool:
        with self._lock:
            if self._active:
                return False
            self._active = True
            return True

    def release(self) -> None:
        with self._lock:
            self._active = False

    @property
    def busy(self) -> bool:
        with self._lock:
            return self._active


# --- Offline (complete file) path ------------------------------------------


def decode_audio_bytes_to_float32(audio_bytes: bytes, *, ffmpeg: str = "ffmpeg"):
    """Convert an arbitrary media upload to mono float32 @ 16 kHz via ffmpeg.

    Offline only — the live WebSocket path never shells out to ffmpeg."""
    if not audio_bytes:
        raise ValueError("empty audio upload")
    with tempfile.NamedTemporaryFile(suffix=".input", delete=True) as handle:
        handle.write(audio_bytes)
        handle.flush()
        proc = subprocess.run(
            [ffmpeg, "-hide_banner", "-loglevel", "error", "-i", handle.name,
             "-f", "s16le", "-ac", "1", "-ar", str(TARGET_RATE), "-"],
            capture_output=True,
            check=False,
        )
    if proc.returncode != 0:
        raise RuntimeError(f"ffmpeg decode failed: {proc.stderr.decode('utf-8', 'replace')[:400]}")
    return pcm_s16le_to_float32(proc.stdout)


def run_offline_transcription(
    shared_model,
    audio_bytes: bytes,
    filename: str,
    config: NemotronConfig,
    *,
    decode_fn=decode_audio_bytes_to_float32,
    transcribe_fn=transcribe_offline_float32,
) -> dict:
    started = time.monotonic()
    audio = decode_fn(audio_bytes)
    result = transcribe_fn(shared_model, audio, config)
    segments = result.get("segments") or []
    duration = round(len(audio) / float(TARGET_RATE), 3) if hasattr(audio, "__len__") else None
    payload = {
        "filename": filename,
        "text": result.get("text", ""),
        "inference_sec": round(time.monotonic() - started, 3),
        "audio_duration_sec": duration,
        "detected_language": config.target_lang,
        "configured_language": config.target_lang,
        "run_engine": RUN_ENGINE,
        "nemo_commit": NEMO_COMMIT,
        "config": config.run_config(realtime=False, transport="http_offline"),
        # Same-engine automatic reference, NOT a verified human transcript.
        "reference_kind": "offline_proxy",
    }
    if segments:
        payload["segments"] = segments
        payload["segments_source"] = "nemo_hypothesis_timestamps"
    else:
        # NeMo did not return timestamps for this configuration; do not fabricate any.
        payload["segments"] = []
        payload["segments_source"] = "unavailable"
    return payload


# --- WebSocket session driver ----------------------------------------------


def session_config_from_start(base: NemotronConfig, session_start: dict) -> NemotronConfig:
    overrides = validate_backend_config(session_start.get("backend_config"))
    overrides = {key: value for key, value in overrides.items() if key not in LOCKED_OVERRIDES}
    return NemotronConfig.from_overrides(overrides, base)


def session_ready_message(session_start: dict, config: NemotronConfig) -> dict:
    return {
        "type": MESSAGE_SESSION_READY,
        "version": session_start.get("version", 1),
        "sample_rate_hz": session_start.get("sample_rate_hz", TARGET_RATE),
        "run_engine": RUN_ENGINE,
        "run_config": config.run_config(realtime=True, transport="websocket"),
    }


def prepare_shared_model(shared_model, config: NemotronConfig):
    """Apply the session's pipeline-global settings before opening its stream.

    The server runs one session at a time, so reinitializing the cache-aware
    context and official endpointer here cannot race another stream."""
    if getattr(shared_model, "config", None) is not None:
        shared_model.configure_streaming(config)
    return shared_model


def create_app(config: ServerConfig, *, backend_state=None):
    from fastapi import FastAPI, HTTPException, Request, WebSocket, WebSocketDisconnect

    app = FastAPI(title="Subtitle Overlay Nemotron STT", version="1.0")
    app.state.config = config
    app.state.backend_state = backend_state or BackendState(config)
    app.state.sessions = SingleSessionManager()

    @app.on_event("startup")
    async def startup():
        # If a caller (e.g. the Colab notebook) already loaded and warmed the model
        # synchronously before creating the app, do not reload it.
        if not app.state.backend_state.is_ready():
            app.state.backend_state.start_background()
        print(
            f"Nemotron server starting: engine={RUN_ENGINE} model={config.backend.model_id} "
            f"nemo={NEMO_COMMIT[:12]} lang={config.backend.target_lang} "
            f"lookahead={config.backend.latency_ms}ms device={config.device}",
            flush=True,
        )

    @app.get("/health")
    async def health():
        return app.state.backend_state.health_payload()

    @app.post("/stt/offline")
    async def stt_offline(request: Request):
        state = app.state.backend_state
        if not state.is_ready():
            raise HTTPException(status_code=503, detail=f"backend not ready: {state.status}")
        audio_bytes = await request.body()
        if not audio_bytes:
            raise HTTPException(status_code=400, detail="empty audio upload")
        if len(audio_bytes) > 64 * 1024 * 1024:
            raise HTTPException(status_code=413, detail="audio upload exceeds 64 MiB")
        filename = request.headers.get("x-audio-filename", "audio")
        try:
            raw_overrides = request.headers.get("x-stt-backend-config")
            overrides = validate_backend_config(json.loads(raw_overrides)) if raw_overrides else {}
            overrides = {k: v for k, v in overrides.items() if k not in LOCKED_OVERRIDES}
            offline_config = NemotronConfig.from_overrides(overrides, config.backend)
            if not app.state.sessions.try_acquire():
                raise HTTPException(status_code=409, detail="server busy: a session is already active")
            try:
                return await asyncio.to_thread(
                    run_offline_transcription,
                    state.shared_model,
                    audio_bytes,
                    filename,
                    offline_config,
                )
            finally:
                app.state.sessions.release()
        except (json.JSONDecodeError, ValueError) as exc:
            raise HTTPException(status_code=400, detail=str(exc)) from exc

    @app.websocket("/stt/stream")
    async def stt_stream(websocket: WebSocket):
        await websocket.accept()
        state = app.state.backend_state
        if not state.is_ready():
            await websocket.send_text(encode_json_message(make_error(f"backend not ready: {state.status}")))
            await websocket.close()
            return
        if not app.state.sessions.try_acquire():
            await websocket.send_text(
                encode_json_message({"type": MESSAGE_ERROR, "message": "server busy", "busy": True})
            )
            await websocket.close()
            return

        session = None
        try:
            first = await websocket.receive_text()
            session_start = decode_json_message(first)
            session_config = session_config_from_start(config.backend, session_start)
            shared_model = prepare_shared_model(state.shared_model, session_config)
            session = shared_model.build_session(
                session_config, source_rate=int(session_start.get("sample_rate_hz", TARGET_RATE))
            )
            await websocket.send_text(encode_json_message(session_ready_message(session_start, session_config)))

            while True:
                message = await websocket.receive()
                if message.get("type") == "websocket.disconnect":
                    break
                if message.get("bytes") is not None:
                    frame = decode_audio_frame(message["bytes"])
                    for event in await asyncio.to_thread(session.push_pcm, frame.payload):
                        event["server_sent_monotonic"] = round(time.monotonic(), 6)
                        await websocket.send_text(encode_json_message(event))
                    continue
                if message.get("text") is not None:
                    control = decode_json_message(message["text"])
                    msg_type = control.get("type")
                    if msg_type == MESSAGE_PING:
                        await websocket.send_text(encode_json_message({"type": MESSAGE_PONG}))
                    elif msg_type == MESSAGE_SESSION_END:
                        for event in await asyncio.to_thread(session.flush):
                            event["server_sent_monotonic"] = round(time.monotonic(), 6)
                            await websocket.send_text(encode_json_message(event))
                        await websocket.send_text(
                            encode_json_message({"type": MESSAGE_SESSION_SUMMARY, **session.stats_snapshot()})
                        )
                        break
                    else:
                        await websocket.send_text(
                            encode_json_message(make_error(f"unexpected message: {msg_type}"))
                        )
        except WebSocketDisconnect:
            pass
        except Exception as exc:  # noqa: BLE001 - report then continue serving
            try:
                await websocket.send_text(
                    encode_json_message({"type": MESSAGE_ERROR, "message": _sanitize_error(exc)})
                )
            except Exception:
                pass
            print(f"Nemotron session error: {exc}", flush=True)
        finally:
            if session is not None:
                try:
                    session.close()
                except Exception as exc:  # noqa: BLE001
                    print(f"Nemotron session close error: {exc}", flush=True)
            app.state.sessions.release()

    return app


def parse_args():
    parser = argparse.ArgumentParser(description="Run the Nemotron 3.5 / NeMo streaming STT server")
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--device", default="cuda")
    parser.add_argument("--device-id", type=int, default=0)
    parser.add_argument("--model-id", default=MODEL_ID)
    parser.add_argument("--target-lang", default="es-ES")
    parser.add_argument("--latency-ms", type=int, default=320)
    parser.add_argument("--stop-history-eou-ms", type=int, default=800)
    parser.add_argument("--residue-tokens-at-end", type=int, default=2)
    parser.add_argument("--compute-dtype", default="float32", choices=["float32", "float16", "bfloat16"])
    parser.add_argument("--no-amp", dest="use_amp", action="store_false", default=True)
    parser.add_argument("--asr-output-granularity", default="segment", choices=["segment", "word"])
    parser.add_argument("--warmup-sec", type=float, default=1.0)
    parser.add_argument(
        "--warmup-audio",
        default=None,
        help="spoken audio used to prove the live pipeline emits text before readiness",
    )
    parser.add_argument("--warmup-speech-sec", type=float, default=12.0)
    args = parser.parse_args()
    backend = NemotronConfig(
        model_id=args.model_id,
        target_lang=args.target_lang,
        latency_ms=args.latency_ms,
        stop_history_eou_ms=args.stop_history_eou_ms,
        residue_tokens_at_end=args.residue_tokens_at_end,
        asr_output_granularity=args.asr_output_granularity,
        compute_dtype=args.compute_dtype,
        use_amp=args.use_amp,
        device=args.device,
        device_id=args.device_id,
    )
    return ServerConfig(
        host=args.host,
        port=args.port,
        device=args.device,
        warmup_sec=args.warmup_sec,
        warmup_audio_path=args.warmup_audio,
        warmup_speech_sec=args.warmup_speech_sec,
        backend=backend,
    )


def main():
    import uvicorn

    config = parse_args()
    uvicorn.run(create_app(config), host=config.host, port=config.port)


if __name__ == "__main__":
    main()
