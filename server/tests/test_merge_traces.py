"""Tests for the trace stitcher.

The stitcher decides where every server event lands on the board's timeline, so
a defect here silently produces a plausible-looking but wrong latency figure.
The property that matters most is causality: no server event may be placed
before the board event that caused it.
"""

import importlib.util
import json
import sys
import tempfile
import unittest
from pathlib import Path

REPO_ROOT = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO_ROOT))

_spec = importlib.util.spec_from_file_location(
    "merge_traces", REPO_ROOT / "scripts" / "merge_traces.py"
)
merge_traces = importlib.util.module_from_spec(_spec)
_spec.loader.exec_module(merge_traces)


# A board that started at this monotonic value, and a server whose monotonic
# clock is deliberately far away from it.
BOARD_BASE_NS = 1_000_000_000_000
SERVER_BASE_NS = 9_500_000_000_000
TRUE_OFFSET_NS = SERVER_BASE_NS - BOARD_BASE_NS  # server clock reads this much later
NETWORK_NS = 40_000_000  # 40 ms one-way, identical for every chunk


CHUNK_PERIOD_NS = 20_000_000  # 20 ms of audio per chunk

# The capture counter has been running since the process started, while the
# protocol's audio_seq restarted at zero on the current STT session. They are
# deliberately offset here so a correlation that leans on either one breaks.
CAPTURE_SEQ_BASE = 100

# A caption that describes audio up to 0.10 s, committed 1.000 s after the board
# started: the answer must be 1000 ms - 80 ms.
TRANSCRIPT_END_MS = 100
COMMIT_TS_US = 1_000_000
EXPECTED_LATENCY_MS = 920.0


def capture_ns(seq):
    return BOARD_BASE_NS + seq * CHUNK_PERIOD_NS


def board_trace(chunk_count=10, with_caption=True):
    """Board events: capture, send, and optionally a caption being committed."""
    events = [
        {
            "name": "trace_start",
            "ph": "i",
            "ts": 0,
            "pid": 111,
            "tid": 111,
            "args": {
                "clock_monotonic_ns": BOARD_BASE_NS,
                "clock_realtime_ns": 1_700_000_000_000_000_000,
                "run_id": "r1",
                "build_id": "abc",
            },
        }
    ]
    for seq in range(chunk_count):
        stamp = capture_ns(seq)
        ts_us = (stamp - BOARD_BASE_NS) // 1000
        events.append(
            {
                "name": "audio_chunk_ready",
                "ph": "i",
                "ts": ts_us,
                "pid": 111,
                "tid": 222,
                "args": {
                    "capture_seq": CAPTURE_SEQ_BASE + seq,
                    "capture_ts_ns": stamp,
                    "bytes": 1920,
                },
            }
        )
        events.append(
            {
                "name": "audio_ws_send",
                "ph": "X",
                "ts": ts_us + 100,
                "dur": 500,
                "pid": 111,
                "tid": 333,
                "args": {"audio_seq": seq, "capture_ts_ns": stamp, "bytes": 1920, "status": 0},
            }
        )

    if with_caption:
        events.append(
            {
                "name": "transcript_dispatch",
                "ph": "i",
                "ts": COMMIT_TS_US - 200,
                "pid": 111,
                "tid": 111,
                "args": {
                    "transcript_seq": 0,
                    "is_final": True,
                    "end_ms": TRANSCRIPT_END_MS,
                    "outcome": "accepted",
                },
            }
        )
        events.append(
            {
                "name": "overlay_commit",
                "ph": "i",
                "ts": COMMIT_TS_US,
                "pid": 111,
                "tid": 111,
                "args": {"transcript_seq": 0, "enabled": True},
            }
        )
    return events


