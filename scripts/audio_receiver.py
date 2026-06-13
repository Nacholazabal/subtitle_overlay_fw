#!/usr/bin/env python3
"""Receive subtitle_overlay_fw TCP PCM chunks and write a WAV file."""

import argparse
import socket
import struct
import wave


STREAM_MAGIC = b"SAUDPCM\x00"
STREAM_HEADER = "!6I"
CHUNK_HEADER = "!QQ5I"
FORMAT_S16_LE = 1


def recv_exact(sock, size):
    data = bytearray()
    while len(data) < size:
        chunk = sock.recv(size - len(data))
        if not chunk:
            raise EOFError("connection closed")
        data.extend(chunk)
    return bytes(data)


def main():
    parser = argparse.ArgumentParser(description="Receive USB audio PCM stream")
    parser.add_argument("--host", default="0.0.0.0")
    parser.add_argument("--port", type=int, default=5000)
    parser.add_argument("--output", default="usb_audio_capture.wav")
    args = parser.parse_args()

    with socket.socket(socket.AF_INET, socket.SOCK_STREAM) as server:
        server.setsockopt(socket.SOL_SOCKET, socket.SO_REUSEADDR, 1)
        server.bind((args.host, args.port))
        server.listen(1)
        print(f"listening on {args.host}:{args.port}")

        conn, addr = server.accept()
        with conn:
            print(f"connected from {addr[0]}:{addr[1]}")

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

            with wave.open(args.output, "wb") as wav:
                wav.setnchannels(channels)
                wav.setsampwidth(2)
                wav.setframerate(rate)

                try:
                    while True:
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
                except EOFError:
                    print("connection closed")


if __name__ == "__main__":
    main()
