import sys
import unittest
from collections import deque
from pathlib import Path

import numpy as np


sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from scripts import stt_simulstreaming_backend as backend
from scripts.stt_simulstreaming_backend import (
    RUN_ENGINE,
    SECONDS_PER_FRAME,
    SimulStreamingConfig,
    SimulStreamingSession,
    TranscriptAdapter,
    frame_threshold_seconds,
    normalize_iter_result,
    pcm_s16le_to_float32,
    resample_to_16k,
    transcribe_offline_float32,
    validate_checkpoint,
)


class FakeOnline:
    """Stand-in for the upstream VACOnlineASRProcessor (dict contract)."""

    def __init__(self, status="voice"):
        self.status = status
        self.results = deque()
        self._finish = {"text": "", "is_final": True}

    def feed(self, result):
        self.results.append(result)

    def set_finish(self, result):
        self._finish = result

    def insert_audio_chunk(self, audio):
        self._last = np.asarray(audio)

    def process_iter(self):
        return self.results.popleft() if self.results else {}

    def finish(self):
        return self._finish


class ConfigTests(unittest.TestCase):
    def test_defaults_match_official_operating_point(self):
        config = SimulStreamingConfig()
        self.assertEqual("small", config.model)
        self.assertEqual("es", config.language)
        self.assertEqual(25, config.frame_threshold)
        self.assertEqual(1, config.beams)
        self.assertEqual("greedy", config.decoder_type)
        self.assertTrue(config.use_vac)

    def test_from_overrides_applies_and_validates(self):
        config = SimulStreamingConfig.from_overrides(
            {"frame_threshold": 12, "beams": 5, "never_fire": True}
        )
        self.assertEqual(12, config.frame_threshold)
        self.assertEqual(5, config.beams)
        self.assertEqual("beam", config.decoder_type)
        self.assertTrue(config.never_fire)

    def test_from_overrides_rejects_unknown_and_bad_types(self):
        with self.assertRaises(ValueError):
            SimulStreamingConfig.from_overrides({"compute_type": "float16"})
        with self.assertRaises(ValueError):
            SimulStreamingConfig.from_overrides({"frame_threshold": "many"})
        with self.assertRaises(ValueError):
            SimulStreamingConfig.from_overrides({"use_vac": 1})  # int, not bool
        with self.assertRaises(ValueError):
            SimulStreamingConfig.from_overrides({"task": "summarize"})

    def test_run_config_reports_engine_and_frame_unit(self):
        run_config = SimulStreamingConfig().run_config(realtime=True, transport="websocket")
        self.assertEqual(RUN_ENGINE, run_config["run_engine"])
        self.assertEqual(0.5, run_config["config_frame_threshold_sec"])
        self.assertEqual(backend.UPSTREAM_COMMIT, run_config["upstream_commit"])

    def test_frame_threshold_unit_is_twenty_ms(self):
        self.assertEqual(0.02, SECONDS_PER_FRAME)
        self.assertEqual(0.5, frame_threshold_seconds(25))
        self.assertEqual(0.24, frame_threshold_seconds(12))