def server_trace(chunk_count=10, extra_delay_ns=0, with_audio_position=True):
    """Server events carrying the board's capture stamp, as the protocol does."""
    events = [
        {
            "name": "trace_start",
            "ph": "i",
            "ts": 0,
            "pid": 777,
            "tid": 777,
            "args": {
                "clock_monotonic_ns": SERVER_BASE_NS,
                "clock_realtime_ns": 1_700_000_000_000_000_000,
                "run_id": "r1",
            },
        }
    ]
    for seq in range(chunk_count):
        stamp = capture_ns(seq)
        # Received later on the server's own clock: offset + transit.
        received_server_ns = stamp + TRUE_OFFSET_NS + NETWORK_NS + seq * extra_delay_ns
        ts_us = (received_server_ns - SERVER_BASE_NS) // 1000
        events.append(
            {
                "name": "audio_frame_rx",
                "ph": "i",
                "ts": ts_us,
                "pid": 777,
                "tid": 888,
                "args": {"audio_seq": seq, "board_capture_ts_ns": stamp, "bytes": 1920},
            }
        )
        push_args = {"audio_seq": seq, "board_capture_ts_ns": stamp}
        if with_audio_position:
            # How far into the stream this chunk reaches.
            push_args["audio_end_sec"] = round((seq + 1) * 0.02, 3)
        events.append(
            {
                "name": "session_push_pcm",
                "ph": "X",
                "ts": ts_us + 50,
                "dur": 6000,
                "pid": 777,
                "tid": 999,
                "args": push_args,
            }
        )
    return events


def write_ndjson(directory, filename, events):
    path = Path(directory) / filename
    with open(path, "w", encoding="utf-8") as handle:
        for event in events:
            handle.write(json.dumps(event) + "\n")
    return str(path)


class OffsetEstimationTests(unittest.TestCase):
    def test_shared_timestamps_recover_the_offset_within_one_transit(self):
        board_meta = merge_traces.trace_start(board_trace())
        server_events = server_trace()
        server_meta = merge_traces.trace_start(server_events)

        offset, method, _detail = merge_traces.estimate_offset(
            board_meta, server_events, server_meta
        )

        self.assertEqual("shared_audio_timestamps", method)
        # The estimator cannot see transit, so it absorbs the fastest one.
        self.assertEqual(TRUE_OFFSET_NS + NETWORK_NS, offset)

    def test_the_estimator_uses_the_fastest_chunk_not_the_average(self):
        # Later chunks are progressively slower; the minimum must win.
        server_events = server_trace(extra_delay_ns=5_000_000)
        offset, _method, _detail = merge_traces.estimate_offset(
            merge_traces.trace_start(board_trace()),
            server_events,
            merge_traces.trace_start(server_events),
        )

        self.assertEqual(TRUE_OFFSET_NS + NETWORK_NS, offset)

    def test_realtime_anchors_are_the_fallback_without_shared_timestamps(self):
        server_events = [
            event for event in server_trace() if event["name"] != "audio_frame_rx"
        ]
        offset, method, _detail = merge_traces.estimate_offset(
            merge_traces.trace_start(board_trace()),
            server_events,
            merge_traces.trace_start(server_events),
        )

        self.assertEqual("realtime_anchors_ntp", method)
        self.assertEqual(TRUE_OFFSET_NS, offset)

    def test_a_trace_without_any_anchor_is_reported_as_unalignable(self):
        offset, method, _detail = merge_traces.estimate_offset({}, [], {})

        self.assertIsNone(offset)
        self.assertEqual("no_anchor", method)


class StitchingTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.fw = write_ndjson(self.directory.name, "fw.jsonl", board_trace())
        self.server = write_ndjson(
            self.directory.name, "server.jsonl", server_trace(extra_delay_ns=3_000_000)
        )
        self.output = str(Path(self.directory.name) / "unified.json")

    def tearDown(self):
        self.directory.cleanup()

    def run_merge(self, *extra):
        argv = sys.argv
        sys.argv = [
            "merge_traces.py",
            "--fw",
            self.fw,
            "--server",
            self.server,
            "--output",
            self.output,
            *extra,
        ]
        try:
            self.assertEqual(0, merge_traces.main())
        finally:
            sys.argv = argv
        return json.loads(Path(self.output).read_text(encoding="utf-8"))

    def test_no_server_event_lands_before_the_capture_that_caused_it(self):
        merged = self.run_merge()
        events = merged["traceEvents"]

        # Keyed on the board's capture timestamp: the only identifier that means
        # the same thing on both sides.
        captures = {
            event["args"]["capture_ts_ns"]: event["ts"]
            for event in events
            if event.get("name") == "audio_chunk_ready"
        }
        arrivals = {
            event["args"]["board_capture_ts_ns"]: event["ts"]
            for event in events
            if event.get("name") == "audio_frame_rx"
        }

        self.assertEqual(10, len(captures))
        self.assertEqual(10, len(arrivals))
        for stamp, capture_ts in captures.items():
            self.assertGreaterEqual(
                arrivals[stamp], capture_ts, f"chunk {stamp} arrives before it was captured"
            )

    def test_the_fastest_chunk_defines_the_zero_of_network_time(self):
        merged = self.run_merge()
        events = merged["traceEvents"]

        captures = {
            e["args"]["capture_ts_ns"]: e["ts"]
            for e in events
            if e.get("name") == "audio_chunk_ready"
        }
        arrivals = {
            e["args"]["board_capture_ts_ns"]: e["ts"]
            for e in events
            if e.get("name") == "audio_frame_rx"
        }
        gaps = [arrivals[stamp] - captures[stamp] for stamp in captures]

        # Transit is reported relative to the best observed chunk, so the
        # fastest one reads as zero and the rest grow from there.
        self.assertEqual(0, min(gaps))
        self.assertGreater(max(gaps), 0)

    def test_each_source_gets_its_own_process_id(self):
        merged = self.run_merge()
        pids = {event["pid"] for event in merged["traceEvents"] if "pid" in event}

        # The board and Colab can genuinely share a pid; the originals are 111
        # and 777 here and must not survive.
        self.assertEqual({1, 2}, pids)

    def test_matched_chunks_get_a_flow_arrow_in_each_direction(self):
        merged = self.run_merge()
        flows = [event for event in merged["traceEvents"] if event.get("ph") in ("s", "f")]

        self.assertEqual(20, len(flows))  # 10 chunks, start + finish
        self.assertEqual(
            {1}, {event["pid"] for event in flows if event["ph"] == "s"}
        )
        self.assertEqual(
            {2}, {event["pid"] for event in flows if event["ph"] == "f"}
        )

    def test_correlation_survives_divergent_sequence_counters(self):
        merged = self.run_merge()
        events = merged["traceEvents"]

        # The board's capture counter starts at 100 and the protocol's audio_seq
        # at 0, exactly as a mid-run STT reconnect leaves them. Pairing must not
        # notice, because it keys on the board's capture timestamp.
        capture_seqs = {
            e["args"]["capture_seq"] for e in events if e.get("name") == "audio_chunk_ready"
        }
        protocol_seqs = {
            e["args"]["audio_seq"] for e in events if e.get("name") == "audio_ws_send"
        }
        self.assertTrue(capture_seqs.isdisjoint(protocol_seqs))

        flows = [e for e in events if e.get("ph") in ("s", "f")]
        self.assertEqual(20, len(flows))

    def test_end_to_end_measures_the_audio_the_caption_describes(self):
        argv = sys.argv
        sys.argv = ["merge_traces.py", "--fw", self.fw, "--server", self.server,
                    "--output", self.output]
        try:
            merge_traces.main()
        finally:
            sys.argv = argv

        board = board_trace()
        latencies, reason = merge_traces.end_to_end_latencies(
            board, merge_traces.trace_start(board), server_trace()
        )

        self.assertIsNone(reason)
        self.assertEqual(1, len(latencies))
        # Pairing the commit with the newest captured chunk instead would give
        # roughly one chunk period, which is what the earlier version reported.
        self.assertAlmostEqual(EXPECTED_LATENCY_MS, latencies[0], places=1)
        self.assertGreater(latencies[0], 100.0)

    def test_end_to_end_says_why_it_cannot_be_computed(self):
        board = board_trace(with_caption=False)
        latencies, reason = merge_traces.end_to_end_latencies(
            board, merge_traces.trace_start(board), server_trace()
        )
        self.assertEqual([], latencies)
        self.assertIn("end_ms", reason)

        board = board_trace()
        latencies, reason = merge_traces.end_to_end_latencies(
            board, merge_traces.trace_start(board), server_trace(with_audio_position=False)
        )
        self.assertEqual([], latencies)
        self.assertIn("audio-position", reason)

    def test_a_file_holding_two_runs_is_detected(self):
        doubled = board_trace() + board_trace()

        self.assertEqual(2, merge_traces.count_trace_starts(doubled))
        self.assertEqual(1, merge_traces.count_trace_starts(board_trace()))

    def test_flows_can_be_turned_off(self):
        merged = self.run_merge("--no-flows")

        self.assertEqual(
            [], [event for event in merged["traceEvents"] if event.get("ph") in ("s", "f")]
        )

    def test_the_output_records_how_the_clocks_were_aligned(self):
        merged = self.run_merge()

        self.assertEqual("shared_audio_timestamps", merged["metadata"]["clock_alignment"])
        self.assertEqual("r1", merged["metadata"]["board_run_id"])
        self.assertEqual(0, merged["metadata"]["unusable_lines"])

    def test_events_are_ordered_by_time(self):
        merged = self.run_merge()
        timestamps = [event["ts"] for event in merged["traceEvents"] if "ts" in event]

        self.assertEqual(sorted(timestamps), timestamps)


