#!/usr/bin/env python3
"""Shared WebSocket protocol helpers for streaming STT sessions."""

import json
import struct
from bisect import bisect_left
from dataclasses import dataclass, field


PROTOCOL_VERSION = 1
MESSAGE_SESSION_START = "session_start"
MESSAGE_SESSION_READY = "session_ready"
MESSAGE_SESSION_END = "session_end"
MESSAGE_SESSION_SUMMARY = "session_summary"
MESSAGE_ERROR = "error"
MESSAGE_PING = "ping"
MESSAGE_PONG = "pong"
MESSAGE_TRANSCRIPT = "transcript"

FORMAT_S16_LE = 1
CHUNK_HEADER = "!QQI"
CHUNK_HEADER_SIZE = struct.calcsize(CHUNK_HEADER)

SESSION_CONFIG_FIELDS = {
    "max_window_sec": (float, lambda value: value > 0),
    "min_silence_sec": (float, lambda value: value >= 0),
    "partial_sec": (float, lambda value: value >= 0),
    "partial_agreement": (int, lambda value: value >= 1),
    "gain": (float, lambda value: value >= 0),
    "vad_threshold": (float, lambda value: 0 < value < 1),
    "vad_neg_threshold": (float, lambda value: 0 <= value < 1),
}


@dataclass(frozen=True)
class AudioFrame:
    seq: int
    timestamp_ns: int
    dropped: int
    payload: bytes


@dataclass
class AudioAvailabilityTimeline:
    """Map audio end seconds to the local monotonic time when those samples arrived."""

    points: list[tuple[float, float]] = field(default_factory=list)

    def mark_available(self, audio_end_sec, monotonic_time):
        audio_end_sec = float(audio_end_sec)
        monotonic_time = float(monotonic_time)
        if self.points and audio_end_sec < self.points[-1][0]:
            return
        self.points.append((audio_end_sec, monotonic_time))

    def available_at(self, audio_end_sec):
        if not self.points:
            return None

        audio_end_sec = float(audio_end_sec)
        ends = [point[0] for point in self.points]
        index = bisect_left(ends, audio_end_sec)
        if index >= len(self.points):
            return self.points[-1][1]
        return self.points[index][1]


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
    config_overrides=None,
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
    if config_overrides:
        message["config_overrides"] = validate_config_overrides(config_overrides)
    return message


def validate_config_overrides(overrides):
    if overrides is None:
        return {}
    if not isinstance(overrides, dict):
        raise ValueError("config_overrides must be a JSON object")

    unknown = sorted(set(overrides) - set(SESSION_CONFIG_FIELDS))
    if unknown:
        raise ValueError(f"unsupported session config override(s): {', '.join(unknown)}")

    normalized = {}
    for name, value in overrides.items():
        converter, predicate = SESSION_CONFIG_FIELDS[name]
        if isinstance(value, bool):
            raise ValueError(f"invalid {name}")
        try:
            converted = converter(value)
        except (TypeError, ValueError) as exc:
            raise ValueError(f"invalid {name}") from exc
        if not predicate(converted):
            raise ValueError(f"invalid {name}")
        normalized[name] = converted
    threshold = normalized.get("vad_threshold")
    negative_threshold = normalized.get("vad_neg_threshold")
    if threshold is not None and negative_threshold is not None and negative_threshold >= threshold:
        raise ValueError("vad_neg_threshold must be lower than vad_threshold")
    return normalized


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
    return validate_config_overrides(message.get("config_overrides"))


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
