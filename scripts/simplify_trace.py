#!/usr/bin/env python3
"""Create a thesis-friendly Perfetto trace from a full unified capture.

The production trace intentionally records enough detail to diagnose failures:
instant events, counters, flow arrows and low-level names.  That is useful for
debugging but noisy in a figure.  This converter keeps the source timestamps
and durations while producing a second, presentation-only view made entirely
of complete duration slices (plus the metadata Perfetto needs to name tracks).

By default the useful interval is detected from the board's transition into
``ready`` until it leaves that state.  The input file is never modified.
"""

from __future__ import annotations

import argparse
import bisect
import json
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Iterable


Event = dict[str, Any]


@dataclass(frozen=True)
class Track:
    pid: int
    tid: int
    process_name: str
    track_name: str
    sort_index: int
    display_name: str
    category: str
    color: str


TRACKS = {
    "stable_session": Track(
        1,
        10,
        "Firmware — Arty Z7",
        "1. Sesión STT conectada",
        10,
        "Sesión STT estable",
        "Firmware",
        "good",
    ),
    "alsa_read": Track(
        1,
        11,
        "Firmware — Arty Z7",
        "2. Captura de audio (ALSA)",
        20,
        "Lectura de audio ALSA",
        "Firmware",
        "thread_state_running",
    ),
    "audio_ws_send": Track(
        1,
        12,
        "Firmware — Arty Z7",
        "3. Envío de audio (WebSocket/TLS)",
        30,
        "Envío WebSocket de audio",
        "Firmware",
        "olive",
    ),
    "subtitle_render": Track(
        1,
        13,
        "Firmware — Arty Z7",
        "4. Render del subtítulo",
        40,
        "Render del subtítulo",
        "Firmware",
        "rail_response",
    ),
    "session_push_pcm": Track(
        2,
        21,
        "Servidor STT — Colab",
        "1. Ingreso y buffering de PCM",
        10,
        "Ingreso de PCM al modelo",
        "Servidor",
        "thread_state_runnable",
    ),
    "nemotron_step": Track(
        2,
        22,
        "Servidor STT — Colab",
        "2. Inferencia streaming Nemotron",
        20,
        "Inferencia Nemotron",
        "Servidor",
        "rail_animation",
    ),
    "transcript_delivery": Track(
        3,
        31,
        "Comunicación y resultado",
        "1. Entrega del transcript: servidor → firmware",
        10,
        "Entrega del transcript a la placa",
        "Comunicación",
        "cq_build_running",
    ),
    "end_to_end": Track(
        3,
        32,
        "Comunicación y resultado",
        "2. Latencia: audio cubierto → overlay actualizado",
        20,
        "Audio hasta subtítulo visible",
        "Resultado",
        "terrible",
    ),
}

SOURCE_SLICE_NAMES = (
    "alsa_read",
    "audio_ws_send",
    "subtitle_render",
    "session_push_pcm",
    "nemotron_step",
)

PROCESS_SORT = {
    "Firmware — Arty Z7": 10,
    "Servidor STT — Colab": 20,
    "Comunicación y resultado": 30,
}


def load_trace(path: Path) -> dict[str, Any]:
    try:
        payload = json.loads(path.read_text(encoding="utf-8"))
    except (OSError, UnicodeDecodeError, json.JSONDecodeError) as exc:
        raise ValueError(f"no se pudo leer {path}: {exc}") from exc

    events = payload.get("traceEvents")
    if not isinstance(events, list):
        raise ValueError(f"{path} no contiene una lista traceEvents")
    return payload


def detect_ready_window(events: Iterable[Event]) -> tuple[int, int]:
    """Return the first complete board ready interval in microseconds."""
    transitions = sorted(
        (
            event
            for event in events
            if event.get("name") == "ws_state"
            and event.get("ph") == "i"
            and isinstance(event.get("ts"), (int, float))
        ),
        key=lambda event: event["ts"],
    )

    start = None
    for event in transitions:
        args = event.get("args") or {}
        timestamp = int(event["ts"])
        if start is None and args.get("to") == "ready":
            start = timestamp
            continue
        if start is not None and timestamp > start and args.get("from") == "ready":
            return start, timestamp

    if start is None:
        raise ValueError("el trace no contiene una transición ws_state hacia ready")
    raise ValueError("el trace entra en ready pero no contiene su transición de salida")


