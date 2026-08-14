#!/usr/bin/env python3
"""Minimal streaming STT server used to exercise the firmware WebSocket client.

It speaks the production contract by importing `server/runtime/protocol.py`
directly, so `session_start` is validated by the same code the Colab server
runs and audio frames are decoded with the same `CHUNK_HEADER`.

Scope note: FastAPI and uvicorn are not installed in the WSL development
environment, so this covers the protocol layer rather than `app.py`'s routing.
The routing layer is covered by `server/tests/test_app.py`, and the whole stack
is covered by the real Colab run.
"""

import argparse
import asyncio
import json
import sys
from pathlib import Path


sys.path.insert(0, str(Path(__file__).resolve().parents[3]))

import websockets

from server.runtime.protocol import (
    MESSAGE_PING,
    MESSAGE_PONG,
    MESSAGE_SESSION_END,
    MESSAGE_SESSION_READY,
    decode_audio_frame,
    decode_json_message,
    encode_json_message,
    validate_session_start,
)


class Summary:
    def __init__(self):
        self.session_start = None
        self.backend_config = None
        self.audio_frames = 0
        self.audio_bytes = 0
        self.first_seq = None
        self.last_seq = None
        self.seq_gaps = 0
        self.pings = 0
        self.session_end = False
        self.errors = []

    def as_dict(self):
        return {
            "session_start": self.session_start,
            "backend_config": self.backend_config,
            "audio_frames": self.audio_frames,
            "audio_bytes": self.audio_bytes,
            "first_seq": self.first_seq,
            "last_seq": self.last_seq,
            "seq_gaps": self.seq_gaps,
            "pings": self.pings,
            "session_end": self.session_end,
            "errors": self.errors,
        }


async def serve_session(websocket, args, summary, done):
    try:
        first = await websocket.recv()
        start = decode_json_message(first)
        summary.backend_config = validate_session_start(start)
        summary.session_start = {k: v for k, v in start.items() if k != "backend_config"}
    except Exception as exc:  # noqa: BLE001 - reported in the summary
        summary.errors.append(f"session_start rejected: {exc}")
        await websocket.close()
        done.set()
        return

    await websocket.send(
        encode_json_message(
            {
                "type": MESSAGE_SESSION_READY,
                "version": 1,
                "sample_rate_hz": start.get("sample_rate_hz"),
                "run_engine": "fake_integration_server",
                "run_config": {
                    "config_latency_ms": summary.backend_config.get("latency_ms"),
                    "config_target_lang": summary.backend_config.get("target_lang"),
                    "config_att_context_size": [56, 6],
                },
            }
        )
    )

    transcript_seq = 0
    try:
        async for message in websocket:
            if isinstance(message, bytes):
                frame = decode_audio_frame(message)
                if summary.first_seq is None:
                    summary.first_seq = frame.seq
                elif frame.seq != (summary.last_seq + 1):
                    summary.seq_gaps += 1
                summary.last_seq = frame.seq
                summary.audio_frames += 1
                summary.audio_bytes += len(frame.payload)

                # Emit a transcript every N chunks so the client's receive path
                # and the sequence guard both get exercised.
                if summary.audio_frames % args.transcript_every == 0:
                    end_sec = summary.audio_frames * 0.02
                    await websocket.send(
                        encode_json_message(
                            {
                                "type": "transcript",
                                "seq": transcript_seq,
                                "is_final": (transcript_seq % 2) == 1,
                                "start_sec": max(0.0, end_sec - 1.0),
                                "end_sec": end_sec,
                                "text": f"linea numero {transcript_seq}",
                                "full_text": f"linea numero {transcript_seq}",
                                "run_engine": "fake_integration_server",
                                "att_context_size": [56, 6],
                                "timestamp_source": "sample_clock",
                                "emit_monotonic": 1.0,
                            }
                        )
                    )
                    transcript_seq += 1

                if summary.audio_frames >= args.expect_chunks:
                    done.set()
                continue

            control = decode_json_message(message)
            if control.get("type") == MESSAGE_PING:
                summary.pings += 1
                await websocket.send(encode_json_message({"type": MESSAGE_PONG}))
            elif control.get("type") == MESSAGE_SESSION_END:
                summary.session_end = True
                done.set()
                break
    except websockets.exceptions.ConnectionClosed:
        pass

    done.set()


async def main_async(args):
    summary = Summary()
    done = asyncio.Event()

    async def handler(websocket):
        await serve_session(websocket, args, summary, done)

    async with websockets.serve(handler, args.host, args.port, ping_interval=None):
        print(f"fake stt server listening on ws://{args.host}:{args.port}", flush=True)
        try:
            await asyncio.wait_for(done.wait(), timeout=args.timeout)
        except (asyncio.TimeoutError, TimeoutError):
            summary.errors.append(f"timed out after {args.timeout}s")

    Path(args.summary).write_text(json.dumps(summary.as_dict(), indent=2) + "\n", encoding="utf-8")
    print(json.dumps(summary.as_dict(), indent=2), flush=True)
    return 1 if summary.errors else 0


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--host", default="127.0.0.1")
    parser.add_argument("--port", type=int, default=8799)
    parser.add_argument("--expect-chunks", type=int, default=25)
    parser.add_argument("--transcript-every", type=int, default=5)
    parser.add_argument("--timeout", type=float, default=30.0)
    parser.add_argument("--summary", default="build/ws-integration/server-summary.json")
    args = parser.parse_args()
    Path(args.summary).parent.mkdir(parents=True, exist_ok=True)
    raise SystemExit(asyncio.run(main_async(args)))


if __name__ == "__main__":
    main()
