#!/usr/bin/env python3
"""Streaming STT WebSocket server for Colab or a GPU VM."""

import argparse
import asyncio
import json
import sys
import time
from dataclasses import dataclass
from pathlib import Path


sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from scripts.stt_receiver import (
    ChunkTranscriber,
    WHISPER_RATE,
    is_hallucination,
)
from scripts.stt_stream_protocol import (
    AudioAvailabilityTimeline,
    MESSAGE_ERROR,
    MESSAGE_PING,
    MESSAGE_PONG,
    MESSAGE_SESSION_END,
    MESSAGE_SESSION_READY,
    MESSAGE_TRANSCRIPT,
    decode_audio_frame,
    decode_json_message,
    encode_json_message,
    make_error,
    validate_session_start,
)


@dataclass
class ServerConfig:
    host: str = "0.0.0.0"
    port: int = 8765
    model: str = "small"
    device: str = "cuda"
    compute_type: str = "float16"
    language: str = "es"
    beam_size: int = 5
    vad_filter: bool = True
    max_window_sec: float = 3.0
    min_silence_sec: float = 0.35
    partial_sec: float = 0.5
    partial_agreement: int = 1
    gain: float = 0.0
    cpu_threads: int = 0
    event_queue_size: int = 64
    jsonl: str | None = None


class SharedWhisperModel:
    """One loaded faster-whisper model, reused by per-session engines."""

    def __init__(self, config):
        from faster_whisper import WhisperModel

        print(
            f"loading streaming STT model={config.model} device={config.device} "
            f"compute_type={config.compute_type} cpu_threads={config.cpu_threads or 'auto'}",
            flush=True,
        )
        kwargs = {}
        if config.cpu_threads > 0:
            kwargs["cpu_threads"] = config.cpu_threads
        self.model = WhisperModel(
            config.model,
            device=config.device,
            compute_type=config.compute_type,
            **kwargs,
        )
        self.config = config


class SessionWhisperEngine:
    """Per-WebSocket-session sequence counter around the shared model."""

    def __init__(self, shared_model):
        self.shared_model = shared_model
        self.seq = 0

    def transcribe_chunk(self, audio, start_sec, end_sec, is_final):
        config = self.shared_model.config
        segments, _info = self.shared_model.model.transcribe(
            audio,
            language=config.language,
            beam_size=config.beam_size,
            vad_filter=config.vad_filter,
        )
        text = " ".join(segment.text.strip() for segment in segments).strip()
        if not text:
            return None
        if is_hallucination(text):
            print(f"stream server dropping hallucination: {text!r}", flush=True)
            return None

        event = {
            "type": MESSAGE_TRANSCRIPT,
            "seq": self.seq,
            "is_final": bool(is_final),
            "start_sec": round(start_sec, 3),
            "end_sec": round(end_sec, 3),
            "text": text,
        }
        self.seq += 1
        return event


