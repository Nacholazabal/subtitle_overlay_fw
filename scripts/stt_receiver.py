#!/usr/bin/env python3
"""Receive board PCM audio and transcribe it with faster-whisper."""

import argparse
import json
import queue
import socket
import struct
import threading
import time
import wave


STREAM_MAGIC = b"SAUDPCM\x00"
STREAM_HEADER = "!6I"
CHUNK_HEADER = "!QQ5I"
FORMAT_S16_LE = 1

WHISPER_RATE = 16000
EXPECTED_CHANNELS = 1
EXPECTED_SAMPLE_WIDTH = 2


def recv_exact(sock, size):
    data = bytearray()
    while len(data) < size:
        chunk = sock.recv(size - len(data))
        if not chunk:
            raise EOFError("connection closed")
        data.extend(chunk)
    return bytes(data)


def pcm_s16le_to_float32(payload):
    import numpy as np

    samples = np.frombuffer(payload, dtype="<i2")
    return samples.astype(np.float32) / 32768.0


def resample_audio(samples, source_rate, target_rate):
    if source_rate == target_rate:
        return samples

    import numpy as np

    if samples.size == 0:
        return samples

    target_size = int(round(samples.size * target_rate / source_rate))
    if target_size <= 0:
        return np.empty(0, dtype=np.float32)

    source_positions = np.arange(samples.size, dtype=np.float64)
    target_positions = np.arange(target_size, dtype=np.float64) * source_rate / target_rate
    resampled = np.interp(target_positions, source_positions, samples)
    return resampled.astype(np.float32)


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
    def __init__(self, host, port, max_queue=32):
        self.host = host
        self.port = port
        self.sock = None
        self.events = queue.Queue(maxsize=max_queue)
        self.stop_event = threading.Event()
        self.worker = threading.Thread(target=self._worker_main, daemon=True)
        self.worker.start()

    def handle_event(self, event):
        try:
            self.events.put_nowait(event)
        except queue.Full:
            print("subtitle TCP queue full: dropping transcript event", flush=True)

    def _connect(self):
        if self.sock is None:
            self.sock = socket.create_connection((self.host, self.port), timeout=2.0)
            self.sock.settimeout(2.0)
            print(f"subtitle TCP connected to {self.host}:{self.port}", flush=True)

    def _send_event(self, event):
        line = json.dumps(event, ensure_ascii=False) + "\n"
        self._connect()
        self.sock.sendall(line.encode("utf-8"))

    def _worker_main(self):
        while not self.stop_event.is_set():
            try:
                event = self.events.get(timeout=0.1)
            except queue.Empty:
                continue

            while not self.stop_event.is_set():
                try:
                    self._send_event(event)
                    break
                except OSError as exc:
                    print(f"subtitle TCP send failed: {exc}", flush=True)
                    self._close_socket()
                    self.stop_event.wait(0.5)

    def _close_socket(self):
        if self.sock is not None:
            try:
                self.sock.close()
            finally:
                self.sock = None

    def close(self):
        self.stop_event.set()
        if self.sock is not None:
            try:
                self.sock.shutdown(socket.SHUT_RDWR)
            except OSError:
                pass
        self._close_socket()
        self.worker.join(timeout=2.0)


class CompositeTranscriptSink(TranscriptSink):
    def __init__(self, sinks):
        self.sinks = sinks

    def handle_event(self, event):
        for sink in self.sinks:
            sink.handle_event(event)

    def close(self):
        for sink in self.sinks:
            sink.close()


class FasterWhisperEngine:
    def __init__(self, model_size, device, compute_type, language, beam_size, vad_filter):
        from faster_whisper import WhisperModel

        print(
            f"loading faster-whisper model={model_size} device={device} "
            f"compute_type={compute_type}",
            flush=True,
        )
        self.model = WhisperModel(model_size, device=device, compute_type=compute_type)
        self.language = language
        self.beam_size = beam_size
        self.vad_filter = vad_filter
        self.seq = 0

    def transcribe_chunk(self, audio, start_sec, end_sec):
        segments, _info = self.model.transcribe(
            audio,
            language=self.language,
            beam_size=self.beam_size,
            vad_filter=self.vad_filter,
        )
        text = " ".join(segment.text.strip() for segment in segments).strip()
        if not text:
            return None

        event = {
            "seq": self.seq,
            "is_final": True,
            "start_sec": round(start_sec, 3),
            "end_sec": round(end_sec, 3),
            "text": text,
        }
        self.seq += 1
        return event


