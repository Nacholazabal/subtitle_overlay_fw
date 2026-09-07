#!/usr/bin/env python3
"""Stitch the board and server traces into one Perfetto timeline.

The two tracers each number their events from their own start, so the files
cannot simply be concatenated. This script puts every event on the **board's**
clock, which is the one that matters: a subtitle's end-to-end latency starts and
ends on the board, so once the server's work is placed on that timeline the whole
round trip can be read directly.

How the clocks are related without trusting NTP
-----------------------------------------------
Every audio chunk carries the board's own capture timestamp in its header, and
the server records it in ``audio_frame_rx.board_capture_ts_ns``. Each of those
events is therefore a sample of both clocks at once::

    delta = server_receive_time - board_capture_time = clock_offset + transit

``transit`` is unknown but never negative, so the smallest observed delta is the
tightest bound on the offset. Using it means the fastest chunk of the run appears
to arrive the instant it was captured, and every other chunk is placed later --
never earlier than its own capture. Causality is preserved, and the reported
network time is relative to the best observed chunk rather than to a
wall-clock guess.

When no such sample exists the script falls back to the realtime anchors written
by both tracers, which is NTP-grade and labelled as such.
"""

import argparse
import json
import sys
from pathlib import Path

# Synthetic process ids. The board and Colab can genuinely share a pid, so the
# originals are replaced; Chrome Trace Format scopes tid within pid, which makes
# the thread tracks unambiguous once the pids differ.
PID_BY_SOURCE = {"board": 1, "server": 2, "stt": 3}


def load_ndjson(path, label):
    """Read one NDJSON trace, reporting anything that could not be parsed."""
    events = []
    broken = 0

    if not path or not Path(path).exists():
        print(f"  {label}: not found ({path})")
        return events, broken

    with open(path, "rb") as handle:
        for lineno, raw in enumerate(handle, 1):
            raw = raw.strip()
            if not raw:
                continue
            try:
                events.append(json.loads(raw.decode("utf-8")))
            except (UnicodeDecodeError, json.JSONDecodeError) as exc:
                broken += 1
                if broken <= 3:
                    print(f"  {label}:{lineno}: unusable line ({exc})", file=sys.stderr)

    suffix = f", {broken} unusable" if broken else ""
    print(f"  {label}: {len(events)} events{suffix}")
    return events, broken


def trace_start(events):
    """Return the trace_start arguments, which carry the clock anchors."""
    for event in events:
        if event.get("name") == "trace_start":
            return event.get("args") or {}
    return {}


def count_trace_starts(events):
    """How many runs this file holds.

    More than one means two different clock bases were appended together, and
    every timestamp after the first run is off by the gap between them.
    """
    return sum(1 for event in events if event.get("name") == "trace_start")


def absolute_ns(event, base_ns):
    """Recover the emitting process's monotonic clock value for an event."""
    return base_ns + int(event.get("ts", 0)) * 1000


def estimate_offset(board_meta, server_events, server_meta):
    """Offset in ns to subtract from server monotonic time to get board time.

    Returns (offset_ns, method, detail) or (None, reason, detail) when the two
    files cannot be related at all.
    """
    board_base = board_meta.get("clock_monotonic_ns")
    server_base = server_meta.get("clock_monotonic_ns")
    if board_base is None or server_base is None:
        return None, "no_anchor", "one of the traces has no clock anchor"

    deltas = []
    for event in server_events:
        if event.get("name") != "audio_frame_rx":
            continue
        captured = (event.get("args") or {}).get("board_capture_ts_ns")
        if not isinstance(captured, int) or captured <= 0:
            continue
        deltas.append(absolute_ns(event, server_base) - captured)

    if deltas:
        offset = min(deltas)
        spread_ms = (max(deltas) - offset) / 1e6
        detail = (
            f"{len(deltas)} shared audio timestamps, "
            f"transit spread {spread_ms:.1f} ms above the fastest chunk"
        )
        return offset, "shared_audio_timestamps", detail

    board_real = board_meta.get("clock_realtime_ns")
    server_real = server_meta.get("clock_realtime_ns")
    if board_real is not None and server_real is not None:
        # (server_mono - board_mono) expressed through the two realtime anchors.
        offset = (server_base - server_real) - (board_base - board_real)
        return offset, "realtime_anchors_ntp", "no shared audio timestamps; NTP-grade only"

    return None, "no_anchor", "no shared timestamps and no realtime anchors"


def rebase(events, shift_us, pid):
    """Move events onto the board timeline and give the source its own pid."""
    for event in events:
        if "ts" in event:
            event["ts"] = int(event["ts"]) + shift_us
        event["pid"] = pid
    return events