def metadata_events() -> list[Event]:
    events: list[Event] = []
    processes: dict[int, str] = {}
    for track in TRACKS.values():
        processes.setdefault(track.pid, track.process_name)

    for pid, name in processes.items():
        events.extend(
            [
                {
                    "name": "process_name",
                    "ph": "M",
                    "pid": pid,
                    "tid": 0,
                    "args": {"name": name},
                },
                {
                    "name": "process_sort_index",
                    "ph": "M",
                    "pid": pid,
                    "tid": 0,
                    "args": {"sort_index": PROCESS_SORT[name]},
                },
            ]
        )

    for track in TRACKS.values():
        events.extend(
            [
                {
                    "name": "thread_name",
                    "ph": "M",
                    "pid": track.pid,
                    "tid": track.tid,
                    "args": {"name": track.track_name},
                },
                {
                    "name": "thread_sort_index",
                    "ph": "M",
                    "pid": track.pid,
                    "tid": track.tid,
                    "args": {"sort_index": track.sort_index},
                },
            ]
        )
    return events


def clip_slice(event: Event, start_us: int, end_us: int, track: Track) -> Event | None:
    timestamp = event.get("ts")
    duration = event.get("dur")
    if not isinstance(timestamp, (int, float)) or not isinstance(duration, (int, float)):
        return None

    source_start = int(timestamp)
    source_end = source_start + max(0, int(duration))
    clipped_start = max(source_start, start_us)
    clipped_end = min(source_end, end_us)
    if clipped_end <= clipped_start:
        return None

    args = dict(event.get("args") or {})
    args["duración_ms"] = round((clipped_end - clipped_start) / 1000.0, 3)
    return {
        "name": track.display_name,
        "cat": track.category,
        "ph": "X",
        "ts": clipped_start - start_us,
        "dur": clipped_end - clipped_start,
        "pid": track.pid,
        "tid": track.tid,
        "cname": track.color,
        "args": args,
    }


def source_slices(events: Iterable[Event], start_us: int, end_us: int) -> list[Event]:
    result = []
    for event in events:
        name = event.get("name")
        if event.get("ph") != "X" or name not in SOURCE_SLICE_NAMES:
            continue
        converted = clip_slice(event, start_us, end_us, TRACKS[name])
        if converted is not None:
            result.append(converted)
    return result


def first_by_sequence(
    events: Iterable[Event], name: str, start_us: int, end_us: int
) -> dict[int, Event]:
    indexed: dict[int, Event] = {}
    for event in events:
        if event.get("name") != name:
            continue
        timestamp = event.get("ts")
        if not isinstance(timestamp, (int, float)) or not start_us <= timestamp <= end_us:
            continue
        sequence = (event.get("args") or {}).get("transcript_seq")
        if not isinstance(sequence, int):
            continue
        previous = indexed.get(sequence)
        if previous is None or timestamp < previous["ts"]:
            indexed[sequence] = event
    return indexed


def duration_slice(
    track: Track, start: int, end: int, window_start: int, args: dict[str, Any]
) -> Event | None:
    if end <= start:
        return None
    duration = end - start
    details = dict(args)
    details["duración_ms"] = round(duration / 1000.0, 3)
    return {
        "name": track.display_name,
        "cat": track.category,
        "ph": "X",
        "ts": start - window_start,
        "dur": duration,
        "pid": track.pid,
        "tid": track.tid,
        "cname": track.color,
        "args": details,
    }


def transcript_delivery_slices(
    events: list[Event], start_us: int, end_us: int
) -> list[Event]:
    emitted = first_by_sequence(events, "transcript_emit", start_us, end_us)
    decoded = first_by_sequence(events, "transcript_decoded", start_us, end_us)
    result = []
    for sequence in sorted(set(emitted) & set(decoded)):
        sent_at = int(emitted[sequence]["ts"])
        received_at = int(decoded[sequence]["ts"])
        converted = duration_slice(
            TRACKS["transcript_delivery"],
            sent_at,
            received_at,
            start_us,
            {
                "secuencia": sequence,
                "tipo": "final"
                if (emitted[sequence].get("args") or {}).get("is_final")
                else "parcial",
            },
        )
        if converted is not None:
            result.append(converted)
    return result