class ChunkTranscriber:
    def __init__(self, engine, sink, target_rate, chunk_sec, max_pending_chunks=4):
        import numpy as np

        self.engine = engine
        self.sink = sink
        self.source_rate = target_rate
        self.target_rate = target_rate
        self.samples_per_chunk = int(round(target_rate * chunk_sec))
        self.buffer = np.empty(0, dtype=np.float32)
        self.next_start_sample = 0
        self.pending_chunks = queue.Queue(maxsize=max_pending_chunks)
        self.stop_event = threading.Event()
        self.worker = threading.Thread(target=self._worker_main, daemon=True)
        self.worker.start()

    def set_source_rate(self, source_rate):
        self.source_rate = source_rate
        if source_rate != self.target_rate:
            print(
                f"resampling audio from {source_rate} Hz to {self.target_rate} Hz for Whisper",
                flush=True,
            )

    def push_pcm(self, payload):
        import numpy as np

        samples = pcm_s16le_to_float32(payload)
        samples = resample_audio(samples, self.source_rate, self.target_rate)
        if samples.size == 0:
            return

        self.buffer = np.concatenate((self.buffer, samples))
        while self.buffer.size >= self.samples_per_chunk:
            chunk = self.buffer[: self.samples_per_chunk].copy()
            self.buffer = self.buffer[self.samples_per_chunk :]
            self._queue_chunk(chunk)

    def flush(self):
        if self.buffer.size > 0:
            chunk = self.buffer.copy()
            self.buffer = self.buffer[:0]
            self._queue_chunk(chunk)
        self.pending_chunks.join()

    def close(self):
        self.flush()
        self.stop_event.set()
        self.worker.join(timeout=2.0)

    def _queue_chunk(self, chunk):
        start_sample = self.next_start_sample
        end_sample = start_sample + chunk.size
        self.next_start_sample = end_sample

        item = (chunk, start_sample, end_sample)
        try:
            self.pending_chunks.put_nowait(item)
            return
        except queue.Full:
            pass

        try:
            self.pending_chunks.get_nowait()
            self.pending_chunks.task_done()
            print("stt queue full: dropping oldest audio chunk", flush=True)
        except queue.Empty:
            pass

        try:
            self.pending_chunks.put_nowait(item)
        except queue.Full:
            print("stt queue full: dropping newest audio chunk", flush=True)

    def _worker_main(self):
        while not self.stop_event.is_set():
            try:
                chunk, start_sample, end_sample = self.pending_chunks.get(timeout=0.1)
            except queue.Empty:
                continue

            try:
                self._transcribe(chunk, start_sample, end_sample)
            finally:
                self.pending_chunks.task_done()

    def _transcribe(self, chunk, start_sample, end_sample):
        start_sec = start_sample / self.target_rate
        end_sec = end_sample / self.target_rate

        t0 = time.monotonic()
        event = self.engine.transcribe_chunk(chunk, start_sec, end_sec)
        elapsed = time.monotonic() - t0
        if event is None:
            print(f"stt empty t={start_sec:.3f}..{end_sec:.3f}s dt={elapsed:.3f}s", flush=True)
            return

        print(f"stt inference dt={elapsed:.3f}s", flush=True)
        self.sink.handle_event(event)


class TcpAudioReceiver:
    def __init__(self, host, port, transcriber):
        self.host = host
        self.port = port
        self.transcriber = transcriber

    def run(self):
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as server:
            server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            server.bind((self.host, self.port))
            server.listen(1)
            print(f"listening for board audio on {self.host}:{self.port}", flush=True)

            conn, addr = server.accept()
            with conn:
                print(f"connected from {addr[0]}:{addr[1]}", flush=True)
                self._receive_stream(conn)

    def _receive_stream(self, conn):
        magic = recv_exact(conn, len(STREAM_MAGIC))
        if magic != STREAM_MAGIC:
            raise RuntimeError(f"bad stream magic: {magic!r}")

        header = recv_exact(conn, struct.calcsize(STREAM_HEADER))
        rate, channels, fmt, chunk_ms, samples_per_chunk, bytes_per_chunk = struct.unpack(
            STREAM_HEADER, header
        )
        validate_audio_format(rate, channels, fmt)
        self.transcriber.set_source_rate(rate)

        print(
            f"stream: {rate} Hz, {channels} ch, {chunk_ms} ms chunks, "
            f"{samples_per_chunk} samples/chunk",
            flush=True,
        )

        try:
            while True:
                self._receive_chunk(conn, rate, channels, fmt, bytes_per_chunk)
        except EOFError:
            print("audio connection closed", flush=True)
            self.transcriber.flush()

    def _receive_chunk(self, conn, rate, channels, fmt, bytes_per_chunk):
        chunk_header = recv_exact(conn, struct.calcsize(CHUNK_HEADER))
        seq, timestamp_ns, payload_bytes, dropped, chunk_rate, chunk_channels, chunk_fmt = (
            struct.unpack(CHUNK_HEADER, chunk_header)
        )
        if payload_bytes > bytes_per_chunk:
            raise RuntimeError(f"bad payload size: {payload_bytes}")
        if (chunk_rate, chunk_channels, chunk_fmt) != (rate, channels, fmt):
            raise RuntimeError("chunk format changed")

        payload = recv_exact(conn, payload_bytes)
        if seq % 50 == 0:
            seconds = timestamp_ns / 1_000_000_000.0
            print(f"audio seq={seq} t={seconds:.3f}s dropped={dropped}", flush=True)

        self.transcriber.push_pcm(payload)


