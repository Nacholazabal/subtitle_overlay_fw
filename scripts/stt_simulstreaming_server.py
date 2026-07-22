#!/usr/bin/env python3
"""FastAPI / WebSocket server exposing the SimulStreaming AlignAtt backend.

Implements the *same* contract the faster-whisper streaming server does so the
existing bridge, session protocol and firmware ACK path are reused verbatim:

    GET  /health          readiness + provenance (loading|warming_up|ready|failed)
    POST /stt/offline      complete-file pseudo-reference transcription
    WS   /stt/stream       per-connection online AlignAtt session

Only the STT engine changes. ``fastapi``/``uvicorn``/``torch``/SimulStreaming are
imported lazily so this module imports in the WSL test environment; the state
machine and session manager are plain classes exercised by unit tests with fakes.

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

from scripts.stt_simulstreaming_backend import (
    MODEL_SHA256,
    RUN_ENGINE,
    TARGET_RATE,
    UPSTREAM_COMMIT,
    SharedSimulModel,
    SimulStreamingConfig,
    SimulStreamingSession,
    pcm_s16le_to_float32,
    transcribe_offline_float32,
)
from scripts.stt_stream_protocol import (
    MESSAGE_ERROR,
    MESSAGE_PING,
    MESSAGE_PONG,
    MESSAGE_SESSION_END,
    MESSAGE_SESSION_READY,
    MESSAGE_SESSION_SUMMARY,
    MESSAGE_TRANSCRIPT,
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


@dataclass
class ServerConfig:
    host: str = "0.0.0.0"
    port: int = 8765
    device: str = "cuda"
    model_path: str = ""
    warmup_sec: float = 1.0
    backend: SimulStreamingConfig = field(default_factory=SimulStreamingConfig)


def _sanitize_error(exc: BaseException) -> str:
    """Human-useful but path/secret-light error string for /health failed state."""
    text = f"{type(exc).__name__}: {exc}"
    home = os.path.expanduser("~")
    if home and home != "/":
        text = text.replace(home, "~")
    return text[:600]


class BackendState:
    """Loading/warming/ready/failed state machine for the shared model.

    ``run_loader`` is what a background thread calls; it is injectable so tests can
    drive every transition without torch. On success ``shared_model`` is set and
    the state is ``ready``; on any exception the state is ``failed`` with a
    sanitized description, and the server stops printing "connection refused"
    forever — /health reports the real failure."""

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

    def _default_loader(self):
        model = SharedSimulModel(self.config.backend)
        self._set_status(STATE_WARMING_UP)
        model.warmup(self.config.warmup_sec)
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
            print(f"SimulStreaming model load FAILED:\n{detail}", flush=True)

    def start_background(self):
        thread = threading.Thread(target=self.run_loader, name="simulstreaming-loader", daemon=True)
        thread.start()
        return thread

    def health_payload(self) -> dict:
        status = self.status
        backend = self.config.backend
        payload = {
            # ``status: ok`` keeps the faster-whisper-era health_check() happy while
            # ``ready``/``run_engine`` are the SimulStreaming-specific truth.
            "status": "ok" if status == STATE_READY else status,
            "ready": status == STATE_READY,
            "state": status,
            "run_engine": RUN_ENGINE,
            "model": backend.model,
            "language": backend.language,
            "task": backend.task,
            "device": self.config.device,
            "upstream_commit": UPSTREAM_COMMIT,
            "model_sha256": MODEL_SHA256,
            "effective_config": backend.as_effective_config(),
            "run_config": backend.run_config(realtime=True, transport="websocket"),
            "uptime_sec": round(time.monotonic() - self.started_monotonic, 2),
        }
        if status == STATE_FAILED:
            payload["error"] = self._error
            payload["error_detail"] = (self._error_detail or "")[:4000]
        return payload


class SingleSessionManager:
    """At most one active GPU session. A second concurrent connection is rejected
    with a clear busy error rather than silently sharing mutable AlignAtt state."""

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

    Colab has ffmpeg; this keeps the offline path independent of torchaudio."""
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
    shared_model: SharedSimulModel,
    audio_bytes: bytes,
    filename: str,
    config: SimulStreamingConfig,
    *,
    decode_fn=decode_audio_bytes_to_float32,
) -> dict:
    started = time.monotonic()
    audio = decode_fn(audio_bytes)
    finals = transcribe_offline_float32(shared_model, audio, TARGET_RATE)
    segments = [
        {
            "start_sec": event.get("start_sec"),
            "end_sec": event.get("end_sec"),
            "text": event.get("text", ""),
        }
        for event in finals
        if event.get("text")
    ]
    text = " ".join(segment["text"] for segment in segments).strip()
    result_config = config.run_config(realtime=False, transport="http_offline")
    duration = round(len(audio) / float(TARGET_RATE), 3) if hasattr(audio, "__len__") else None
    return {
        "filename": filename,
        "text": text,
        "segments": segments,
        "inference_sec": round(time.monotonic() - started, 3),
        "audio_duration_sec": duration,
        "detected_language": config.language,
        "run_engine": RUN_ENGINE,
        "upstream_commit": UPSTREAM_COMMIT,
        "model_sha256": MODEL_SHA256,
        "config": result_config,
        "reference_kind": "offline_proxy",
    }


