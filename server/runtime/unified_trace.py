#!/usr/bin/env python3
"""Unified tracing for server-side components.

Emits Chrome Trace Format events compatible with firmware traces.
All events can be merged and visualized in Perfetto.
"""

import json
import os
import time
from pathlib import Path
from typing import Optional


class UnifiedTracer:
    """Unified trace event emitter for Python components."""

    def __init__(self, output_path: Optional[str] = None, source: str = "server"):
        """Initialize tracer.

        Args:
            output_path: Path to NDJSON trace file (default: logs/server_trace.jsonl)
            source: Source identifier (e.g., "server", "bridge", "stt")
        """
        self.output_path = Path(output_path or "logs/server_trace.jsonl")
        self.output_path.parent.mkdir(parents=True, exist_ok=True)

        self.source = source
        self.pid = os.getpid()
        self.tid = 1  # Main thread
        self.base_time = time.monotonic_ns()

        # Open trace file
        self.file = open(self.output_path, "w", encoding="utf-8")

        # Emit metadata
        self._emit_metadata("process_name", {"name": f"subtitle-{source}"})
        self._emit_metadata("thread_name", {"name": "main"})

    def close(self):
        """Flush and close trace file."""
        if self.file:
            self.file.flush()
            self.file.close()
            self.file = None

    def __enter__(self):
        return self

    def __exit__(self, *args):
        self.close()

    def _emit_metadata(self, name: str, args: dict):
        """Emit metadata event."""
        event = {
            "name": name,
            "ph": "M",
            "pid": self.pid,
            "tid": self.tid,
            "args": args
        }
        self.file.write(json.dumps(event) + "\n")
        self.file.flush()

    def _emit(self, name: str, phase: str, ts_ns: int, dur_ns: int = 0, args: Optional[dict] = None):
        """Emit trace event in Chrome Trace Format."""
        # Convert to relative timestamp (microseconds)
        ts_us = (ts_ns - self.base_time) // 1000

        event = {
            "name": name,
            "ph": phase,
            "ts": ts_us,
            "pid": self.pid,
            "tid": self.tid,
            "cat": self.source,
        }

        if dur_ns > 0:
            event["dur"] = dur_ns // 1000  # microseconds

        if args:
            event["args"] = args

        self.file.write(json.dumps(event) + "\n")
        self.file.flush()

    def instant(self, name: str, **args):
        """Emit an instant event (point in time)."""
        now_ns = time.monotonic_ns()
        self._emit(name, "i", now_ns, args=args if args else None)

    def begin(self, name: str, **args) -> int:
        """Begin an async event.

        Returns:
            Timestamp to pass to end()
        """
        now_ns = time.monotonic_ns()
        self._emit(name, "B", now_ns, args=args if args else None)
        return now_ns

    def end(self, name: str, start_ns: int, **args):
        """End an async event started with begin()."""
        now_ns = time.monotonic_ns()
        self._emit(name, "E", now_ns, args=args if args else None)

    def duration(self, name: str, duration_ns: int, **args):
        """Emit a complete event (duration in one call)."""
        end_ns = time.monotonic_ns()
        start_ns = end_ns - duration_ns
        self._emit(name, "X", start_ns, dur_ns=duration_ns, args=args if args else None)


class TraceScope:
    """Context manager for scoped trace events."""

    def __init__(self, tracer: UnifiedTracer, name: str, **args):
        self.tracer = tracer
        self.name = name
        self.args = args
        self.start_ns = None

    def __enter__(self):
        self.start_ns = self.tracer.begin(self.name, **self.args)
        return self

    def __exit__(self, *args):
        if self.start_ns:
            self.tracer.end(self.name, self.start_ns)


# Example usage:
if __name__ == "__main__":
    with UnifiedTracer(output_path="test_trace.jsonl", source="test") as tracer:
        # Instant event
        tracer.instant("startup", config="test")

        # Scoped event
        with TraceScope(tracer, "processing"):
            time.sleep(0.01)  # Simulate work
            tracer.instant("checkpoint", progress=50)

        # Duration event
        start = time.monotonic_ns()
        time.sleep(0.005)
        end = time.monotonic_ns()
        tracer.duration("compute", end - start, ops=1000)

    print("✓ Test trace written to test_trace.jsonl")
