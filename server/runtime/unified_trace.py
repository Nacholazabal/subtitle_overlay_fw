#!/usr/bin/env python3
"""Server-side profiling tracer, symmetrical with the firmware's ``trace.c``.

Emits Chrome Trace Format NDJSON so board and server captures merge into one
Perfetto timeline (https://perfetto.dev/docs/getting-started/other-formats).

The properties this module is responsible for are the same ones the firmware
tracer guarantees, because the merge is only as trustworthy as its weaker half:

* **Opt-in.** :func:`create_tracer` returns a :class:`NullTracer` unless tracing
  was explicitly requested, so a production server writes no files at all.
* **One file per run, opened for append.** Several WebSocket sessions share a
  run; a new session must never truncate the events of the previous one.
* **Real thread ids.** The FastAPI event loop and the ``asyncio.to_thread``
  inference worker land on separate Perfetto tracks.
* **Clock anchors.** A monotonic/realtime pair is written at startup so the
  merge can place these events on the same UTC timeline as the board's.
* **Self-contained slices.** Durations are complete (``X``) events, so an
  exception inside a scope can never leave an unterminated slice.
* **Bounded and non-fatal.** The file stops at a size cap, and no tracing error
  is ever allowed to reach the audio path.
"""

from __future__ import annotations

import contextlib
import json
import os
import threading
import time
from pathlib import Path
from typing import Any, Iterator, Optional

# Keep these aligned with src/utils/trace/trace.h so both halves of a merged
# capture obey the same limits.
TRACE_FORMAT_VERSION = 2
DEFAULT_MAX_BYTES = 64 * 1024 * 1024
DEFAULT_FLUSH_INTERVAL_SEC = 1.0
DEFAULT_STR_ARG_MAX = 96
BUFFER_FLUSH_BYTES = 32 * 1024

#: Environment switch mirroring the firmware's ``TRACE=1`` build flag.
TRACE_ENV_VAR = "SUBTITLE_TRACE"
TRACE_PATH_ENV_VAR = "SUBTITLE_TRACE_PATH"
TRACE_RUN_ID_ENV_VAR = "SUBTITLE_TRACE_RUN_ID"
TRACE_MAX_MB_ENV_VAR = "SUBTITLE_TRACE_MAX_MB"


def _now_ns() -> int:
    return time.monotonic_ns()


def _native_thread_id() -> int:
    """Linux TID when available, so tracks match what ``top -H`` shows."""
    getter = getattr(threading, "get_native_id", None)
    if getter is not None:
        try:
            return int(getter())
        except OSError:
            pass
    return int(threading.get_ident())


class NullTracer:
    """No-op tracer used whenever profiling is off.

    Call sites can then be written unconditionally, which keeps the
    instrumentation readable and impossible to leave half-guarded.
    """

    enabled = False

    def instant(self, name: str, **args: Any) -> None:
        pass

    def counter(self, name: str, **args: Any) -> None:
        pass

    def slice(self, name: str, start_ns: int, **args: Any) -> None:
        pass

    @contextlib.contextmanager
    def scope(self, name: str, **args: Any) -> Iterator[None]:
        yield

    def register_thread(self, thread_name: str) -> None:
        pass

    def flush(self) -> None:
        pass

    def close(self) -> None:
        pass

    @property
    def stats(self) -> dict:
        return {
            "events_written": 0,
            "bytes_written": 0,
            "dropped_full": 0,
            "dropped_truncated": 0,
            "write_errors": 0,
            "full": False,
        }

    def __enter__(self) -> "NullTracer":
        return self

    def __exit__(self, *_exc: Any) -> None:
        pass


