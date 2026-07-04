import queue
import sys
import unittest
from pathlib import Path


sys.path.insert(0, str(Path(__file__).resolve().parents[1]))

from scripts.stt_receiver import PartialStabilityFilter, TcpTranscriptSink


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


if __name__ == "__main__":
    unittest.main()
