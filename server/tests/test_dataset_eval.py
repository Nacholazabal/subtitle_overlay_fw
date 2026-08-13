#!/usr/bin/env python3

import json
import unittest
from pathlib import Path
from tempfile import TemporaryDirectory

import numpy as np

from server.evaluation.dataset_manifest import DatasetClip
from server.evaluation.dataset import (
    EvaluationIdentityError,
    EvaluationIncompleteError,
    NemotronDatasetEvaluator,
    build_summary,
    consolidate_streaming_text,
    error_rate,
    fixed_nemotron_config,
    render_report,
)


def make_clip(clip_id: str, reference: str) -> DatasetClip:
    return DatasetClip(
        clip_id=clip_id,
        audio_relpath=f"{clip_id}.flac",
        reference_relpath=f"{clip_id}.txt",
        audio_sha256=(clip_id * 64)[:64],
        reference_sha256=("f" + clip_id * 64)[:64],
        reference_raw=reference,
        reference_normalized=reference,
        reference_words=len(reference.split()),
    )


class FakeSession:
    def __init__(self, text):
        self.text = text
        self.closed = False

    def push_float32(self, audio):
        return [
            {"seq": 0, "is_final": False, "text": self.text.split()[0], "end_sec": 0.4},
            {
                "seq": 1,
                "is_final": True,
                "text": self.text,
                "full_text": self.text,
                "end_sec": 0.8,
                "final_reason": "model_eou",
            },
        ]

    def flush(self):
        return []

    def close(self):
        self.closed = True

    def stats_snapshot(self):
        return {
            "partial_revisions": 0,
            "inference_rtf": 0.1,
            "eou_count": 1,
        }


class FakeSharedModel:
    def __init__(self, transcript="hola mundo", gpu_name="fake"):
        self.transcript = transcript
        self.gpu_name = gpu_name
        self.configure_calls = 0

    def provenance(self):
        return {
            "model_revision": "model-rev",
            "chunk_size_in_secs": 0.08,
            "gpu_name": self.gpu_name,
        }

    def build_session(self, config, source_rate):
        return FakeSession(self.transcript)

    def configure_streaming(self, config):
        self.configure_calls += 1


class MetricTests(unittest.TestCase):
    def test_error_rates_use_human_normalization(self):
        result = error_rate("¡Hola, mundo!", "hola mundo", unit="word")
        self.assertEqual(0, result.edits)
        self.assertEqual(0.0, result.rate)

    def test_streaming_text_contains_only_committed_finals(self):
        events = [
            {"is_final": False, "full_text": "texto viejo"},
            {"is_final": True, "full_text": "texto final"},
            {"is_final": True, "text": "segunda línea"},
        ]
        self.assertEqual("texto final segunda línea", consolidate_streaming_text(events))

    def test_fixed_configuration_is_the_selected_560_600_2_point(self):
        config = fixed_nemotron_config()
        self.assertEqual(560, config.latency_ms)
        self.assertEqual((56, 6), config.att_context_size)
        self.assertEqual(600, config.stop_history_eou_ms)
        self.assertEqual(2, config.residue_tokens_at_end)