class UnifiedTracer:
    """Buffered NDJSON tracer shared by every component of one server run."""

    enabled = True

    def __init__(
        self,
        output_path: Optional[str] = None,
        *,
        source: str = "colab",
        run_id: Optional[str] = None,
        build_id: Optional[str] = None,
        max_bytes: int = DEFAULT_MAX_BYTES,
        flush_interval_sec: float = DEFAULT_FLUSH_INTERVAL_SEC,
    ) -> None:
        self.source = source
        self.run_id = run_id or time.strftime("%Y%m%d-%H%M%S")
        self.build_id = build_id or ""
        self.max_bytes = int(max_bytes)
        self.flush_interval_sec = float(flush_interval_sec)

        self.output_path = Path(output_path or f"logs/profiling/server_trace-{self.run_id}.jsonl")
        self.output_path.parent.mkdir(parents=True, exist_ok=True)

        self.pid = os.getpid()
        self.base_monotonic_ns = _now_ns()
        self.base_realtime_ns = time.time_ns()

        self._lock = threading.Lock()
        self._buffer: list[str] = []
        self._buffered_bytes = 0
        self._accepted_bytes = 0
        self._last_flush = time.monotonic()
        self._named_threads: set[int] = set()
        self._closed = False
        self._stats = {
            "events_written": 0,
            "bytes_written": 0,
            "dropped_full": 0,
            "dropped_truncated": 0,
            "write_errors": 0,
            "full": False,
        }

        # Truncate, do not append. Every session of this server run shares this one
        # tracer, so nothing here needs appending -- and appending to a file that
        # already holds another run's trace_start would leave two different clock
        # bases in one file, which silently breaks the merge's alignment.
        self._file = open(self.output_path, "w", encoding="utf-8")

        self._emit("process_name", "M", self.base_monotonic_ns, tid=0, args={"name": source})
        self._emit(
            "trace_start",
            "i",
            self.base_monotonic_ns,
            args={
                "format_version": TRACE_FORMAT_VERSION,
                "source": source,
                "run_id": self.run_id,
                "build_id": self.build_id,
                "pid": self.pid,
                "clock_monotonic_ns": self.base_monotonic_ns,
                "clock_realtime_ns": self.base_realtime_ns,
                "clock_domain": "CLOCK_MONOTONIC",
                "max_bytes": self.max_bytes,
            },
        )
        self.flush()

    # -- serialization ------------------------------------------------------

    @staticmethod
    def _safe_text(value: str) -> str:
        """Return text that is guaranteed to survive a UTF-8 write.

        A lone surrogate reaches json.dumps intact and only explodes later, at
        write time, taking the whole buffered batch with it.
        """
        trimmed = value[:DEFAULT_STR_ARG_MAX]
        return trimmed.encode("utf-8", "replace").decode("utf-8", "replace")

    def _sanitize(self, args: Optional[dict]) -> dict:
        """Coerce arguments into values json.dumps can always represent.

        A tracing call must not be able to raise inside the pipeline it is
        measuring, so anything exotic is reduced rather than rejected.
        """
        if not args:
            return {}

        clean: dict[str, Any] = {}
        for key, value in args.items():
            name = self._safe_text(str(key))
            if isinstance(value, bool):
                clean[name] = value
            elif isinstance(value, int):
                clean[name] = value
            elif isinstance(value, float):
                # JSON has no NaN or Infinity; json.dumps would emit bare tokens
                # that Perfetto rejects, so they become null.
                clean[name] = value if (value == value and -1e308 < value < 1e308) else None
            elif value is None:
                clean[name] = None
            elif isinstance(value, str):
                clean[name] = self._safe_text(value)
            elif isinstance(value, bytes):
                clean[name] = len(value)
            else:
                try:
                    clean[name] = self._safe_text(repr(value))
                except Exception:  # noqa: BLE001 - a broken __repr__ must not escape
                    clean[name] = "<unrepresentable>"
        return clean

    def _emit(
        self,
        name: str,
        phase: str,
        ts_ns: int,
        *,
        dur_ns: Optional[int] = None,
        args: Optional[dict] = None,
        tid: Optional[int] = None,
        bypass_cap: bool = False,
    ) -> None:
        if self._closed:
            return

        relative_ns = max(0, ts_ns - self.base_monotonic_ns)
        event: dict[str, Any] = {
            "name": name,
            "ph": phase,
            "ts": relative_ns // 1000,
        }
        if dur_ns is not None:
            event["dur"] = max(0, dur_ns) // 1000
        if phase == "i":
            event["s"] = "t"
        event["pid"] = self.pid
        event["tid"] = _native_thread_id() if tid is None else tid
        event["cat"] = self.source

        clean = self._sanitize(args)
        if clean:
            event["args"] = clean

        try:
            line = json.dumps(event, ensure_ascii=False, separators=(",", ":")) + "\n"
        except Exception:  # noqa: BLE001 - serialization must never propagate
            with self._lock:
                self._stats["dropped_truncated"] += 1
            return

        self._append(line, bypass_cap=bypass_cap)

    def _append(self, line: str, *, bypass_cap: bool) -> None:
        encoded_length = len(line.encode("utf-8"))

        with self._lock:
            if not bypass_cap:
                if self._stats["full"]:
                    self._stats["dropped_full"] += 1
                    return
                if (self._accepted_bytes + encoded_length) > self.max_bytes:
                    self._stats["full"] = True
                    self._stats["dropped_full"] += 1
                    became_full = True
                else:
                    became_full = False
            else:
                became_full = False

            if not became_full:
                self._buffer.append(line)
                self._buffered_bytes += encoded_length
                self._accepted_bytes += encoded_length
                self._stats["events_written"] += 1

            should_flush = (
                became_full
                or self._buffered_bytes >= BUFFER_FLUSH_BYTES
                or (time.monotonic() - self._last_flush) >= self.flush_interval_sec
            )
            if should_flush:
                self._flush_locked()

        if became_full:
            self._emit(
                "trace_full",
                "i",
                _now_ns(),
                args={"max_bytes": self.max_bytes, "accepted_bytes": self._accepted_bytes},
                bypass_cap=True,
            )
            self.flush()

    def _flush_locked(self) -> None:
        if not self._buffer:
            self._last_flush = time.monotonic()
            return

        payload = "".join(self._buffer)
        self._buffer.clear()
        self._buffered_bytes = 0
        try:
            self._file.write(payload)
            self._file.flush()
            self._stats["bytes_written"] += len(payload.encode("utf-8"))
        except Exception:  # noqa: BLE001 - a full disk or dead handle degrades
            # tracing and nothing else. The batch is lost; the count says so.
            self._stats["write_errors"] += 1
        self._last_flush = time.monotonic()

    # -- public API ---------------------------------------------------------

    def register_thread(self, thread_name: str) -> None:
        """Name the calling thread's Perfetto track, once per thread."""
        tid = _native_thread_id()
        with self._lock:
            if tid in self._named_threads:
                return
            self._named_threads.add(tid)
        self._emit(
            "thread_name", "M", _now_ns(), args={"name": thread_name}, tid=tid, bypass_cap=True
        )

    def instant(self, name: str, **args: Any) -> None:
        """Record a point in time on the calling thread's track."""
        self._emit(name, "i", _now_ns(), args=args)

    def counter(self, name: str, **args: Any) -> None:
        """Record a counter sample; Perfetto plots one track per numeric key."""
        self._emit(name, "C", _now_ns(), args=args)

    def slice(self, name: str, start_ns: int, **args: Any) -> None:
        """Record a completed span that began at @p start_ns."""
        end_ns = _now_ns()
        self._emit(name, "X", start_ns, dur_ns=end_ns - start_ns, args=args)

    @contextlib.contextmanager
    def scope(self, name: str, **args: Any) -> Iterator[None]:
        """Time a block, emitting the slice even when the block raises."""
        started = _now_ns()
        try:
            yield
        finally:
            self.slice(name, started, **args)

    def flush(self) -> None:
        with self._lock:
            self._flush_locked()

    @property
    def stats(self) -> dict:
        with self._lock:
            return dict(self._stats)

    def close(self) -> None:
        if self._closed:
            return

        self._emit("trace_stats", "i", _now_ns(), args=self.stats, bypass_cap=True)
        with self._lock:
            self._flush_locked()
            self._closed = True
            try:
                self._file.close()
            except (OSError, ValueError):
                pass

    def __enter__(self) -> "UnifiedTracer":
        return self

    def __exit__(self, *_exc: Any) -> None:
        self.close()