class AsyncTranscriptSink:
    """Thread-safe sink from ChunkTranscriber worker thread to an asyncio queue."""

    def __init__(self, loop, event_queue, timeline, jsonl_path=None):
        self.loop = loop
        self.event_queue = event_queue
        self.timeline = timeline
        self.jsonl_path = jsonl_path
        self.jsonl = open(jsonl_path, "w", encoding="utf-8") if jsonl_path else None

    def handle_event(self, event):
        event = dict(event)
        event["type"] = MESSAGE_TRANSCRIPT
        if "queue_wait_sec" in event:
            event["server_queue_sec"] = event["queue_wait_sec"]
        if "infer_sec" in event:
            event["gpu_infer_sec"] = event["infer_sec"]
        if "end_sec" in event:
            available_at = self.timeline.available_at(float(event["end_sec"]))
            if available_at is not None:
                now = time.monotonic()
                corrected_lag = round(now - available_at, 3)
                event["server_emit_lag_sec"] = corrected_lag
                event["emit_lag_sec"] = corrected_lag
                event["server_audio_available_monotonic"] = round(available_at, 6)
        if "chunk_sec" in event:
            event["audio_buffer_sec"] = event["chunk_sec"]
        event.setdefault("server_sent_monotonic", round(time.monotonic(), 6))

        if self.jsonl is not None:
            self.jsonl.write(json.dumps(event, ensure_ascii=False) + "\n")
            self.jsonl.flush()

        future = asyncio.run_coroutine_threadsafe(self._enqueue_event(event), self.loop)
        try:
            future.result(timeout=2.0)
        except Exception as exc:
            print(f"stream server failed to enqueue event: {exc}", flush=True)

    async def _enqueue_event(self, event):
        if not self.event_queue.full():
            await self.event_queue.put(event)
            return

        if not event.get("is_final", False):
            print("stream server event queue full: dropping partial", flush=True)
            return

        kept = []
        dropped_partial = False
        while not self.event_queue.empty():
            queued = self.event_queue.get_nowait()
            self.event_queue.task_done()
            if (not dropped_partial) and (not queued.get("is_final", False)):
                dropped_partial = True
                continue
            kept.append(queued)

        for queued in kept:
            await self.event_queue.put(queued)

        if not self.event_queue.full():
            await self.event_queue.put(event)
        else:
            print("stream server event queue full: dropping final", flush=True)

    def close(self):
        if self.jsonl is not None:
            self.jsonl.close()
            self.jsonl = None


def build_run_config(config):
    return {
        "run_engine": "stream_server",
        "config_model": config.model,
        "config_device": config.device,
        "config_compute_type": config.compute_type,
        "config_cpu_threads": config.cpu_threads,
        "config_max_window_sec": config.max_window_sec,
        "config_min_silence_sec": config.min_silence_sec,
        "config_partial_sec": config.partial_sec,
        "config_partial_agreement": config.partial_agreement,
        "config_beam_size": config.beam_size,
        "config_vad_filter": config.vad_filter,
        "config_lossless_live": True,
        "config_realtime": True,
        "config_gain": config.gain,
        "config_partial_backpressure": True,
        "config_transport": "websocket",
    }


async def _send_events(websocket, event_queue):
    while True:
        event = await event_queue.get()
        try:
            event["server_send_wall_sec"] = round(time.time(), 6)
            await websocket.send_text(encode_json_message(event))
        finally:
            event_queue.task_done()


async def _receive_audio(websocket, transcriber, session, timeline):
    rate = int(session["sample_rate_hz"])
    channels = int(session["channels"])
    audio_end_sec = 0.0

    while True:
        message = await websocket.receive()
        if "bytes" in message and message["bytes"] is not None:
            frame = decode_audio_frame(message["bytes"])
            received_at = time.monotonic()
            audio_end_sec += len(frame.payload) / float(rate * channels * 2)
            timeline.mark_available(audio_end_sec, received_at)
            transcriber.push_pcm(frame.payload)
            continue

        if "text" in message and message["text"] is not None:
            control = decode_json_message(message["text"])
            msg_type = control.get("type")
            if msg_type == MESSAGE_PING:
                await websocket.send_text(encode_json_message({"type": MESSAGE_PONG}))
            elif msg_type == MESSAGE_SESSION_END:
                transcriber.flush()
                return True
            else:
                await websocket.send_text(encode_json_message(make_error(f"unexpected message: {msg_type}")))
            continue

        if message.get("type") == "websocket.disconnect":
            return False


