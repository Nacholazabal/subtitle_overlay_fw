import sys
import tempfile
import unittest
from pathlib import Path
from unittest import mock

import numpy as np


sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from server.runtime.nemotron import (
    MODEL_ID,
    NEMO_COMMIT,
    RUN_ENGINE,
    TARGET_RATE,
    NemotronConfig,
)
from server.runtime.app import (
    LOCKED_OVERRIDES,
    STATE_FAILED,
    STATE_READY,
    BackendState,
    ServerConfig,
    SingleSessionManager,
    decode_audio_bytes_to_float32,
    prepare_shared_model,
    run_offline_transcription,
    session_config_from_start,
    session_ready_message,
)


class FakeEngine:
    frame_samples = 1600

    def open(self):
        pass

    def close(self):
        pass

    def step(self, samples, *, is_first, is_last, valid_length):
        return []


class FakeShared:
    """Stand-in for SharedNemotronModel."""

    def __init__(self, config=None):
        self.config = config or NemotronConfig()
        self.configure_calls = []
        self.sessions = []

    def configure_streaming(self, config):
        self.configure_calls.append(config)
        from dataclasses import replace

        self.config = replace(
            self.config,
            latency_ms=config.latency_ms,
            stop_history_eou_ms=config.stop_history_eou_ms,
            residue_tokens_at_end=config.residue_tokens_at_end,
        )

    def build_session(self, config=None, source_rate=TARGET_RATE):
        from server.runtime.nemotron import NemotronSession

        session = NemotronSession(FakeEngine(), config or self.config, source_rate=source_rate)
        self.sessions.append(session)
        return session

    def warmup(self, seconds=1.0):
        return True

    def provenance(self):
        return {"run_engine": RUN_ENGINE, "model_id": self.config.model_id, "nemo_commit": NEMO_COMMIT}


