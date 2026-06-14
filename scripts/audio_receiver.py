#!/usr/bin/env python3
"""Receive subtitle_overlay_fw TCP PCM chunks and optionally simulate STT."""

import argparse
import json
import random
import socket
import struct
import wave


STREAM_MAGIC = b"SAUDPCM\x00"
STREAM_HEADER = "!6I"
CHUNK_HEADER = "!QQ5I"
FORMAT_S16_LE = 1

FAKE_WORDS = (
    "hola",
    "sistema",
    "audio",
    "video",
    "subtitulo",
    "prueba",
    "senal",
    "pantalla",
    "latencia",
    "pipeline",
    "captura",
    "texto",
    "resultado",
    "final",
    "parcial",
)


def recv_exact(sock, size):
    data = bytearray()
    while len(data) < size:
        chunk = sock.recv(size - len(data))
        if not chunk:
            raise EOFError("connection closed")
        data.extend(chunk)
    return bytes(data)


class FakeSTTEngine:
    """Generate STT-like transcript events from received audio chunks."""

    def __init__(self, chunk_word_interval, words_per_final, words=None, seed=None):
        if chunk_word_interval <= 0:
            raise ValueError("chunk_word_interval must be positive")
        if words_per_final <= 0:
            raise ValueError("words_per_final must be positive")

        self.chunk_word_interval = chunk_word_interval
        self.words_per_final = words_per_final
        self.words = tuple(words or FAKE_WORDS)
        self.random = random.Random(seed)
        self.event_seq = 0
        self.word_count = 0
        self.phrase_words = []
        self.phrase_start_seq = None
        self.phrase_start_sec = None

    def process_chunk(self, seq, timestamp_ns, chunk_ms, dropped):
        if (seq + 1) % self.chunk_word_interval != 0:
            return None

        end_sec = timestamp_ns / 1_000_000_000.0
        if self.phrase_start_seq is None:
            self.phrase_start_seq = max(0, seq + 1 - self.chunk_word_interval)
            self.phrase_start_sec = max(
                0.0, end_sec - (self.chunk_word_interval * chunk_ms / 1000.0)
            )

        self.word_count += 1
        self.phrase_words.append(self.random.choice(self.words))
        is_final = self.word_count >= self.words_per_final

        event = {
            "type": "final" if is_final else "partial",
            "is_final": is_final,
            "seq": self.event_seq,
            "chunk_start": self.phrase_start_seq,
            "chunk_end": seq,
            "start_sec": round(self.phrase_start_sec, 3),
            "end_sec": round(end_sec, 3),
            "text": " ".join(self.phrase_words),
            "dropped": dropped,
        }
        self.event_seq += 1

        if is_final:
            self.word_count = 0
            self.phrase_words = []
            self.phrase_start_seq = None
            self.phrase_start_sec = None

        return event


class SubtitleSink:
    """Hook for future subtitle/bitmap senders."""

    def handle_event(self, event):
        raise NotImplementedError

    def close(self):
        pass


class ConsoleSubtitleSink(SubtitleSink):
    def handle_event(self, event):
        print(
            "stt "
            f"{event['type']}#{event['seq']} "
            f"chunks={event['chunk_start']}..{event['chunk_end']} "
            f"t={event['start_sec']:.3f}..{event['end_sec']:.3f}s "
            f"text={event['text']!r}"
        )


class JsonlSubtitleSink(SubtitleSink):
    def __init__(self, path):
        self.file = open(path, "w", encoding="utf-8")

    def handle_event(self, event):
        self.file.write(json.dumps(event, ensure_ascii=False) + "\n")
        self.file.flush()

    def close(self):
        self.file.close()


class TcpSubtitleSink(SubtitleSink):
    def __init__(self, host, port):
        self.host = host
        self.port = port
        self.sock = None

    def _connect(self):
        if self.sock is None:
            self.sock = socket.create_connection((self.host, self.port), timeout=2.0)
            print(f"subtitle sink connected to {self.host}:{self.port}")

    def handle_event(self, event):
        line = json.dumps(event, ensure_ascii=False) + "\n"
        try:
            self._connect()
            self.sock.sendall(line.encode("utf-8"))
        except OSError as exc:
            print(f"subtitle sink send failed: {exc}")
            self.close()

    def close(self):
        if self.sock is not None:
            self.sock.close()
            self.sock = None


class CompositeSubtitleSink(SubtitleSink):
    def __init__(self, sinks):
        self.sinks = sinks

    def handle_event(self, event):
        for sink in self.sinks:
            sink.handle_event(event)

    def close(self):
        for sink in self.sinks:
            sink.close()