class CheckpointTests(unittest.TestCase):
    def _write(self, path, data=b"weights"):
        path.write_bytes(data)
        return path

    def test_missing_file_raises(self):
        with self.assertRaises(FileNotFoundError):
            validate_checkpoint(Path("/nonexistent/whisper-small.pt"))

    def test_directory_is_rejected_as_faster_whisper_style(self):
        import tempfile

        with tempfile.TemporaryDirectory() as tmp:
            directory = Path(tmp) / "ct2-model.pt"
            directory.mkdir()
            with self.assertRaises(ValueError):
                validate_checkpoint(directory)

    def test_wrong_suffix_and_wrong_checksum_are_rejected(self):
        import hashlib
        import tempfile

        with tempfile.TemporaryDirectory() as tmp:
            wrong_suffix = self._write(Path(tmp) / "model.bin")
            with self.assertRaises(ValueError):
                validate_checkpoint(wrong_suffix)

            checkpoint = self._write(Path(tmp) / "whisper-small.pt", b"not the real weights")
            with self.assertRaises(ValueError):
                validate_checkpoint(checkpoint, expected_sha256="0" * 64)

            good_sha = hashlib.sha256(b"not the real weights").hexdigest()
            self.assertEqual(good_sha, validate_checkpoint(checkpoint, expected_sha256=good_sha))

    def test_link_as_known_name_gives_whisper_a_valid_filename(self):
        import os
        import tempfile
        from scripts.stt_simulstreaming_backend import link_checkpoint_as_known_name

        with tempfile.TemporaryDirectory() as tmp:
            drive_file = Path(tmp) / "whisper-small.pt"
            drive_file.write_bytes(b"weights")
            linked = link_checkpoint_as_known_name(drive_file, "small")
            self.assertEqual("small.pt", os.path.basename(linked))
            self.assertEqual(b"weights", Path(linked).read_bytes())
            # Already-canonical names are passed through unchanged.
            canonical = Path(tmp) / "small.pt"
            canonical.write_bytes(b"w")
            self.assertEqual(os.path.abspath(str(canonical)),
                             link_checkpoint_as_known_name(canonical, "small"))


class AudioHelperTests(unittest.TestCase):
    def test_pcm_s16le_round_trips_to_float(self):
        pcm = np.array([0, 32767, -32768], dtype="<i2").tobytes()
        audio = pcm_s16le_to_float32(pcm)
        self.assertEqual(3, audio.size)
        self.assertAlmostEqual(0.0, audio[0], places=6)
        self.assertAlmostEqual(1.0, audio[1], places=3)
        self.assertAlmostEqual(-1.0, audio[2], places=3)

    def test_odd_length_pcm_is_truncated_safely(self):
        self.assertEqual(1, pcm_s16le_to_float32(b"\x00\x00\x05").size)

    def test_resample_changes_length_to_target_rate(self):
        audio = np.ones(48000, dtype="float32")
        resampled = resample_to_16k(audio, 48000)
        self.assertEqual(16000, resampled.size)
        self.assertIs(audio.dtype, resample_to_16k(audio, 16000).dtype)


class NormalizeTests(unittest.TestCase):
    def test_empty_dict_is_no_output(self):
        self.assertIsNone(normalize_iter_result({}))
        self.assertIsNone(normalize_iter_result(None))

    def test_dict_and_tuple_forms(self):
        as_dict = normalize_iter_result({"text": " hola ", "start": 0.0, "end": 1.0, "is_final": True})
        self.assertEqual("hola", as_dict["text"])
        self.assertTrue(as_dict["is_final"])

        as_tuple = normalize_iter_result((1.0, 2.0, " mundo "))
        self.assertEqual("mundo", as_tuple["text"])
        self.assertFalse(as_tuple["is_final"])

    def test_final_with_empty_text_is_kept(self):
        result = normalize_iter_result({"text": "", "is_final": True})
        self.assertIsNotNone(result)
        self.assertTrue(result["is_final"])


