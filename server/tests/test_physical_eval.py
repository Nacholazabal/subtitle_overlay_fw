import tempfile
import sys
import unittest
import wave
import itertools
from pathlib import Path

import numpy as np


sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from server.evaluation.physical import (
    _accepted_latency,
    _bitparallel_edit_distance,
    assign_final_events,
    captured_audio_metrics,
    completed_cues_for_capture,
    estimate_capture_alignment,
    observed_board_delivery,
    observed_partial_pipeline,
    continuous_accuracy,
    sequence_is_valid,
)


class PhysicalAnalysisTests(unittest.TestCase):
    def test_bitparallel_distance_matches_small_dynamic_program(self):
        def dynamic(reference, hypothesis):
            previous = list(range(len(hypothesis) + 1))
            for row, expected in enumerate(reference, 1):
                current = [row]
                for column, actual in enumerate(hypothesis, 1):
                    current.append(min(
                        current[-1] + 1,
                        previous[column] + 1,
                        previous[column - 1] + (expected != actual),
                    ))
                previous = current
            return previous[-1]

        strings = ["".join(value) for size in range(4) for value in itertools.product("ab", repeat=size)]
        for reference in strings:
            for hypothesis in strings:
                self.assertEqual(
                    dynamic(reference, hypothesis),
                    _bitparallel_edit_distance(list(reference), list(hypothesis)),
                )

    def test_continuous_accuracy_ignores_model_rollup_boundaries(self):
        cues = [
            {"reference_raw": "hola mundo"},
            {"reference_raw": "esto funciona"},
        ]
        events = [
            {"seq": 1, "is_final": True, "full_text": "mundo esto funciona"},
            {"seq": 0, "is_final": True, "full_text": "hola"},
        ]
        result = continuous_accuracy(cues, events, {0, 1})
        self.assertEqual("continuous_transcript", result["mode"])
        self.assertEqual(0.0, result["micro_wer_vs_human"]["rate"])
        self.assertEqual(0.0, result["micro_cer_vs_human"]["rate"])

    def test_vectorized_audio_metrics_report_level_and_clipping(self):
        rate = 48000
        audio = np.full(rate, 1000, dtype="<i2")
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "audio.wav"
            with wave.open(str(path), "wb") as handle:
                handle.setnchannels(1)
                handle.setsampwidth(2)
                handle.setframerate(rate)
                handle.writeframes(audio.tobytes())
            metrics = captured_audio_metrics(path)
        self.assertEqual(1.0, metrics["duration_sec"])
        self.assertAlmostEqual(100.0 * 1000 / 32767, metrics["peak_percent"], places=3)
        self.assertEqual(0.0, metrics["clipped_percent"])

    def test_partial_coverage_requires_full_clip_and_following_eou_gap(self):
        cues = [
            {"clip_id": "a", "end_sec": 8.0},
            {"clip_id": "b", "end_sec": 12.0},
            {"clip_id": "c", "end_sec": 16.0},
        ]
        completed = completed_cues_for_capture(
            cues,
            capture_offset_sec=2.0,
            capture_duration_sec=15.0,
            closure_gap_sec=1.2,
        )
        self.assertEqual(["a"], [cue["clip_id"] for cue in completed])

    def test_observed_delivery_recovers_counts_after_abrupt_bridge_failure(self):
        events = [{"seq": 0}, {"seq": 1}, {"seq": 2}]
        acks = [
            {"seq": 0, "session_id": 1, "status": "accepted", "sent_wall_sec": 1.0, "ack_latency_sec": 0.01},
            {"seq": 1, "session_id": 1, "status": "delivery_unknown", "sent_wall_sec": 2.0},
            {"seq": 2, "session_id": 2, "status": "accepted", "sent_wall_sec": 3.0, "ack_latency_sec": 0.02},
        ]
        result = observed_board_delivery(
            events,
            acks,
            {"subtitle_session": {"handshake_ok": True, "protocol_version": 1}},
        )
        self.assertEqual(3, result["generated"])
        self.assertEqual(2, result["accepted"])
        self.assertEqual(1, result["delivery_unknown"])
        self.assertEqual(2, result["connections"])
        self.assertEqual(1, result["reconnections"])
        self.assertFalse(result["protocol_ok"])

    def test_observed_partial_pipeline_uses_logged_board_drop_counter(self):
        with tempfile.TemporaryDirectory() as temporary:
            run_dir = Path(temporary)
            (run_dir / "live").mkdir()
            (run_dir / "live" / "bridge.log").write_text(
                "audio seq=50 dropped=4\naudio seq=100 dropped=4\n",
                encoding="utf-8",
            )
            result = observed_partial_pipeline(
                run_dir,
                [{"seq": 0}, {"seq": 1}],
                {"board_dropped_chunks_start": 4},
                True,
            )
        self.assertTrue(result["protocol_ok"])
        self.assertEqual(0, result["board_dropped_chunks_during_session"])
        self.assertTrue(result["incomplete_summary"])

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
