import json
import queue
import socket
import sys
import threading
import time
import unittest
from pathlib import Path


sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from scripts.stt_receiver import ChunkTranscriber, PartialStabilityFilter, TcpTranscriptSink


class PartialStabilityFilterTests(unittest.TestCase):
    def test_agreement_two_suppresses_first_partial(self):
        filt = PartialStabilityFilter(agreement=2)

        events = filt.handle_event({"seq": 1, "is_final": False, "text": "hola com"})

        self.assertEqual([], events)

    def test_agreement_two_emits_common_word_prefix(self):
        filt = PartialStabilityFilter(agreement=2)

        filt.handle_event({"seq": 1, "is_final": False, "text": "hola com"})
        events = filt.handle_event({"seq": 2, "is_final": False, "text": "hola como estas"})

        self.assertEqual(1, len(events))
        self.assertEqual("hola", events[0]["text"])
        self.assertEqual(2, events[0]["seq"])

    def test_agreement_two_suppresses_duplicate_stable_prefix(self):
        filt = PartialStabilityFilter(agreement=2)

        filt.handle_event({"seq": 1, "is_final": False, "text": "hola com"})
        filt.handle_event({"seq": 2, "is_final": False, "text": "hola como"})
        events = filt.handle_event({"seq": 3, "is_final": False, "text": "hola compra"})

        self.assertEqual([], events)

    def test_agreement_ignores_case_and_terminal_punctuation(self):
        filt = PartialStabilityFilter(agreement=2)

        filt.handle_event({"seq": 1, "is_final": False, "text": "Hola, mundo"})
        events = filt.handle_event({"seq": 2, "is_final": False, "text": "hola mundo cruel"})

        self.assertEqual(1, len(events))
        self.assertEqual("Hola, mundo", events[0]["text"])

    def test_duplicate_partial_is_suppressed_after_normalization(self):
        filt = PartialStabilityFilter(agreement=2)

        filt.handle_event({"seq": 1, "is_final": False, "text": "Gracias a Dios"})
        emitted = filt.handle_event({"seq": 2, "is_final": False, "text": "gracias a Dios."})
        duplicate = filt.handle_event({"seq": 3, "is_final": False, "text": "Gracias a Dios."})

        self.assertEqual(1, len(emitted))
        self.assertEqual([], duplicate)

    def test_final_resets_partial_history_and_passes_through(self):
        filt = PartialStabilityFilter(agreement=2)

        filt.handle_event({"seq": 1, "is_final": False, "text": "hola com"})
        final = {"seq": 2, "is_final": True, "text": "hola como estas"}
        final_events = filt.handle_event(final)
        next_partial_events = filt.handle_event({"seq": 3, "is_final": False, "text": "otra"})

        self.assertEqual([final], final_events)
        self.assertEqual([], next_partial_events)