def index_by_capture_stamp(events, name, key):
    """Map board capture timestamp -> first event of @p name carrying it.

    The board's capture timestamp is the only identifier that means the same
    thing on both sides. The two sequence counters do not: the capture counter
    runs from process start, while the protocol's audio_seq restarts at zero on
    every STT session, so a reconnect makes them disagree.
    """
    indexed = {}
    for event in events:
        stamp = (event.get("args") or {}).get(key)
        if event.get("name") == name and isinstance(stamp, int) and stamp > 0:
            indexed.setdefault(stamp, event)
    return indexed


def build_flows(board_events, server_events):
    """Draw one arrow per audio chunk, from the board's send to the server's work."""
    sends = index_by_capture_stamp(board_events, "audio_ws_send", "capture_ts_ns")
    arrivals = index_by_capture_stamp(
        server_events, "session_push_pcm", "board_capture_ts_ns"
    )

    paired = sorted(set(sends) & set(arrivals))
    flows = []
    for flow_id, stamp in enumerate(paired):
        start, finish = sends[stamp], arrivals[stamp]
        common = {"name": "audio", "cat": "audio", "id": flow_id, "bp": "e"}
        flows.append(dict(common, ph="s", ts=start["ts"], pid=start["pid"], tid=start["tid"]))
        flows.append(dict(common, ph="f", ts=finish["ts"], pid=finish["pid"], tid=finish["tid"]))
    return flows, len(paired)


def percentile(values, fraction):
    if not values:
        return None
    ordered = sorted(values)
    index = min(len(ordered) - 1, int(round(fraction * (len(ordered) - 1))))
    return ordered[index]


def stage_summary(board_events, server_events):
    """Report how long each stage took, in milliseconds."""
    stages = {}

    def collect(events, name, label):
        durations = [
            event["dur"] / 1000.0
            for event in events
            if event.get("name") == name and "dur" in event
        ]
        if durations:
            stages[label] = durations

    collect(board_events, "alsa_read", "board: alsa_read")
    collect(board_events, "audio_ws_send", "board: audio_ws_send")
    collect(board_events, "subtitle_render", "board: subtitle_render")
    collect(server_events, "session_push_pcm", "server: session_push_pcm")
    collect(server_events, "nemotron_step", "server: nemotron_step")

    return stages


def end_to_end_latencies(board_events, board_meta, server_events):
    """Milliseconds from the audio a caption describes to that caption's commit.

    Both endpoints are board events on the board's own clock, so this figure does
    not depend on the clock alignment at all. What it does need is the answer to
    "which audio does this caption describe?", and that is the transcript's
    ``end_sec``: the server states how far into the stream the text reaches.

    The server's ``session_push_pcm`` slices provide the mapping from that audio
    position back to the board timestamp of the chunk that completed it. Pairing a
    commit with the *most recently captured* chunk instead -- which an earlier
    version of this script did -- measures the age of the newest audio in the
    buffer, roughly one chunk period, and has nothing to do with latency.
    """
    board_base = board_meta.get("clock_monotonic_ns")
    if board_base is None:
        return [], "the board trace has no clock anchor"

    # audio position (seconds) -> board capture timestamp of the completing chunk
    timeline = sorted(
        (args["audio_end_sec"], args["board_capture_ts_ns"])
        for args in (event.get("args") or {} for event in server_events)
        if isinstance(args.get("audio_end_sec"), (int, float))
        and isinstance(args.get("board_capture_ts_ns"), int)
    )
    if not timeline:
        return [], "the server trace has no audio-position samples"

    # transcript sequence -> the audio position that transcript covers
    covered_until = {}
    for event in board_events:
        if event.get("name") != "transcript_dispatch":
            continue
        args = event.get("args") or {}
        seq, end_ms = args.get("transcript_seq"), args.get("end_ms")
        if isinstance(seq, int) and isinstance(end_ms, int) and end_ms > 0:
            covered_until.setdefault(seq, end_ms / 1000.0)

    if not covered_until:
        return [], "no transcript_dispatch events carried an end_ms"

    latencies = []
    for event in board_events:
        if event.get("name") != "overlay_commit":
            continue
        seq = (event.get("args") or {}).get("transcript_seq")
        end_sec = covered_until.get(seq)
        if end_sec is None:
            continue

        # First chunk whose audio reaches the end of what the caption describes.
        capture_ns = next((stamp for position, stamp in timeline if position >= end_sec), None)
        if capture_ns is None:
            continue

        commit_ns = board_base + int(event["ts"]) * 1000
        if commit_ns > capture_ns:
            latencies.append((commit_ns - capture_ns) / 1e6)

    if not latencies:
        return [], "no caption could be matched to the audio it describes"
    return latencies, None


