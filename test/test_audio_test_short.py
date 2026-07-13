import json
import sys
import tempfile
import types
import unittest
from pathlib import Path
from unittest import mock


sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from scripts import audio_test_short
from scripts.audio_test_short import (
    accuracy_result,
    assign_events,
    error_rate,
    make_report,
    normalize_text,
    offline_reference,
    overlay_timeline,
    reliability_result,
)
from scripts.stt_stream_server import ServerConfig, transcribe_offline_bytes


class TextMetricsTests(unittest.TestCase):
    def test_normalize_spanish_keeps_accents_and_removes_punctuation(self):
        self.assertEqual("qué tal niño", normalize_text("¿Qué tal, NIÑO?"))

    def test_wer_and_cer_are_deterministic(self):
        wer = error_rate("hola mundo", "hola cruel", "word")
        cer = error_rate("á b", "a b", "character")

        self.assertEqual(1, wer["edits"])
        self.assertEqual(0.5, wer["rate"])
        self.assertEqual(1, cer["edits"])

    def test_empty_reference_does_not_divide_by_zero(self):
        self.assertEqual(0.0, error_rate("", "", "word")["rate"])
        self.assertEqual(1.0, error_rate("", "hola", "word")["rate"])

    def test_accuracy_score_is_clamped_at_zero(self):
        result = accuracy_result("uno", "dos tres cuatro", "human")
        self.assertEqual(0.0, result["score"])


class AssignmentAndDisplayTests(unittest.TestCase):
    def test_assigns_event_to_clip_with_largest_audio_overlap(self):
        clips = [
            {"name": "a", "play_start_wall_sec": 110.0, "play_end_wall_sec": 120.0},
            {"name": "b", "play_start_wall_sec": 126.0, "play_end_wall_sec": 136.0},
        ]
        events = [
            {"seq": 1, "start_sec": 9.0, "end_sec": 11.0},
            {"seq": 2, "start_sec": 25.0, "end_sec": 27.0},
        ]

        assigned = assign_events(events, clips, audio_start_wall_sec=100.0)

        self.assertEqual([1], [event["seq"] for event in assigned["a"]])
        self.assertEqual([2], [event["seq"] for event in assigned["b"]])

    def test_overlay_last_state_is_capped_by_firmware_clear_timeout(self):
        records = overlay_timeline(
            [
                {
                    "seq": 1,
                    "is_final": True,
                    "text": "hola",
                    "bridge_received_wall_sec": 100.0,
                }
            ]
        )

        self.assertEqual(audio_test_short.CLEAR_TIMEOUT_SEC, records[0]["visible_duration_sec"])
        self.assertEqual(["hola"], records[0]["lines"])


class OfflineCacheTests(unittest.TestCase):
    def test_cache_is_reused_for_same_audio_and_model_config(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            clip = root / "clip.webm"
            clip.write_bytes(b"fake webm")
            health = {
                "run_config": {
                    "config_model": "small",
                    "config_language": "es",
                    "config_device": "cuda",
                    "config_compute_type": "float16",
                    "config_beam_size": 5,
                    "config_vad_filter": True,
                }
            }
            response = {"text": "hola", "segments": [], "config": health["run_config"]}
            with mock.patch.object(audio_test_short, "CACHE_ROOT", root / "cache"), mock.patch.object(
                audio_test_short, "request_json", return_value=response
            ) as request:
                first, first_path, first_hit = offline_reference(clip, "wss://test/stt/stream", health)
                second, second_path, second_hit = offline_reference(clip, "wss://test/stt/stream", health)

            self.assertFalse(first_hit)
            self.assertTrue(second_hit)
            self.assertEqual(first_path, second_path)
            self.assertEqual(first["text"], second["text"])
            request.assert_called_once()


class OfflineTranscriptionTests(unittest.TestCase):
    def test_complete_file_uses_shared_model_and_returns_segments(self):
        fake_audio_module = types.ModuleType("faster_whisper.audio")
        fake_audio_module.decode_audio = lambda _source, sampling_rate: [0.0] * sampling_rate
        fake_package = types.ModuleType("faster_whisper")
        fake_package.audio = fake_audio_module

        class Segment:
            start = 0.0
            end = 1.0
            text = " hola mundo "

        class Info:
            language = "es"

        class SharedModel:
            config = ServerConfig()

            @staticmethod
            def transcribe(_audio):
                return iter([Segment()]), Info()

        with mock.patch.dict(
            sys.modules,
            {"faster_whisper": fake_package, "faster_whisper.audio": fake_audio_module},
        ):
            result = transcribe_offline_bytes(SharedModel(), b"audio", "clip.webm")

        self.assertEqual("hola mundo", result["text"])
        self.assertEqual("clip.webm", result["filename"])
        self.assertEqual(False, result["config"]["config_realtime"])
        self.assertEqual("http_offline", result["config"]["config_transport"])


class ReliabilityTests(unittest.TestCase):
    def test_protocol_error_invalidates_reliability(self):
        done = {
            "status": "complete",
            "chunks_forwarded": 100,
            "board_dropped_chunks": 0,
            "jobs_submitted": 10,
            "dropped_audio_jobs": 0,
            "events_emitted": 10,
            "events_dropped": 0,
        }

        self.assertEqual(100.0, reliability_result(done, True)["score"])
        self.assertEqual(0.0, reliability_result(done, False)["score"])


class ReportTests(unittest.TestCase):
    def test_report_labels_offline_reference_as_proxy_without_human_text(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            (root / "live").mkdir()
            (root / "offline").mkdir()
            source = root / "sample.webm"
            source.write_bytes(b"audio")
            (root / "live" / "events.jsonl").write_text(
                json.dumps(
                    {
                        "seq": 1,
                        "is_final": True,
                        "start_sec": 10.0,
                        "end_sec": 11.0,
                        "text": "hola",
                        "bridge_received_wall_sec": 111.2,
                        "bridge_receive_lag_sec": 0.2,
                        "segment_reason": "silence",
                    }
                )
                + "\n",
                encoding="utf-8",
            )
            (root / "offline" / "sample.json").write_text(
                json.dumps({"text": "hola", "segments": []}), encoding="utf-8"
            )
            manifest = {
                "run_id": "test-run",
                "clips": [
                    {
                        "name": "sample",
                        "source_path": str(source),
                        "play_start_wall_sec": 110.0,
                        "play_end_wall_sec": 120.0,
                    }
                ],
            }
            ready = {"audio_start_wall_sec": 100.0, "run_config": {"config_model": "small"}}
            done = {
                "status": "complete",
                "chunks_forwarded": 10,
                "board_dropped_chunks": 0,
                "jobs_submitted": 1,
                "dropped_audio_jobs": 0,
                "events_emitted": 1,
                "events_dropped": 0,
            }

            report = make_report(root, manifest, ready, done)

            self.assertEqual("offline_proxy", report["scores"]["accuracy"]["reference_kind"])
            self.assertEqual(100.0, report["scores"]["accuracy"]["score"])
            self.assertTrue((root / "report.md").exists())
            self.assertTrue((root / "overlay_timeline.json").exists())


if __name__ == "__main__":
    unittest.main()
