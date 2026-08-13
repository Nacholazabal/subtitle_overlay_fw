import json
import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock


sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from server.audio_tests import short_audio as audio_test_short
from server.audio_tests.short_audio import (
    NEMOTRON_PROFILE,
    backend_metrics,
    board_delivery_result,
    accuracy_result,
    assign_events,
    counter_delta_for_interval,
    error_rate,
    final_sweep_status,
    load_sweep_cases,
    latency_progression,
    make_report,
    normalize_text,
    offline_reference,
    offline_signature,
    overlay_timeline,
    replicate_summaries,
    reliability_result,
    render_sweep_markdown,
    select_best_by_metric,
    select_profile,
)


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

    def test_partial_skip_counter_is_attributed_to_its_clip_interval(self):
        events = [
            {"bridge_received_wall_sec": 99.0, "partial_jobs_skipped": 20},
            {"bridge_received_wall_sec": 101.0, "partial_jobs_skipped": 22},
            {"bridge_received_wall_sec": 111.0, "partial_jobs_skipped": 25},
        ]

        self.assertEqual(2, counter_delta_for_interval(events, "partial_jobs_skipped", 100, 110))


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


class ReliabilityTests(unittest.TestCase):
    @staticmethod
    def accepted_delivery(count=10):
        return {
            "handshake_ok": True,
            "protocol_version": 1,
            "session_id": 1,
            "generated": count,
            "sent": count,
            "accepted": count,
            "rejected": 0,
            "delivery_unknown": 0,
            "sink_dropped_partials": 0,
            "sink_dropped_finals": 0,
            "ack_latency_sec": [0.01] * count,
        }

    def test_protocol_error_invalidates_reliability(self):
        done = {
            "status": "complete",
            "chunks_forwarded": 100,
            "board_dropped_chunks": 0,
            "jobs_submitted": 10,
            "dropped_audio_jobs": 0,
            "events_emitted": 10,
            "events_dropped": 0,
            "subtitle_delivery": self.accepted_delivery(),
        }

        self.assertEqual(100.0, reliability_result(done, True)["score"])
        self.assertEqual(0.0, reliability_result(done, False)["score"])

    def test_startup_drop_counter_and_partial_skips_do_not_reduce_reliability(self):
        done = {
            "status": "complete",
            "chunks_forwarded": 100,
            "board_dropped_chunks_start": 769,
            "board_dropped_chunks_end": 769,
            "board_dropped_chunks_during_session": 0,
            "jobs_submitted": 30,
            "partial_jobs_skipped": 22,
            "final_jobs_dropped": 0,
            "events_emitted": 20,
            "events_dropped": 0,
            "subtitle_delivery": self.accepted_delivery(20),
        }

        result = reliability_result(done, True)

        self.assertEqual(100.0, result["score"])
        self.assertEqual(22, result["partial_jobs_skipped"])
        self.assertEqual(769, result["board_dropped_chunks_start"])

    def test_real_session_audio_loss_reduces_reliability(self):
        done = {
            "status": "complete",
            "chunks_forwarded": 97,
            "board_dropped_chunks_during_session": 3,
            "jobs_submitted": 10,
            "partial_jobs_skipped": 5,
            "final_jobs_dropped": 0,
            "events_emitted": 10,
            "events_dropped": 0,
            "subtitle_delivery": self.accepted_delivery(),
        }

        self.assertEqual(97.0, reliability_result(done, True)["score"])

    def test_missing_or_rejected_ack_invalidates_total_reliability(self):
        done = {
            "status": "complete",
            "chunks_forwarded": 10,
            "jobs_submitted": 1,
            "events_emitted": 2,
            "events_dropped": 0,
            "subtitle_delivery": {
                **self.accepted_delivery(2),
                "accepted": 1,
                "rejected": 1,
            },
        }

        self.assertFalse(board_delivery_result(done)["protocol_ok"])
        self.assertEqual(0.0, reliability_result(done, True)["score"])


