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
    FASTER_WHISPER_PROFILE,
    NEMOTRON_PROFILE,
    SIMULSTREAMING_PROFILE,
    backend_metrics,
    board_delivery_result,
    accuracy_result,
    assign_events,
    counter_delta_for_interval,
    error_rate,
    load_sweep_cases,
    make_report,
    normalize_text,
    offline_reference,
    offline_signature,
    overlay_timeline,
    replicate_summaries,
    reliability_result,
    select_profile,
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
            transcribe_kwargs = None

            class model:
                @staticmethod
                def transcribe(_audio, **kwargs):
                    SharedModel.transcribe_kwargs = kwargs
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
        self.assertEqual(0.5, SharedModel.transcribe_kwargs["vad_parameters"]["threshold"])


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
    def test_default_sweep_is_interleaved_factorial_with_control_replicas(self):
        cases = load_sweep_cases()

        self.assertEqual(6, len(cases))
        self.assertEqual("control_replica_1", cases[0]["name"])
        self.assertEqual(1.0, cases[0]["partial_sec"])
        self.assertEqual(2, cases[0]["partial_agreement"])
        self.assertEqual(4.0, cases[3]["max_window_sec"])
        self.assertEqual(3.0, cases[4]["max_window_sec"])
        self.assertEqual("control_w4_s05", cases[5]["group"])
        self.assertEqual(3, cases[5]["replicate"])

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


class ProfileTests(unittest.TestCase):
    def test_default_profile_is_faster_whisper(self):
        self.assertIs(FASTER_WHISPER_PROFILE, select_profile(None))
        self.assertIs(SIMULSTREAMING_PROFILE, select_profile("simulstreaming_alignatt"))

    def test_unknown_profile_raises(self):
        with self.assertRaises(RuntimeError):
            select_profile("whisperx")

    def test_simulstreaming_profile_verifies_engine(self):
        SIMULSTREAMING_PROFILE.verify_health({"run_engine": "simulstreaming_alignatt"})
        with self.assertRaises(RuntimeError):
            SIMULSTREAMING_PROFILE.verify_health({"run_engine": "stream_server"})
        # faster_whisper accepts the legacy server that omits run_engine.
        FASTER_WHISPER_PROFILE.verify_health({"status": "ok"})

    def test_simulstreaming_config_env_is_a_single_backend_json(self):
        env = SIMULSTREAMING_PROFILE.config_env({"frame_threshold": 12, "use_vac": True})
        self.assertIn("STT_BACKEND_CONFIG_JSON", env)
        self.assertEqual(
            {"frame_threshold": 12, "use_vac": True},
            json.loads(env["STT_BACKEND_CONFIG_JSON"]),
        )

    def test_faster_whisper_config_env_uses_named_env_vars(self):
        env = FASTER_WHISPER_PROFILE.config_env({"max_window_sec": 4.0})
        self.assertEqual("4.0", env["STT_MAX_WINDOW_SEC"])
        self.assertNotIn("STT_BACKEND_CONFIG_JSON", env)

    def test_offline_signatures_differ_across_backends(self):
        health = {"run_config": {"config_model": "small", "run_engine": "simulstreaming_alignatt"}}
        fw = offline_signature(health, None, FASTER_WHISPER_PROFILE)
        simul = offline_signature(health, None, SIMULSTREAMING_PROFILE)
        self.assertNotEqual(fw, simul)
        self.assertEqual("faster_whisper", fw["__profile__"])
        self.assertEqual("simulstreaming_alignatt", simul["__profile__"])


class NemotronProfileTests(unittest.TestCase):
    def test_profile_is_selectable_by_name(self):
        self.assertIs(NEMOTRON_PROFILE, select_profile("nemotron_3_5_nemo"))
        self.assertEqual("run_stt_colab_nemotron.sh", NEMOTRON_PROFILE.launcher)

    def test_profile_refuses_to_run_against_another_backend(self):
        NEMOTRON_PROFILE.verify_health({"run_engine": "nemotron_3_5_nemo"})
        NEMOTRON_PROFILE.verify_health({"run_config": {"run_engine": "nemotron_3_5_nemo"}})
        for wrong in ("simulstreaming_alignatt", "stream_server", None):
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

    def test_offline_signature_differs_from_the_other_backends(self):
        health = {"run_config": {"run_engine": "nemotron_3_5_nemo"}}
        self.assertNotEqual(
            offline_signature(health, None, NEMOTRON_PROFILE),
            offline_signature(health, None, SIMULSTREAMING_PROFILE),
        )

    def test_sweep_is_gated_until_the_smoke_test_passes(self):
        with self.assertRaises(RuntimeError) as raised:
            load_sweep_cases(None, NEMOTRON_PROFILE)
        self.assertIn("smoke", str(raised.exception))

    def test_sweep_file_still_rejects_foreign_parameters(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "sweep.json"
            path.write_text(json.dumps([{"name": "bad", "frame_threshold": 25}]), encoding="utf-8")
            with self.assertRaises(ValueError):
                load_sweep_cases(path, NEMOTRON_PROFILE)


class SimulStreamingSweepTests(unittest.TestCase):
    def test_initial_sweep_file_loads_four_alignatt_cases(self):
        cases = load_sweep_cases(None, SIMULSTREAMING_PROFILE)
        self.assertEqual(4, len(cases))
        self.assertEqual(
            ["official_default", "paper_margin", "no_trim", "paper_no_trim"],
            [case["name"] for case in cases],
        )
        self.assertEqual(12, cases[1]["frame_threshold"])
        self.assertTrue(cases[2]["never_fire"])

    def test_sweep_rejects_faster_whisper_params_under_simul_profile(self):
        with tempfile.TemporaryDirectory() as temporary:
            path = Path(temporary) / "sweep.json"
            path.write_text(json.dumps([{"name": "bad", "max_window_sec": 4.0}]), encoding="utf-8")
            with self.assertRaises(ValueError):
                load_sweep_cases(path, SIMULSTREAMING_PROFILE)


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
        self.assertEqual(2, metrics["empty_decodes"])
        self.assertEqual(1, metrics["last_word_truncations"])
        self.assertAlmostEqual(0.4, metrics["update_rate_hz"])

    def test_nemotron_final_reasons_are_kept_separate(self):
        events = [
            {"seq": 0, "is_final": True, "text": "línea", "final_reason": "display_rollup"},
            {"seq": 1, "is_final": True, "text": "frase", "final_reason": "model_eou"},
            {"seq": 2, "is_final": True, "text": "cierre", "final_reason": "session_flush"},
        ]
        metrics = backend_metrics(events, {"eou_count": 2}, audio_duration_sec=3.0)
        self.assertEqual(2, metrics["model_eou_count"])
        self.assertEqual(1, metrics["model_eou_events"])
        self.assertEqual(1, metrics["display_rollup_finals"])
        self.assertEqual(1, metrics["session_flush_finals"])


if __name__ == "__main__":
    unittest.main()
