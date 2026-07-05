#!/usr/bin/env python3
"""Shared WebSocket protocol helpers for streaming STT sessions."""

import json
import struct
from dataclasses import dataclass


PROTOCOL_VERSION = 1
MESSAGE_SESSION_START = "session_start"
MESSAGE_SESSION_READY = "session_ready"
MESSAGE_SESSION_END = "session_end"
MESSAGE_ERROR = "error"
MESSAGE_PING = "ping"
MESSAGE_PONG = "pong"
MESSAGE_TRANSCRIPT = "transcript"

FORMAT_S16_LE = 1
CHUNK_HEADER = "!QQI"
CHUNK_HEADER_SIZE = struct.calcsize(CHUNK_HEADER)


@dataclass(frozen=True)
class AudioFrame:
    seq: int
    timestamp_ns: int
    dropped: int
    payload: bytes


def encode_json_message(message):
    return json.dumps(message, ensure_ascii=False, separators=(",", ":"))


def decode_json_message(raw):
    if isinstance(raw, bytes):
        raw = raw.decode("utf-8")
    parsed = json.loads(raw)
    if not isinstance(parsed, dict):
        raise ValueError("expected JSON object")
    return parsed


def make_session_start(
    sample_rate_hz,
    channels,
    fmt,
    chunk_ms,
    samples_per_chunk,
    bytes_per_chunk,
    client_monotonic=None,
):
    message = {
        "type": MESSAGE_SESSION_START,
        "version": PROTOCOL_VERSION,
        "sample_rate_hz": int(sample_rate_hz),
        "channels": int(channels),
        "format": int(fmt),
        "chunk_ms": int(chunk_ms),
        "samples_per_chunk": int(samples_per_chunk),
        "bytes_per_chunk": int(bytes_per_chunk),
    }
    if client_monotonic is not None:
        message["client_monotonic"] = float(client_monotonic)
    return message


def validate_session_start(message):
    if message.get("type") != MESSAGE_SESSION_START:
        raise ValueError("first message must be session_start")
    if int(message.get("version", 0)) != PROTOCOL_VERSION:
        raise ValueError(f"unsupported protocol version: {message.get('version')}")
    if int(message.get("sample_rate_hz", 0)) <= 0:
        raise ValueError("invalid sample_rate_hz")
    if int(message.get("channels", 0)) != 1:
        raise ValueError("only mono audio is supported")
    if int(message.get("format", 0)) != FORMAT_S16_LE:
        raise ValueError("only S16_LE audio is supported")
    if int(message.get("bytes_per_chunk", 0)) <= 0:
        raise ValueError("invalid bytes_per_chunk")


def encode_audio_frame(seq, timestamp_ns, dropped, payload):
    if not isinstance(payload, (bytes, bytearray, memoryview)):
        raise TypeError("payload must be bytes-like")
    header = struct.pack(CHUNK_HEADER, int(seq), int(timestamp_ns), int(dropped))
    return header + bytes(payload)


def decode_audio_frame(frame):
    if len(frame) < CHUNK_HEADER_SIZE:
        raise ValueError("audio frame is shorter than header")
    seq, timestamp_ns, dropped = struct.unpack(CHUNK_HEADER, frame[:CHUNK_HEADER_SIZE])
    return AudioFrame(seq=seq, timestamp_ns=timestamp_ns, dropped=dropped, payload=frame[CHUNK_HEADER_SIZE:])


def make_error(message):
    return {"type": MESSAGE_ERROR, "message": str(message)}