class DegradedInputTests(unittest.TestCase):
    def setUp(self):
        self.directory = tempfile.TemporaryDirectory()
        self.output = str(Path(self.directory.name) / "unified.json")

    def tearDown(self):
        self.directory.cleanup()

    def _merge(self, fw, server):
        argv = sys.argv
        sys.argv = ["merge_traces.py", "--fw", fw, "--server", server, "--output", self.output]
        try:
            return merge_traces.main()
        finally:
            sys.argv = argv

    def test_a_board_only_capture_still_produces_a_timeline(self):
        fw = write_ndjson(self.directory.name, "fw.jsonl", board_trace())

        self.assertEqual(0, self._merge(fw, str(Path(self.directory.name) / "missing.jsonl")))

        merged = json.loads(Path(self.output).read_text(encoding="utf-8"))
        self.assertEqual("single_source", merged["metadata"]["clock_alignment"])
        self.assertTrue(any(e.get("name") == "audio_chunk_ready" for e in merged["traceEvents"]))

    def test_unusable_lines_are_counted_rather_than_hidden(self):
        path = Path(self.directory.name) / "fw.jsonl"
        with open(path, "w", encoding="utf-8") as handle:
            for event in board_trace(chunk_count=2):
                handle.write(json.dumps(event) + "\n")
            handle.write('{"name":"broken",\n')  # truncated line

        self.assertEqual(0, self._merge(str(path), str(Path(self.directory.name) / "none.jsonl")))

        merged = json.loads(Path(self.output).read_text(encoding="utf-8"))
        self.assertEqual(1, merged["metadata"]["unusable_lines"])

    def test_two_empty_inputs_fail_instead_of_writing_an_empty_trace(self):
        missing = str(Path(self.directory.name) / "nothing.jsonl")

        self.assertEqual(1, self._merge(missing, missing))
        self.assertFalse(Path(self.output).exists())


if __name__ == "__main__":
    unittest.main()