# --- WebSocket session driver (framework-agnostic core) --------------------


def session_config_from_start(base: SimulStreamingConfig, session_start: dict) -> SimulStreamingConfig:
    overrides = validate_backend_config(session_start.get("backend_config"))
    # Never let a client swap the loaded checkpoint at session time.
    overrides = {key: value for key, value in overrides.items() if key not in ("model", "cif_ckpt_path")}
    return SimulStreamingConfig.from_overrides(overrides, base)


def session_ready_message(session_start: dict, config: SimulStreamingConfig) -> dict:
    return {
        "type": MESSAGE_SESSION_READY,
        "version": session_start.get("version", 1),
        "sample_rate_hz": session_start.get("sample_rate_hz", TARGET_RATE),
        "run_engine": RUN_ENGINE,
        "run_config": config.run_config(realtime=True, transport="websocket"),
    }


def create_app(config: ServerConfig, *, backend_state: BackendState | None = None):
    from fastapi import FastAPI, HTTPException, Request, WebSocket, WebSocketDisconnect

    app = FastAPI(title="Subtitle Overlay SimulStreaming STT", version="1.0")
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
            f"SimulStreaming server starting: engine={RUN_ENGINE} model={config.backend.model} "
            f"upstream={UPSTREAM_COMMIT[:12]} device={config.device}",
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
            overrides = {k: v for k, v in overrides.items() if k not in ("model", "cif_ckpt_path")}
            offline_config = SimulStreamingConfig.from_overrides(overrides, config.backend)
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
            online = state.shared_model.build_online()
            session = SimulStreamingSession(
                online, session_config, source_rate=int(session_start.get("sample_rate_hz", TARGET_RATE))
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
        except Exception as exc:  # noqa: BLE001 - report then propagate
            try:
                await websocket.send_text(
                    encode_json_message({"type": MESSAGE_ERROR, "message": _sanitize_error(exc)})
                )
            except Exception:
                pass
            print(f"SimulStreaming session error: {exc}", flush=True)
        finally:
            app.state.sessions.release()

    return app


def parse_args():
    parser = argparse.ArgumentParser(description="Run the SimulStreaming AlignAtt STT server")
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--device", default="cuda")
    parser.add_argument("--model-path", required=True, help="path to the OpenAI Whisper small .pt checkpoint")
    parser.add_argument("--language", default="es")
    parser.add_argument("--task", default="transcribe", choices=["transcribe", "translate"])
    parser.add_argument("--min-chunk-sec", type=float, default=1.0)
    parser.add_argument("--beams", type=int, default=1)
    parser.add_argument("--vac", dest="use_vac", action="store_true", default=True)
    parser.add_argument("--no-vac", dest="use_vac", action="store_false")
    parser.add_argument("--frame-threshold", type=int, default=25)
    parser.add_argument("--audio-max-len", type=float, default=30.0)
    parser.add_argument("--audio-min-len", type=float, default=0.0)
    parser.add_argument("--never-fire", action="store_true", default=False)
    parser.add_argument("--cif-ckpt-path", default="")
    parser.add_argument("--warmup-sec", type=float, default=1.0)
    args = parser.parse_args()
    backend = SimulStreamingConfig(
        model="small",
        language=args.language,
        task=args.task,
        min_chunk_sec=args.min_chunk_sec,
        beams=args.beams,
        use_vac=args.use_vac,
        frame_threshold=args.frame_threshold,
        audio_max_len=args.audio_max_len,
        audio_min_len=args.audio_min_len,
        never_fire=args.never_fire,
        cif_ckpt_path=args.cif_ckpt_path,
        model_path=args.model_path,
    )
    return ServerConfig(
        host=args.host,
        port=args.port,
        device=args.device,
        model_path=args.model_path,
        warmup_sec=args.warmup_sec,
        backend=backend,
    )


def main():
    import uvicorn

    config = parse_args()
    uvicorn.run(create_app(config), host=config.host, port=config.port)


if __name__ == "__main__":
    main()