def end_to_end_slices(events: list[Event], start_us: int, end_us: int) -> list[Event]:
    """Build one bar per transcript, using only the selected ready session."""
    capture_times = {
        args["capture_ts_ns"]: int(event["ts"])
        for event in events
        if event.get("name") == "audio_chunk_ready"
        and start_us <= event.get("ts", -1) <= end_us
        and isinstance((args := event.get("args") or {}).get("capture_ts_ns"), int)
    }

    audio_positions = sorted(
        (
            float(args["audio_end_sec"]),
            args["board_capture_ts_ns"],
        )
        for event in events
        if event.get("name") == "session_push_pcm"
        and start_us <= event.get("ts", -1) <= end_us
        and isinstance((args := event.get("args") or {}).get("audio_end_sec"), (int, float))
        and isinstance(args.get("board_capture_ts_ns"), int)
        and args["board_capture_ts_ns"] in capture_times
    )
    positions = [position for position, _stamp in audio_positions]

    dispatched = first_by_sequence(events, "transcript_dispatch", start_us, end_us)
    commits = first_by_sequence(events, "overlay_commit", start_us, end_us)
    result = []
    for sequence in sorted(set(dispatched) & set(commits)):
        dispatch = dispatched[sequence]
        dispatch_args = dispatch.get("args") or {}
        if dispatch_args.get("outcome") not in (None, "accepted"):
            continue
        end_ms = dispatch_args.get("end_ms")
        if not isinstance(end_ms, int) or end_ms <= 0:
            continue

        position_index = bisect.bisect_left(positions, end_ms / 1000.0)
        if position_index >= len(audio_positions):
            continue
        capture_stamp = audio_positions[position_index][1]
        captured_at = capture_times[capture_stamp]
        committed_at = int(commits[sequence]["ts"])
        converted = duration_slice(
            TRACKS["end_to_end"],
            captured_at,
            committed_at,
            start_us,
            {
                "secuencia": sequence,
                "tipo": "final" if dispatch_args.get("is_final") else "parcial",
                "fin_audio_ms": end_ms,
            },
        )
        if converted is not None:
            result.append(converted)
    return result


def build_presentation_trace(
    source: dict[str, Any], start_us: int | None = None, end_us: int | None = None
) -> dict[str, Any]:
    events = source["traceEvents"]
    detected_start, detected_end = detect_ready_window(events)
    window_start = detected_start if start_us is None else start_us
    window_end = detected_end if end_us is None else end_us
    if window_end <= window_start:
        raise ValueError("el fin de la ventana debe ser posterior al inicio")

    output = metadata_events()
    output.append(
        duration_slice(
            TRACKS["stable_session"],
            window_start,
            window_end,
            window_start,
            {"duración_s": round((window_end - window_start) / 1_000_000.0, 3)},
        )
    )
    output.extend(source_slices(events, window_start, window_end))
    output.extend(transcript_delivery_slices(events, window_start, window_end))
    output.extend(end_to_end_slices(events, window_start, window_end))
    output = [event for event in output if event is not None]
    output.sort(key=lambda event: (event.get("ts", -1), event.get("pid", 0), event.get("tid", 0)))

    source_metadata = source.get("metadata") or {}
    return {
        "traceEvents": output,
        "displayTimeUnit": "ms",
        "metadata": {
            "source": "subtitle_overlay_fw presentation profiling",
            "derived_from": source_metadata.get("source", "unified trace"),
            "board_run_id": source_metadata.get("board_run_id"),
            "server_run_id": source_metadata.get("server_run_id"),
            "window_start_source_ms": round(window_start / 1000.0, 3),
            "window_end_source_ms": round(window_end / 1000.0, 3),
            "window_duration_ms": round((window_end - window_start) / 1000.0, 3),
            "presentation_only": True,
            "omitted": "instant events, counters and flow arrows",
        },
    }


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(
        description="Create a clean, duration-only Perfetto trace for presentation"
    )
    parser.add_argument("input", type=Path, help="Existing unified_trace.json")
    parser.add_argument("output", type=Path, help="Destination JSON")
    parser.add_argument(
        "--start-ms",
        type=float,
        help="Override the automatically detected ready-window start",
    )
    parser.add_argument(
        "--end-ms",
        type=float,
        help="Override the automatically detected ready-window end",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    try:
        source = load_trace(args.input)
        presentation = build_presentation_trace(
            source,
            None if args.start_ms is None else round(args.start_ms * 1000),
            None if args.end_ms is None else round(args.end_ms * 1000),
        )
    except ValueError as exc:
        print(f"Error: {exc}", file=sys.stderr)
        return 2

    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(presentation, indent=2), encoding="utf-8")

    phases = {event.get("ph") for event in presentation["traceEvents"]}
    slices = sum(event.get("ph") == "X" for event in presentation["traceEvents"])
    metadata = presentation["metadata"]
    print(f"Presentation trace: {args.output}")
    print(f"  stable window : {metadata['window_duration_ms'] / 1000.0:.3f} s")
    print(f"  duration bars : {slices}")
    print(f"  event phases  : {', '.join(sorted(phase for phase in phases if phase))}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