class TcpTranscriptSinkTests(unittest.TestCase):
    @staticmethod
    def queue_only_sink(maxsize=1):
        sink = TcpTranscriptSink.__new__(TcpTranscriptSink)
        sink.events = queue.Queue(maxsize=maxsize)
        sink.stats_lock = threading.Lock()
        sink.stats = {
            "generated": 0,
            "first_generated_wall_sec": None,
            "sink_dropped_partials": 0,
            "sink_dropped_finals": 0,
        }
        return sink

    @staticmethod
    def start_board_server(handler):
        listener = socket.socket(socket.AF_INET, socket.SOCK_STREAM)
        listener.bind(("127.0.0.1", 0))
        listener.listen(1)
        port = listener.getsockname()[1]

        def server_main():
            try:
                client, _address = listener.accept()
                with client:
                    handler(client)
            finally:
                listener.close()

        thread = threading.Thread(target=server_main, daemon=True)
        thread.start()
        return port, thread

    def test_preserves_final_by_dropping_queued_partial(self):
        sink = self.queue_only_sink()

        sink.handle_event({"seq": 1, "is_final": False, "text": "parcial"})
        sink.handle_event({"seq": 2, "is_final": True, "text": "final"})

        queued = sink.events.get_nowait()
        self.assertEqual(2, queued["seq"])
        self.assertTrue(queued["is_final"])

    def test_handshake_and_ack_are_correlated(self):
        def handler(client):
            client.sendall(b'{"type":"session_ready","version":1,"session_id":4}\n')
            event = json.loads(client.makefile("r", encoding="utf-8").readline())
            response = {
                "type": "transcript_ack",
                "version": 1,
                "session_id": 4,
                "seq": event["seq"],
                "status": "accepted",
            }
            client.sendall((json.dumps(response) + "\n").encode())

        port, server = self.start_board_server(handler)
        sink = TcpTranscriptSink("127.0.0.1", port, socket_timeout=0.5)
        try:
            self.assertTrue(sink.wait_ready(1.0))
            sink.handle_event(
                {
                    "seq": 9,
                    "is_final": True,
                    "start_sec": 0.0,
                    "end_sec": 1.0,
                    "text": "hola",
                }
            )
            self.assertTrue(sink.flush(1.0))
            snapshot = sink.stats_snapshot()
            self.assertEqual(4, snapshot["session_id"])
            self.assertEqual(1, snapshot["accepted"])
            self.assertEqual(0, snapshot["delivery_unknown"])
        finally:
            sink.close()
            server.join(timeout=1.0)

    def test_invalid_handshake_version_never_becomes_ready(self):
        def handler(client):
            client.sendall(b'{"type":"session_ready","version":2,"session_id":1}\n')

        port, server = self.start_board_server(handler)
        sink = TcpTranscriptSink("127.0.0.1", port, socket_timeout=0.2)
        try:
            self.assertFalse(sink.wait_ready(0.4))
            self.assertGreaterEqual(sink.stats_snapshot()["protocol_errors"], 1)
        finally:
            sink.close()
            server.join(timeout=1.0)

    def test_missing_ack_is_unknown_and_event_is_not_retried(self):
        received = []

        def handler(client):
            client.sendall(b'{"type":"session_ready","version":1,"session_id":5}\n')
            received.append(client.makefile("r", encoding="utf-8").readline())

        port, server = self.start_board_server(handler)
        sink = TcpTranscriptSink("127.0.0.1", port, socket_timeout=0.2)
        try:
            self.assertTrue(sink.wait_ready(1.0))
            sink.handle_event(
                {
                    "seq": 3,
                    "is_final": False,
                    "start_sec": 0.0,
                    "end_sec": 0.5,
                    "text": "hola",
                }
            )
            self.assertTrue(sink.flush(1.0))
            self.assertEqual(1, sink.stats_snapshot()["delivery_unknown"])
            time.sleep(0.1)
            self.assertEqual(1, len(received))
        finally:
            sink.close()
            server.join(timeout=1.0)

    def test_ack_with_wrong_session_is_delivery_unknown(self):
        def handler(client):
            client.sendall(b'{"type":"session_ready","version":1,"session_id":8}\n')
            event = json.loads(client.makefile("r", encoding="utf-8").readline())
            client.sendall(
                (
                    json.dumps(
                        {
                            "type": "transcript_ack",
                            "version": 1,
                            "session_id": 9,
                            "seq": event["seq"],
                            "status": "accepted",
                        }
                    )
                    + "\n"
                ).encode()
            )

        port, server = self.start_board_server(handler)
        sink = TcpTranscriptSink("127.0.0.1", port, socket_timeout=0.2)
        try:
            self.assertTrue(sink.wait_ready(1.0))
            sink.handle_event(
                {
                    "seq": 4,
                    "is_final": True,
                    "start_sec": 0.0,
                    "end_sec": 1.0,
                    "text": "hola",
                }
            )
            self.assertTrue(sink.flush(1.0))
            snapshot = sink.stats_snapshot()
            self.assertEqual(1, snapshot["delivery_unknown"])
            self.assertEqual(0, snapshot["accepted"])
        finally:
            sink.close()
            server.join(timeout=1.0)

    def test_wire_event_strips_analysis_only_fields(self):
        event = {
            "seq": 3,
            "is_final": False,
            "start_sec": 1.0,
            "end_sec": 2.0,
            "text": "hola",
            "infer_sec": 0.123,
            "config_max_window_sec": 4.0,
        }

        wire = TcpTranscriptSink._wire_event(event)

        self.assertEqual(
            {
                "seq": 3,
                "is_final": False,
                "start_sec": 1.0,
                "end_sec": 2.0,
                "text": "hola",
            },
            wire,
        )


