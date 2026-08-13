import base64
import unittest
import sys
import tempfile
from pathlib import Path
from types import SimpleNamespace
from unittest import mock


sys.path.insert(0, str(Path(__file__).resolve().parents[2]))

from server.runtime.bridge import (
    BridgeTranscriptSink,
    StreamingBridge,
    board_drop_delta,
    decode_backend_config_base64,
    parse_args,
)
from server.runtime.protocol import (
    AudioAvailabilityTimeline,
    FORMAT_S16_LE,
    MESSAGE_SESSION_START,
    PROTOCOL_VERSION,
    decode_audio_frame,
    encode_audio_frame,
    make_session_start,
    validate_backend_config,
    validate_session_start,
)
from server.runtime.bridge import parse_backend_config


class SttStreamProtocolTests(unittest.TestCase):
    def test_audio_frame_round_trips_header_and_payload(self):
        payload = b"\x01\x02\x03\x04"

        encoded = encode_audio_frame(seq=7, timestamp_ns=123456789, dropped=2, payload=payload)
        decoded = decode_audio_frame(encoded)

        self.assertEqual(7, decoded.seq)
        self.assertEqual(123456789, decoded.timestamp_ns)
        self.assertEqual(2, decoded.dropped)
        self.assertEqual(payload, decoded.payload)

    def test_session_start_validates_supported_format(self):
        message = make_session_start(
            sample_rate_hz=48000,
            channels=1,
            fmt=FORMAT_S16_LE,
            chunk_ms=20,
            samples_per_chunk=960,
            bytes_per_chunk=1920,
        )

        validate_session_start(message)

        self.assertEqual(MESSAGE_SESSION_START, message["type"])
        self.assertEqual(PROTOCOL_VERSION, message["version"])

    def test_session_start_rejects_stereo(self):
        message = make_session_start(
            sample_rate_hz=48000,
            channels=2,
            fmt=FORMAT_S16_LE,
            chunk_ms=20,
            samples_per_chunk=960,
            bytes_per_chunk=3840,
        )

        with self.assertRaises(ValueError):
            validate_session_start(message)

    def test_session_start_round_trips_backend_config(self):
        message = make_session_start(
            sample_rate_hz=48000,
            channels=1,
            fmt=FORMAT_S16_LE,
            chunk_ms=20,
            samples_per_chunk=960,
            bytes_per_chunk=1920,
            backend_config={"latency_ms": 560, "stop_history_eou_ms": 600},
        )

        overrides = validate_session_start(message)

        self.assertEqual({"latency_ms": 560, "stop_history_eou_ms": 600}, overrides)

    def test_session_start_rejects_non_scalar_backend_config(self):
        base = dict(
            sample_rate_hz=48000,
            channels=1,
            fmt=FORMAT_S16_LE,
            chunk_ms=20,
            samples_per_chunk=960,
            bytes_per_chunk=1920,
        )

        with self.assertRaises(ValueError):
            make_session_start(**base, backend_config={"nested": {"bad": True}})

    def test_audio_timeline_maps_to_first_available_chunk(self):
        timeline = AudioAvailabilityTimeline()

        timeline.mark_available(0.5, 100.0)
        timeline.mark_available(1.0, 100.5)

        self.assertEqual(100.0, timeline.available_at(0.25))
        self.assertEqual(100.5, timeline.available_at(0.75))
        self.assertEqual(100.5, timeline.available_at(1.5))


