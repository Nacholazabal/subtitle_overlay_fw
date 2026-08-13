#!/usr/bin/env python3
"""Bridge the current board TCP audio stream to a streaming STT WebSocket server."""

import argparse
import asyncio
import base64
import binascii
import contextlib
import json
import struct
import sys
import time
import wave
from pathlib import Path


sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from server.runtime.transport import (
    CHUNK_HEADER,
    EXPECTED_SAMPLE_WIDTH,
    STREAM_HEADER,
    STREAM_MAGIC,
    CompositeTranscriptSink,
    ConsoleTranscriptSink,
    JsonlTranscriptSink,
    TcpTranscriptSink,
    validate_audio_format,
)
from server.runtime.protocol import (
    AudioAvailabilityTimeline,
    MESSAGE_ERROR,
    MESSAGE_SESSION_END,
    MESSAGE_SESSION_READY,
    MESSAGE_SESSION_SUMMARY,
    MESSAGE_TRANSCRIPT,
    decode_json_message,
    encode_audio_frame,
    encode_json_message,
    make_session_start,
    validate_backend_config,
)


def board_drop_delta(start, end):
    return max(0, int(end or 0) - int(start or 0))


def parse_backend_config(raw):
    """Parse the optional generic backend-config JSON passed by the launcher.

    The Nemotron server validates the keys it understands; the bridge only
    checks that it is a JSON object of scalar values."""
    if not raw:
        return None
    parsed = json.loads(raw) if isinstance(raw, str) else raw
    return validate_backend_config(parsed)


def decode_backend_config_base64(raw):
    """Decode JSON transported safely through WSL and Windows PowerShell.

    Windows PowerShell removes the quotes embedded in a JSON argument when it
    serializes native-process argv. URL-safe Base64 contains no quote characters,
    so the launcher can cross that boundary losslessly and the bridge still owns
    JSON/schema validation.
    """
    if not raw:
        return None
    if not isinstance(raw, str):
        raise ValueError("backend config Base64 must be text")
    try:
        payload = base64.b64decode(raw.encode("ascii"), altchars=b"-_", validate=True)
        return payload.decode("utf-8")
    except (UnicodeEncodeError, UnicodeDecodeError, binascii.Error) as exc:
        raise ValueError("backend config is not valid URL-safe Base64 UTF-8") from exc


def write_json_file(path, payload):
    if not path:
        return
    output = Path(path)
    output.parent.mkdir(parents=True, exist_ok=True)
    temporary = output.with_suffix(output.suffix + ".tmp")
    temporary.write_text(json.dumps(payload, ensure_ascii=False, indent=2) + "\n", encoding="utf-8")
    temporary.replace(output)


async def read_exactly(reader, size):
    try:
        return await reader.readexactly(size)
    except asyncio.IncompleteReadError as exc:
        if exc.partial:
            raise EOFError("connection closed mid-frame") from exc
        raise EOFError("connection closed") from exc


class BridgeTranscriptSink:
    def __init__(self, sink, timeline=None, wall_timeline=None):
        self.sink = sink
        self.timeline = timeline or AudioAvailabilityTimeline()
        self.wall_timeline = wall_timeline or AudioAvailabilityTimeline()

    def set_timelines(self, timeline, wall_timeline):
        self.timeline = timeline
        self.wall_timeline = wall_timeline

    def handle_event(self, event):
        event = dict(event)
        event.pop("type", None)
        received_at = time.monotonic()
        received_wall = time.time()
        event["bridge_received_monotonic"] = round(received_at, 6)
        event["bridge_received_wall_sec"] = round(received_wall, 6)
        end_sec = event.get("end_sec")
        if isinstance(end_sec, (int, float)) and not isinstance(end_sec, bool):
            available_at = self.timeline.available_at(float(end_sec))
            if available_at is not None:
                event["bridge_receive_lag_sec"] = round(received_at - available_at, 3)
                event["bridge_audio_available_monotonic"] = round(available_at, 6)
            available_wall = self.wall_timeline.available_at(float(end_sec))
            if available_wall is not None:
                event["bridge_audio_available_wall_sec"] = round(available_wall, 6)
        self.sink.handle_event(event)

    def close(self):
        self.sink.close()


