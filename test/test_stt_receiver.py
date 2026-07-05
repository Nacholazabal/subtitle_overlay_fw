import queue
import sys
import threading
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
    def test_preserves_final_by_dropping_queued_partial(self):
        sink = TcpTranscriptSink.__new__(TcpTranscriptSink)
        sink.events = queue.Queue(maxsize=1)

        sink.handle_event({"seq": 1, "is_final": False, "text": "parcial"})
        sink.handle_event({"seq": 2, "is_final": True, "text": "final"})

        queued = sink.events.get_nowait()
        self.assertEqual(2, queued["seq"])
        self.assertTrue(queued["is_final"])

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


if __name__ == "__main__":
    unittest.main()