class BackendStateTests(unittest.TestCase):
    def test_loader_success_transitions_to_ready(self):
        sentinel = FakeShared()
        state = BackendState(ServerConfig(), loader=lambda: sentinel)
        self.assertFalse(state.is_ready())
        state.run_loader()
        self.assertEqual(STATE_READY, state.status)
        self.assertIs(sentinel, state.shared_model)

    def test_loader_failure_records_sanitized_error_and_traceback(self):
        import os

        secret_path = os.path.join(os.path.expanduser("~"), "secret", "path")

        def boom():
            raise RuntimeError(f"cuda out of memory at {secret_path}")

        state = BackendState(ServerConfig(), loader=boom)
        state.run_loader()
        self.assertEqual(STATE_FAILED, state.status)
        payload = state.health_payload()
        self.assertFalse(payload["ready"])
        self.assertEqual(STATE_FAILED, payload["state"])
        self.assertIn("cuda out of memory", payload["error"])
        self.assertIn("error_detail", payload)
        # The home prefix is folded to ~ so /health never leaks a full local path.
        self.assertNotIn(os.path.expanduser("~"), payload["error"])
        self.assertIn("~/secret/path", payload["error"])

    def test_health_is_not_ready_while_loading(self):
        state = BackendState(ServerConfig())
        payload = state.health_payload()
        self.assertFalse(payload["ready"])
        self.assertEqual("loading", payload["status"])
        self.assertEqual(RUN_ENGINE, payload["run_engine"])

    def test_ready_health_payload_reports_engine_and_config(self):
        state = BackendState(ServerConfig(device="cuda"), loader=FakeShared)
        state.run_loader()
        payload = state.health_payload()
        self.assertTrue(payload["ready"])
        self.assertEqual("ok", payload["status"])
        self.assertEqual(RUN_ENGINE, payload["run_engine"])
        self.assertEqual(MODEL_ID, payload["model"])
        self.assertEqual("es-ES", payload["language"])
        self.assertEqual("cuda", payload["device"])
        self.assertEqual(NEMO_COMMIT, payload["nemo_commit"])
        self.assertEqual([56, 3], payload["effective_config"]["att_context_size"])
        self.assertEqual(RUN_ENGINE, payload["run_config"]["run_engine"])
        self.assertEqual(RUN_ENGINE, payload["provenance"]["run_engine"])

    def test_provenance_never_breaks_health(self):
        class BrokenProvenance(FakeShared):
            def provenance(self):
                raise RuntimeError("no gpu")

        state = BackendState(ServerConfig(), loader=BrokenProvenance)
        state.run_loader()
        payload = state.health_payload()
        self.assertTrue(payload["ready"])
        self.assertIn("provenance_error", payload["provenance"])

    def test_default_loader_uses_spoken_canary_before_ready(self):
        class LoadedModel(FakeShared):
            def __init__(self):
                super().__init__()
                self.warmup_audio = None

            def warmup(self, seconds=1.0, *, speech_audio=None):
                self.warmup_audio = speech_audio
                return {
                    "silence_warmup": True,
                    "speech_canary": True,
                    "events_emitted": 1,
                }

        loaded = LoadedModel()
        with tempfile.TemporaryDirectory() as root:
            canary = Path(root) / "canary.webm"
            canary.write_bytes(b"media")
            decoded = np.ones(TARGET_RATE * 20, dtype="float32")
            config = ServerConfig(
                warmup_audio_path=str(canary),
                warmup_speech_sec=2.0,
            )
            with (
                mock.patch("server.runtime.app.SharedNemotronModel", return_value=loaded),
                mock.patch(
                    "server.runtime.app.decode_audio_bytes_to_float32",
                    return_value=decoded,
                ),
            ):
                state = BackendState(config)
                state.run_loader()

        self.assertTrue(state.is_ready())
        self.assertEqual(TARGET_RATE * 2, loaded.warmup_audio.size)
        self.assertTrue(state.health_payload()["streaming_canary"]["speech_canary"])

    def test_missing_spoken_canary_fails_readiness(self):
        config = ServerConfig(warmup_audio_path="/definitely/missing/canary.webm")
        with mock.patch("server.runtime.app.SharedNemotronModel", return_value=FakeShared()):
            state = BackendState(config)
            state.run_loader()
        self.assertEqual(STATE_FAILED, state.status)
        self.assertIn("canary not found", state.health_payload()["error"])


class SessionManagerTests(unittest.TestCase):
    def test_only_one_session_at_a_time(self):
        manager = SingleSessionManager()
        self.assertTrue(manager.try_acquire())
        self.assertFalse(manager.try_acquire())
        self.assertTrue(manager.busy)
        manager.release()
        self.assertFalse(manager.busy)
        self.assertTrue(manager.try_acquire())


class SessionConfigTests(unittest.TestCase):
    def test_backend_config_overrides_apply(self):
        session_start = {"backend_config": {"latency_ms": 560, "stop_history_eou_ms": 400}}
        config = session_config_from_start(NemotronConfig(), session_start)
        self.assertEqual(560, config.latency_ms)
        self.assertEqual(400, config.stop_history_eou_ms)

    def test_locked_overrides_cannot_swap_the_loaded_model(self):
        session_start = {
            "backend_config": {"model_id": "nvidia/other-model", "decoder_type": "ctc", "latency_ms": 80}
        }
        config = session_config_from_start(NemotronConfig(), session_start)
        self.assertEqual(MODEL_ID, config.model_id)
        self.assertEqual("rnnt", config.decoder_type)
        self.assertEqual(80, config.latency_ms)  # unlocked keys still apply

    def test_locked_override_list_covers_model_identity(self):
        for key in ("model_id", "decoder_type", "compute_dtype", "device"):
            self.assertIn(key, LOCKED_OVERRIDES)

    def test_invalid_override_is_rejected(self):
        with self.assertRaises(ValueError):
            session_config_from_start(NemotronConfig(), {"backend_config": {"latency_ms": 250}})

    def test_session_ready_message_advertises_engine(self):
        message = session_ready_message({"version": 1, "sample_rate_hz": 48000}, NemotronConfig())
        self.assertEqual(RUN_ENGINE, message["run_engine"])
        self.assertEqual(48000, message["sample_rate_hz"])
        self.assertEqual(1, message["version"])
        self.assertEqual(RUN_ENGINE, message["run_config"]["run_engine"])
        self.assertEqual(320, message["run_config"]["config_latency_ms"])