class StreamingBridge:
    def __init__(self, args):
        self.args = args
        sink, self.subtitle_sink = build_sink(args)
        self.bridge_sink = BridgeTranscriptSink(sink)
        self.wav = None
        self.session_done = asyncio.Event()
        self.session_active = False
        self.session_started = False
        self.session_error = None
        self.run_config = {}
        self.session_summary = {}
        self.forward_summary = {}
        self.subtitle_delivery_summary = {}

    async def run(self):
        server = await asyncio.start_server(self._handle_board, self.args.host, self.args.port)
        sockets = ", ".join(str(sock.getsockname()) for sock in server.sockets or [])
        print(f"stream bridge listening for board audio on {sockets}", flush=True)
        stop_watcher = None
        if self.args.stop_file:
            stop_watcher = asyncio.create_task(self._watch_stop_file())
        try:
            async with server:
                if self.args.single_session:
                    await self.session_done.wait()
                else:
                    await server.serve_forever()
        finally:
            if stop_watcher is not None:
                stop_watcher.cancel()
                with contextlib.suppress(asyncio.CancelledError):
                    await stop_watcher
            self.bridge_sink.close()
        if self.session_error is not None:
            raise RuntimeError(f"stream bridge session failed: {self.session_error}")

    async def _wait_for_subtitle_ready(self):
        if self.subtitle_sink is None:
            return
        subtitle_ready_timeout = getattr(self.args, "subtitle_ready_timeout", 15.0)
        subtitle_ready = await asyncio.to_thread(
            self.subtitle_sink.wait_ready, subtitle_ready_timeout
        )
        if not subtitle_ready:
            raise RuntimeError("subtitle TCP handshake timed out; board channel is not ready")

    async def _watch_stop_file(self):
        while not self.session_done.is_set():
            if Path(self.args.stop_file).exists() and not self.session_started:
                self.session_summary = {"status": "stopped_before_session"}
                write_json_file(self.args.done_file, self.session_summary)
                self.session_done.set()
                return
            await asyncio.sleep(0.1)

    async def _handle_board(self, reader, writer):
        if self.session_active or (self.args.single_session and self.session_done.is_set()):
            writer.close()
            await writer.wait_closed()
            return
        self.session_active = True
        self.session_started = True
        peer = writer.get_extra_info("peername")
        print(f"board audio connected from {peer}", flush=True)
        try:
            await self._run_session(reader)
        except Exception as exc:
            self.session_error = exc
            print(f"stream bridge session failed: {exc}", flush=True)
        finally:
            writer.close()
            with contextlib.suppress(ConnectionError):
                await writer.wait_closed()
            if self.wav is not None:
                self.wav.close()
                self.wav = None
            self.session_active = False
            summary = {
                "status": "error" if self.session_error is not None else "complete",
                "finished_wall_sec": round(time.time(), 6),
                "error": str(self.session_error) if self.session_error is not None else None,
                "run_config": self.run_config,
                **self.forward_summary,
                **self.session_summary,
                "subtitle_delivery": self.subtitle_delivery_summary,
            }
            write_json_file(self.args.done_file, summary)
            print("board audio connection closed", flush=True)
            if self.args.single_session:
                self.session_done.set()

    async def _run_session(self, reader):
        import websockets

        async with websockets.connect(
            self.args.stream_url,
            ping_interval=self.args.ws_ping_interval,
            max_size=None,
        ) as websocket:
            stream_info = await self._read_stream_header(reader)
            await websocket.send(
                encode_json_message(
                    make_session_start(
                        *stream_info,
                        client_monotonic=time.monotonic(),
                        backend_config=parse_backend_config(getattr(self.args, "backend_config_json", None)),
                    )
                )
            )
            ready = decode_json_message(await websocket.recv())
            if ready.get("type") != MESSAGE_SESSION_READY:
                raise RuntimeError(f"unexpected server response: {ready}")
            run_config = ready.get("run_config")
            self.run_config = run_config if isinstance(run_config, dict) else {}
            print(f"stream server session ready: {ready}", flush=True)
            if isinstance(run_config, dict):
                print(
                    "stream server config: "
                    f"model={run_config.get('config_model')} "
                    f"lookahead={run_config.get('config_latency_ms')}ms "
                    f"eou_history={run_config.get('config_stop_history_eou_ms')}ms "
                    f"residue_tokens={run_config.get('config_residue_tokens_at_end')} "
                    f"language={run_config.get('config_target_lang')}",
                    flush=True,
                )

            await self._wait_for_subtitle_ready()

            receiver = asyncio.create_task(self._receive_transcripts(websocket))
            try:
                await self._forward_audio(reader, websocket, stream_info)
            except EOFError:
                print("board audio EOF; flushing streaming server", flush=True)
            finally:
                await websocket.send(encode_json_message({"type": MESSAGE_SESSION_END}))
                try:
                    await asyncio.wait_for(receiver, timeout=35.0)
                except TimeoutError:
                    receiver.cancel()
                    with contextlib.suppress(asyncio.CancelledError):
                        await receiver
                if self.subtitle_sink is not None:
                    queue_drained = await asyncio.to_thread(self.subtitle_sink.flush, 10.0)
                    self.subtitle_delivery_summary = self.subtitle_sink.stats_snapshot()
                    self.subtitle_delivery_summary["queue_drained"] = queue_drained

    async def _read_stream_header(self, reader):
        magic = await read_exactly(reader, len(STREAM_MAGIC))
        if magic != STREAM_MAGIC:
            raise RuntimeError(f"bad stream magic: {magic!r}")

        header = await read_exactly(reader, struct.calcsize(STREAM_HEADER))
        rate, channels, fmt, chunk_ms, samples_per_chunk, bytes_per_chunk = struct.unpack(
            STREAM_HEADER,
            header,
        )
        validate_audio_format(rate, channels, fmt)
        print(
            f"board stream: {rate} Hz, {channels} ch, {chunk_ms} ms chunks, "
            f"{samples_per_chunk} samples/chunk",
            flush=True,
        )

        if self.args.save_wav:
            self.wav = wave.open(self.args.save_wav, "wb")
            self.wav.setnchannels(channels)
            self.wav.setsampwidth(EXPECTED_SAMPLE_WIDTH)
            self.wav.setframerate(rate)
            print(f"saving exact board audio to {self.args.save_wav}", flush=True)

        return rate, channels, fmt, chunk_ms, samples_per_chunk, bytes_per_chunk

    async def _forward_audio(self, reader, websocket, stream_info):
        rate, channels, fmt, _chunk_ms, _samples_per_chunk, bytes_per_chunk = stream_info
        first_timestamp_ns = None
        audio_start_monotonic = None
        audio_end_sec = 0.0
        timeline = AudioAvailabilityTimeline()
        wall_timeline = AudioAvailabilityTimeline()
        self.bridge_sink.set_timelines(timeline, wall_timeline)
        forwarded = 0
        first_seq = None
        last_seq = None
        board_dropped_start = None
        board_dropped_end = None
        audio_start_wall = None

        while True:
            if self.args.stop_file and Path(self.args.stop_file).exists():
                print("stream bridge stop marker detected", flush=True)
                break
            chunk_header = await read_exactly(reader, struct.calcsize(CHUNK_HEADER))
            seq, timestamp_ns, payload_bytes, dropped, chunk_rate, chunk_channels, chunk_fmt = (
                struct.unpack(CHUNK_HEADER, chunk_header)
            )
            if payload_bytes > bytes_per_chunk:
                raise RuntimeError(f"bad payload size: {payload_bytes}")
            if (chunk_rate, chunk_channels, chunk_fmt) != (rate, channels, fmt):
                raise RuntimeError("chunk format changed")

            payload = await read_exactly(reader, payload_bytes)
            payload_received_at = time.monotonic()
            payload_received_wall = time.time()
            if first_timestamp_ns is None:
                first_timestamp_ns = timestamp_ns
                audio_start_monotonic = payload_received_at
                audio_start_wall = payload_received_wall
                first_seq = seq
                board_dropped_start = int(dropped)
                write_json_file(
                    self.args.ready_file,
                    {
                        "status": "ready",
                        "ready_wall_sec": round(payload_received_wall, 6),
                        "audio_start_wall_sec": round(payload_received_wall, 6),
                        "sample_rate_hz": rate,
                        "channels": channels,
                        "chunk_ms": _chunk_ms,
                        "first_chunk_seq": int(seq),
                        "board_dropped_chunks_start": board_dropped_start,
                        "run_config": self.run_config,
                        "subtitle_session": (
                            self.subtitle_sink.stats_snapshot()
                            if self.subtitle_sink is not None
                            else None
                        ),
                    },
                )

            await websocket.send(encode_audio_frame(seq, timestamp_ns, dropped, payload))
            forwarded += 1
            last_seq = seq
            board_dropped_end = max(board_dropped_end or 0, int(dropped))
            audio_end_sec += payload_bytes / float(rate * channels * EXPECTED_SAMPLE_WIDTH)
            timeline.mark_available(audio_end_sec, payload_received_at)
            wall_timeline.mark_available(audio_end_sec, payload_received_wall)

            if self.wav is not None:
                self.wav.writeframes(payload)
            if (seq % 50) == 0:
                capture_age = (timestamp_ns - first_timestamp_ns) / 1_000_000_000.0
                bridge_age = time.monotonic() - audio_start_monotonic
                print(
                    f"audio seq={seq} capture_age={capture_age:.3f}s "
                    f"bridge_age={bridge_age:.3f}s dropped={dropped}",
                    flush=True,
                )

        self.forward_summary = {
            "audio_start_wall_sec": round(audio_start_wall, 6) if audio_start_wall is not None else None,
            "audio_end_wall_sec": round(time.time(), 6),
            "audio_duration_sec": round(audio_end_sec, 3),
            "chunks_forwarded": forwarded,
            "first_chunk_seq": first_seq,
            "last_chunk_seq": last_seq,
            "board_dropped_chunks_start": board_dropped_start or 0,
            "board_dropped_chunks_end": board_dropped_end or 0,
            "board_dropped_chunks_during_session": board_drop_delta(
                board_dropped_start, board_dropped_end
            ),
            # Legacy absolute counter from the board, retained for old tooling.
            "board_dropped_chunks": board_dropped_end or 0,
        }

    async def _receive_transcripts(self, websocket):
        try:
            while True:
                raw = await websocket.recv()
                if isinstance(raw, bytes):
                    print("stream bridge ignoring binary server frame", flush=True)
                    continue
                message = decode_json_message(raw)
                msg_type = message.get("type")
                if msg_type == MESSAGE_TRANSCRIPT:
                    self.bridge_sink.handle_event(message)
                elif msg_type == MESSAGE_SESSION_SUMMARY:
                    self.session_summary = {
                        key: value for key, value in message.items() if key != "type"
                    }
                elif msg_type == MESSAGE_ERROR:
                    print(f"stream server error: {message.get('message')}", flush=True)
                else:
                    print(f"stream server control: {message}", flush=True)
        except Exception as exc:
            if exc.__class__.__name__.startswith("ConnectionClosed"):
                return
            raise


