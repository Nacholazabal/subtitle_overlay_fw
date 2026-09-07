#!/usr/bin/env python3
"""Merge firmware and server trace files into unified Perfetto timeline.

Usage:
    python3 scripts/merge_traces.py [--output trace.json]

Reads:
    - /tmp/fw_trace.jsonl (from board, copy with scp)
    - logs/server_trace.jsonl (from Python server)
    - logs/stt_events.jsonl (legacy format, optional)

Outputs:
    - Merged Chrome Trace Format JSON for Perfetto
"""

import argparse
import json
import sys
from pathlib import Path


def load_chrome_trace_events(path):
    """Load events from Chrome Trace Format NDJSON file."""
    events = []
    if not Path(path).exists():
        print(f"  Skip: {path} not found", file=sys.stderr)
        return events

    repaired_utf8 = 0
    with open(path, encoding="utf-8", errors="replace") as f:
        for lineno, line in enumerate(f, 1):
            line = line.strip()
            if not line:
                continue
            if "\ufffd" in line:
                repaired_utf8 += 1
            try:
                event = json.loads(line)
                events.append(event)
            except json.JSONDecodeError as exc:
                print(f"  Warning: {path}:{lineno}: {exc}", file=sys.stderr)

    print(f"  Loaded {len(events)} events from {path}")
    if repaired_utf8:
        print(
            f"  Warning: repaired truncated UTF-8 in {repaired_utf8} "
            f"line(s) from {path}",
            file=sys.stderr,
        )
    return events


def convert_legacy_stt_events(path):
    """Convert legacy stt_events.jsonl to Chrome Trace Format."""
    events = []
    if not Path(path).exists():
        return events

    with open(path, encoding="utf-8") as f:
        for lineno, line in enumerate(f, 1):
            line = line.strip()
            if not line:
                continue
            try:
                evt = json.loads(line)

                # Extract timing
                start_sec = evt.get("start_sec")
                end_sec = evt.get("end_sec")
                if start_sec is None or end_sec is None:
                    continue

                # Convert to Chrome Trace Format
                ts_us = int(float(start_sec) * 1_000_000)
                dur_us = int((float(end_sec) - float(start_sec)) * 1_000_000)

                name = "transcript"
                if evt.get("is_final"):
                    name = "final"

                args = {
                    "text": evt.get("text", "")[:50],
                    "seq": evt.get("seq"),
                }

                # Add timing metrics if available
                for key in ["queue_wait_sec", "gpu_infer_sec", "emit_lag_sec"]:
                    if key in evt:
                        args[key] = evt[key]

                events.append({
                    "name": name,
                    "ph": "X",  # Complete event
                    "ts": ts_us,
                    "dur": dur_us,
                    "pid": 100,  # Separate process for STT
                    "tid": 1,
                    "cat": "stt",
                    "args": args
                })

            except (json.JSONDecodeError, ValueError, TypeError) as exc:
                print(f"  Warning: {path}:{lineno}: {exc}", file=sys.stderr)

    if events:
        print(f"  Converted {len(events)} legacy STT events from {path}")
    return events


def merge_and_export(output_path, fw_trace, server_trace, stt_events):
    """Merge all traces and export to Perfetto JSON."""
    all_events = []

    # Load Chrome Trace Format events (already in correct format)
    all_events.extend(load_chrome_trace_events(fw_trace))
    all_events.extend(load_chrome_trace_events(server_trace))

    # Convert legacy STT events
    all_events.extend(convert_legacy_stt_events(stt_events))

    if not all_events:
        print("Error: no events found in any trace file", file=sys.stderr)
        return False

    # Export to Chrome Trace Format JSON
    output = {
        "traceEvents": all_events,
        "displayTimeUnit": "ms",
        "metadata": {
            "source": "subtitle_overlay_fw unified profiling"
        }
    }

    Path(output_path).write_text(json.dumps(output, indent=2), encoding="utf-8")
    print(f"\n✓ Merged {len(all_events)} events → {output_path}")
    print(f"  Open in Perfetto: https://ui.perfetto.dev")

    return True


def main():
    parser = argparse.ArgumentParser(
        description="Merge firmware and server traces for Perfetto"
    )
    parser.add_argument(
        "--fw",
        default="/tmp/fw_trace.jsonl",
        help="Firmware trace (default: /tmp/fw_trace.jsonl from board)"
    )
    parser.add_argument(
        "--server",
        default="logs/server_trace.jsonl",
        help="Server trace (default: logs/server_trace.jsonl)"
    )
    parser.add_argument(
        "--stt",
        default="logs/stt_events.jsonl",
        help="Legacy STT events (default: logs/stt_events.jsonl)"
    )
    parser.add_argument(
        "--output",
        default="logs/unified_trace.json",
        help="Output Perfetto JSON (default: logs/unified_trace.json)"
    )

    args = parser.parse_args()

    print("Merging traces...")
    success = merge_and_export(args.output, args.fw, args.server, args.stt)
    return 0 if success else 1


if __name__ == "__main__":
    sys.exit(main())