class EvaluatorTests(unittest.TestCase):
    def setUp(self):
        self.temporary = TemporaryDirectory()
        self.root = Path(self.temporary.name)
        self.clips = [make_clip("a", "hola mundo"), make_clip("b", "hola mundo")]

    def tearDown(self):
        self.temporary.cleanup()

    @staticmethod
    def audio_loader(_path):
        return np.zeros(16000, dtype="float32")

    @staticmethod
    def offline_transcriber(shared, _audio, _config):
        return {"text": shared.transcript, "segments": []}

    def evaluator(self, model=None, commit="commit-a"):
        return NemotronDatasetEvaluator(
            model or FakeSharedModel(),
            self.root,
            self.clips,
            self.root / "results",
            project_commit=commit,
            audio_loader=self.audio_loader,
            offline_transcriber=self.offline_transcriber,
            checkpoint_every=1,
        )

    def test_runs_offline_then_same_production_session_accelerated(self):
        model = FakeSharedModel()
        evaluator = self.evaluator(model)
        self.assertEqual("complete", evaluator.run_offline()["status"])
        self.assertEqual("complete", evaluator.run_streaming()["status"])
        summary = json.loads((self.root / "results" / "summary.json").read_text())
        self.assertEqual("complete", summary["status"])
        self.assertEqual(0.0, summary["offline"]["micro_wer_vs_human"]["rate"])
        self.assertEqual(0.0, summary["streaming"]["micro_wer_vs_offline"]["rate"])
        self.assertEqual(1, model.configure_calls)
        stream_rows = [
            json.loads(line)
            for line in (self.root / "results" / "streaming_results.jsonl")
            .read_text()
            .splitlines()
        ]
        self.assertTrue(all(row["mode"] == "accelerated_production_session_no_sleep" for row in stream_rows))

    def test_resume_skips_successful_clips(self):
        evaluator = self.evaluator()
        evaluator.run_offline()

        def must_not_run(*_args):
            raise AssertionError("completed clip was repeated")

        resumed = NemotronDatasetEvaluator(
            FakeSharedModel(),
            self.root,
            self.clips,
            self.root / "results",
            project_commit="commit-a",
            audio_loader=must_not_run,
            offline_transcriber=must_not_run,
            checkpoint_every=1,
        )
        progress = resumed.run_offline()
        self.assertEqual(0, progress["processed_this_run"])

    def test_streaming_refuses_to_start_before_offline_is_complete(self):
        with self.assertRaises(EvaluationIncompleteError):
            self.evaluator().run_streaming()

    def test_identity_change_refuses_to_mix_results(self):
        self.evaluator()
        with self.assertRaises(EvaluationIdentityError):
            self.evaluator(commit="another-commit")

    def test_runtime_gpu_change_refuses_to_mix_rtf_measurements(self):
        self.evaluator(FakeSharedModel(gpu_name="T4"))
        with self.assertRaises(EvaluationIdentityError):
            self.evaluator(FakeSharedModel(gpu_name="L4"))

    def test_failed_clip_is_retried_and_historical_error_is_preserved(self):
        calls = 0

        def fail_once(shared, _audio, _config):
            nonlocal calls
            calls += 1
            if calls == 1:
                raise RuntimeError("temporary CUDA failure")
            return {"text": shared.transcript, "segments": []}

        evaluator = NemotronDatasetEvaluator(
            FakeSharedModel(),
            self.root,
            self.clips,
            self.root / "retry-results",
            project_commit="commit-a",
            audio_loader=self.audio_loader,
            offline_transcriber=fail_once,
            checkpoint_every=1,
        )
        self.assertEqual("incomplete", evaluator.run_offline()["status"])
        self.assertEqual("complete", evaluator.run_offline()["status"])
        summary = evaluator.write_reports()
        self.assertEqual(1, summary["execution_history"]["historical_errors"])
        self.assertIn("temporary CUDA failure", (self.root / "retry-results" / "errors.jsonl").read_text())

    def test_report_marks_human_reference_and_accelerated_latency_scope(self):
        evaluator = self.evaluator()
        evaluator.run_all()
        summary = build_summary(
            evaluator.identity,
            list(evaluator.store.latest("offline").values()),
            list(evaluator.store.latest("streaming").values()),
        )
        report = render_report(summary)
        self.assertIn("referencias humanas", report)
        self.assertIn("no", report.lower())
        self.assertIn("latencia física", report)
        self.assertIn("session_flush", report)


class NotebookContractTests(unittest.TestCase):
    def test_notebook_is_offline_then_streaming_and_has_no_server_transport(self):
        notebook = json.loads(
            (Path(__file__).resolve().parents[2] / "server" / "notebooks" / "nemotron_dataset_eval.ipynb")
            .read_text(encoding="utf-8")
        )
        source = "\n".join(
            "".join(cell.get("source", [])) for cell in notebook["cells"]
        )
        self.assertLess(source.index("evaluator.run_offline()"), source.index("evaluator.run_streaming()"))
        self.assertNotIn("pyngrok", source)
        self.assertNotIn("create_app(", source)
        self.assertNotIn("audiotestshort.sh", source)
        self.assertIn("fixed_nemotron_config()", source)
        self.assertIn("streaming cache-aware acelerado", source)


if __name__ == "__main__":
    unittest.main()