def create_app(config):
    from fastapi import FastAPI, WebSocket, WebSocketDisconnect

    app = FastAPI(title="Subtitle Overlay Streaming STT", version="1.0")
    app.state.config = config
    app.state.shared_model = None

    @app.on_event("startup")
    async def startup():
        app.state.shared_model = SharedWhisperModel(config)
        print("streaming STT server ready", flush=True)

    @app.get("/health")
    async def health():
        return {
            "status": "ok",
            "model": config.model,
            "device": config.device,
            "compute_type": config.compute_type,
            "transport": "websocket",
        }

    @app.websocket("/stt/stream")
    async def stt_stream(websocket: WebSocket):
        await websocket.accept()
        sink = None
        transcriber = None
        sender = None
        event_queue = None
        try:
            first = await websocket.receive_text()
            session = decode_json_message(first)
            validate_session_start(session)

            event_queue = asyncio.Queue(maxsize=config.event_queue_size)
            timeline = AudioAvailabilityTimeline()
            sink = AsyncTranscriptSink(asyncio.get_running_loop(), event_queue, timeline, config.jsonl)
            engine = SessionWhisperEngine(app.state.shared_model)
            transcriber = ChunkTranscriber(
                engine,
                sink,
                WHISPER_RATE,
                config.max_window_sec,
                partial_sec=config.partial_sec,
                min_silence_sec=config.min_silence_sec,
                gain=config.gain,
                partial_agreement=config.partial_agreement,
                drop_oldest=True,
                realtime=True,
                run_config=build_run_config(config),
            )
            transcriber.set_source_rate(int(session["sample_rate_hz"]))
            await websocket.send_text(
                encode_json_message(
                    {
                        "type": MESSAGE_SESSION_READY,
                        "version": session["version"],
                        "sample_rate_hz": session["sample_rate_hz"],
                    }
                )
            )

            sender = asyncio.create_task(_send_events(websocket, event_queue))
            normal_end = await _receive_audio(websocket, transcriber, session, timeline)
            if normal_end and event_queue is not None:
                try:
                    await asyncio.wait_for(event_queue.join(), timeout=5.0)
                except TimeoutError:
                    print("stream server timed out draining final events", flush=True)
        except WebSocketDisconnect:
            pass
        except Exception as exc:
            try:
                await websocket.send_text(encode_json_message({"type": MESSAGE_ERROR, "message": str(exc)}))
            except Exception:
                pass
            raise
        finally:
            if sender is not None:
                sender.cancel()
            if transcriber is not None:
                transcriber.close()
            if sink is not None:
                sink.close()

    return app


def parse_args():
    parser = argparse.ArgumentParser(description="Run the streaming STT WebSocket server")
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=8765)
    parser.add_argument("--model", default="small")
    parser.add_argument("--device", default="cuda")
    parser.add_argument("--compute-type", default="float16")
    parser.add_argument("--cpu-threads", type=int, default=0)
    parser.add_argument("--language", default="es")
    parser.add_argument("--beam-size", type=int, default=5)
    parser.add_argument("--vad-filter", action="store_true", default=True)
    parser.add_argument("--no-vad-filter", dest="vad_filter", action="store_false")
    parser.add_argument("--max-window-sec", type=float, default=3.0)
    parser.add_argument("--min-silence-sec", type=float, default=0.35)
    parser.add_argument("--partial-sec", type=float, default=0.5)
    parser.add_argument("--partial-agreement", type=int, default=1)
    parser.add_argument("--gain", type=float, default=0.0)
    parser.add_argument("--event-queue-size", type=int, default=64)
    parser.add_argument("--jsonl", help="optional server-side event JSONL path")
    args = parser.parse_args()
    if args.max_window_sec <= 0:
        parser.error("--max-window-sec must be positive")
    if args.min_silence_sec < 0:
        parser.error("--min-silence-sec must be zero or positive")
    if args.partial_sec < 0:
        parser.error("--partial-sec must be zero or positive")
    if args.partial_agreement < 1:
        parser.error("--partial-agreement must be positive")
    return ServerConfig(**vars(args))


def main():
    import uvicorn

    config = parse_args()
    uvicorn.run(create_app(config), host=config.host, port=config.port)


if __name__ == "__main__":
    main()
