import io
import json
import unittest
from pathlib import Path
from tempfile import TemporaryDirectory

import numpy as np

from server.evaluation.fleurs import (
    FleursEvaluator,
    _audio_array,
    fixed_fleurs_config,
)


def rows():
    # A factory, rather than a list of evaluator-owned audio, mirrors HF's
    # restartable iterable dataset while keeping this test offline/small.
    for clip_id, reference in (("001", "Son dos manzanas."), ("002", "Buenos días")):
        yield {
            "id": clip_id,
            "path": f"audio/test/{clip_id}.wav",
            "audio": {"array": np.zeros(16000, dtype="float32"), "sampling_rate": 16000},
            "raw_transcription": reference,
            "speaker_id": 7,
        }


class FakeSession:
    def __init__(self, text):
        self.text = text

    def push_float32(self, audio):
        return [{"is_final": True, "full_text": self.text, "text": self.text, "end_sec": 0.5}]

    def flush(self):
        return []

    def stats_snapshot(self):
        return {"events_emitted": 1}

    def close(self):
        pass


class FakeModel:
    def __init__(self):
        self.calls = 0

    def provenance(self):
        return {"model_revision": "fake", "run_engine": "fake", "nemo_commit": "fake", "gpu_name": "cpu-test"}

    def configure_streaming(self, config):
        pass

    def build_session(self, config, source_rate):
        return FakeSession("Son 2 manzanas" if self.calls == 1 else "Buenos días")


class FleursEvaluationTests(unittest.TestCase):
    def setUp(self):
        self.temp = TemporaryDirectory()
        self.root = Path(self.temp.name)

    def tearDown(self):
        self.temp.cleanup()

    def evaluator(self, model=None, fingerprint="hf-commit-abc"):
        model = model or FakeModel()

        def offline(shared, audio, config):
            shared.calls += 1
            return {"text": "Son 2 manzanas" if shared.calls == 1 else "Buenos días", "segments": []}

        return FleursEvaluator(
            model, rows, self.root / "results", project_commit="test-commit",
            dataset_fingerprint=fingerprint, config=fixed_fleurs_config(),
            checkpoint_every=1, offline_transcriber=offline,
        )

    def test_runs_both_production_paths_and_serializes_two_named_wer_profiles(self):
        evaluator = self.evaluator()
        self.assertEqual("complete", evaluator.run_all()["status"])
        summary = json.loads((self.root / "results/summary.json").read_text())
        self.assertEqual(2, summary["identity"]["expected_clips"])
        self.assertEqual(["001:001.wav", "002:002.wav"], summary["identity"]["clip_ids"])
        self.assertIn("numeric_es", summary["phases"]["offline"]["wer"])
        self.assertGreater(summary["phases"]["offline"]["wer"]["legacy"]["rate"], 0)
        self.assertEqual(0.0, summary["phases"]["offline"]["wer"]["numeric_es"]["rate"])
        offline = [json.loads(line) for line in (self.root / "results/offline_results.jsonl").read_text().splitlines()]
        self.assertEqual(0.0, offline[0]["wer"]["numeric_es"]["rate"])
        self.assertEqual("Son 2 manzanas", offline[0]["hypothesis_raw"])
        self.assertTrue((self.root / "results/report.md").is_file())
        self.assertTrue((self.root / "results/model_provenance.json").is_file())

    def test_resume_skips_successful_clip_instead_of_retranscribing(self):
        model = FakeModel()
        first = self.evaluator(model)
        self.assertEqual("complete", first.run_offline()["status"])
        self.assertEqual(2, model.calls)
        resumed = self.evaluator(model)
        self.assertEqual(0, resumed.run_offline()["processed_this_run"])
        self.assertEqual(2, model.calls)

    def test_dataset_identity_change_cannot_mix_results(self):
        self.evaluator()
        with self.assertRaises(RuntimeError):
            self.evaluator(fingerprint="different-hf-commit")

    def test_same_sentence_id_with_distinct_recordings_is_not_duplicate(self):
        def repeated_sentence_rows():
            for filename in ("first.wav", "second.wav"):
                yield {
                    "id": 1816,
                    "path": f"audio/test/{filename}",
                    "audio": {"array": np.zeros(16000, dtype="float32"), "sampling_rate": 16000},
                    "raw_transcription": "La misma frase.",
                }

        model = FakeModel()
        evaluator = FleursEvaluator(
            model, repeated_sentence_rows, self.root / "repeated",
            project_commit="test-commit", dataset_fingerprint="hf-commit-abc",
            offline_transcriber=lambda shared, audio, config: {"text": "La misma frase."},
        )
        self.assertEqual(["1816:first.wav", "1816:second.wav"], evaluator.identity["clip_ids"])
        self.assertEqual("complete", evaluator.run_offline()["status"])
        self.assertEqual(2, len(evaluator.store.latest("offline")))

    def test_duplicate_recording_still_fails(self):
        def duplicate_rows():
            row = next(rows())
            yield row
            yield row

        with self.assertRaisesRegex(ValueError, "duplicate FLEURS recording"):
            FleursEvaluator(
                FakeModel(), duplicate_rows, self.root / "duplicate",
                project_commit="test-commit", dataset_fingerprint="hf-commit-abc",
            )

    def test_audio_validation_rejects_wrong_rate_and_stereo(self):
        with self.assertRaisesRegex(ValueError, "16000 Hz"):
            _audio_array({"audio": {"array": np.zeros(8), "sampling_rate": 8000}})
        with self.assertRaisesRegex(ValueError, "mono"):
            _audio_array({"audio": {"array": np.zeros((8, 2)), "sampling_rate": 16000}})

    def test_decodes_embedded_wav_bytes_without_torchcodec(self):
        try:
            import soundfile as sf
        except ImportError:
            self.skipTest("optional soundfile dependency is not installed locally")
        buffer = io.BytesIO()
        sf.write(buffer, np.zeros(160, dtype="float32"), 16000, format="WAV")
        decoded = _audio_array({"audio": {"bytes": buffer.getvalue(), "path": None}})
        self.assertEqual((160,), decoded.shape)

    def test_notebook_targets_only_fleurs_test_and_does_not_start_fastapi(self):
        notebook_path = Path(__file__).resolve().parents[1] / "notebooks" / "nemotron_fleurs_eval.ipynb"
        notebook = json.loads(notebook_path.read_text(encoding="utf-8"))
        source = "\n".join("".join(c.get("source", [])) for c in notebook["cells"])
        self.assertIn("load_dataset('google/fleurs', 'es_419', split='test', streaming=True", source)
        self.assertIn("Audio(decode=False)", source)
        self.assertIn("DATASET_REVISION", source)
        self.assertNotIn("create_app(", source)
        self.assertIn("feat/perf-profiling", source)


if __name__ == "__main__":
    unittest.main()
