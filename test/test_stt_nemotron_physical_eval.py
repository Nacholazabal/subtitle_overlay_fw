import tempfile
import sys
import unittest
import wave
from pathlib import Path

import numpy as np


sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from scripts.stt_nemotron_physical_eval import (
    _accepted_latency,
    assign_final_events,
    estimate_capture_alignment,
    sequence_is_valid,
)


class PhysicalAnalysisTests(unittest.TestCase):
    def test_sequence_must_start_at_zero_without_gaps(self):
        self.assertTrue(sequence_is_valid([{"seq": 0}, {"seq": 1}]))
        self.assertFalse(sequence_is_valid([{"seq": 1}]))
        self.assertFalse(sequence_is_valid([{"seq": 0}, {"seq": 2}]))

    def test_finals_are_assigned_by_capture_aligned_overlap(self):
        cues = [
            {"clip_id": "a", "start_sec": 6.0, "end_sec": 8.0},
            {"clip_id": "b", "start_sec": 9.2, "end_sec": 12.0},
        ]
        events = [
            {"seq": 0, "is_final": True, "start_sec": 11.2, "end_sec": 12.9},
            {"seq": 1, "is_final": True, "start_sec": 14.3, "end_sec": 16.5},
            {"seq": 3, "is_final": True, "start_sec": 15.0, "end_sec": 15.0},
            {"seq": 2, "is_final": False, "start_sec": 1.0, "end_sec": 2.0},
        ]
        assigned, unassigned = assign_final_events(events, cues, capture_offset_sec=5.0)
        self.assertEqual([0], [event["seq"] for event in assigned["a"]])
        self.assertEqual([1, 3], [event["seq"] for event in assigned["b"]])
        self.assertEqual([], unassigned)

    def test_latency_correlates_event_audio_availability_with_accepted_ack(self):
        events = [{"seq": 0, "bridge_audio_available_wall_sec": 100.0}]
        acks = [{"seq": 0, "status": "accepted", "ack_wall_sec": 100.4, "ack_latency_sec": 0.01}]
        result = _accepted_latency(events, acks)
        self.assertEqual(100.0, result["within_1_5_sec_percent"])
        self.assertAlmostEqual(0.4, result["audio_available_to_board_accepted_sec"]["p50"])

    def test_waveform_alignment_recovers_known_capture_offset(self):
        rate = 16000
        replay = np.zeros(rate * 12, dtype="float32")
        rng = np.random.default_rng(7)
        replay[rate * 6 : rate * 9] = rng.normal(0, 0.1, rate * 3)
        cue_sheet = {
            "cues": [{"start_sec": 6.0, "end_sec": 9.0, "source_duration_sec": 3.0}]
        }
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            replay_path = root / "replay.wav"
            capture_path = root / "capture.wav"
            with wave.open(str(replay_path), "wb") as handle:
                handle.setnchannels(1)
                handle.setsampwidth(2)
                handle.setframerate(rate)
                handle.writeframes((replay * 32767).astype("<i2").tobytes())
            capture_rate = 48000
            offset_sec = 2.25
            capture = np.zeros(int(capture_rate * 16), dtype="float32")
            replay_48 = np.interp(
                np.arange(rate * 12 * 3) / capture_rate,
                np.arange(rate * 12) / rate,
                replay,
            )
            start = int(offset_sec * capture_rate)
            capture[start : start + replay_48.size] = replay_48
            with wave.open(str(capture_path), "wb") as handle:
                handle.setnchannels(1)
                handle.setsampwidth(2)
                handle.setframerate(capture_rate)
                handle.writeframes((capture * 32767).astype("<i2").tobytes())
            result = estimate_capture_alignment(
                replay_path,
                capture_path,
                cue_sheet,
                expected_offset_sec=2.0,
                search_radius_sec=1.0,
            )
        self.assertTrue(result["valid"])
        self.assertAlmostEqual(offset_sec, result["capture_offset_sec"], delta=0.01)


if __name__ == "__main__":
    unittest.main()