def validate_audio_format(rate, channels, fmt):
    if rate <= 0:
        raise RuntimeError(f"invalid audio rate: {rate}")
    if channels != EXPECTED_CHANNELS:
        raise RuntimeError(f"expected mono audio, got {channels} channels")
    if fmt != FORMAT_S16_LE:
        raise RuntimeError(f"unsupported audio format: {fmt}")


def run_wav(path, transcriber):
    with wave.open(path, "rb") as wav:
        rate = wav.getframerate()
        channels = wav.getnchannels()
        sample_width = wav.getsampwidth()
        if sample_width != EXPECTED_SAMPLE_WIDTH:
            raise RuntimeError(f"expected 16-bit PCM WAV, got sample width {sample_width}")
        validate_audio_format(rate, channels, FORMAT_S16_LE)
        transcriber.set_source_rate(rate)

        frames_per_read = max(1, rate // 10)
        print(f"reading WAV {path}: {rate} Hz, {channels} ch", flush=True)
        while True:
            payload = wav.readframes(frames_per_read)
            if not payload:
                break
            transcriber.push_pcm(payload)

    transcriber.flush()


def build_sink(args):
    sinks = [ConsoleTranscriptSink()]
    if args.jsonl:
        sinks.append(JsonlTranscriptSink(args.jsonl))
    if args.send_subtitles:
        sinks.append(TcpTranscriptSink(args.subtitle_host, args.subtitle_port))
    return CompositeTranscriptSink(sinks)


def parse_args():
    parser = argparse.ArgumentParser(description="Receive PCM audio and run faster-whisper STT")
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=5000)
    parser.add_argument("--wav", help="transcribe a 16 kHz mono S16_LE WAV instead of TCP")
    parser.add_argument("--chunk-sec", type=float, default=2.0)
    parser.add_argument("--model", default="small")
    parser.add_argument("--device", default="cpu")
    parser.add_argument("--compute-type", default="int8")
    parser.add_argument("--language", default="es")
    parser.add_argument("--beam-size", type=int, default=5)
    parser.add_argument("--vad-filter", action="store_true")
    parser.add_argument("--jsonl", help="write transcript events as JSON Lines")
    parser.add_argument("--send-subtitles", action="store_true")
    parser.add_argument("--subtitle-host", default="192.168.1.10")
    parser.add_argument("--subtitle-port", type=int, default=5001)
    args = parser.parse_args()

    if args.chunk_sec <= 0.0:
        parser.error("--chunk-sec must be positive")
    if args.chunk_sec < 1.0 or args.chunk_sec > 3.0:
        print("warning: recommended --chunk-sec range is 1.0 to 3.0 seconds", flush=True)

    return args


def main():
    args = parse_args()
    sink = build_sink(args)
    transcriber = None

    try:
        engine = FasterWhisperEngine(
            model_size=args.model,
            device=args.device,
            compute_type=args.compute_type,
            language=args.language,
            beam_size=args.beam_size,
            vad_filter=args.vad_filter,
        )
        transcriber = ChunkTranscriber(engine, sink, WHISPER_RATE, args.chunk_sec)

        if args.wav:
            run_wav(args.wav, transcriber)
        else:
            TcpAudioReceiver(args.host, args.port, transcriber).run()
    finally:
        if transcriber is not None:
            transcriber.close()
        sink.close()


if __name__ == "__main__":
    main()
