#!/usr/bin/env python3
"""Bridge the current board TCP audio stream to a streaming STT WebSocket server."""

import argparse
import asyncio
import struct
import sys
import time
import wave
from pathlib import Path


sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from scripts.stt_receiver import (
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
from scripts.stt_stream_protocol import (
    MESSAGE_ERROR,
    MESSAGE_SESSION_END,
    MESSAGE_SESSION_READY,
    MESSAGE_TRANSCRIPT,
    decode_json_message,
    encode_audio_frame,
    encode_json_message,
    make_session_start,
)


async def read_exactly(reader, size):
    try:
        return await reader.readexactly(size)
    except asyncio.IncompleteReadError as exc:
        if exc.partial:
            raise EOFError("connection closed mid-frame") from exc
        raise EOFError("connection closed") from exc


class BridgeTranscriptSink:
    def __init__(self, sink, audio_start_monotonic=None):
        self.sink = sink
        self.audio_start_monotonic = audio_start_monotonic

    def set_audio_start(self, value):
        self.audio_start_monotonic = value

    def handle_event(self, event):
        event = dict(event)
        event.pop("type", None)
        received_at = time.monotonic()
        event["bridge_received_monotonic"] = round(received_at, 6)
        if self.audio_start_monotonic is not None and "end_sec" in event:
            audio_end = self.audio_start_monotonic + float(event["end_sec"])
            event["bridge_receive_lag_sec"] = round(received_at - audio_end, 3)
        self.sink.handle_event(event)

    def close(self):
        self.sink.close()


class StreamingBridge:
    def __init__(self, args):
        self.args = args
        self.bridge_sink = BridgeTranscriptSink(build_sink(args))
        self.wav = None

    async def run(self):
        server = await asyncio.start_server(self._handle_board, self.args.host, self.args.port)
        sockets = ", ".join(str(sock.getsockname()) for sock in server.sockets or [])
        print(f"stream bridge listening for board audio on {sockets}", flush=True)
        try:
            async with server:
                await server.serve_forever()
        finally:
            self.bridge_sink.close()

    async def _handle_board(self, reader, writer):
        peer = writer.get_extra_info("peername")
        print(f"board audio connected from {peer}", flush=True)
        try:
            await self._run_session(reader)
        finally:
            writer.close()
            await writer.wait_closed()
            if self.wav is not None:
                self.wav.close()
                self.wav = None
            print("board audio connection closed", flush=True)

    async def _run_session(self, reader):
        import websockets

        async with websockets.connect(
            self.args.stream_url,
            ping_interval=self.args.ws_ping_interval,
            max_size=None,
        ) as websocket:
            stream_info = await self._read_stream_header(reader)
            await websocket.send(encode_json_message(make_session_start(*stream_info, client_monotonic=time.monotonic())))
            ready = decode_json_message(await websocket.recv())
            if ready.get("type") != MESSAGE_SESSION_READY:
                raise RuntimeError(f"unexpected server response: {ready}")
            print(f"stream server session ready: {ready}", flush=True)

            receiver = asyncio.create_task(self._receive_transcripts(websocket))
            try:
                await self._forward_audio(reader, websocket, stream_info)
            except EOFError:
                print("board audio EOF; flushing streaming server", flush=True)
            finally:
                await websocket.send(encode_json_message({"type": MESSAGE_SESSION_END}))
                try:
                    await asyncio.wait_for(receiver, timeout=5.0)
                except TimeoutError:
                    receiver.cancel()

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
        forwarded = 0

        while True:
            chunk_header = await read_exactly(reader, struct.calcsize(CHUNK_HEADER))
            seq, timestamp_ns, payload_bytes, dropped, chunk_rate, chunk_channels, chunk_fmt = (
                struct.unpack(CHUNK_HEADER, chunk_header)
            )
            if payload_bytes > bytes_per_chunk:
                raise RuntimeError(f"bad payload size: {payload_bytes}")
            if (chunk_rate, chunk_channels, chunk_fmt) != (rate, channels, fmt):
                raise RuntimeError("chunk format changed")

            payload = await read_exactly(reader, payload_bytes)
            if first_timestamp_ns is None:
                first_timestamp_ns = timestamp_ns
                audio_start_monotonic = time.monotonic()
                self.bridge_sink.set_audio_start(audio_start_monotonic)

            await websocket.send(encode_audio_frame(seq, timestamp_ns, dropped, payload))
            forwarded += 1

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
    if args.jsonl:
        sinks.append(JsonlTranscriptSink(args.jsonl))
    if args.send_subtitles:
        sinks.append(TcpTranscriptSink(args.subtitle_host, args.subtitle_port))
    return CompositeTranscriptSink(sinks)


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
    parser.add_argument("--ws-ping-interval", type=float, default=20.0)
    return parser.parse_args()


def main():
    args = parse_args()
    asyncio.run(StreamingBridge(args).run())


if __name__ == "__main__":
    main()