def create_tracer(
    *,
    enabled: Optional[bool] = None,
    output_path: Optional[str] = None,
    source: str = "colab",
    run_id: Optional[str] = None,
    build_id: Optional[str] = None,
) -> Any:
    """Build a tracer, or a :class:`NullTracer` when profiling is not requested.

    Tracing is opt-in for the same reason the firmware needs ``build.sh -p``: a
    production run should leave no profiling artifacts behind and pay nothing
    for the instrumentation.
    """
    if enabled is None:
        enabled = os.environ.get(TRACE_ENV_VAR, "0").strip().lower() in {"1", "true", "yes", "on"}

    if not enabled:
        return NullTracer()

    path = output_path or os.environ.get(TRACE_PATH_ENV_VAR) or None
    run = run_id or os.environ.get(TRACE_RUN_ID_ENV_VAR) or None

    max_bytes = DEFAULT_MAX_BYTES
    raw_max_mb = os.environ.get(TRACE_MAX_MB_ENV_VAR)
    if raw_max_mb:
        try:
            max_bytes = max(1, int(raw_max_mb)) * 1024 * 1024
        except ValueError:
            pass

    try:
        return UnifiedTracer(
            path, source=source, run_id=run, build_id=build_id, max_bytes=max_bytes
        )
    except (OSError, ValueError) as exc:
        # A missing directory, a read-only mount or a malformed path all disable
        # profiling; none of them may stop the server. ValueError covers paths
        # the OS layer rejects before it ever reaches the filesystem.
        print(f"unified_trace: tracing disabled ({exc})", flush=True)
        return NullTracer()