class BackendConfigTests(unittest.TestCase):
    def test_session_start_carries_generic_backend_config(self):
        message = make_session_start(
            sample_rate_hz=48000,
            channels=1,
            fmt=FORMAT_S16_LE,
            chunk_ms=20,
            samples_per_chunk=960,
            bytes_per_chunk=1920,
            backend_config={"frame_threshold": 12, "use_vac": True, "cif_ckpt_path": ""},
        )

        self.assertEqual(
            {"frame_threshold": 12, "use_vac": True, "cif_ckpt_path": ""},
            message["backend_config"],
        )
        self.assertEqual("", message["backend_config"]["cif_ckpt_path"])

    def test_backend_config_must_be_object_of_scalars(self):
        with self.assertRaises(ValueError):
            validate_backend_config([1, 2, 3])
        with self.assertRaises(ValueError):
            validate_backend_config({"nested": {"bad": 1}})
        with self.assertRaises(ValueError):
            validate_backend_config({"": 1})

    def test_bridge_parses_backend_config_json_string(self):
        self.assertEqual({"beams": 1}, parse_backend_config('{"beams": 1}'))
        self.assertIsNone(parse_backend_config(None))
        self.assertIsNone(parse_backend_config(""))

    def test_bridge_decodes_backend_config_transport_losslessly(self):
        raw = '{"latency_ms":320,"target_lang":"es-ES"}'
        encoded = base64.urlsafe_b64encode(raw.encode("utf-8")).decode("ascii")

        self.assertEqual(raw, decode_backend_config_base64(encoded))
        with self.assertRaisesRegex(ValueError, "Base64"):
            decode_backend_config_base64("%%%")

    def test_cli_normalizes_base64_transport_back_to_json(self):
        raw = '{"latency_ms":80,"target_lang":"es-ES"}'
        encoded = base64.urlsafe_b64encode(raw.encode("utf-8")).decode("ascii")
        argv = [
            "stt_stream_bridge.py",
            "--stream-url",
            "ws://example.test/stt/stream",
            "--backend-config-base64",
            encoded,
        ]

        with mock.patch.object(sys, "argv", argv):
            args = parse_args()

        self.assertEqual(raw, args.backend_config_json)


class RecordingSink:
    def __init__(self):
        self.events = []

    def handle_event(self, event):
        self.events.append(event)

    def close(self):
        pass


class BridgeTranscriptSinkTests(unittest.TestCase):
    def test_strips_protocol_type_before_forwarding(self):
        sink = RecordingSink()
        timeline = AudioAvailabilityTimeline()
        timeline.mark_available(1.0, 100.0)
        bridge = BridgeTranscriptSink(sink, timeline=timeline)

        bridge.handle_event(
            {
                "type": "transcript",
                "seq": 1,
                "is_final": False,
                "start_sec": 0.0,
                "end_sec": 1.0,
                "text": "hola",
            }
        )

        self.assertEqual(1, len(sink.events))
        self.assertNotIn("type", sink.events[0])
        self.assertEqual("hola", sink.events[0]["text"])
        self.assertIn("bridge_receive_lag_sec", sink.events[0])


class StreamingBridgeLifecycleTests(unittest.IsolatedAsyncioTestCase):
    async def test_stop_marker_before_board_writes_done_file(self):
        with tempfile.TemporaryDirectory() as temporary:
            root = Path(temporary)
            stop_file = root / "stop"
            done_file = root / "done.json"
            stop_file.touch()
            args = SimpleNamespace(
                jsonl=None,
                send_subtitles=False,
                subtitle_host="127.0.0.1",
                subtitle_port=5001,
                stop_file=str(stop_file),
                done_file=str(done_file),
            )
            bridge = StreamingBridge(args)

            await bridge._watch_stop_file()

            self.assertTrue(bridge.session_done.is_set())
            self.assertTrue(done_file.exists())

    async def test_subtitle_readiness_timeout_blocks_bridge_readiness(self):
        class NeverReadySink:
            def wait_ready(self, timeout):
                self.timeout = timeout
                return False

        args = SimpleNamespace(
            jsonl=None,
            send_subtitles=False,
            subtitle_host="127.0.0.1",
            subtitle_port=5001,
            subtitle_ready_timeout=0.01,
            stop_file=None,
            done_file=None,
        )
        bridge = StreamingBridge(args)
        bridge.subtitle_sink = NeverReadySink()

        with self.assertRaisesRegex(RuntimeError, "handshake timed out"):
            await bridge._wait_for_subtitle_ready()


class BridgeTracingTests(unittest.TestCase):
    def test_bridge_only_counts_board_drops_after_session_start(self):
        self.assertEqual(0, board_drop_delta(769, 769))
        self.assertEqual(3, board_drop_delta(769, 772))

if __name__ == "__main__":
    unittest.main()
