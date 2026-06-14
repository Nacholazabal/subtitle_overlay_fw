import json
import queue
import socket
import threading
import time
import unittest

from scripts.audio_receiver import FakeSTTEngine, TcpSubtitleSink


class AudioReceiverTests(unittest.TestCase):
    def test_fake_stt_generates_partial_and_final_events(self):
        engine = FakeSTTEngine(chunk_word_interval=1, words_per_final=2, words=("hola",), seed=1)

        partial = engine.process_chunk(seq=0, timestamp_ns=1_000_000_000, chunk_ms=20, dropped=0)
        final = engine.process_chunk(seq=1, timestamp_ns=1_020_000_000, chunk_ms=20, dropped=0)

        self.assertIsNotNone(partial)
        self.assertEqual("partial", partial["type"])
        self.assertFalse(partial["is_final"])
        self.assertEqual("hola", partial["text"])
        self.assertIsNotNone(final)
        self.assertEqual("final", final["type"])
        self.assertTrue(final["is_final"])
        self.assertEqual("hola hola", final["text"])

    def test_tcp_subtitle_sink_serializes_ndjson_on_worker(self):
        received = []
        ready = threading.Event()

        def server_main(server):
            server.listen(1)
            ready.set()
            conn, _addr = server.accept()
            with conn:
                received.append(conn.recv(4096))

        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as server:
            server.bind(("127.0.0.1", 0))
            host, port = server.getsockname()
            thread = threading.Thread(target=server_main, args=(server,), daemon=True)
            thread.start()
            self.assertTrue(ready.wait(timeout=1.0))

            sink = TcpSubtitleSink(host, port, max_queue=4)
            try:
                sink.handle_event(
                    {
                        "seq": 1,
                        "is_final": True,
                        "type": "final",
                        "start_sec": 0.0,
                        "end_sec": 1.0,
                        "text": "hola",
                    }
                )

                deadline = time.monotonic() + 2.0
                while not received and time.monotonic() < deadline:
                    time.sleep(0.01)
            finally:
                sink.close()

        self.assertTrue(received)
        decoded = json.loads(received[0].decode("utf-8").strip())
        self.assertEqual("hola", decoded["text"])
        self.assertTrue(decoded["is_final"])

    def test_tcp_subtitle_sink_preserves_final_by_dropping_queued_partial(self):
        sink = TcpSubtitleSink.__new__(TcpSubtitleSink)
        sink.events = queue.Queue(maxsize=1)

        sink.handle_event({"seq": 1, "is_final": False, "type": "partial", "text": "a"})
        sink.handle_event({"seq": 2, "is_final": True, "type": "final", "text": "b"})

        queued = sink.events.get_nowait()
        self.assertEqual(2, queued["seq"])
        self.assertTrue(queued["is_final"])


if __name__ == "__main__":
    unittest.main()
