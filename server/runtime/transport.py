#!/usr/bin/env python3
"""Board audio framing and acknowledged firmware subtitle transport."""

import json
import queue
import socket
import threading
import time


STREAM_MAGIC = b"SAUDPCM\\x00"
STREAM_HEADER = "!6I"
CHUNK_HEADER = "!QQ5I"
FORMAT_S16_LE = 1
EXPECTED_CHANNELS = 1
EXPECTED_SAMPLE_WIDTH = 2


class TranscriptSink:
    def handle_event(self, event):
        raise NotImplementedError

    def close(self):
        pass


class ConsoleTranscriptSink(TranscriptSink):
    def handle_event(self, event):
        final_mark = "final" if event["is_final"] else "partial"
        print(
            f"stt {final_mark}#{event['seq']} "
            f"t={event['start_sec']:.3f}..{event['end_sec']:.3f}s "
            f"text={event['text']!r}",
            flush=True,
        )


class JsonlTranscriptSink(TranscriptSink):
    def __init__(self, path):
        self.file = open(path, "w", encoding="utf-8")

    def handle_event(self, event):
        self.file.write(json.dumps(event, ensure_ascii=False) + "\n")
        self.file.flush()

    def close(self):
        self.file.close()


class TcpTranscriptSink(TranscriptSink):
    PROTOCOL_VERSION = 1
    RESPONSE_MAX_BYTES = 512

    def __init__(self, host, port, max_queue=32, ack_jsonl=None, socket_timeout=2.0):
        self.host = host
        self.port = port
        self.socket_timeout = socket_timeout
        self.sock = None
        self.events = queue.Queue(maxsize=max_queue)
        self.stop_event = threading.Event()
        self.ready_event = threading.Event()
        self.stats_lock = threading.Lock()
        self.ack_lock = threading.Lock()
        self.ack_file = None
        if ack_jsonl:
            self.ack_file = open(ack_jsonl, "w", encoding="utf-8")
        self.stats = {
            "handshake_ok": False,
            "protocol_version": None,
            "session_id": None,
            "connections": 0,
            "generated": 0,
            "sent": 0,
            "accepted": 0,
            "rejected": 0,
            "delivery_unknown": 0,
            "sink_dropped_partials": 0,
            "sink_dropped_finals": 0,
            "protocol_errors": 0,
            "ack_latency_sec": [],
            "first_generated_wall_sec": None,
            "first_ack_wall_sec": None,
        }
        self.worker = threading.Thread(target=self._worker_main, daemon=True)
        self.worker.start()

    def handle_event(self, event):
        queued_event = dict(event)
        generated_wall = time.time()
        queued_event["_sink_generated_wall_sec"] = generated_wall
        with self.stats_lock:
            self.stats["generated"] += 1
            if self.stats["first_generated_wall_sec"] is None:
                self.stats["first_generated_wall_sec"] = generated_wall
        try:
            self.events.put_nowait(queued_event)
            return
        except queue.Full:
            pass

        if not queued_event.get("is_final", False):
            with self.stats_lock:
                self.stats["sink_dropped_partials"] += 1
            self._record_sink_drop(queued_event, "sink_dropped_partial")
            print("subtitle TCP queue full: dropping partial transcript", flush=True)
            return

        if self._drop_one_partial_from_queue():
            try:
                self.events.put_nowait(queued_event)
                return
            except queue.Full:
                pass

        with self.stats_lock:
            self.stats["sink_dropped_finals"] += 1
        self._record_sink_drop(queued_event, "sink_dropped_final")
        print("subtitle TCP queue full: dropping final transcript", flush=True)

    def _drop_one_partial_from_queue(self):
        kept = []
        dropped = False

        while True:
            try:
                queued = self.events.get_nowait()
            except queue.Empty:
                break
            self.events.task_done()

            if (not dropped) and (not queued.get("is_final", False)):
                dropped = True
                with self.stats_lock:
                    self.stats["sink_dropped_partials"] += 1
                self._record_sink_drop(queued, "sink_dropped_partial")
                continue

            kept.append(queued)

        for queued in kept:
            try:
                self.events.put_nowait(queued)
            except queue.Full:
                break

        return dropped

    def _connect(self):
        if self.sock is None:
            sock = socket.create_connection(
                (self.host, self.port), timeout=self.socket_timeout
            )
            sock.settimeout(self.socket_timeout)
            self.sock = sock
            try:
                ready = self._recv_message()
                if ready.get("type") != "session_ready":
                    raise RuntimeError(f"unexpected subtitle handshake: {ready}")
                if ready.get("version") != self.PROTOCOL_VERSION:
                    raise RuntimeError(
                        f"unsupported subtitle protocol version: {ready.get('version')}"
                    )
                session_id = ready.get("session_id")
                if not isinstance(session_id, int) or isinstance(session_id, bool) or session_id <= 0:
                    raise RuntimeError(f"invalid subtitle session id: {session_id!r}")
            except Exception:
                self._close_socket()
                raise

            with self.stats_lock:
                self.stats["handshake_ok"] = True
                self.stats["protocol_version"] = self.PROTOCOL_VERSION
                self.stats["session_id"] = session_id
                self.stats["connections"] += 1
            self.ready_event.set()
            print(
                f"subtitle TCP connected to {self.host}:{self.port}, session={session_id}",
                flush=True,
            )

    def _recv_message(self):
        data = bytearray()
        while len(data) < self.RESPONSE_MAX_BYTES:
            chunk = self.sock.recv(1)
            if not chunk:
                raise EOFError("subtitle TCP connection closed")
            if chunk == b"\n":
                try:
                    message = json.loads(data.decode("utf-8"))
                except (UnicodeDecodeError, json.JSONDecodeError) as exc:
                    raise RuntimeError("invalid subtitle protocol JSON") from exc
                if not isinstance(message, dict):
                    raise RuntimeError("subtitle protocol response must be an object")
                return message
            if chunk != b"\r":
                data.extend(chunk)
        raise RuntimeError("subtitle protocol response too large")

    def _send_event(self, event):
        line = json.dumps(self._wire_event(event), ensure_ascii=False) + "\n"
        self._connect()
        session_id = self.stats_snapshot()["session_id"]
        sent_wall = time.time()
        sent_monotonic = time.monotonic()
        try:
            self.sock.sendall(line.encode("utf-8"))
            with self.stats_lock:
                self.stats["sent"] += 1
            ack = self._recv_message()
            ack_wall = time.time()
            if ack.get("type") != "transcript_ack":
                raise RuntimeError(f"unexpected subtitle response: {ack}")
            if ack.get("version") != self.PROTOCOL_VERSION:
                raise RuntimeError(f"invalid ACK version: {ack.get('version')}")
            if ack.get("session_id") != session_id:
                raise RuntimeError(
                    f"ACK session mismatch: expected {session_id}, got {ack.get('session_id')}"
                )
            if ack.get("seq") != event.get("seq"):
                raise RuntimeError(
                    f"ACK sequence mismatch: expected {event.get('seq')}, got {ack.get('seq')}"
                )
            status = ack.get("status")
            if status not in {
                "accepted",
                "rejected_old_seq",
                "dropped_event_pool",
                "dropped_subtitle_queue",
            }:
                raise RuntimeError(f"invalid ACK status: {status!r}")
        except Exception as exc:
            self._record_unknown(event, session_id, sent_wall, exc)
            raise

        latency = max(0.0, time.monotonic() - sent_monotonic)
        record = {
            "generated_wall_sec": event.get("_sink_generated_wall_sec"),
            "sent_wall_sec": sent_wall,
            "ack_wall_sec": ack_wall,
            "ack_latency_sec": latency,
            "session_id": session_id,
            "seq": event.get("seq"),
            "is_final": bool(event.get("is_final", False)),
            "status": status,
        }
        with self.stats_lock:
            self.stats["ack_latency_sec"].append(latency)
            if self.stats["first_ack_wall_sec"] is None:
                self.stats["first_ack_wall_sec"] = ack_wall
            if status == "accepted":
                self.stats["accepted"] += 1
            else:
                self.stats["rejected"] += 1
        self._write_ack_record(record)

    def _record_unknown(self, event, session_id, sent_wall, exc):
        record = {
            "generated_wall_sec": event.get("_sink_generated_wall_sec"),
            "sent_wall_sec": sent_wall,
            "ack_wall_sec": None,
            "ack_latency_sec": None,
            "session_id": session_id,
            "seq": event.get("seq"),
            "is_final": bool(event.get("is_final", False)),
            "status": "delivery_unknown",
            "error": str(exc),
        }
        with self.stats_lock:
            self.stats["delivery_unknown"] += 1
            self.stats["protocol_errors"] += 1
        self._write_ack_record(record)

    def _record_sink_drop(self, event, status):
        with self.stats_lock:
            session_id = self.stats.get("session_id")
        record = {
            "generated_wall_sec": event.get("_sink_generated_wall_sec"),
            "sent_wall_sec": None,
            "ack_wall_sec": None,
            "ack_latency_sec": None,
            "session_id": session_id,
            "seq": event.get("seq"),
            "is_final": bool(event.get("is_final", False)),
            "status": status,
        }
        self._write_ack_record(record)

    def _write_ack_record(self, record):
        if getattr(self, "ack_file", None) is not None:
            with self.ack_lock:
                self.ack_file.write(json.dumps(record, ensure_ascii=False) + "\n")
                self.ack_file.flush()

    @staticmethod
    def _wire_event(event):
        """Keep the firmware-facing NDJSON small and contract-only."""
        return {
            "seq": event["seq"],
            "is_final": event["is_final"],
            "start_sec": event["start_sec"],
            "end_sec": event["end_sec"],
            "text": event["text"],
        }

    def _worker_main(self):
        while not self.stop_event.is_set():
            if self.sock is None:
                try:
                    self._connect()
                except (OSError, EOFError, RuntimeError) as exc:
                    print(f"subtitle TCP handshake failed: {exc}", flush=True)
                    if isinstance(exc, RuntimeError):
                        with self.stats_lock:
                            self.stats["protocol_errors"] += 1
                    self.stop_event.wait(0.5)
                    continue
            try:
                event = self.events.get(timeout=0.1)
            except queue.Empty:
                continue

            try:
                self._send_event(event)
            except (OSError, EOFError, RuntimeError) as exc:
                print(f"subtitle TCP delivery failed: {exc}", flush=True)
                self._close_socket()
                self.ready_event.clear()
            finally:
                self.events.task_done()

    def _close_socket(self):
        if self.sock is not None:
            try:
                self.sock.close()
            finally:
                self.sock = None

    def wait_ready(self, timeout):
        return self.ready_event.wait(timeout)

    def flush(self, timeout=5.0):
        deadline = time.monotonic() + timeout
        while self.events.unfinished_tasks and time.monotonic() < deadline:
            time.sleep(0.01)
        return self.events.unfinished_tasks == 0

    def stats_snapshot(self):
        with self.stats_lock:
            result = dict(self.stats)
            latencies = list(result.pop("ack_latency_sec"))
        result["ack_latency_sec"] = latencies
        result["reconnections"] = max(0, int(result.get("connections", 0)) - 1)
        if result["first_generated_wall_sec"] is not None and result["first_ack_wall_sec"] is not None:
            result["time_to_first_subtitle_sec"] = max(
                0.0,
                result["first_ack_wall_sec"] - result["first_generated_wall_sec"],
            )
        else:
            result["time_to_first_subtitle_sec"] = None
        return result

    def close(self):
        self.flush(timeout=5.0)
        self.stop_event.set()
        if self.sock is not None:
            try:
                self.sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
        self.worker.join(timeout=2.0)
        self._close_socket()
        if self.ack_file is not None:
            self.ack_file.close()
            self.ack_file = None


class CompositeTranscriptSink(TranscriptSink):
    def __init__(self, sinks):
        self.sinks = sinks

    def handle_event(self, event):
        for sink in self.sinks:
            sink.handle_event(event)

    def close(self):
        for sink in self.sinks:
            sink.close()


def validate_audio_format(rate, channels, fmt):
    if rate <= 0:
        raise RuntimeError(f"invalid audio rate: {rate}")
    if channels != EXPECTED_CHANNELS:
        raise RuntimeError(f"expected mono audio, got {channels} channels")
    if fmt != FORMAT_S16_LE:
        raise RuntimeError(f"unsupported audio format: {fmt}")