class BitmapSubtitleSink(SubtitleSink):
    """Placeholder for the future subtitle bitmap/result sender."""

    def handle_event(self, event):
        del event


class AudioReceiver:
    def __init__(self, host, port, output, stt_engine=None, subtitle_sink=None):
        self.host = host
        self.port = port
        self.output = output
        self.stt_engine = stt_engine
        self.subtitle_sink = subtitle_sink

    def run(self):
        with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as server:
            server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
            server.bind((self.host, self.port))
            server.listen(1)
            print(f"listening on {self.host}:{self.port}")

            conn, addr = server.accept()
            with conn:
                print(f"connected from {addr[0]}:{addr[1]}")
                self._receive_stream(conn)

    def _receive_stream(self, conn):
        magic = recv_exact(conn, len(STREAM_MAGIC))
        if magic != STREAM_MAGIC:
            raise RuntimeError(f"bad stream magic: {magic!r}")

        header = recv_exact(conn, struct.calcsize(STREAM_HEADER))
        rate, channels, fmt, chunk_ms, samples_per_chunk, bytes_per_chunk = struct.unpack(
            STREAM_HEADER, header
        )
        if fmt != FORMAT_S16_LE:
            raise RuntimeError(f"unsupported audio format: {fmt}")

        print(
            f"stream: {rate} Hz, {channels} ch, {chunk_ms} ms chunks, "
            f"{samples_per_chunk} samples/chunk"
        )

        with wave.open(self.output, "wb") as wav:
            wav.setnchannels(channels)
            wav.setsampwidth(2)
            wav.setframerate(rate)

            try:
                while True:
                    self._receive_chunk(conn, wav, rate, channels, fmt, chunk_ms, bytes_per_chunk)
            except EOFError:
                print("connection closed")

    def _receive_chunk(self, conn, wav, rate, channels, fmt, chunk_ms, bytes_per_chunk):
        chunk_header = recv_exact(conn, struct.calcsize(CHUNK_HEADER))
        seq, timestamp_ns, payload_bytes, dropped, chunk_rate, chunk_channels, chunk_fmt = (
            struct.unpack(CHUNK_HEADER, chunk_header)
        )
        if payload_bytes > bytes_per_chunk:
            raise RuntimeError(f"bad payload size: {payload_bytes}")
        if (chunk_rate, chunk_channels, chunk_fmt) != (rate, channels, fmt):
            raise RuntimeError("chunk format changed")

        payload = recv_exact(conn, payload_bytes)
        wav.writeframes(payload)

        if seq % 50 == 0:
            seconds = timestamp_ns / 1_000_000_000.0
            print(f"seq={seq} t={seconds:.3f}s dropped={dropped}")

        if self.stt_engine is not None and self.subtitle_sink is not None:
            event = self.stt_engine.process_chunk(seq, timestamp_ns, chunk_ms, dropped)
            if event is not None:
                self.subtitle_sink.handle_event(event)


def main():
    parser = argparse.ArgumentParser(description="Receive USB audio PCM stream")
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=5000)
    parser.add_argument("--output", default="usb_audio_capture.wav")
    parser.add_argument("--simulate-stt", action="store_true")
    parser.add_argument("--jsonl", help="write simulated STT events as JSON Lines")
    parser.add_argument("--words-per-final", type=int, default=6)
    parser.add_argument("--chunk-word-interval", type=int, default=25)
    parser.add_argument("--stt-seed", type=int, help="seed for repeatable fake transcripts")
    parser.add_argument("--send-subtitles", action="store_true")
    parser.add_argument("--subtitle-host", default="192.168.1.10")
    parser.add_argument("--subtitle-port", type=int, default=5001)
    args = parser.parse_args()

    sinks = []
    if args.simulate_stt:
        sinks.append(ConsoleSubtitleSink())
    if args.jsonl:
        sinks.append(JsonlSubtitleSink(args.jsonl))
    if args.send_subtitles:
        sinks.append(TcpSubtitleSink(args.subtitle_host, args.subtitle_port))

    stt_engine = None
    subtitle_sink = None
    if sinks:
        stt_engine = FakeSTTEngine(
            args.chunk_word_interval,
            args.words_per_final,
            seed=args.stt_seed,
        )
        subtitle_sink = CompositeSubtitleSink(sinks)

    receiver = AudioReceiver(
        args.host,
        args.port,
        args.output,
        stt_engine=stt_engine,
        subtitle_sink=subtitle_sink,
    )

    try:
        receiver.run()
    finally:
        if subtitle_sink is not None:
            subtitle_sink.close()


if __name__ == "__main__":
    main()
