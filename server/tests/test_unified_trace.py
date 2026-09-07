"""Tests for the server-side profiling tracer and the session instrumentation.

These assert the properties a merged capture is later accepted on: tracing is
opt-in, a run's file is never truncated by a new session, slices cannot dangle,
the file is bounded, and no tracing failure can reach the audio path.
"""

import json
import os
import sys
import tempfile
import threading
import unittest
from pathlib import Path

import numpy as np

sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from server.runtime.nemotron import NemotronConfig, NemotronSession, TARGET_RATE
from server.runtime.unified_trace import (
    TRACE_ENV_VAR,
    NullTracer,
    UnifiedTracer,
    create_tracer,
)


def read_events(path):
    """Parse the NDJSON trace, asserting nothing about ordering."""
    events = []
    with open(path, encoding="utf-8") as handle:
        for line in handle:
            line = line.strip()
            if line:
                events.append(json.loads(line))
    return events


def names_of(events):
    return [event["name"] for event in events]


def trace_start_args(events):
    return next(event for event in events if event["name"] == "trace_start")["args"]


class TracerFileFormatTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.path = os.path.join(self.directory.name, "server_trace.jsonl")

    def tearDown(self):
        self.directory.cleanup()

    def test_startup_writes_process_metadata_and_both_clock_anchors(self):
        tracer = UnifiedTracer(self.path, source="colab", run_id="r1", build_id="b1")
        tracer.close()

        events = read_events(self.path)
        self.assertIn("process_name", names_of(events))

        start = next(event for event in events if event["name"] == "trace_start")
        # Without both anchors the merge cannot place this file on a UTC timeline.
        self.assertIn("clock_monotonic_ns", start["args"])
        self.assertIn("clock_realtime_ns", start["args"])
        self.assertEqual("r1", start["args"]["run_id"])
        self.assertEqual("b1", start["args"]["build_id"])
        self.assertEqual("CLOCK_MONOTONIC", start["args"]["clock_domain"])

    def test_every_session_of_one_run_shares_a_single_trace_start(self):
        tracer = UnifiedTracer(self.path, source="colab", run_id="r1")
        tracer.instant("session_open", session_id="1")
        tracer.instant("session_close", session_id="1")
        tracer.instant("session_open", session_id="2")
        tracer.close()

        events = read_events(self.path)
        # The original bug was a tracer per session, each truncating the file.
        # Sessions now borrow the run's tracer, so all of them survive...
        self.assertEqual(
            ["1", "2"],
            [e["args"]["session_id"] for e in events if e["name"] == "session_open"],
        )
        # ...under exactly one clock base. Two of them in one file would make
        # every event after the first run land at the wrong time.
        self.assertEqual(1, len([e for e in events if e["name"] == "trace_start"]))

    def test_a_new_run_does_not_append_to_an_older_run_file(self):
        first = UnifiedTracer(self.path, source="colab", run_id="r1")
        first.instant("session_open", session_id="1")
        first.close()

        second = UnifiedTracer(self.path, source="colab", run_id="r2")
        second.instant("session_open", session_id="2")
        second.close()

        events = read_events(self.path)
        # Appending would leave two clock bases in one file, which silently
        # breaks the merge. One file means one run.
        self.assertEqual(1, len([e for e in events if e["name"] == "trace_start"]))
        self.assertEqual("r2", trace_start_args(events)["run_id"])

    def test_slices_are_complete_events_that_cannot_be_left_open(self):
        tracer = UnifiedTracer(self.path, source="colab")
        with tracer.scope("nemotron_step", samples=1600):
            pass
        tracer.close()

        events = read_events(self.path)
        slices = [event for event in events if event["name"] == "nemotron_step"]
        self.assertEqual(1, len(slices))
        self.assertEqual("X", slices[0]["ph"])
        self.assertIn("dur", slices[0])
        self.assertEqual([], [event for event in events if event["ph"] in ("B", "E")])

    def test_scope_still_emits_its_slice_when_the_block_raises(self):
        tracer = UnifiedTracer(self.path, source="colab")

        with self.assertRaises(ValueError):
            with tracer.scope("nemotron_step"):
                raise ValueError("inference blew up")
        tracer.close()

        # A failed step is exactly the one worth seeing on the timeline.
        self.assertIn("nemotron_step", names_of(read_events(self.path)))

    def test_counter_samples_use_the_counter_phase(self):
        tracer = UnifiedTracer(self.path, source="colab")
        tracer.counter("server_health", buffered_samples=320, session_errors=0)
        tracer.close()

        counters = [event for event in read_events(self.path) if event["name"] == "server_health"]
        self.assertEqual(1, len(counters))
        self.assertEqual("C", counters[0]["ph"])
        self.assertEqual(320, counters[0]["args"]["buffered_samples"])

    def test_close_records_the_tracers_own_statistics(self):
        tracer = UnifiedTracer(self.path, source="colab")
        tracer.instant("session_open", session_id="1")
        tracer.close()

        stats = next(event for event in read_events(self.path) if event["name"] == "trace_stats")
        self.assertEqual(0, stats["args"]["write_errors"])
        self.assertFalse(stats["args"]["full"])

    def test_close_is_idempotent(self):
        tracer = UnifiedTracer(self.path, source="colab")
        tracer.close()
        tracer.close()

        stats = [event for event in read_events(self.path) if event["name"] == "trace_stats"]
        self.assertEqual(1, len(stats))


class TracerRobustnessTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.path = os.path.join(self.directory.name, "server_trace.jsonl")

    def tearDown(self):
        self.directory.cleanup()

    def test_the_file_stops_growing_at_the_size_cap(self):
        tracer = UnifiedTracer(self.path, source="colab", max_bytes=4096)
        for index in range(2000):
            tracer.instant("spam", index=index)
        tracer.close()

        self.assertLess(os.path.getsize(self.path), 16 * 1024)
        events = read_events(self.path)
        # The capture must say it stopped rather than simply ending.
        self.assertIn("trace_full", names_of(events))
        stats = next(event for event in events if event["name"] == "trace_stats")
        self.assertTrue(stats["args"]["full"])
        self.assertGreater(stats["args"]["dropped_full"], 0)

    def test_arguments_are_coerced_into_serializable_values(self):
        class Unserializable:
            def __repr__(self):
                return "<opaque>"

        tracer = UnifiedTracer(self.path, source="colab")
        tracer.instant(
            "odd_args",
            payload=b"\x00\x01\x02\x03",
            obj=Unserializable(),
            long_text="x" * 500,
            nothing=None,
            flag=True,
        )
        tracer.close()

        args = next(event for event in read_events(self.path) if event["name"] == "odd_args")["args"]
        # Bytes become their length: the trace must not carry audio payloads.
        self.assertEqual(4, args["payload"])
        self.assertEqual("<opaque>", args["obj"])
        self.assertLessEqual(len(args["long_text"]), 96)
        self.assertIsNone(args["nothing"])
        self.assertIs(True, args["flag"])

    def test_non_finite_numbers_become_null_instead_of_invalid_json(self):
        tracer = UnifiedTracer(self.path, source="colab")
        tracer.instant(
            "non_finite",
            nan=float("nan"),
            positive=float("inf"),
            negative=float("-inf"),
            ordinary=1.5,
        )
        tracer.close()

        raw = open(self.path, encoding="utf-8").read()
        # json.dumps would otherwise emit bare NaN/Infinity, which Perfetto rejects.
        self.assertNotIn("NaN", raw)
        self.assertNotIn("Infinity", raw)

        args = next(e for e in read_events(self.path) if e["name"] == "non_finite")["args"]
        self.assertIsNone(args["nan"])
        self.assertIsNone(args["positive"])
        self.assertIsNone(args["negative"])
        self.assertEqual(1.5, args["ordinary"])

    def test_a_broken_repr_does_not_escape_into_the_caller(self):
        class Hostile:
            def __repr__(self):
                raise RuntimeError("repr is broken")

        tracer = UnifiedTracer(self.path, source="colab")
        tracer.instant("hostile", value=Hostile())
        tracer.close()

        args = next(e for e in read_events(self.path) if e["name"] == "hostile")["args"]
        self.assertEqual("<unrepresentable>", args["value"])

    def test_a_lone_surrogate_cannot_take_down_the_buffered_batch(self):
        tracer = UnifiedTracer(self.path, source="colab")
        # Survives json.dumps and only fails at write time, losing the batch.
        tracer.instant("surrogate", text="ok\ud800tail")
        tracer.instant("after_surrogate", index=1)
        tracer.close()

        emitted = names_of(read_events(self.path))
        self.assertIn("surrogate", emitted)
        self.assertIn("after_surrogate", emitted)
        self.assertEqual(0, tracer.stats["write_errors"])

    def test_text_with_quotes_and_newlines_stays_on_one_line(self):
        tracer = UnifiedTracer(self.path, source="colab")
        tracer.instant("escaping", reason='he said "stop"\nand \\ left\ttabbed')
        tracer.close()

        with open(self.path, encoding="utf-8") as handle:
            lines = [line for line in handle if line.strip()]
        for line in lines:
            json.loads(line)  # every line parses on its own

        args = next(event for event in read_events(self.path) if event["name"] == "escaping")["args"]
        self.assertEqual('he said "stop"\nand \\ left\ttabbed', args["reason"])

    def test_concurrent_writers_produce_whole_lines(self):
        tracer = UnifiedTracer(self.path, source="colab")

        def worker(name):
            tracer.register_thread(name)
            for index in range(200):
                tracer.instant("concurrent", index=index, who=name)

        threads = [
            threading.Thread(target=worker, args=(f"worker-{index}",)) for index in range(4)
        ]
        for thread in threads:
            thread.start()
        for thread in threads:
            thread.join()
        tracer.close()

        events = read_events(self.path)
        self.assertEqual(800, len([e for e in events if e["name"] == "concurrent"]))
        # One track per worker, which is what makes concurrency visible.
        thread_names = [e for e in events if e["name"] == "thread_name"]
        self.assertEqual(4, len(thread_names))
        self.assertEqual(4, len({e["tid"] for e in thread_names}))

    def test_thread_registration_happens_once_per_thread(self):
        tracer = UnifiedTracer(self.path, source="colab")
        tracer.register_thread("fastapi-loop")
        tracer.register_thread("fastapi-loop")
        tracer.register_thread("fastapi-loop")
        tracer.close()

        events = read_events(self.path)
        self.assertEqual(1, len([e for e in events if e["name"] == "thread_name"]))

    def test_a_write_failure_is_counted_and_never_raised(self):
        tracer = UnifiedTracer(self.path, source="colab")
        tracer._file.close()  # simulate the handle dying mid-run

        tracer.instant("after_failure", index=1)
        tracer.flush()

        self.assertGreater(tracer.stats["write_errors"], 0)
        tracer.close()


class TracerOptInTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.path = os.path.join(self.directory.name, "server_trace.jsonl")
        self._saved_env = os.environ.get(TRACE_ENV_VAR)
        os.environ.pop(TRACE_ENV_VAR, None)

    def tearDown(self):
        if self._saved_env is None:
            os.environ.pop(TRACE_ENV_VAR, None)
        else:
            os.environ[TRACE_ENV_VAR] = self._saved_env
        self.directory.cleanup()

    def test_tracing_is_off_by_default_and_writes_no_file(self):
        tracer = create_tracer(output_path=self.path)

        self.assertIsInstance(tracer, NullTracer)
        tracer.instant("ignored")
        tracer.counter("ignored")
        with tracer.scope("ignored"):
            pass
        tracer.close()

        # A production run must leave no profiling artifact behind.
        self.assertFalse(os.path.exists(self.path))

    def test_the_environment_switch_turns_tracing_on(self):
        os.environ[TRACE_ENV_VAR] = "1"
        tracer = create_tracer(output_path=self.path, source="colab")
        try:
            self.assertIsInstance(tracer, UnifiedTracer)
        finally:
            tracer.close()
        self.assertTrue(os.path.exists(self.path))

    def test_an_unwritable_destination_disables_tracing_instead_of_failing(self):
        unwritable = os.path.join(self.directory.name, "not-a-dir", "\0bad")
        tracer = create_tracer(enabled=True, output_path=unwritable)

        self.assertIsInstance(tracer, NullTracer)

    def test_null_tracer_offers_the_whole_interface(self):
        tracer = NullTracer()

        tracer.instant("a", x=1)
        tracer.counter("b", y=2)
        tracer.slice("c", 0, z=3)
        tracer.register_thread("t")
        tracer.flush()
        with tracer.scope("d"):
            pass
        tracer.close()

        self.assertFalse(tracer.enabled)
        self.assertEqual(0, tracer.stats["events_written"])


