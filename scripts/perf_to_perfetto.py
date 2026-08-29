#!/usr/bin/env python3
"""Convert perf script output to Perfetto/Chrome trace format.

Usage:
    python3 scripts/perf_to_perfetto.py logs/perf_trace.txt [output.json]
"""

import argparse
import json
import re
import sys
from collections import defaultdict
from pathlib import Path


# Parse perf script output line
# Format: time comm pid/tid event: address symbol
PERF_LINE_RE = re.compile(
    r'^\s*(\S+):\s+(\S+)\s+(\d+)/(\d+)\s+(\S+):\s+([0-9a-f]+)\s+(.+)$'
)


def parse_perf_script(path):
    """Parse perf script output and yield events."""
    with open(path) as f:
        for lineno, line in enumerate(f, 1):
            line = line.strip()
            if not line:
                continue

            match = PERF_LINE_RE.match(line)
            if not match:
                # Skip header lines or malformed lines
                continue

            time_str, comm, pid, tid, event, addr, symbol = match.groups()

            try:
                # Parse timestamp (seconds with microsecond precision)
                timestamp_sec = float(time_str.rstrip(':'))
            except ValueError:
                print(f"Warning: line {lineno}: invalid timestamp {time_str!r}", file=sys.stderr)
                continue

            yield {
                "ts_sec": timestamp_sec,
                "comm": comm,
                "pid": int(pid),
                "tid": int(tid),
                "event": event,
                "addr": addr,
                "symbol": symbol.strip(),
            }


def events_to_perfetto(events, output_path):
    """Convert perf events to Perfetto trace format."""

    # Find earliest timestamp as baseline
    first_ts = min(e["ts_sec"] for e in events) if events else 0

    # Group samples by process/thread to build call stacks
    trace_events = []

    for event in events:
        ts_us = int((event["ts_sec"] - first_ts) * 1_000_000)

        # Create an instant event for each sample
        # In perf, each sample is a point in time, not a duration
        trace_events.append({
            "name": event["symbol"],
            "cat": "perf",
            "ph": "i",  # Instant event
            "ts": ts_us,
            "pid": event["pid"],
            "tid": event["tid"],
            "s": "t",  # Thread scope
            "args": {
                "event": event["event"],
                "comm": event["comm"],
                "addr": event["addr"],
            }
        })

    # Write Perfetto JSON
    output = {
        "traceEvents": trace_events,
        "displayTimeUnit": "ms",
        "metadata": {
            "perfetto_version": "perf_script_converter",
            "command_line": " ".join(sys.argv),
        }
    }

    Path(output_path).write_text(json.dumps(output, indent=2))
    print(f"Converted {len(trace_events)} perf samples → {output_path}")
    print(f"Open in Perfetto: https://ui.perfetto.dev")


def main():
    parser = argparse.ArgumentParser(
        description="Convert perf script output to Perfetto trace format"
    )
    parser.add_argument(
        "input",
        help="perf script output file (e.g., logs/perf_trace.txt)"
    )
    parser.add_argument(
        "output",
        nargs="?",
        default="logs/perfetto_trace.json",
        help="Output Perfetto JSON file (default: logs/perfetto_trace.json)"
    )

    args = parser.parse_args()

    if not Path(args.input).exists():
        print(f"Error: input file not found: {args.input}", file=sys.stderr)
        sys.exit(1)

    print(f"Parsing perf trace: {args.input}")
    events = list(parse_perf_script(args.input))

    if not events:
        print("Warning: no perf events found in input", file=sys.stderr)
        sys.exit(1)

    print(f"Found {len(events)} perf samples")
    events_to_perfetto(events, args.output)


if __name__ == "__main__":
    main()