class PrepareSharedModelTests(unittest.TestCase):
    def test_latency_change_is_pushed_to_the_shared_pipeline(self):
        shared = FakeShared(NemotronConfig(latency_ms=320))
        prepare_shared_model(shared, NemotronConfig(latency_ms=560))
        self.assertEqual(1, len(shared.configure_calls))
        self.assertEqual(560, shared.configure_calls[0].latency_ms)

    def test_same_config_is_still_verified_by_the_shared_pipeline(self):
        shared = FakeShared(NemotronConfig(latency_ms=320))
        prepare_shared_model(shared, NemotronConfig(latency_ms=320))
        self.assertEqual(1, len(shared.configure_calls))

    def test_endpointer_overrides_are_pushed_to_the_shared_pipeline(self):
        shared = FakeShared(NemotronConfig())
        requested = NemotronConfig(stop_history_eou_ms=400, residue_tokens_at_end=5)
        prepare_shared_model(shared, requested)
        self.assertEqual(400, shared.config.stop_history_eou_ms)
        self.assertEqual(5, shared.config.residue_tokens_at_end)


class ServerModuleContractTests(unittest.TestCase):
    def test_no_pep563_future_annotations(self):
        # PEP 563 stringizes annotations, which breaks FastAPI's resolution of the
        # locally-imported ``request: Request`` parameter and yields HTTP 422 on
        # /stt/offline. The server module must keep concrete annotations.
        import server.runtime.app as srv

        self.assertNotIn("annotations", vars(srv))
        stripped = [line.strip() for line in Path(srv.__file__).read_text(encoding="utf-8").splitlines()]
        self.assertNotIn("from __future__ import annotations", stripped)

    def test_module_imports_without_torch_nemo_or_fastapi(self):
        self.assertNotIn("torch", sys.modules)
        self.assertNotIn("nemo", sys.modules)
        self.assertNotIn("fastapi", sys.modules)


class OfflineHandlerTests(unittest.TestCase):
    def _run(self, transcribe_result, config=None):
        return run_offline_transcription(
            FakeShared(),
            b"webm-bytes",
            "clip.webm",
            config or NemotronConfig(),
            decode_fn=lambda _bytes: np.zeros(TARGET_RATE, dtype="float32"),
            transcribe_fn=lambda _shared, _audio, _config: transcribe_result,
        )

    def test_offline_returns_proxy_labelled_transcription(self):
        result = self._run({"text": "hola offline", "segments": []})
        self.assertEqual("clip.webm", result["filename"])
        self.assertEqual("hola offline", result["text"])
        self.assertEqual("offline_proxy", result["reference_kind"])
        self.assertEqual(RUN_ENGINE, result["run_engine"])
        self.assertEqual(NEMO_COMMIT, result["nemo_commit"])
        self.assertEqual("es-ES", result["detected_language"])
        self.assertEqual("es-ES", result["configured_language"])
        self.assertEqual(1.0, result["audio_duration_sec"])
        self.assertIn("inference_sec", result)
        self.assertFalse(result["config"]["config_realtime"])
        self.assertEqual("http_offline", result["config"]["config_transport"])

    def test_missing_timestamps_are_reported_not_fabricated(self):
        result = self._run({"text": "hola", "segments": []})
        self.assertEqual([], result["segments"])
        self.assertEqual("unavailable", result["segments_source"])

    def test_real_timestamps_are_labelled_with_their_origin(self):
        segments = [{"start_sec": 0.0, "end_sec": 1.0, "text": "hola"}]
        result = self._run({"text": "hola", "segments": segments})
        self.assertEqual(segments, result["segments"])
        self.assertEqual("nemo_hypothesis_timestamps", result["segments_source"])

    def test_offline_config_is_reported_for_the_cache_signature(self):
        result = self._run({"text": "hola", "segments": []}, NemotronConfig(latency_ms=560))
        config = result["config"]
        self.assertEqual(RUN_ENGINE, config["run_engine"])
        self.assertEqual(MODEL_ID, config["config_model"])
        self.assertEqual(NEMO_COMMIT, config["nemo_commit"])
        self.assertEqual("es-ES", config["config_target_lang"])
        self.assertEqual("rnnt", config["config_decoder_type"])
        self.assertEqual(560, config["config_latency_ms"])
        self.assertEqual([56, 6], config["config_att_context_size"])
        self.assertTrue(config["config_strip_lang_tags"])

    def test_empty_upload_is_rejected_before_ffmpeg(self):
        with self.assertRaises(ValueError):
            decode_audio_bytes_to_float32(b"")