def print_summary(stages):
    if not stages:
        return
    print("\nStage durations (ms):")
    print(f"  {'stage':<40} {'n':>6} {'p50':>9} {'p90':>9} {'max':>9}")
    for label, values in stages.items():
        print(
            f"  {label:<40} {len(values):>6} "
            f"{percentile(values, 0.50):>9.1f} "
            f"{percentile(values, 0.90):>9.1f} "
            f"{max(values):>9.1f}"
        )


def main():
    parser = argparse.ArgumentParser(
        description="Stitch board and server traces into one Perfetto timeline"
    )
    parser.add_argument("--fw", default="logs/fw_trace.jsonl", help="Board trace")
    parser.add_argument(
        "--server", default="logs/profiling/server_trace.jsonl", help="Server trace"
    )
    parser.add_argument(
        "--output", default="logs/unified_trace.json", help="Perfetto JSON destination"
    )
    parser.add_argument(
        "--no-flows", action="store_true", help="Skip the per-chunk flow arrows"
    )
    args = parser.parse_args()

    print("Loading traces...")
    board_events, board_broken = load_ndjson(args.fw, "board")
    server_events, server_broken = load_ndjson(args.server, "server")

    if not board_events and not server_events:
        print("Error: no events in either trace", file=sys.stderr)
        return 1

    board_meta = trace_start(board_events)
    server_meta = trace_start(server_events)

    for label, events in (("board", board_events), ("server", server_events)):
        runs = count_trace_starts(events)
        if runs > 1:
            print(
                f"\n  WARNING: the {label} trace holds {runs} runs. Only the first clock\n"
                f"  base is used, so every later run's timestamps are wrong. Capture one\n"
                f"  run per file.",
                file=sys.stderr,
            )

    print("\nAligning clocks...")
    shift_us = 0
    if board_events and server_events:
        offset, method, detail = estimate_offset(board_meta, server_events, server_meta)
        if offset is None:
            print(f"  NOT ALIGNED: {detail}")
            print("  Server events keep their own origin; cross-source timing is meaningless.")
            method = None
        else:
            board_base = board_meta["clock_monotonic_ns"]
            server_base = server_meta["clock_monotonic_ns"]
            shift_us = (server_base - offset - board_base) // 1000
            print(f"  method: {method}")
            print(f"  {detail}")
            print(f"  server events shifted by {shift_us / 1000.0:+.1f} ms onto the board clock")
            if method == "realtime_anchors_ntp":
                print("  NOTE: NTP-grade alignment; do not read sub-100 ms cross-source gaps.")
    elif not board_events:
        print("  server trace only; nothing to align")
    else:
        print("  board trace only; nothing to align")

    rebase(board_events, 0, PID_BY_SOURCE["board"])
    rebase(server_events, shift_us, PID_BY_SOURCE["server"])

    merged = board_events + server_events

    if not args.no_flows and board_events and server_events:
        flows, paired = build_flows(board_events, server_events)
        merged.extend(flows)
        print(f"\nFlows: {paired} audio chunks linked board -> server")

    merged.sort(key=lambda event: event.get("ts", 0))

    print_summary(stage_summary(board_events, server_events))

    latencies, reason = end_to_end_latencies(board_events, board_meta, server_events)
    if latencies:
        print("\nEnd to end, audio captured -> caption committed (ms):")
        print(
            f"  n={len(latencies)}  "
            f"p50={percentile(latencies, 0.50):.0f}  "
            f"p90={percentile(latencies, 0.90):.0f}  "
            f"max={max(latencies):.0f}"
        )
    else:
        print(f"\nEnd to end: not available -- {reason}")

    output = {
        "traceEvents": merged,
        "displayTimeUnit": "ms",
        "metadata": {
            "source": "subtitle_overlay_fw unified profiling",
            "board_run_id": board_meta.get("run_id"),
            "server_run_id": server_meta.get("run_id"),
            "board_build_id": board_meta.get("build_id"),
            "clock_alignment": method if board_events and server_events else "single_source",
            "unusable_lines": board_broken + server_broken,
        },
    }

    destination = Path(args.output)
    destination.parent.mkdir(parents=True, exist_ok=True)
    destination.write_text(json.dumps(output, indent=2), encoding="utf-8")

    print(f"\n{len(merged)} events -> {destination}")
    print("Open it at https://ui.perfetto.dev")
    return 0


if __name__ == "__main__":
    sys.exit(main())
