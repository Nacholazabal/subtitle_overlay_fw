import sys
import unittest
from pathlib import Path

import numpy as np


sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from scripts.stt_simulstreaming_backend import RUN_ENGINE, SimulStreamingConfig
from scripts.stt_simulstreaming_server import (
    STATE_FAILED,
    STATE_READY,
    BackendState,
    ServerConfig,
    SingleSessionManager,
    decode_audio_bytes_to_float32,
    run_offline_transcription,
    session_config_from_start,
    session_ready_message,
)


class FakeOnline:
    def __init__(self):
        self.status = "voice"

    def insert_audio_chunk(self, audio):
        pass

    def process_iter(self):
        return {}

    def finish(self):
        return {"text": "hola offline", "start": 0.0, "end": 1.0, "is_final": True}


class FakeShared:
    def __init__(self):
        self.config = SimulStreamingConfig()

    def build_online(self):
        return FakeOnline()


class BackendStateTests(unittest.TestCase):
    def test_loader_success_transitions_to_ready(self):
        sentinel = FakeShared()
        state = BackendState(ServerConfig(), loader=lambda: sentinel)
        self.assertFalse(state.is_ready())
        state.run_loader()
        self.assertEqual(STATE_READY, state.status)
        self.assertIs(sentinel, state.shared_model)

    def test_loader_failure_records_sanitized_error(self):
        def boom():
            raise RuntimeError("cuda out of memory at /home/secret/path")

        state = BackendState(ServerConfig(), loader=boom)
        state.run_loader()
        self.assertEqual(STATE_FAILED, state.status)
        payload = state.health_payload()
        self.assertFalse(payload["ready"])
        self.assertIn("cuda out of memory", payload["error"])
        self.assertIn("error_detail", payload)

    def test_ready_health_payload_reports_engine_and_provenance(self):
        state = BackendState(ServerConfig(device="cuda"), loader=FakeShared)
        state.run_loader()
        payload = state.health_payload()
        self.assertTrue(payload["ready"])
        self.assertEqual("ok", payload["status"])
        self.assertEqual(RUN_ENGINE, payload["run_engine"])
        self.assertEqual("small", payload["model"])
        self.assertEqual("cuda", payload["device"])
        self.assertIn("effective_config", payload)
        self.assertIn("model_sha256", payload)


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
    def test_backend_config_overrides_apply_but_model_cannot_be_swapped(self):
        base = SimulStreamingConfig()
        session_start = {"backend_config": {"frame_threshold": 12, "never_fire": True, "model": "large"}}
        config = session_config_from_start(base, session_start)
        self.assertEqual(12, config.frame_threshold)
        self.assertTrue(config.never_fire)
        self.assertEqual("small", config.model)  # model swap ignored

    def test_session_ready_message_advertises_engine(self):
        message = session_ready_message({"version": 1, "sample_rate_hz": 48000}, SimulStreamingConfig())
        self.assertEqual(RUN_ENGINE, message["run_engine"])
        self.assertEqual(48000, message["sample_rate_hz"])
        self.assertEqual(RUN_ENGINE, message["run_config"]["run_engine"])


class ServerModuleContractTests(unittest.TestCase):
    def test_no_pep563_future_annotations(self):
        # PEP 563 stringizes annotations, which breaks FastAPI's resolution of the
        # locally-imported ``request: Request`` parameter and yields HTTP 422 on
        # /stt/offline. The server module must keep concrete annotations.
        import scripts.stt_simulstreaming_server as srv

        # A real `from __future__ import annotations` statement binds the name
        # `annotations` into the module globals; the docstring merely mentions it.
        self.assertNotIn("annotations", vars(srv))
        stripped = [line.strip() for line in Path(srv.__file__).read_text(encoding="utf-8").splitlines()]
        self.assertNotIn("from __future__ import annotations", stripped)


class OfflineHandlerTests(unittest.TestCase):
    def test_offline_returns_proxy_labelled_transcription(self):
        result = run_offline_transcription(
            FakeShared(),
            b"webm-bytes",
            "clip.webm",
            SimulStreamingConfig(),
            decode_fn=lambda _bytes: np.zeros(16000, dtype="float32"),
        )
        self.assertEqual("hola offline", result["text"])
        self.assertEqual("offline_proxy", result["reference_kind"])
        self.assertEqual(RUN_ENGINE, result["run_engine"])
        self.assertEqual(False, result["config"]["config_realtime"])
        self.assertEqual("http_offline", result["config"]["config_transport"])

    def test_empty_upload_is_rejected_before_ffmpeg(self):
        with self.assertRaises(ValueError):
            decode_audio_bytes_to_float32(b"")


if __name__ == "__main__":
    unittest.main()