class LiveSessionFlowTests(unittest.TestCase):
    """End-to-end mapping through the real session, with a scripted fake engine."""

    def test_session_produces_seq_zero_partials_then_eou_final_then_summary(self):
        from collections import deque

        from server.runtime.nemotron import NemotronSession

        class ScriptedEngine:
            frame_samples = 1600

            def __init__(self):
                self.scripted = deque(
                    [
                        [{"partial_transcript": "<es-ES> hola", "final_transcript": ""}],
                        [{"partial_transcript": "hola mundo", "final_transcript": ""}],
                        [{"partial_transcript": "", "final_transcript": "hola mundo"}],
                    ]
                )

            def open(self):
                pass

            def close(self):
                pass

            def step(self, samples, *, is_first, is_last, valid_length):
                return self.scripted.popleft() if self.scripted else []

        session = NemotronSession(ScriptedEngine(), NemotronConfig(), source_rate=48000)
        events = []
        # 4800 samples @48k -> 1600 @16k -> exactly one frame per push.
        for _ in range(3):
            events.extend(session.push_pcm(np.zeros(4800, dtype="<i2").tobytes()))
        events.extend(session.flush())

        self.assertEqual([0, 1, 2], [event["seq"] for event in events])
        self.assertEqual([False, False, True], [event["is_final"] for event in events])
        self.assertEqual("hola", events[0]["text"])
        self.assertEqual("hola mundo", events[2]["text"])
        self.assertTrue(events[2]["eou"])
        for event in events:
            self.assertIsInstance(event["start_sec"], float)
            self.assertIsInstance(event["end_sec"], float)

        summary = session.stats_snapshot()
        self.assertEqual(1, summary["eou_count"])
        self.assertEqual(1, summary["finals_emitted"])
        self.assertEqual(0, summary["flush_finals"])
        self.assertEqual(3, summary["chunks_received"])
        self.assertEqual(4, summary["streaming_steps"])  # 3 audio frames + the padded last frame
        self.assertEqual(RUN_ENGINE, events[0]["run_engine"])

    def test_flush_emits_the_pending_line_when_no_eou_fired(self):
        from collections import deque

        from server.runtime.nemotron import NemotronSession

        class ScriptedEngine:
            frame_samples = 1600

            def __init__(self):
                self.scripted = deque([[{"partial_transcript": "sin cierre", "final_transcript": ""}]])

            def open(self):
                pass

            def close(self):
                pass

            def step(self, samples, *, is_first, is_last, valid_length):
                return self.scripted.popleft() if self.scripted else []

        session = NemotronSession(ScriptedEngine(), NemotronConfig(), source_rate=TARGET_RATE)
        session.push_float32(np.zeros(1600, dtype="float32"))
        flushed = session.flush()
        self.assertEqual(1, len(flushed))
        self.assertTrue(flushed[0]["is_final"])
        self.assertTrue(flushed[0]["forced_flush"])
        self.assertEqual("sin cierre", flushed[0]["text"])
        self.assertEqual(1, session.stats_snapshot()["flush_finals"])


if __name__ == "__main__":
    unittest.main()