class ChunkTranscriberQueuePolicyTests(unittest.TestCase):
    def make_transcriber(self, maxsize=4):
        transcriber = ChunkTranscriber.__new__(ChunkTranscriber)
        transcriber.pending_chunks = queue.Queue(maxsize=maxsize)
        transcriber.drop_oldest = True
        transcriber.realtime = False
        transcriber.target_rate = 16000
        transcriber._audio_start_monotonic = None
        transcriber._job_seq = 0
        transcriber._dropped_jobs = 0
        transcriber._queue_lock = threading.Lock()
        transcriber._partial_jobs_outstanding = 0
        return transcriber

    def queue_job(self, transcriber, is_final, start=0, end=16000):
        transcriber._queue_job(
            chunk=object(),
            start_sample=start,
            end_sample=end,
            is_final=is_final,
            reason="max_window" if is_final else "partial_tick",
            trailing_silence_samples=0,
        )

    def test_drops_new_partial_when_partial_is_already_pending(self):
        transcriber = self.make_transcriber()

        self.queue_job(transcriber, is_final=False)
        self.queue_job(transcriber, is_final=False, end=32000)

        self.assertEqual(1, transcriber.pending_chunks.qsize())
        self.assertEqual(1, transcriber._partial_jobs_outstanding)
        self.assertEqual(1, transcriber._dropped_jobs)

    def test_final_purges_pending_partial_and_is_preserved(self):
        transcriber = self.make_transcriber()

        self.queue_job(transcriber, is_final=False)
        self.queue_job(transcriber, is_final=True)

        self.assertEqual(1, transcriber.pending_chunks.qsize())
        queued = transcriber.pending_chunks.get_nowait()
        self.assertTrue(queued["is_final"])
        self.assertEqual(0, transcriber._partial_jobs_outstanding)
        self.assertEqual(1, transcriber._dropped_jobs)

    def test_finished_partial_allows_later_partial(self):
        transcriber = self.make_transcriber()

        self.queue_job(transcriber, is_final=False)
        queued = transcriber.pending_chunks.get_nowait()
        transcriber._finish_job(queued)
        transcriber.pending_chunks.task_done()
        self.queue_job(transcriber, is_final=False, start=16000, end=32000)

        self.assertEqual(1, transcriber.pending_chunks.qsize())
        self.assertEqual(1, transcriber._partial_jobs_outstanding)
        self.assertEqual(0, transcriber._dropped_jobs)

    def test_queue_job_preserves_vad_metrics(self):
        transcriber = self.make_transcriber()
        vad_metrics = {
            "vad_segment_count": 2,
            "vad_speech_ratio": 0.75,
            "tail_rms_dbfs": -42.0,
        }

        transcriber._queue_job(
            chunk=object(),
            start_sample=0,
            end_sample=16000,
            is_final=True,
            reason="silence",
            trailing_silence_samples=8000,
            vad_metrics=vad_metrics,
        )
        vad_metrics["vad_speech_ratio"] = 0.1

        queued = transcriber.pending_chunks.get_nowait()
        self.assertEqual(
            {
                "vad_segment_count": 2,
                "vad_speech_ratio": 0.75,
                "tail_rms_dbfs": -42.0,
            },
            queued["vad_metrics"],
        )

    def test_annotate_event_adds_vad_metrics(self):
        transcriber = self.make_transcriber()
        transcriber.run_config = {}
        job = {
            "reason": "silence",
            "trailing_silence_samples": 8000,
            "audio_end_monotonic": None,
            "job_id": 7,
            "vad_metrics": {
                "vad_segment_count": 1,
                "vad_speech_ratio": 0.62,
                "vad_trailing_silence_sec": 0.5,
                "window_rms_dbfs": -21.0,
            },
        }
        event = {"start_sec": 1.0, "end_sec": 2.0}

        transcriber._annotate_event(event, job, queue_wait=0.1, infer_sec=0.2, emitted_at=0.0)

        self.assertEqual(1, event["vad_segment_count"])
        self.assertEqual(0.62, event["vad_speech_ratio"])
        self.assertEqual(0.5, event["vad_trailing_silence_sec"])
        self.assertEqual(-21.0, event["window_rms_dbfs"])


if __name__ == "__main__":
    unittest.main()