class ScriptedEngine:
    """Minimal streaming engine, matching the contract NemotronSession expects."""

    frame_samples = 1600

    def __init__(self):
        self.steps = 0

    def open(self):
        pass

    def close(self):
        pass

    def step(self, samples, *, is_first, is_last, valid_length):
        self.steps += 1
        if self.steps == 1:
            return [{"partial_transcript": "hola", "final_transcript": ""}]
        return []


class SessionInstrumentationTests(unittest.TestCase):
    """The production path: what a real Colab session writes to the trace."""

    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.path = os.path.join(self.directory.name, "server_trace.jsonl")
        self.tracer = UnifiedTracer(self.path, source="colab", run_id="run-under-test")

    def tearDown(self):
        self.tracer.close()
        self.directory.cleanup()

    def _run_session(self):
        session = NemotronSession(ScriptedEngine(), NemotronConfig(), source_rate=TARGET_RATE)
        session.attach_tracer(self.tracer)
        session.push_pcm(np.zeros(1600, dtype="<i2").tobytes(), audio_seq=42)
        session.flush()
        self.tracer.flush()
        return read_events(self.path)

    def test_a_session_emits_the_push_and_step_slices(self):
        events = self._run_session()
        emitted = names_of(events)

        self.assertIn("session_push_pcm", emitted)
        # Renamed from gpu_inference: this measures the NeMo call's wall time,
        # which is not a claim about pure CUDA kernel time.
        self.assertIn("nemotron_step", emitted)
        self.assertNotIn("gpu_inference", emitted)

    def test_the_push_slice_carries_the_boards_audio_sequence(self):
        events = self._run_session()

        push = next(event for event in events if event["name"] == "session_push_pcm")
        self.assertEqual("X", push["ph"])
        self.assertEqual(42, push["args"]["audio_seq"])
        self.assertEqual(3200, push["args"]["input_bytes"])

    def test_the_inference_worker_gets_its_own_named_track(self):
        events = self._run_session()

        worker = [
            event
            for event in events
            if event["name"] == "thread_name" and event["args"]["name"] == "inference-worker"
        ]
        self.assertEqual(1, len(worker))

    def test_a_session_without_a_tracer_runs_untouched(self):
        session = NemotronSession(ScriptedEngine(), NemotronConfig(), source_rate=TARGET_RATE)

        # No attach_tracer(): the default NullTracer must keep the session working.
        events = session.push_pcm(np.zeros(1600, dtype="<i2").tobytes())
        events.extend(session.flush())

        self.assertTrue(any(event["is_final"] for event in events))
        self.assertIsInstance(session.tracer, NullTracer)


if __name__ == "__main__":
    unittest.main()