class TranscriptAdapterTests(unittest.TestCase):
    def test_seq_starts_at_zero_and_accumulates_visible_text(self):
        adapter = TranscriptAdapter(SimulStreamingConfig())
        first = adapter.ingest({"text": "hola", "start": 0.0, "end": 1.0, "is_final": False})[0]
        second = adapter.ingest({"text": "mundo", "start": 1.0, "end": 2.0, "is_final": False})[0]

        self.assertEqual(0, first["seq"])
        self.assertEqual("hola", first["text"])
        self.assertEqual("hola", first["delta_text"])
        self.assertEqual(1, second["seq"])
        self.assertEqual("hola mundo", second["text"])
        self.assertEqual("mundo", second["delta_text"])
        self.assertEqual(RUN_ENGINE, second["run_engine"])

    def test_final_resets_segment_and_reports_visible_state(self):
        adapter = TranscriptAdapter(SimulStreamingConfig())
        adapter.ingest({"text": "hola", "start": 0.0, "end": 1.0, "is_final": False})
        final = adapter.ingest({"text": "mundo", "start": 1.0, "end": 2.0, "is_final": True})[0]
        after = adapter.ingest({"text": "nuevo", "start": 2.0, "end": 3.0, "is_final": False})[0]

        self.assertTrue(final["is_final"])
        self.assertEqual("hola mundo", final["text"])
        self.assertEqual("nuevo", after["text"])  # accumulator reset
        self.assertEqual(1, adapter.stats_snapshot()["finals_emitted"])

    def test_long_segment_bounds_visible_text_but_keeps_full_text(self):
        from scripts.stt_simulstreaming_backend import VISIBLE_TEXT_MAX_CHARS

        adapter = TranscriptAdapter(SimulStreamingConfig())
        event = None
        for i in range(60):
            event = adapter.ingest(
                {"text": f"palabra{i}", "start": float(i), "end": float(i) + 1, "is_final": False}
            )[0]
        # Firmware-visible text stays bounded (line/buffer safe); full_text complete.
        self.assertLessEqual(len(event["text"]), VISIBLE_TEXT_MAX_CHARS)
        self.assertTrue(event["full_text"].startswith("palabra0 palabra1"))
        self.assertGreater(len(event["full_text"]), len(event["text"]))
        self.assertTrue(event["text"] in event["full_text"] or event["text"].split()[-1] == "palabra59")

    def test_bounded_tail_keeps_whole_words_from_the_end(self):
        from scripts.stt_simulstreaming_backend import bounded_tail

        self.assertEqual("hola mundo", bounded_tail("hola mundo", 50))
        self.assertEqual("c d", bounded_tail("a b c d", 3))

    def test_force_final_flushes_pending_visible_text_once(self):
        adapter = TranscriptAdapter(SimulStreamingConfig())
        adapter.ingest({"text": "colgado", "start": 0.0, "end": 1.0, "is_final": False})
        flushed = adapter.force_final()
        self.assertEqual(1, len(flushed))
        self.assertTrue(flushed[0]["is_final"])
        self.assertEqual("colgado", flushed[0]["text"])
        self.assertEqual([], adapter.force_final())


class SessionTests(unittest.TestCase):
    def test_push_and_flush_never_lose_the_last_text(self):
        online = FakeOnline()
        session = SimulStreamingSession(online, SimulStreamingConfig(), source_rate=16000)

        online.feed({"text": "hola", "start": 0.0, "end": 1.0, "is_final": False})
        events = session.push_float32(np.zeros(1600, dtype="float32"))
        self.assertEqual(1, len(events))
        self.assertEqual("hola", events[0]["text"])
        self.assertEqual("voice", events[0]["vac_status"])

        online.feed({"text": "mundo", "start": 1.0, "end": 2.0, "is_final": False})
        session.push_float32(np.zeros(1600, dtype="float32"))

        # No is_final arrived; flush must emit the accumulated "hola mundo" as final.
        finals = session.flush()
        self.assertTrue(any(event["is_final"] and event["text"] == "hola mundo" for event in finals))
        self.assertEqual([], session.flush())  # idempotent

    def test_vac_endpoint_emits_final(self):
        online = FakeOnline()
        session = SimulStreamingSession(online, SimulStreamingConfig(), source_rate=16000)
        online.feed({"text": "cierre", "start": 0.0, "end": 1.0, "is_final": True})
        events = session.push_float32(np.zeros(1600, dtype="float32"))
        self.assertEqual(1, len(events))
        self.assertTrue(events[0]["is_final"])


class OfflineTests(unittest.TestCase):
    def test_offline_streams_file_and_returns_final_text(self):
        class FakeShared:
            config = SimulStreamingConfig()

            def build_online(self):
                online = FakeOnline()
                online.set_finish({"text": "hola offline", "start": 0.0, "end": 1.0, "is_final": True})
                return online

        finals = transcribe_offline_float32(FakeShared(), np.zeros(32000, dtype="float32"), 16000)
        self.assertTrue(any(event["text"] == "hola offline" for event in finals))


if __name__ == "__main__":
    unittest.main()