class SweepConfigTests(unittest.TestCase):
    def test_custom_sweep_rejects_unknown_parameter(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "sweep.json"
            path.write_text(json.dumps([{"name": "bad", "model": "large"}]), encoding="utf-8")

            with self.assertRaises(ValueError):
                load_sweep_cases(path)

    def test_control_replicates_report_mean_and_range_per_metric(self):
        rows = [
            {
                "name": f"control_{index}",
                "group": "control",
                "status": "complete",
                "accuracy": value,
                "latency": 100.0,
                "readability": value,
                "reliability": 100.0,
                "wer_percent": 100.0 - value,
                "cer_percent": 10.0,
                "latency_p90_sec": 0.8,
            }
            for index, value in enumerate((70.0, 80.0, 90.0), 1)
        ]

        summaries = replicate_summaries(rows)

        self.assertEqual(1, len(summaries))
        self.assertEqual(80.0, summaries[0]["metrics"]["accuracy"]["mean"])
        self.assertEqual(20.0, summaries[0]["metrics"]["accuracy"]["range"])

    def test_invalid_run_cannot_win_a_metric(self):
        rows = [
            {
                "name": "invalid_but_accurate",
                "status": "invalid",
                "protocol_valid": False,
                "accuracy": 99.0,
                "latency": 99.0,
                "readability": 99.0,
                "reliability": 0.0,
            },
            {
                "name": "valid",
                "status": "complete",
                "protocol_valid": True,
                "accuracy": 80.0,
                "latency": 90.0,
                "readability": 10.0,
                "reliability": 100.0,
            },
        ]

        selected = select_best_by_metric(rows)

        self.assertEqual(["valid"], selected["accuracy"])
        self.assertEqual(["valid"], selected["latency"])

    def test_completed_sweep_reports_invalid_runs_separately_from_errors(self):
        invalid = [{"status": "invalid", "protocol_valid": False}]

        self.assertEqual(
            "complete_with_invalid_runs",
            final_sweep_status("running", 0, invalid),
        )
        self.assertEqual(
            "complete_with_errors",
            final_sweep_status("running", 1, invalid),
        )
        self.assertEqual("interrupted", final_sweep_status("interrupted", 130, invalid))


class LatencyProgressionTests(unittest.TestCase):
    def test_reports_p90_growth_across_ordered_thirds(self):
        result = latency_progression([0.1, 0.2, 0.3, 0.5, 0.6, 0.7, 1.0, 1.2, 1.4])

        self.assertTrue(result["available"])
        self.assertEqual(0.3, result["first_third"]["p90"])
        self.assertEqual(1.4, result["last_third"]["p90"])
        self.assertEqual(1.1, result["p90_delta_last_minus_first_sec"])
        self.assertTrue(result["attention"])

    def test_too_few_samples_are_not_called_drift(self):
        result = latency_progression([0.2, 0.3])

        self.assertFalse(result["available"])
        self.assertIsNone(result["p90_delta_last_minus_first_sec"])


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
                        "final_reason": "model_eou",
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
                "run_engine": "nemotron_3_5_nemo",
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
            self.assertEqual(0.2, report["global_metrics"]["final_latency"]["p90"])
            self.assertEqual(0.2, report["global_metrics"]["model_eou_latency"]["p90"])
            self.assertFalse(report["global_metrics"]["latency_progression"]["available"])
            self.assertTrue((root / "report.md").exists())
            self.assertTrue((root / "overlay_timeline.json").exists())


class NemotronProfileTests(unittest.TestCase):
    def test_profile_is_selectable_by_name(self):
        self.assertIs(NEMOTRON_PROFILE, select_profile("nemotron_3_5_nemo"))
        self.assertEqual("run.sh", NEMOTRON_PROFILE.launcher)

    def test_profile_refuses_to_run_against_another_backend(self):
        NEMOTRON_PROFILE.verify_health({"run_engine": "nemotron_3_5_nemo"})
        NEMOTRON_PROFILE.verify_health({"run_config": {"run_engine": "nemotron_3_5_nemo"}})
        for wrong in ("another_engine", "unknown", None):
            with self.assertRaises(RuntimeError):
                NEMOTRON_PROFILE.verify_health({"run_engine": wrong})

    def test_session_config_travels_as_backend_config_json(self):
        env = NEMOTRON_PROFILE.config_env({"latency_ms": 560, "stop_history_eou_ms": 400})
        self.assertIn("STT_BACKEND_CONFIG_JSON", env)
        self.assertEqual(
            {"latency_ms": 560, "stop_history_eou_ms": 400},
            json.loads(env["STT_BACKEND_CONFIG_JSON"]),
        )

    def test_validator_rejects_foreign_and_invalid_parameters(self):
        self.assertEqual({"latency_ms": 320}, NEMOTRON_PROFILE.validator({"latency_ms": 320}))
        with self.assertRaises(ValueError):
            NEMOTRON_PROFILE.validator({"frame_threshold": 25})
        with self.assertRaises(ValueError):
            NEMOTRON_PROFILE.validator({"latency_ms": 250})

    def test_offline_cache_signature_pins_engine_model_and_operating_point(self):
        health = {
            "run_config": {
                "run_engine": "nemotron_3_5_nemo",
                "nemo_commit": "2639d4bef8d1450782263a8f616242acfb6fecb9",
                "config_model": "nvidia/nemotron-3.5-asr-streaming-0.6b",
                "config_target_lang": "es-ES",
                "config_decoder_type": "rnnt",
                "config_latency_ms": 320,
                "config_att_context_size": [56, 3],
                "config_stop_history_eou_ms": 800,
                "config_residue_tokens_at_end": 2,
                "config_strip_lang_tags": True,
            }
        }
        signature = offline_signature(health, None, NEMOTRON_PROFILE)
        self.assertEqual("nemotron_3_5_nemo", signature["__profile__"])
        for key in (
            "run_engine",
            "nemo_commit",
            "config_model",
            "config_target_lang",
            "config_decoder_type",
            "config_latency_ms",
            "config_att_context_size",
            "config_strip_lang_tags",
        ):
            self.assertIn(key, signature)
        # A different operating point must not reuse the 320 ms cache entry.
        other = offline_signature(health, {"latency_ms": 560}, NEMOTRON_PROFILE)
        self.assertNotEqual(signature, other)
        # Live endpointing does not change full-file model.transcribe().
        endpoint_only = offline_signature(
            health,
            {"stop_history_eou_ms": 400, "residue_tokens_at_end": 4},
            NEMOTRON_PROFILE,
        )
        self.assertEqual(signature, endpoint_only)

    def test_confirmatory_sweep_interleaves_four_groups_with_three_replicates(self):
        cases = load_sweep_cases(None, NEMOTRON_PROFILE)

        self.assertEqual(12, len(cases))
        self.assertEqual(
            [
                "control_320_r1",
                "candidate_560_r1",
                "mid_eou_600_r1",
                "fast_eou_400_r1",
                "mid_eou_600_r2",
                "fast_eou_400_r2",
                "control_320_r2",
                "candidate_560_r2",
                "fast_eou_400_r3",
                "mid_eou_600_r3",
                "candidate_560_r3",
                "control_320_r3",
            ],
            [case["name"] for case in cases],
        )
        for group in ("control_320", "candidate_560", "mid_eou_600", "fast_eou_400"):
            members = [case for case in cases if case.get("group") == group]
            self.assertEqual([1, 2, 3], [case["replicate"] for case in members])
        self.assertEqual({320, 560}, {case["latency_ms"] for case in cases})
        self.assertEqual({400, 600, 800}, {case["stop_history_eou_ms"] for case in cases})
        self.assertEqual({2}, {case["residue_tokens_at_end"] for case in cases})
        self.assertNotIn(160, {case["latency_ms"] for case in cases})
        self.assertNotIn(1120, {case["latency_ms"] for case in cases})

    def test_nemotron_sweep_report_uses_backend_specific_columns(self):
        sweep = {
            "sweep_id": "test-sweep",
            "status": "complete_with_invalid_runs",
            "profile": "nemotron_3_5_nemo",
            "runs": [
                {
                    "name": "control_320_r1",
                    "run_id": "test-run",
                    "status": "invalid",
                    "protocol_valid": False,
                    "effective_config": {
                        "latency_ms": 320,
                        "att_context_size": [56, 3],
                        "stop_history_eou_ms": 800,
                        "residue_tokens_at_end": 2,
                    },
                    "accuracy": 82.54,
                    "wer_percent": 17.46,
                    "cer_percent": 12.58,
                    "latency": 89.27,
                    "latency_p90_sec": 1.56,
                    "latency_p95_sec": 2.5,
                    "final_latency_p90_sec": 1.1,
                    "model_eou_latency_p90_sec": 1.2,
                    "time_to_first_subtitle_sec": 1.77,
                    "clip_metrics": {
                        "desay-short": {"latency_p90_sec": 0.56},
                        "noticiero-short": {"latency_p90_sec": 1.4},
                        "rel-short": {"latency_p90_sec": 3.33},
                    },
                    "latency_progression": {
                        "p90_delta_last_minus_first_sec": 2.77
                    },
                    "model_eou_count": 14,
                    "model_eou_per_min": 3.5,
                    "model_eou_duration_p50_sec": 2.4,
                    "display_rollup_finals": 53,
                    "session_flush_finals": 0,
                    "update_rate_hz": 2.34,
                    "partial_stability": 0.875,
                    "partial_replacements": 7,
                    "board_acks_accepted": 493,
                    "board_events_generated": 494,
                    "board_acceptance_percent": 99.8,
                    "board_rejected": 0,
                    "board_delivery_unknown": 1,
                    "board_reconnections": 1,
                    "partial_jobs_skipped": 0,
                    "real_audio_drops": 0,
                    "final_jobs_dropped": 0,
                }
            ],
            "best_by_metric": {},
            "replicate_summaries": [],
        }

        markdown = render_sweep_markdown(sweep)

        self.assertIn("Nemotron parameter sweep", markdown)
        self.assertIn("320 ms", markdown)
        self.assertIn("EOU hist.", markdown)
        self.assertIn("Fragmentación y estabilidad", markdown)
        self.assertIn("87.50%", markdown)
        self.assertIn("1.20 s", markdown)
        self.assertIn("p90 relato", markdown)
        self.assertIn("493/494", markdown)
        self.assertIn("**NO**", markdown)
        self.assertIn("Corridas válidas: 0/1", markdown)
        self.assertIn("Ninguna corrida", markdown)

    def test_sweep_file_still_rejects_foreign_parameters(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "sweep.json"
            path.write_text(json.dumps([{"name": "bad", "frame_threshold": 25}]), encoding="utf-8")
            with self.assertRaises(ValueError):
                load_sweep_cases(path, NEMOTRON_PROFILE)


class BackendMetricsTests(unittest.TestCase):
    def test_partial_replacements_and_stability(self):
        events = [
            {"seq": 0, "is_final": False, "text": "hola"},
            {"seq": 1, "is_final": False, "text": "hola mundo"},   # extension, stable
            {"seq": 2, "is_final": False, "text": "adios"},        # replacement
            {"seq": 3, "is_final": True, "text": "adios amigo", "truncated_last_word": True},
        ]
        metrics = backend_metrics(events, {"empty_decodes": 2}, audio_duration_sec=10.0)
        self.assertEqual(1, metrics["finals"])
        self.assertEqual(3, metrics["partials"])
        self.assertEqual(1, metrics["partial_replacements"])
        self.assertAlmostEqual(0.6667, metrics["partial_stability"])
        self.assertEqual(2, metrics["empty_decodes"])
        self.assertEqual(1, metrics["last_word_truncations"])
        self.assertAlmostEqual(0.4, metrics["update_rate_hz"])

    def test_nemotron_final_reasons_are_kept_separate(self):
        events = [
            {"seq": 0, "is_final": True, "text": "línea", "final_reason": "display_rollup"},
            {
                "seq": 1,
                "is_final": True,
                "text": "frase",
                "final_reason": "model_eou",
                "start_sec": 1.0,
                "end_sec": 2.5,
            },
            {"seq": 2, "is_final": True, "text": "cierre", "final_reason": "session_flush"},
            {"seq": 3, "is_final": True, "text": "último frame", "final_reason": "session_end_final"},
        ]
        metrics = backend_metrics(events, {"eou_count": 2}, audio_duration_sec=3.0)
        self.assertEqual(2, metrics["model_eou_count"])
        self.assertEqual(1, metrics["model_eou_events"])
        self.assertEqual(1, metrics["display_rollup_finals"])
        self.assertEqual(1, metrics["session_flush_finals"])
        self.assertEqual(1, metrics["session_end_finals"])
        self.assertEqual(20.0, metrics["model_eou_per_min"])
        self.assertEqual(1.5, metrics["model_eou_duration_sec"]["p50"])


if __name__ == "__main__":
    unittest.main()
