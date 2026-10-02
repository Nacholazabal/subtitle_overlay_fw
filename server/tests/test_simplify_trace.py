"""Tests for the presentation-only Perfetto trace converter."""

import importlib.util
import sys
import unittest
from pathlib import Path


REPO_ROOT = Path(__file__).resolve().parents[2]
_spec = importlib.util.spec_from_file_location(
    "simplify_trace", REPO_ROOT / "scripts" / "simplify_trace.py"
)
simplify_trace = importlib.util.module_from_spec(_spec)
sys.modules[_spec.name] = simplify_trace
_spec.loader.exec_module(simplify_trace)


def event(name, phase, timestamp, *, duration=None, pid=1, args=None):
    value = {
        "name": name,
        "ph": phase,
        "ts": timestamp,
        "pid": pid,
        "tid": pid,
        "args": args or {},
    }
    if duration is not None:
        value["dur"] = duration
    return value


def sample_trace():
    capture_stamp = 1_000_000_000
    events = [
        event("ws_state", "i", 1000, args={"to": "ready"}),
        event(
            "audio_chunk_ready",
            "i",
            1500,
            args={"capture_ts_ns": capture_stamp},
        ),
        event("alsa_read", "X", 1100, duration=200, args={"status": 0}),
        event("audio_ws_send", "X", 1600, duration=20, args={"status": 0}),
        event(
            "session_push_pcm",
            "X",
            1700,
            duration=30,
            pid=2,
            args={
                "audio_end_sec": 0.5,
                "board_capture_ts_ns": capture_stamp,
            },
        ),
        event("nemotron_step", "X", 1800, duration=80, pid=2),
        event(
            "transcript_emit",
            "i",
            1900,
            pid=2,
            args={"transcript_seq": 7, "is_final": True},
        ),
        event(
            "transcript_decoded",
            "i",
            1950,
            args={"transcript_seq": 7},
        ),
        event(
            "transcript_dispatch",
            "i",
            1960,
            args={
                "transcript_seq": 7,
                "end_ms": 500,
                "is_final": True,
                "outcome": "accepted",
            },
        ),
        event(
            "subtitle_render",
            "X",
            1970,
            duration=10,
            args={"transcript_seq": 7},
        ),
        event("overlay_commit", "i", 2000, args={"transcript_seq": 7}),
        # A redraw must not create a second end-to-end bar.
        event("overlay_commit", "i", 2100, args={"transcript_seq": 7}),
        event("pipeline_health", "C", 2200, args={"drops": 0}),
        event("audio", "s", 2250),
        event("audio", "f", 2260, pid=2),
        event("ws_state", "i", 3000, args={"from": "ready", "to": "backoff"}),
        # Outside the stable window.
        event("alsa_read", "X", 3100, duration=200),
    ]
    return {"traceEvents": events, "metadata": {"source": "test unified"}}


class PresentationTraceTests(unittest.TestCase):
    def test_detects_the_complete_ready_window(self):
        self.assertEqual((1000, 3000), simplify_trace.detect_ready_window(sample_trace()["traceEvents"]))

    def test_output_contains_only_metadata_and_duration_bars(self):
        result = simplify_trace.build_presentation_trace(sample_trace())

        self.assertEqual({"M", "X"}, {entry["ph"] for entry in result["traceEvents"]})
        self.assertNotIn("pipeline_health", {entry["name"] for entry in result["traceEvents"]})
        self.assertNotIn("audio", {entry["name"] for entry in result["traceEvents"]})

    def test_low_level_names_are_replaced_with_explanatory_spanish(self):
        result = simplify_trace.build_presentation_trace(sample_trace())
        names = {entry["name"] for entry in result["traceEvents"]}

        self.assertIn("Lectura de audio ALSA", names)
        self.assertIn("Inferencia Nemotron", names)
        self.assertIn("Render del subtítulo", names)
        self.assertNotIn("alsa_read", names)

    def test_end_to_end_uses_the_first_commit_and_audio_covered(self):
        result = simplify_trace.build_presentation_trace(sample_trace())
        spans = [
            entry
            for entry in result["traceEvents"]
            if entry["name"] == "Audio hasta subtítulo visible"
        ]

        self.assertEqual(1, len(spans))
        self.assertEqual(500, spans[0]["ts"])
        self.assertEqual(500, spans[0]["dur"])
        self.assertEqual(0.5, spans[0]["args"]["duración_ms"])

    def test_server_to_board_delivery_becomes_a_duration_bar(self):
        result = simplify_trace.build_presentation_trace(sample_trace())
        spans = [
            entry
            for entry in result["traceEvents"]
            if entry["name"] == "Entrega del transcript a la placa"
        ]

        self.assertEqual(1, len(spans))
        self.assertEqual(50, spans[0]["dur"])

    def test_source_trace_is_not_modified(self):
        source = sample_trace()
        original_timestamp = source["traceEvents"][2]["ts"]

        simplify_trace.build_presentation_trace(source)

        self.assertEqual(original_timestamp, source["traceEvents"][2]["ts"])


if __name__ == "__main__":
    unittest.main()
