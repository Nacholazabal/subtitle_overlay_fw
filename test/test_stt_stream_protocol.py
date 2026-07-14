import unittest
import sys
from pathlib import Path


sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from scripts.stt_stream_bridge import BridgeTranscriptSink
from scripts.stt_stream_protocol import (
    AudioAvailabilityTimeline,
    FORMAT_S16_LE,
    MESSAGE_SESSION_START,
    PROTOCOL_VERSION,
    decode_audio_frame,
    encode_audio_frame,
    make_session_start,
    validate_session_start,
)


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

    def test_audio_timeline_maps_to_first_available_chunk(self):
        timeline = AudioAvailabilityTimeline()

        timeline.mark_available(0.5, 100.0)
        timeline.mark_available(1.0, 100.5)

        self.assertEqual(100.0, timeline.available_at(0.25))
        self.assertEqual(100.5, timeline.available_at(0.75))
        self.assertEqual(100.5, timeline.available_at(1.5))


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


if __name__ == "__main__":
    unittest.main()