def build_sink(args):
    sinks = [ConsoleTranscriptSink()]
    subtitle_sink = None
    if args.jsonl:
        sinks.append(JsonlTranscriptSink(args.jsonl))
    if args.send_subtitles:
        subtitle_sink = TcpTranscriptSink(
            args.subtitle_host,
            args.subtitle_port,
            ack_jsonl=getattr(args, "board_ack_jsonl", None),
        )
        sinks.append(subtitle_sink)
    return CompositeTranscriptSink(sinks), subtitle_sink


def parse_args():
    parser = argparse.ArgumentParser(description="Bridge board TCP audio to a streaming STT server")
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=5000)
    parser.add_argument("--stream-url", required=True, help="ws(s)://server/stt/stream")
    parser.add_argument("--jsonl", help="write transcript events as JSON Lines")
    parser.add_argument("--save-wav", help="save the exact PCM received from the board")
    parser.add_argument("--send-subtitles", action="store_true")
    parser.add_argument("--subtitle-host", default="192.168.1.10")
    parser.add_argument("--subtitle-port", type=int, default=5001)
    parser.add_argument("--board-ack-jsonl", help="write board transcript ACKs as JSON Lines")
    parser.add_argument("--subtitle-ready-timeout", type=float, default=15.0)
    parser.add_argument("--ws-ping-interval", type=float, default=20.0)
    parser.add_argument("--ready-file", help="write JSON after board, Colab, and first audio are ready")
    parser.add_argument("--done-file", help="write final session summary JSON")
    parser.add_argument("--stop-file", help="finish the active session when this file appears")
    parser.add_argument("--single-session", action="store_true", help="exit after one board session")
    backend_config = parser.add_mutually_exclusive_group()
    backend_config.add_argument(
        "--backend-config-json",
        help="Nemotron session configuration as a JSON object",
    )
    backend_config.add_argument(
        "--backend-config-base64",
        help="URL-safe Base64 encoding of --backend-config-json for WSL/PowerShell transport",
    )
    args = parser.parse_args()
    backend_option = "--backend-config-json"
    if args.backend_config_base64 is not None:
        backend_option = "--backend-config-base64"
        try:
            args.backend_config_json = decode_backend_config_base64(
                args.backend_config_base64
            )
        except ValueError as exc:
            parser.error(f"{backend_option} is invalid: {exc}")
    if args.backend_config_json is not None:
        try:
            parse_backend_config(args.backend_config_json)
        except (json.JSONDecodeError, ValueError) as exc:
            parser.error(f"{backend_option} is not a valid JSON object: {exc}")
    if args.subtitle_ready_timeout <= 0:
        parser.error("--subtitle-ready-timeout must be positive")
    return args


def main():
    args = parse_args()
    asyncio.run(StreamingBridge(args).run())


if __name__ == "__main__":
    main()
